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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"


static ino_t _next_ino(struct toyfs_sb_info *sbi)
{
	return __atomic_fetch_add(&sbi->s_top_ino, 1, __ATOMIC_CONSUME);
}

int toyfs_new_inode(struct zus_sb_info *zsbi, struct zus_inode_info *zii,
		    void *app_ptr, struct zufs_ioc_new_inode *ioc_new)
{
	ino_t ino;
	mode_t mode;
	size_t symlen;
	struct toyfs_inode *ti;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct toyfs_inode_info *tii =  Z2II(zii);
	struct zus_inode *zi = &ioc_new->zi;
	struct toyfs_inode_info *dir_tii = Z2II(ioc_new->dir_ii);
	struct toyfs_page *page;
	bool symlong;
	const char *symname = (const char *)app_ptr;

	mode = zi->i_mode;
	DBG("new_inode:sbi=%p tii=%p mode=%o\n", sbi, tii, mode);

	if (!(zi_isdir(zi) || zi_isreg(zi) || zi_islnk(zi) || S_ISFIFO(mode)))
		return -ENOTSUP;
	if (zi->i_size >= PAGE_SIZE)
		return -EINVAL;

	ti = toyfs_acquire_inode(sbi);
	if (!ti)
		return -ENOSPC;

	ino = _next_ino(tii->sbi);
	memset(ti, 0, sizeof(*ti));
	memcpy(&ti->zi, zi, sizeof(ti->zi));
	tii->ti = ti;
	tii->ino = ino;
	tii->zii.zi = &tii->ti->zi;
	ti->i_parent_ino = TOYFS_NULL_INO;
	ti->zi.i_ino = ino;

	if (zi_isdir(zi)) {
		DBG("new_inode(dir): ino=%lu\n", ino);
		list_init(&ti->ti.dir.d_childs);
		ti->ti.dir.d_ndentry = 0;
		ti->ti.dir.d_off_max = 2;
		ti->zi.i_size = PAGE_SIZE;
		ti->i_parent_ino = dir_tii->zii.zi->i_ino;
		zus_std_new_dir(dir_tii->zii.zi, &ti->zi);
	} else if (zi_isreg(zi)) {
		DBG("new_inode(reg): ino=%lu\n", ino);
		list_init(&ti->ti.reg.r_iblkrefs);
		ti->ti.reg.r_first_parent = dir_tii->zii.zi->i_ino;
		if (ioc_new->flags & ZI_TMPFILE)
			ti->zi.i_nlink = 1;
	} else if (zi_islnk(zi)) {
		symlen = ti->zi.i_size;
		symlong = symlen > sizeof(ti->zi.i_symlink);
		symname = symlong ? (const char *)symname :
			  (const char *)zi->i_symlink;
		DBG("new_inode(symlnk): ino=%lu lnk=%.*s\n",
		    ino, (int)symlen, symname);
		if (symlong) {
			page = toyfs_acquire_page(sbi);
			if (!page) {
				toyfs_release_inode(sbi, ti);
				return -ENOSPC;
			}
			memcpy(page->dat, symname, symlen);
			ti->ti.symlnk.sl_long = page;
		}
	} else if (S_ISFIFO(mode)) {
		DBG("new_inode(fifo): ino=%lu\n", ino);
		ti->ti.reg.r_first_parent = dir_tii->zii.zi->i_ino;
	}


	toyfs_track_inode(tii);
	ioc_new->zi.i_ino = ino;
	return 0;
}

int toyfs_free_inode(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_sb_info *sbi = tii->sbi;
	struct toyfs_inode *ti = tii->ti;
	struct zus_inode *zi = tii->zii.zi;

	DBG("free_inode: ino=%lu mode=%o nlink=%ld size=%ld\n",
	    tii->ino, (int)zi->i_mode,
	    (long)zi->i_nlink, (long)zi->i_size);

	if (zi_isdir(zi)) {
		DBG("free_inode(dir): ino=%lu\n", tii->ino);
		if (tii->ti->ti.dir.d_ndentry)
			return -ENOTEMPTY;
		zi->i_dir.parent = 0; /* TODO: Maybe zus_std helper ? */
	} else if (zi_islnk(zi)) {
		DBG("free_inode(symlink): ino=%lu symlnk=%s\n",
		    tii->ino, toyfs_symlink_value(tii));
		toyfs_release_symlink(tii);
	} else if (zi_isreg(zi)) {
		DBG("free_inode(reg): ino=%lu\n", tii->ino);
		toyfs_truncate(tii, 0);
	} else {
		DBG("free_inode: ino=%lu mode=%o\n", tii->ino, zi->i_mode);
		zi->i_rdev = 0;
	}

	toyfs_untrack_inode(tii);
	toyfs_release_inode(sbi, ti);
	return 0;
}

int toyfs_iget(struct zus_sb_info *zsbi, struct zus_inode_info *zii, ulong ino)
{
	int err = 0;
	struct toyfs_inode_info *tii;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	DBG("iget: ino=%lu\n", ino);

	toyfs_assert(zii->op);
	tii = toyfs_find_inode(sbi, ino);
	if (tii) {
		zii->zi = tii->zii.zi;
		DBG("iget: ino=%lu zi=%p\n", ino, zii->zi);
	} else {
		err = -ENOENT;
		DBG("iget: ino=%lu err=%d\n", ino, err);
	}
	return err;
}

void toyfs_evict(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	DBG("evict: ino=%lu\n", tii->ino);
	/* TODO: What here? */
}

static int _setattr(struct toyfs_inode_info *tii,
		    uint enable_bits, ulong truncate_size)
{
	int err = 0;
	struct zus_inode *zi = tii->zii.zi;


	DBG("setattr: ino=%lu enable_bits=%x truncate_size=%lu\n",
	    tii->ino, enable_bits, truncate_size);

	/* TODO: CL-FLUSH */
	if (enable_bits & STATX_MODE)
		DBG("setattr: mode=%o\n", zi->i_mode);
	if (enable_bits & STATX_NLINK)
		DBG("setattr: nlink=%o\n", zi->i_nlink);
	if (enable_bits & (STATX_UID | STATX_GID))
		DBG("setattr: uid=%u gid=%u\n", zi->i_uid, zi->i_gid);
	if (enable_bits & (STATX_ATIME | STATX_MTIME | STATX_CTIME))
		DBG("setattr: atime=%lu mtime=%lu ctime=%lu\n",
		    (uint64_t)zi->i_atime,
		    (uint64_t)zi->i_mtime,
		    (uint64_t)zi->i_ctime);

	if (enable_bits & STATX_SIZE)
		err = toyfs_truncate(tii, truncate_size);

	return err;
}

int toyfs_setattr(struct zus_inode_info *zii,
		  uint enable_bits, ulong truncate_size)
{
	return _setattr(Z2II(zii), enable_bits, truncate_size);
}

