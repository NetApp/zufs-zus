/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The toyfs reference file-system implementation via zufs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
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

#include "zus.h"
#include "toyfs.h"


static int _get_symlink(struct toyfs_inode_info *tii, void **symlink)
{
	struct toyfs_inode *ti = tii->ti;
	struct toyfs_pmemb *pmemb;

	DBG("get_symlink: ino=%lu\n", tii->ino);

	if (!zi_islnk(toyfs_ti2zi(ti)))
		return -EINVAL;

	if (ti->i_size < sizeof(ti->i_symlink)) {
		*symlink = ti->i_symlink;
		return 0;
	}

	pmemb = toyfs_dpp2pmemb(tii->sbi, ti->i_sym_dpp);
	*symlink = pmemb;

	return 0;
}

int toyfs_get_symlink(struct zus_inode_info *zii, void **symlink)
{
	return _get_symlink(Z2II(zii), symlink);
}

void toyfs_release_symlink(struct toyfs_inode_info *tii)
{
	struct toyfs_inode *ti = tii->ti;
	const size_t symlen = ti->i_size;
	struct toyfs_pmemb *pmemb;

	if (symlen > sizeof(ti->i_symlink)) {
		pmemb = toyfs_dpp2pmemb(tii->sbi, ti->i_sym_dpp);
		toyfs_release_pmemb(tii->sbi, pmemb);
	}
	ti->i_size = 0;
	ti->i_sym_dpp = 0;
}
