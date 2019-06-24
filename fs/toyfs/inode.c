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
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "zus.h"
#include "toyfs.h"


static ino_t _next_ino(struct toyfs_sb_info *sbi)
{
	return __atomic_fetch_add(&sbi->s_top_ino, 1, __ATOMIC_CONSUME);
}

static bool issupported(const struct zus_inode *zi)
{
	const mode_t mode = zi->i_mode;

	return zi_isdir(zi) || zi_isreg(zi) || zi_islnk(zi) ||
	       S_ISCHR(mode) || S_ISBLK(mode) ||
	       S_ISFIFO(mode) || S_ISSOCK(mode);
}

struct zus_inode_info *
toyfs_new_inode(struct zus_sb_info *zsbi,
		void *app_ptr, struct zufs_ioc_new_inode *ioc_new)
{
	ino_t ino;
	mode_t mode;
	size_t symlen;
	struct toyfs_inode *ti;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct toyfs_inode_info *tii = NULL;
	struct zus_inode *zi = &ioc_new->zi;
	struct toyfs_inode_info *dir_tii = Z2II(ioc_new->dir_ii);
	struct toyfs_pmemb *pmemb;
	bool symlong;
	const char *symname = (const char *)app_ptr;
	struct zus_inode_info *zii;

	zii = toyfs_zii_alloc(zsbi);
	if (!zii)
		return NULL;

	tii =  Z2II(zii);
	mode = zi->i_mode;
	DBG("new_inode:sbi=%p tii=%p mode=%o\n",
		(void *)sbi, (void *)tii, mode);

	if (!issupported(zi))
		goto out_err;
	if (zi->i_size >= PAGE_SIZE)
		goto out_err;

	ti = toyfs_acquire_inode(sbi);
	if (!ti)
		goto out_err;

	ino = _next_ino(tii->sbi);
	memset(ti, 0, sizeof(*ti));
	memcpy(ti, zi, sizeof(*ti));
	tii->ti = ti;
	tii->ino = ino;
	tii->zii.zi = toyfs_ti2zi(tii->ti);
	ti->i_ino = ino;

	if (zi_isdir(zi)) {
		DBG("new_inode(dir): ino=%lu\n", ino);
		toyfs_list_init(toyfs_childs_list_of(tii));
		ti->i_size = 0;
		ti->i_dir.parent = dir_tii->ti->i_ino;
		zus_std_new_dir(dir_tii->zii.zi, toyfs_ti2zi(ti));
	} else if (zi_isreg(zi)) {
		DBG("new_inode(reg): ino=%lu\n", ino);
		toyfs_list_init(toyfs_iblkrefs_list_of(tii));
		if (ioc_new->flags & ZI_TMPFILE)
			ti->i_nlink = 1;
	} else if (zi_islnk(zi)) {
		symlen = ti->i_size;
		symlong = symlen >= sizeof(ti->i_symlink);
		symname = symlong ? (const char *)symname :
			  (const char *)zi->i_symlink;
		DBG("new_inode(symlnk): ino=%lu lnk=%.*s\n",
		    ino, (int)symlen, symname);
		if (symlong) {
			pmemb = toyfs_acquire_pmemb(sbi);
			if (!pmemb) {
				toyfs_release_inode(sbi, ti);
				goto out_err;
			}
			memcpy(pmemb->dat, symname, symlen);
			tii->ti->i_sym_dpp = toyfs_page2dpp(sbi, pmemb);
		}
	} else
		DBG("new_inode: ino=%lu mode=%o\n", ino, mode);

	toyfs_lock_inodes(sbi);
	toyfs_i_track(tii);
	tii->ref++;
	toyfs_unlock_inodes(sbi);

	return zii;

out_err:
	if (tii)
		toyfs_tii_free(tii);
	return NULL;
}

void toyfs_free_inode(struct toyfs_inode_info *tii)
{
	struct toyfs_sb_info *sbi = tii->sbi;
	struct toyfs_inode *ti = tii->ti;
	struct zus_inode *zi = tii->zii.zi;

	DBG("free_inode: ino=%lu mode=%o nlink=%ld size=%ld\n",
	    tii->ino, (int)zi->i_mode,
	    (long)zi->i_nlink, (long)zi->i_size);

	if (zi_isdir(zi)) {
		DBG("free_inode(dir): ino=%lu\n", tii->ino);
		if (tii->ti->i_size)
			return;
		toyfs_release_dir(tii);
		zi->i_dir.parent = 0; /* TODO: Maybe zus_std helper ? */
	} else if (zi_islnk(zi)) {
		DBG("free_inode(symlink): ino=%lu \n", tii->ino);
		toyfs_release_symlink(tii);
	} else if (zi_isreg(zi)) {
		DBG("free_inode(reg): ino=%lu\n", tii->ino);
		toyfs_truncate(tii, 0);
	} else {
		DBG("free_inode: ino=%lu mode=%o\n", tii->ino, zi->i_mode);
		zi->i_rdev = 0;
	}
	toyfs_drop_xattr(tii);
	toyfs_release_inode(sbi, ti);
}

int toyfs_iget(struct zus_sb_info *zsbi, ulong ino, struct zus_inode_info **zii)
{
	int err = 0;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct toyfs_inode_info *tii;
	struct toyfs_inode_ref *tir;

	DBG("iget: ino=%lu\n", ino);

	tir = toyfs_find_inode_ref_by_ino(sbi, ino);
	if (!tir) {
		*zii = NULL;
		DBG("iget: ino=%lu => -ENOENT\n", ino);
		return -ENOENT;
	}

	toyfs_lock_inodes(sbi);
	tii = tir->tii;
	if (!tir->tii) {
		tii = toyfs_alloc_ii(sbi);
		if (unlikely(!tii)) {
			DBG("iget: ino=%lu => ENOMEM\n", ino);
			err = -ENOMEM;
			goto out;
		}
		tir->tii = tii;
		tii->mapped = true;
	}
	++tii->ref;

	*zii = &tii->zii;
	DBG("iget: ino=%lu zi=%p\n", ino, (void *)tii->zii.zi);
out:
	toyfs_unlock_inodes(sbi);
	return err;
}

void toyfs_evict(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_sb_info *sbi = tii->sbi;
	struct toyfs_inode *ti = tii->ti;

	DBG("evict: ino=%lu\n", tii->ino);

	toyfs_lock_inodes(sbi);
	if (--tii->ref)
		goto out;

	toyfs_sbi_lock(tii->sbi);
	if (!ti->i_nlink) {
		toyfs_free_inode(tii);
		if (tii->mapped)
			toyfs_i_untrack(tii, true);
	} else {
		if (tii->mapped)
			toyfs_i_untrack(tii, false);
	}

	toyfs_tii_free(tii);
	toyfs_sbi_unlock(sbi);

out:
	toyfs_unlock_inodes(sbi);
}

static int _setattr(struct toyfs_inode_info *tii, uint enable_bits)
{
	int err = 0;
	struct zus_inode *zi = tii->zii.zi;


	DBG("setattr: ino=%lu enable_bits=%x \n", tii->ino, enable_bits);

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
	return err;
}

int toyfs_setattr(struct zus_inode_info *zii, uint enable_bits)
{
	return _setattr(Z2II(zii), enable_bits);
}

