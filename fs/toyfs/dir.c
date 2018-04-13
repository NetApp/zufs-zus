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
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"

static mode_t _mode_of(const struct toyfs_inode_info *tii)
{
	return tii->ti->zi.i_mode;
}

static loff_t _next_doff(struct toyfs_inode_info *dir_tii)
{
	loff_t off, *d_off_max = &dir_tii->ti->ti.dir.d_off_max;

	off = __atomic_fetch_add(d_off_max, 1, __ATOMIC_CONSUME);
	return off * PAGE_SIZE;
}

static void _set_dirent(struct toyfs_dirent *dirent,
			const char *name, size_t nlen,
			const struct toyfs_inode_info *tii, loff_t off)
{
	toyfs_assert(nlen < sizeof(dirent->d_name));

	memset(dirent, 0, sizeof(*dirent)); /* TODO: rm */
	list_init(&dirent->d_head);
	dirent->d_nlen = nlen;
	dirent->d_ino = tii->ino;
	dirent->d_type = IFTODT(_mode_of(tii));
	dirent->d_off = off;
	strncpy(dirent->d_name, name, nlen);
}

static bool _hasname(const struct toyfs_dirent *dirent,
		     const struct zufs_str *str)
{
	return (dirent->d_nlen == str->len) &&
	       !strncmp(dirent->d_name, str->name, dirent->d_nlen);
}

int toyfs_add_dentry(struct zus_inode_info *dir_zii,
		     struct zus_inode_info *zii, struct zufs_str *str)
{
	loff_t doff;
	struct toyfs_dirent *dirent;
	struct list_head *childs;
	struct toyfs_inode_info *dir_tii = Z2II(dir_zii);
	struct toyfs_inode_info *tii = Z2II(zii);
	const ino_t dirino = dir_tii->ino;
	const ino_t ino = tii->ino;

	DBG("add_dentry: dirino=%lu %.*s ino=%lu mode=%o\n",
	    dirino, str->len, str->name, ino, _mode_of(tii));

	childs = &dir_tii->ti->ti.dir.d_childs;
	dirent = toyfs_acquire_dirent(dir_tii->sbi);
	if (!dirent)
		return -ENOSPC;

	doff = _next_doff(dir_tii);
	_set_dirent(dirent, str->name, str->len, tii, doff);
	list_add_tail(&dirent->d_head, childs);
	dir_tii->ti->ti.dir.d_ndentry++;
	dir_tii->ti->zi.i_size = (size_t)(doff + PAGE_SIZE + 2);
	zus_std_add_dentry(dir_tii->zii.zi, tii->zii.zi);

	DBG("add_dentry: dirino=%lu dirnlink=%u dirsize=%ld "
	    "%.*s ino=%lu nlink=%d\n", dirino, dir_tii->zii.zi->i_nlink,
	    (long)dir_tii->ti->zi.i_size, str->len, str->name,
	    ino, (int)tii->zii.zi->i_nlink);
	if (zi_islnk(tii->zii.zi))
		DBG("add_dentry: symlnk=%s\n", toyfs_symlink_value(tii));
	return 0;
}

int toyfs_remove_dentry(struct zus_inode_info *dir_zii, struct zufs_str *str)
{
	ino_t ino;
	mode_t mode;
	struct toyfs_dirent *dirent = NULL;
	struct list_head *childs, *itr;
	struct toyfs_inode_info *tii;
	struct zus_inode *zi;
	const char *symval;
	struct toyfs_inode_info *dir_tii = Z2II(dir_zii);

	DBG("remove_dentry: dirino=%lu %.*s\n",
	    dir_tii->ino, str->len, str->name);

	childs = &dir_tii->ti->ti.dir.d_childs;
	itr = childs->next;
	while (itr != childs) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		if (_hasname(dirent, str))
			break;
		dirent = NULL;
		itr = itr->next;
	}
	if (!dirent)
		return -ENOENT;

	ino = dirent->d_ino;
	tii = toyfs_find_inode(dir_tii->sbi, ino);
	if (!tii)
		return -ENOENT;

	zi = tii->zii.zi;
	if (zi_isdir(zi) && tii->ti->ti.dir.d_ndentry)
		return -ENOTEMPTY;

	if (zi_islnk(zi)) {
		symval = toyfs_symlink_value(tii);
		DBG("remove_dentry(symlnk): ino=%lu symlnk=%s\n", ino, symval);
	} else {
		mode = zi->i_mode;
		DBG("remove_dentry: ino=%lu mode=%o\n", ino, mode);
	}

	list_del(&dirent->d_head);
	dir_tii->ti->ti.dir.d_ndentry--;
	zus_std_remove_dentry(dir_tii->zii.zi, zi);
	toyfs_release_dirent(dir_tii->sbi, dirent);

	/*
	 * XXX: Force free_inode by setting i_nlink to 0
	 * TODO: Maybe in zus? Maybe in zuf?
	 */
	if (zi_isdir(zi) && (zi->i_nlink == 1) && !tii->ti->ti.dir.d_ndentry)
		zi->i_nlink = 0;

	return 0;
}


