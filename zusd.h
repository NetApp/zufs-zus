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

struct thread_param {
	const char* path;
	int policy;
	int rr_priority;
};

int zus_mount_thread_start(struct thread_param *tp);
void zus_mount_thread_stop(void);
void zus_join(void);
