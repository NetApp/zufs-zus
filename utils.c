/*
 * utils.c - Common utilities provided to fs via libzus
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
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

		fprintf(fp, "[<%p>] 0x%lx %s+0x%lx\n",
			(void *)ip, (long)sp, sym, (long)off);
	}
	return 0;
}

static void _dump_addr2line(FILE *fp)
{
	int i, len;
	void *arr[BACKTRACE_MAX];

	len = unw_backtrace(arr, BACKTRACE_MAX);
	fprintf(fp, "addr2line -a -C -e %s -f -p -s ", program_invocation_name);
	for (i = 1; i < len - 2; ++i)
		fprintf(fp, "%p ", arr[i]);
	fputs("\n", fp);
}

void zus_warn(const char *cond, const char *file, int line)
{
	FILE *fp = stderr;

	fprintf(fp, "%s: %s (%s:%d)\n", __func__, cond, file, line);
	_dump_backtrace(fp);
	_dump_addr2line(fp);
}

void zus_bug(const char *cond, const char *file, int line)
{
	FILE *fp = stderr;

	fprintf(fp, "%s: %s (%s:%d)\n", __func__, cond, file, line);
	_dump_backtrace(fp);
	_dump_addr2line(fp);
	abort();
}