struct toyfs_dir_context;
typedef bool (*toyfs_filldir_t)(struct toyfs_dir_context *, const char *,
				size_t, loff_t, ino_t, mode_t);

struct toyfs_dir_context {
	toyfs_filldir_t actor;
	loff_t pos;
};

struct toyfs_getdents_ctx {
	struct toyfs_dir_context dir_ctx;
	struct zufs_readdir_iter rdi;
	struct toyfs_inode_info *dir_tii;
	size_t emit_count;
};

static bool _filldir(struct toyfs_dir_context *dir_ctx, const char *name,
		     size_t len, loff_t pos, ino_t ino, mode_t dt)
{
	bool status;
	struct toyfs_getdents_ctx *ctx =
		container_of(dir_ctx, struct toyfs_getdents_ctx, dir_ctx);

	status = zufs_zde_emit(&ctx->rdi, ino, (uint8_t)dt,
			       pos, name, (uint8_t)len);
	if (status)
		ctx->emit_count++;
	DBG("filldir: %.*s ino=%ld dt=%d emit_count=%d status=%d\n",
	    (int)len, name, ino, dt, (int)ctx->emit_count, (int)status);
	return status;
}

static void _init_getdents_ctx(struct toyfs_getdents_ctx *ctx,
			       struct toyfs_inode_info *dir_tii,
			       struct zufs_ioc_readdir *ioc_readdir,
			       void *app_ptr)
{
	zufs_readdir_iter_init(&ctx->rdi, ioc_readdir, app_ptr);
	ctx->dir_ctx.actor = _filldir;
	ctx->dir_ctx.pos = ioc_readdir->pos;
	ctx->dir_tii = dir_tii;
	ctx->emit_count = 0;
}

static bool _emit(struct toyfs_dir_context *ctx, const char *name,
		  size_t namelen, ino_t ino, mode_t type)
{
	return ctx->actor(ctx, name, namelen, ctx->pos, ino, type);
}

static bool _emit_dirent(struct toyfs_dir_context *ctx,
			 const struct toyfs_dirent *dirent)
{
	ctx->pos = dirent->d_off;
	return _emit(ctx, dirent->d_name, dirent->d_nlen,
		     dirent->d_ino, dirent->d_type);
}

static bool _iterate_dir(struct toyfs_inode_info *dir_tii,
			 struct toyfs_dir_context *ctx)
{
	bool ok = true;
	struct toyfs_dirent *dirent;
	struct list_head *itr, *childs;
	struct toyfs_inode *dir_ti = dir_tii->ti;

	if (ctx->pos == 0) {
		ok = _emit(ctx, ".", 1, dir_ti->zi.i_ino, DT_DIR);
		ctx->pos = 1;
	}
	if ((ctx->pos == 1) && ok) {
		ok = _emit(ctx, "..", 2, dir_ti->i_parent_ino, DT_DIR);
		ctx->pos = 2;
	}
	childs = &dir_ti->ti.dir.d_childs;
	itr = childs->next;
	while ((itr != childs) && ok) {
		dirent = container_of(itr, struct toyfs_dirent, d_head);
		itr = itr->next;
		if (dirent->d_off >= ctx->pos) {
			ok = _emit_dirent(ctx, dirent);
			ctx->pos = dirent->d_off + 1;
		}
	}
	return (itr != childs);
}

int toyfs_iterate_dir(struct toyfs_inode_info *dir_tii,
		      struct zufs_ioc_readdir *zir, void *buf)
{

	struct toyfs_getdents_ctx ctx;

	_init_getdents_ctx(&ctx, dir_tii, zir, buf);
	zir->more = _iterate_dir(dir_tii, &ctx.dir_ctx);
	zir->pos = ctx.dir_ctx.pos;
	DBG("iterate_dir: dir-ino=%lu emit_count=%lu more=%d pos=%ld\n",
	    dir_tii->ino, ctx.emit_count, (int)zir->more, zir->pos);
	return 0;
}

int toyfs_readdir(void *app_ptr, struct zufs_ioc_readdir *zir)
{
	return toyfs_iterate_dir(Z2II(zir->dir_ii), zir, app_ptr);
}
