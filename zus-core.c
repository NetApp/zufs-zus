/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zus-core.c - A program that calls into the ZUS IOCTL server API
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#define _GNU_SOURCE

#include <errno.h>
#include <unistd.h>

#include "zus.h"
#include "zusd.h"

ulong g_DBGMASK;
