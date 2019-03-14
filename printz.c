#include "printz.h"
#include "zus.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <dlfcn.h>
#include <errno.h>
#include <ctype.h>

struct module_ddbg {
	char name[ZUS_LIBFS_MAX_PATH];
	__u16 n_dbg_entries;
	struct _ddebug *dbg_entries[0];
};

static struct ddbg_db {
	struct module_ddbg *modules[ZUS_LIBFS_MAX_NR];
	__u16 mod_count;
	__u32 next_id;
} ddbg_db = {};

static void _init_ddbg(struct _ddebug *dd, const char *modname)
{
	char *no_path_name = strrchr(dd->filename, '/');

	dd->id = ++ddbg_db.next_id;
	dd->modname = modname;
	if (no_path_name)
		dd->filename = no_path_name + 1;
}

int zus_add_module_ddbg(const char *fs_name, void *handle)
{
	struct _ddebug *iter, *stop;
	size_t mddbg_sz;
	int i;
	int n_dbg;
	struct module_ddbg *modd;

	if (strlen(fs_name) >= sizeof(modd->name)) {
		ERROR("Name too-long fs_name=%s\n", fs_name);
		return -EINVAL;
	}

	iter = dlsym(handle, "__start_zus_ddbg");
	if (!iter) {
		ERROR("Unable to get library start symbol\n");
		return -EINVAL;
	}

	stop = dlsym(handle, "__stop_zus_ddbg");
	if (!stop) {
		ERROR("Unable to get library start symbol\n");
		return -EINVAL;
	}
	n_dbg = stop - iter;
	mddbg_sz = sizeof(struct module_ddbg) +
			n_dbg * sizeof(struct _ddebug *);
	ddbg_db.modules[ddbg_db.mod_count] = calloc(1, mddbg_sz);
	if (!ddbg_db.modules[ddbg_db.mod_count])
		return -ENOMEM;
	modd = ddbg_db.modules[ddbg_db.mod_count];

	strncpy(modd->name, fs_name, sizeof(modd->name) - 1);

	modd->n_dbg_entries = n_dbg;
	for (i = 0; i < n_dbg; ++i, ++iter) {
		_init_ddbg(iter, modd->name);
		modd->dbg_entries[i] = iter;
	}

	++ddbg_db.mod_count;
	return 0;
}

void zus_free_ddbg_db(void)
{
	int i;

	for (i = 0; i < ddbg_db.mod_count && ddbg_db.modules[i]; ++i)
		free(ddbg_db.modules[i]);
}

static void _copy_format(const char *fmt, char *buff, size_t sz)
{
	size_t len = strlen(fmt);
	size_t i;
	size_t buff_i = 0;

	for (i = 0; i < len && buff_i < sz; ++i) {
		switch (fmt[i]) {
		case '\n':
		case '\t':
			if (buff_i + 2 > sz)
				return;
			*buff++ = '\\';
			*buff++ = fmt[i] == '\n' ? 'n' : 't';
			buff_i += 2;
			break;
		default:
			*buff++ = fmt[i];
			++buff_i;
		}
	}
	*buff = 0;
}

#define MAX_FORMAT_SIZE 512

int zus_ddbg_read(struct zufs_ddbg_info *zdi)
{
	char *buff = zdi->msg;
	size_t buff_sz = zdi->len;
	struct module_ddbg *modd;
	struct _ddebug *ddbg;
	int mod_i, i;
	size_t buff_jmp;
	char format[MAX_FORMAT_SIZE + 1];

	for (mod_i = 0; mod_i < ddbg_db.mod_count; ++mod_i) {
		modd = ddbg_db.modules[mod_i];
		for (i = 0; i < modd->n_dbg_entries && buff_sz > 0; ++i) {
			ddbg = modd->dbg_entries[i];
			if (ddbg->id <= zdi->id)
				continue;

			_copy_format(ddbg->format, format, MAX_FORMAT_SIZE);
			buff_jmp =
				snprintf(buff, buff_sz,
					"%s:%d [%s] %s =%s \"%s\"\n",
					 ddbg->filename, ddbg->lineno,
					 ddbg->modname, ddbg->function,
					 ddbg->active ? "p" : "_",
					 format);
			if (buff_jmp > buff_sz) {
				*buff = 0;
				goto out;
			}
			buff_sz -= buff_jmp;
			buff += buff_jmp;
			zdi->id = ddbg->id;
		}
		++modd;
	}
out:
	zdi->len = strlen(zdi->msg);
	return 0;
}

enum ddbg_cmd {
	DDBG_CMD_UNSET = 0,
	DDBG_CMD_ENABLE,
	DDBG_CMD_DISABLE,
};

#define MAX_DDBG_CMD_TOKENS 9
struct ddbg_ctl {
	char *tokens[MAX_DDBG_CMD_TOKENS];
	int ntokens;

	const char *modname;
	const char *function;
	const char *filename;
	unsigned int lineno;
	const char *format;
	enum ddbg_cmd cmd;
};

