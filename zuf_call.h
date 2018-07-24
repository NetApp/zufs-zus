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

#define _GNU_SOURCE

#include <fcntl.h>
#include <sys/ioctl.h>

#include "zus.h"

/* Just a wraper for the commom unexpected print */
static inline
int _ioctl(int fd, ulong zu_vect, struct zufs_ioc_hdr *hdr, const char* msg)
{
	int ret;

	ret = ioctl(fd, zu_vect, hdr);
	if (ret) {
		ERROR("Unexpected ioctl => %d errno=%d zu_n=%lx zu_s=%s hdr=%d\n",
		      ret, errno, zu_vect, msg, hdr->err);
		return ret;
	}

	return hdr->err;
}

static inline
int zuf_register_fs(int fd, struct zus_fs_info *zfi)
{
	struct zufs_ioc_register_fs zirf = {
		.zus_zfi = zfi,
		.rfi = zfi->rfi,
	};

	return _ioctl(fd, ZU_IOC_REGISTER_FS, &zirf.hdr, "ZU_IOC_REGISTER_FS");
}

static inline
int zuf_recieve_mount(int fd, struct  zufs_ioc_mount *zim)
{
	return _ioctl(fd, ZU_IOC_MOUNT, &zim->hdr, "ZU_IOC_MOUNT");
}

static inline
int zuf_numa_map(int fd, struct zufs_ioc_numa_map *zinm)
{
	return _ioctl(fd, ZU_IOC_NUMA_MAP, &zinm->hdr, "ZU_IOC_NUMA_MAP");
}

static inline
int zuf_grab_pmem(int fd, uint pmem_kern_id, struct zufs_ioc_pmem *zip)
{
	zip->pmem_kern_id = pmem_kern_id;
	return _ioctl(fd, ZU_IOC_GRAB_PMEM, &zip->hdr, "ZU_IOC_GRAB_PMEM");
}

static inline
int zuf_zt_init(int fd, int cpu_num, uint max_command)
{
	struct zufs_ioc_init zii = {
		.affinity = cpu_num,
		.max_command = max_command,
	};

	return _ioctl(fd, ZU_IOC_INIT_THREAD, &zii.hdr, "ZU_IOC_INIT_THREAD");
}

static inline
int zuf_wait_opt(int fd, struct zufs_ioc_wait_operation *opt /*OUT*/)
{
	return _ioctl(fd, ZU_IOC_WAIT_OPT, &opt->hdr, "ZU_IOC_WAIT_OPT");
}

static inline
int zuf_break_all(int fd)
{
	struct zufs_ioc_break_all zba = {};

	return _ioctl(fd, ZU_IOC_BREAK_ALL, &zba.hdr, "ZU_IOC_BREAK_ALL");
}

static inline
int zuf_iomap_exec(int fd, struct zufs_ioc_iomap_exec *ziome)
{
	return _ioctl(fd, ZU_IOC_IOMAP_EXEC, &ziome->hdr, "ZU_IOC_IOMAP_EXEC");
}
