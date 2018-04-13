/*
 * The toyfs reference file-system implementation via zufs
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include "toyfs.h"

void toyfs_panicf(const char *file, int line, const char *fmt, ...)
{
	va_list ap;
	FILE *fp = stderr;

	flockfile(fp);
	fputs("toyfs: ", fp);
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fprintf(fp, " (%s:%d)\n", file, line);
	funlockfile(fp);
	abort();
}

void toyfs_mutex_init(pthread_mutex_t *mutex)
{
	int err;
	pthread_mutexattr_t attr;

	err = pthread_mutexattr_init(&attr);
	toyfs_panic_if_err(err, "pthread_mutexattr_init");

	err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	toyfs_panic_if_err(err, "pthread_mutexattr_settype");

	err = pthread_mutex_init(mutex, &attr);
	toyfs_panic_if_err(err, "pthread_mutex_init");

	err = pthread_mutexattr_destroy(&attr);
	toyfs_panic_if_err(err, "pthread_mutexattr_destroy");
}

void toyfs_mutex_destroy(pthread_mutex_t *mutex)
{
	int err;

	err = pthread_mutex_destroy(mutex);
	toyfs_panic_if_err(err, "pthread_mutex_destroy");
}

void toyfs_mutex_lock(pthread_mutex_t *mutex)
{
	int err;

	err = pthread_mutex_lock(mutex);
	toyfs_panic_if_err(err, "pthread_mutex_lock");
}

void toyfs_mutex_unlock(pthread_mutex_t *mutex)
{
	int err;

	err = pthread_mutex_unlock(mutex);
	toyfs_panic_if_err(err, "pthread_mutex_unlock");
}

struct toyfs_sb_info *toyfs_zsbi_to_sbi(struct zus_sb_info *zsbi)
{
	return container_of(zsbi, struct toyfs_sb_info, s_zus_sbi);
}

struct toyfs_inode_info *toyfs_zii_to_tii(struct zus_inode_info *zii)
{
	return zii ? container_of(zii, struct toyfs_inode_info, zii) : NULL;
}

/*. . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . . .*/

const struct zus_zii_operations toyfs_zii_op = {
	.evict = toyfs_evict,
	.read = toyfs_read,
	.write = toyfs_write,
	.setattr = toyfs_setattr,
	.get_symlink = toyfs_get_symlink,
	.sync = toyfs_sync,
	.fallocate = toyfs_fallocate,
	.seek = toyfs_seek,
	.get_block = toyfs_get_block,
};

const struct zus_sbi_operations toyfs_sbi_op = {
	.zii_alloc = toyfs_zii_alloc,
	.zii_free = toyfs_zii_free,
	.new_inode = toyfs_new_inode,
	.free_inode = toyfs_free_inode,
	.add_dentry = toyfs_add_dentry,
	.remove_dentry = toyfs_remove_dentry,
	.lookup = toyfs_lookup,
	.iget = toyfs_iget,
	.rename = toyfs_rename,
	.readdir = toyfs_readdir,
	.clone = toyfs_clone,
	.statfs = toyfs_statfs,
};

const struct zus_zfi_operations toyfs_zfi_op = {
	.sbi_alloc = toyfs_sbi_alloc,
	.sbi_free = toyfs_sbi_free,
	.sbi_init = toyfs_sbi_init,
	.sbi_fini = toyfs_sbi_fini,
};

/* Is not const because it is hanged on a list_head */
static struct zus_fs_info toyfs_zfi = {
	.rfi.fsname = "toyfs",
	.rfi.FS_magic = TOYFS_SUPER_MAGIC,
	.rfi.FS_ver_major = TOYFS_MAJOR_VERSION,
	.rfi.FS_ver_minor = TOYFS_MINOR_VERSION,
	.rfi.dt_offset = 0,
	.rfi.s_time_gran = 1,
	.rfi.def_mode = 0755,
	.rfi.s_maxbytes = MAX_LFS_FILESIZE,
	.rfi.acl_on = 1,
	.op = &toyfs_zfi_op,
	.sbi_op = &toyfs_sbi_op,
	.user_page_size = 0,
	.next_sb_id = 0,
};

int toyfs_register_fs(int fd)
{
	return zus_register_one(fd, &toyfs_zfi);
}

