/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * utils.c - Common utilities provided to fs via libzus
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/resource.h>
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <zus.h>

#define BACKTRACE_MAX 128

static int _dump_backtrace(FILE *fp)
{
	int err, lim = BACKTRACE_MAX;
	char sym[256] = "";
	unw_word_t ip, sp, off;
	unw_context_t context;
	unw_cursor_t cursor;

	err = unw_getcontext(&context);
	if (err != UNW_ESUCCESS)
		return err;

	err = unw_init_local(&cursor, &context);
	if (err != UNW_ESUCCESS)
		return err;

	while (lim-- > 0) {
		ip = sp = off = 0;
		err = unw_step(&cursor);
		if (err <= 0)
			return err;

		err = unw_get_reg(&cursor, UNW_REG_IP, &ip);
		if (err)
			return err;

		err = unw_get_reg(&cursor, UNW_REG_SP, &sp);
		if (err)
			return err;

		err = unw_get_proc_name(&cursor, sym, sizeof(sym) - 1, &off);
		if (err)
			return err;

		fprintf(fp, LOG_STR(LOG_WARNING) "zus_warn:        [<%p>] 0x%lx %s+0x%lx\n",
			(void *)ip, (long)sp, sym, (long)off);
	}
	return 0;
}

static void _dump_addr2line(FILE *fp)
{
	int i, len;
	void *arr[BACKTRACE_MAX];
	char ptrS[2048];
	char *m = ptrS;
	int s = sizeof(ptrS);

	len = unw_backtrace(arr, BACKTRACE_MAX);

	for (i = 0; i < len - 3; ++i) {
		int l;

		if (!(i % 5)) {
			l = snprintf(m, s, "\\\n				");
			s -= l; m += l;
		}
		l = snprintf(m, s, "%p ", arr[i + 1]);
		s -= l; m += l;
	}

	fprintf(fp, LOG_STR(LOG_WARNING) "zus_warn: addr2line -a -C -e %s -f -p -s %s\n",
		program_invocation_name, ptrS);
}

void zus_dump_stack(FILE *fp, const char *fmt, ...)
{
	va_list args;

	flockfile(fp);

	va_start(args, fmt);
	vfprintf(fp, fmt, args);
	va_end(args);

	_dump_backtrace(fp);
	_dump_addr2line(fp);

	funlockfile(fp);
}

void zus_warn(const char *cond, const char *file, int line)
{
	zus_dump_stack(stderr, LOG_STR(LOG_WARNING) "%s: %s (%s:%d)\n", __func__,
		       cond, file, line);
}

void zus_bug(const char *cond, const char *file, int line)
{
	zus_dump_stack(stderr, LOG_STR(LOG_ERR) "%s: %s (%s:%d)\n", __func__,
		       cond, file, line);
	abort();
}

#define ZUS_MAX_FILES 16384

int zus_increase_max_files(void)
{
	struct rlimit rl;
	int err;

	err = getrlimit(RLIMIT_NOFILE, &rl);
	if (err) {
		ERROR("getrlimit failed => %d", -errno);
		return -errno;
	}

	if (rl.rlim_cur < ZUS_MAX_FILES)
		rl.rlim_cur = ZUS_MAX_FILES;
	if (rl.rlim_max < ZUS_MAX_FILES)
		rl.rlim_max = ZUS_MAX_FILES;

	err = setrlimit(RLIMIT_NOFILE, &rl);
	if (err) {
		ERROR("setrlimit %lu/%lu failed => %d\n", rl.rlim_cur,
		      rl.rlim_max, -errno);
		return -errno;
	}

	return 0;
}

