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
#ifndef ___PR_H__
#define ___PR_H__

#include <stdbool.h>
#include <stdio.h>

#include "printz.h"

/* FIXME*/
#define ERROR(fmt, a...) \
	fprintf(stderr, LOG_STR(LOG_ERR) \
		"zus: [%s:%d]: " fmt, __func__, __LINE__, ##a)
#define INFO(fmt, a...) \
	fprintf(stderr, LOG_STR(LOG_INFO) "zus: ~info~ " fmt, ##a)

extern ulong g_DBGMASK;
#define ZUS_DBGPRNT  (g_DBGMASK & 1)

#define DBG(fmt, a...) \
	do { if (unlikely(ZUS_DBGPRNT)) \
		fprintf(stderr, LOG_STR(LOG_INFO) \
			"zus: [%s:%d]: " fmt, __func__, __LINE__, ##a); \
	} while (0)

#define DBGCONT(fmt, a...) \
	do { if (unlikely(ZUS_DBGPRNT)) fprintf(stderr, fmt, ##a); } while (0)

#define md_dbg_err DBG
#define md_warn_cnd(silent, s, args ...) \
	do {if (!silent) \
		fprintf(stderr, LOG_STR(LOG_WARNING) \
			"md-zus: [%s:%d] " s, __func__, __LINE__, ## args); \
	} while (0)

#endif /* define ___PR_H__ */