static int _tokenize(char *buf, struct ddbg_ctl *cmd)
{
	while (*buf) {
		char *end;

		/* Skip leading whitespace */
		for (; isspace(*buf); ++buf)
			;
		if (!*buf)
			break;
		/* Skip comment */
		if (*buf == '#')
			break;

		if (*buf == '"' || *buf == '\'') {
			int quote = *buf++;

			for (end = buf; *end && *end != quote; end++)
				;
			if (!*end) {
				ERROR("unclosed quote: %s\n", buf);
				return -EINVAL;	/* unclosed quote */
			}
		} else {
			for (end = buf; *end && !isspace(*end); end++)
				;
		}

		/* `buf' is start of word, `end' is one past its end */
		if (cmd->ntokens == MAX_DDBG_CMD_TOKENS) {
			ERROR("too many ddbg cmd tokens\n");
			return -EINVAL;
		}
		if (*end)
			*end++ = 0;
		cmd->tokens[cmd->ntokens++] = buf;
		buf = end;
	}
	return 0;
}

#define NO_LINE_NUMBER ((unsigned int) -1)
enum {
	DDBG_CRIT_MOD = 1,
	DDBG_CRIT_FUNC,
	DDBG_CRIT_FILE,
	DDBG_CRIT_LINENO,
	DDBG_CRIT_FMT,
	DDBG_CRIT_ENABLE,
	DDBG_CRIT_DISABLE,
};

static int _token_type(const char *token)
{
	if (strcmp(token, "module") == 0)
		return DDBG_CRIT_MOD;
	if (strcmp(token, "func") == 0)
		return DDBG_CRIT_FUNC;
	if (strcmp(token, "file") == 0)
		return DDBG_CRIT_FILE;
	if (strcmp(token, "line") == 0)
		return DDBG_CRIT_LINENO;
	if (strcmp(token, "format") == 0)
		return DDBG_CRIT_FMT;
	if (strcmp(token, "+p") == 0)
		return DDBG_CRIT_ENABLE;
	if (strcmp(token, "-p") == 0)
		return DDBG_CRIT_DISABLE;
	return -EINVAL;
}

#define REQUIRE_ADDITIONAL_TOKEN \
	do { if (++i >= ddc->ntokens) return -EINVAL; } while (0)

static int _parse(struct ddbg_ctl *ddc)
{
	int i = 0;
	int ttype;

	while (i < ddc->ntokens) {
		ttype = _token_type(ddc->tokens[i]);
		switch (ttype) {
		case DDBG_CRIT_MOD:
			REQUIRE_ADDITIONAL_TOKEN;
			ddc->modname = ddc->tokens[i];
			break;
		case DDBG_CRIT_FUNC:
			REQUIRE_ADDITIONAL_TOKEN;
			ddc->function = ddc->tokens[i];
			break;
		case DDBG_CRIT_FILE:
			REQUIRE_ADDITIONAL_TOKEN;
			ddc->filename = ddc->tokens[i];
			break;
		case DDBG_CRIT_LINENO: {
			char *leftover = NULL;

			REQUIRE_ADDITIONAL_TOKEN;
			ddc->lineno = strtoul(ddc->tokens[i], &leftover, 10);
			if (*leftover) /* Someone is having fun */
				return -EINVAL;
			break;
		}
		case DDBG_CRIT_FMT:
			REQUIRE_ADDITIONAL_TOKEN;
			ddc->format = ddc->tokens[i];
			break;
		case DDBG_CRIT_ENABLE:
			ddc->cmd = DDBG_CMD_ENABLE;
			break;
		case DDBG_CRIT_DISABLE:
			ddc->cmd = DDBG_CMD_DISABLE;
			break;
		default:
			ERROR("Unkonwn token %s\n", ddc->tokens[i]);
			return -EINVAL;
		}
		++i;
	}
	if (ddc->cmd == DDBG_CMD_UNSET) {
		ERROR("no ddbg command is given\n");
		return -EINVAL;
	}
	return 0;
}

static int _process(struct ddbg_ctl *ddc)
{
	bool enable = ddc->cmd == DDBG_CMD_ENABLE;
	struct module_ddbg *modd;
	struct _ddebug *ddbg;
	int mod_i, i;

	for (mod_i = 0; mod_i < ddbg_db.mod_count; ++mod_i) {
		modd = ddbg_db.modules[mod_i];
		if (ddc->modname && strcmp(ddc->modname, modd->name))
			continue;
		for (i = 0; i < modd->n_dbg_entries; ++i) {
			ddbg = modd->dbg_entries[i];
			if (ddc->filename &&
			    strcmp(ddc->filename, ddbg->filename))
				continue;
			if (ddc->function &&
			    strcmp(ddc->function, ddbg->function))
				continue;
			if (ddc->lineno != NO_LINE_NUMBER &&
			    ddc->lineno != ddbg->lineno)
				continue;
			if (ddc->format && !strstr(ddbg->format, ddc->format))
				continue;
			ddbg->active = enable;
		}
		++modd;
	}
	return 0;
}

int zus_ddbg_write(struct zufs_ddbg_info *zdi)
{
	struct ddbg_ctl ddc = {
		.lineno = NO_LINE_NUMBER,
		.cmd = DDBG_CMD_UNSET,
	};
	int err;

	err = _tokenize(zdi->msg, &ddc);
	if (err)
		return err;

	err = _parse(&ddc);
	if (err)
		return err;

	err = _process(&ddc);
	if (err)
		return err;

	return 0;
}
