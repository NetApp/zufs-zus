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
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "_pr.h"
#include "zus.h"
#include "toyfs.h"

static int _hasname(const struct toyfs_dirent *dirent,
		    const struct zufs_str *str)
{
	return (dirent->d_nlen == str->len) &&
	       !strncmp(dirent->d_name, str->name, dirent->d_nlen);
}

struct toyfs_dirent *
toyfs_lookup_dirent(struct toyfs_inode_info *dir_tii,
		    struct zufs_str *str)
{
	struct toyfs_dirent *dirent;
	struct toyfs_list_head *childs, *itr;

	childs = &dir_tii->ti->ti.dir.d_childs;
	itr = childs->next;
	while (itr != childs) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		if (_hasname(dirent, str))
			return dirent;
		itr = itr->next;
	}
	return NULL;
}

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

static int _rename_inplace(struct toyfs_inode_info *dir_ii,
			   struct toyfs_inode_info *ii,
			   struct zufs_str *old_name,
			   struct zufs_str *new_name)
{
	struct toyfs_dirent *dirent;

	dirent = toyfs_lookup_dirent(dir_ii, old_name);
	if (!dirent)
		return -ENOENT;
	if (dirent->d_ino != ii->ino)
		return -EINVAL;

	memcpy(dirent->d_name, new_name->name, new_name->len);
	dirent->d_nlen = new_name->len;
	dirent->d_name[dirent->d_nlen] = '\0';
	return 0;
}

static int _rename_move(struct toyfs_inode_info *old_dir_ii,
			struct toyfs_inode_info *new_dir_ii,
			struct toyfs_inode_info *ii,
			struct zufs_str *old_name,
			struct zufs_str *new_name)
{
	struct toyfs_dirent *dirent;

	dirent = toyfs_lookup_dirent(old_dir_ii, old_name);
	if (!dirent)
		return -ENOENT;
	if (dirent->d_ino != ii->ino)
		return -EINVAL;

	toyfs_remove_dirent(old_dir_ii, ii, dirent);
	toyfs_add_dirent(new_dir_ii, ii, new_name, dirent);
	return 0;
}

static int _rename_replace(struct toyfs_inode_info *old_dir_ii,
			   struct toyfs_inode_info *new_dir_ii,
			   struct toyfs_inode_info *old_ii,
			   struct toyfs_inode_info *new_ii,
			   struct zufs_str *old_name,
			   struct zufs_str *new_name)
{
	int err;

	err = toyfs_add_dentry(&new_dir_ii->zii, &new_ii->zii, new_name);
	if (err)
		return err;
	err = toyfs_remove_dentry(&old_dir_ii->zii, &old_ii->zii, old_name);
	if (err)
		return err;
	return 0;
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
	struct toyfs_dirent *dirent;

	DBG("rename: olddir_ino=%lu newdir_ino=%lu "
	    "old_name=%.*s new_name=%.*s time=%lu\n",
	    old_dir_ii->ino, new_dir_ii->ino,
	    old_name->len, old_name->name,
	    new_name->len, new_name->name, time);

	if (!old_ii)
		return -EINVAL;

	dirent = toyfs_lookup_dirent(new_dir_ii, new_name);
	if (dirent) {
		if (!new_ii)
			return -EINVAL;
		err = _rename_replace(old_dir_ii, new_dir_ii, old_ii,
				      new_ii, old_name, new_name);

	} else {
		if (old_dir_ii->ino == new_dir_ii->ino)
			err = _rename_inplace(old_dir_ii, old_ii,
					      old_name, new_name);
		else
			err = _rename_move(old_dir_ii, new_dir_ii,
					   old_ii, old_name, new_name);
	}
	if (err)
		return err;

	old_dir_ii->zii.zi->i_mtime = time;
	old_dir_ii->zii.zi->i_ctime = time;
	new_dir_ii->zii.zi->i_mtime = time;
	new_dir_ii->zii.zi->i_ctime = time;
	old_ii->zii.zi->i_ctime = time;

	return 0;
}

int toyfs_rename(struct zufs_ioc_rename *zir)
{
	int err;
	struct toyfs_inode_info *old_ii = Z2II(zir->old_zus_ii);

	if (!old_ii)
		return -EINVAL;

	toyfs_sbi_lock(old_ii->sbi);
	err = _rename(Z2II(zir->old_dir_ii),
		      Z2II(zir->new_dir_ii),
		      old_ii,
		      Z2II(zir->new_zus_ii),
		      &zir->old_d_str,
		      &zir->new_d_str,
		      zir->time);
	toyfs_sbi_unlock(old_ii->sbi);
	return err;
}
