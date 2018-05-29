/*
 * zuf_call.c - C Wrappers over the ZUFS_IOCTL Api
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */
#ifndef ___PR_H__
#define ___PR_H__

#include <stdbool.h>
#include <stdio.h>

/* FIXME*/
#define ERROR(fmt, a...) printf("!!! %s:%d: " fmt, __func__, __LINE__, ##a)
#define INFO(fmt, a...) printf("zus %s:%d: " fmt, __func__, __LINE__, ##a)

extern bool g_DBG;

#define DBG(fmt, a...) if (g_DBG) printf("zus %s:%d: " fmt, __func__, __LINE__, ##a)
#define DBGCONT(fmt, a...) do { if (g_DBG) printf(fmt, ##a); } while(0)

#define md_dbg_err DBG

#endif /* define ___PR_H__ */
