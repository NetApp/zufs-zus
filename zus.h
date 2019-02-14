/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zus.h - C Wrappers over the ZUFS_IOCTL Api
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#ifndef __ZUS_H__
#define __ZUS_H__

#include "zus_api.h"
#include "_pr.h"
#include "a_list.h"

#ifndef likely
#define likely(x_)	__builtin_expect(!!(x_), 1)
#define unlikely(x_)	__builtin_expect(!!(x_), 0)
#endif

/* utils.c */
void zus_dump_stack(FILE *, bool warn, const char *fmt, ...);
void zus_warn(const char *cond, const char *file, int line);
void zus_bug(const char *cond, const char *file, int line);


/* printz.c */
int zus_add_module_ddbg(const char *fs_name, void *handle);
void zus_free_ddbg_db(void);
int zus_ddbg_read(struct zufs_ddbg_info *zdi);
int zus_ddbg_write(struct zufs_ddbg_info *zdi);

#define ZUS_LIBFS_MAX_NR	16	/* see also MAX_LOCKDEP_FSs in zuf */
#define ZUS_LIBFS_MAX_PATH	256

#endif /* define __ZUS_H__ */
