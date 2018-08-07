/*
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */
#include <pthread.h>

#include "zus.h"

int zus_mount_thread_start(struct zus_thread_params *tp, const char* zuf_path);
void zus_mount_thread_stop(void);
void zus_join(void);
