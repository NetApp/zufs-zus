/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zuf_call.c - C Wrappers over the ZUFS_IOCTL Api
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#ifndef ___PRINTZ_H__
#define ___PRINTZ_H__

#include <stdbool.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/types.h>
#include <linux/types.h>

#define LOG_STR(l) LOG_XSTR(l)
#define LOG_XSTR(l) "<"#l">"

struct _ddebug {
	__u32 id;
	const char *modname;
	const char *function;
	const char *filename;
	unsigned int lineno;
	const char *format;
	bool active;
} __attribute__((aligned(8)));

#define DEFINE_DYNAMIC_DEBUG_METADATA(name, fmt)	\
	static struct _ddebug				\
	__attribute__((aligned(8)))			\
	__attribute__((section("zus_ddbg"))) name = {	\
			.function = __func__,		\
			.filename = __FILE__,		\
			.lineno = __LINE__,		\
			.format = fmt,			\
			.active = false,		\
}

#define dyn_dbg_pr(fmt, args...)					\
do {									\
	DEFINE_DYNAMIC_DEBUG_METADATA(desc, fmt);			\
	if (desc.active)						\
		fprintf(stderr, LOG_STR(LOG_INFO) "%s: " fmt,		\
			desc.modname, ## args);				\
} while (0)

#define __pr(s, args ...) fprintf(stderr, s, ## args)

#define pr_crit(s, args ...) __pr("<2>" s, ## args)
#define pr_err(s, args ...) __pr("<3>" s, ## args)
#define pr_warning(s, args ...) __pr("<4>" s, ## args)
#define pr_warn pr_warning
#define pr_info(s, args ...) __pr("<5>" s, ## args)
#define pr_debug(s, args ...) dyn_dbg_pr(s, ## args)

#endif /* define ___PRINTZ_H__ */
