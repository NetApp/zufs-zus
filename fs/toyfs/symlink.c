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


static int _get_symlink(struct toyfs_inode_info *tii, void **symlink)
{
	struct toyfs_inode *ti = tii->ti;

	DBG("get_symlink: ino=%lu\n", tii->ino);

	if (!zi_islnk(&ti->zi))
		return -EINVAL;

	if (ti->zi.i_size > sizeof(ti->zi.i_symlink))
		*symlink = ti->ti.symlnk.sl_long->dat;
	else
		*symlink = ti->zi.i_symlink;
	return 0;
}

int toyfs_get_symlink(struct zus_inode_info *zii, void **symlink)
{
	return _get_symlink(Z2II(zii), symlink);
}


const char *toyfs_symlink_value(const struct toyfs_inode_info *tii)
{
	const struct toyfs_inode *ti = tii->ti;
	const struct zus_inode *zi = &ti->zi;
	const char *symlnk = NULL;

	if (zi_islnk(zi)) {
		if (zi->i_size > sizeof(zi->i_symlink))
			symlnk = (const char *)ti->ti.symlnk.sl_long->dat;
		else
			symlnk = (const char *)ti->zi.i_symlink;
	}
	return symlnk;
}

void toyfs_release_symlink(struct toyfs_inode_info *tii)
{
	struct toyfs_inode *ti = tii->ti;
	const size_t symlen = ti->zi.i_size;
	struct toyfs_page *page;

	if (symlen > sizeof(ti->zi.i_symlink)) {
		page = ti->ti.symlnk.sl_long;
		toyfs_release_page(tii->sbi, page);
		ti->ti.symlnk.sl_long = NULL;
	}
	ti->zi.i_size = 0;
}
