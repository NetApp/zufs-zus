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
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"

static int
_hasname(const struct toyfs_dirent *dirent, const struct zufs_str *str)
{
	return (dirent->d_nlen == str->len) &&
	       !strncmp(dirent->d_name, str->name, dirent->d_nlen);
}

static ino_t _lookup(struct toyfs_inode_info *dir_tii, struct zufs_str *str)
{
	ino_t ino = TOYFS_NULL_INO;
	struct toyfs_dirent *dirent = NULL;
	struct list_head *childs, *itr;

	DBG("lookup: dirino=%lu %.*s\n",
	    dir_tii->ino, str->len, str->name);

	childs = &dir_tii->ti->ti.dir.d_childs;
	itr = childs->next;
	while (itr != childs) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		if (_hasname(dirent, str)) {
			ino = dirent->d_ino;
			break;
		}
		itr = itr->next;
	}
	return ino;
}

ulong toyfs_lookup(struct zus_inode_info *dir_zii, struct zufs_str *str)
{
	return _lookup(Z2II(dir_zii), str);
}

static int _rename(struct toyfs_inode_info *old_dir_ii,
		   struct toyfs_inode_info *new_dir_ii,
		   struct toyfs_inode_info *old_ii,
		   struct toyfs_inode_info *new_ii,
		   struct zufs_str *old_name,
		   struct zufs_str *new_name,
		   uint64_t time)
{
	int err;

	if (!new_ii) {
		DBG("rename: add_dentry: dirino=%lu ino=%lu "
		    "new_name=%.*s\n", new_dir_ii->ino,
		    old_ii->ino, new_name->len, new_name->name);
		err = toyfs_add_dentry(&new_dir_ii->zii,
				       &old_ii->zii, new_name);
		if (err)
			goto out;
		new_dir_ii->zii.zi->i_ctime = time;
	}
	if (old_name->len) {
		DBG("rename: remove_dentry: dirino=%lu ino=%lu "
		    "old_name=%.*s\n", old_dir_ii->ino,
		    old_ii->ino, old_name->len, old_name->name);
		err = toyfs_remove_dentry(&old_dir_ii->zii, old_name);
		if (err)
			goto out;
		old_dir_ii->zii.zi->i_ctime = time;
	}

out:
	DBG("rename: err=%d\n", err);
	return err;
}

int toyfs_rename(struct zufs_ioc_rename *zir)
{
	return _rename(Z2II(zir->old_dir_ii),
		       Z2II(zir->new_dir_ii),
		       Z2II(zir->old_zus_ii),
		       Z2II(zir->new_zus_ii),
		       &zir->old_d_str,
		       &zir->new_d_str,
		       zir->time);
}
