/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#include <pthread.h>

#include "zus.h"

int zus_mount_thread_start(struct zus_thread_params *tp, const char* zuf_path);
void zus_mount_thread_stop(void);
void zus_join(void);
void zus_register_sigactions(void);
