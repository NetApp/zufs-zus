/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The toyfs reference file-system implementation via zufs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *      Shachar Sharon <sshachar@netapp.com>
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "zus.h"
#include "toyfs.h"


static ino_t _lookup(struct toyfs_inode_info *dir_tii, struct zufs_str *str)
{
	struct toyfs_dirent *dirent;

	DBG("lookup: dirino=%lu %.*s\n",
	    dir_tii->ino, str->len, str->name);

	dirent = toyfs_lookup_dirent(dir_tii, str);
	return dirent ? dirent->d_ino : TOYFS_NULL_INO;
}

ulong toyfs_lookup(struct zus_inode_info *dir_zii, struct zufs_str *str)
{
	return _lookup(Z2II(dir_zii), str);
}

static int _do_rename(struct toyfs_inode_info *old_dir_ii,
		      struct toyfs_inode_info *new_dir_ii,
		      struct toyfs_inode_info *old_ii,
		      struct toyfs_inode_info *new_ii,
		      struct zufs_str *old_name,
		      struct zufs_str *new_name,
		      uint64_t time, uint flags)
{
	int err;
	struct toyfs_dirent *old_de, *new_de;

	DBG("rename: olddir_ino=%lu newdir_ino=%lu "
	    "old_name=%.*s new_name=%.*s time=%lu\n",
	    old_dir_ii->ino, new_dir_ii->ino,
	    old_name->len, old_name->name,
	    new_name->len, new_name->name, time);

	if (!old_ii)
		return -EINVAL;

	if (flags)  /* RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT */
		return -ENOTSUP;

	old_de = toyfs_lookup_dirent(old_dir_ii, old_name);
	if (unlikely(!old_de))
		return -ENOENT;

	new_de = toyfs_lookup_dirent(new_dir_ii, new_name);
	if (!new_de) {
		err = toyfs_add_dirent(new_dir_ii, old_ii, new_name, &new_de);
		if (err)
			return err;
	}
	toyfs_remove_dirent(old_dir_ii, old_ii, old_de);

	if (S_ISDIR(old_ii->ti->i_mode))
		old_ii->ti->i_nlink += 1;

	old_dir_ii->ti->i_mtime = time;
	old_dir_ii->ti->i_ctime = time;
	new_dir_ii->ti->i_mtime = time;
	new_dir_ii->ti->i_ctime = time;
	old_ii->ti->i_ctime = time;

	return 0;
}

int toyfs_rename(struct zufs_ioc_rename *zir)
{
	int err;
	struct toyfs_inode_info *old_dir_ii = Z2II(zir->old_dir_ii);
	struct toyfs_inode_info *new_dir_ii = Z2II(zir->new_dir_ii);
	struct toyfs_inode_info *old_ii = Z2II(zir->old_zus_ii);
	struct toyfs_inode_info *new_ii = Z2II(zir->new_zus_ii);
	struct zufs_str *old_name = &zir->old_d_str;
	struct zufs_str *new_name = &zir->new_d_str;

	if (!old_ii)
		return -EINVAL;

	err = _do_rename(old_dir_ii, new_dir_ii,
			 old_ii, new_ii, old_name, new_name,
			 zir->time, zir->flags);
	return err;
}
