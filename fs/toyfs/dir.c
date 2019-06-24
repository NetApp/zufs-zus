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
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "zus.h"
#include "toyfs.h"

static mode_t _mode_of(const struct toyfs_inode_info *tii)
{
	return tii->ti->i_mode;
}

static size_t _namelen_to_nde(const struct toyfs_dirent *de, size_t nlen)
{
	size_t nde = 1;
	const size_t base = sizeof(de->d_name);
	const size_t desz = sizeof(*de);

	if (nlen > base)
		nde += (nlen - base + desz - 1) / desz;

	return nde;
}

static void _set_dirent(struct toyfs_dirent *dirent,
			const char *name, size_t nlen,
			const struct toyfs_inode_info *tii, loff_t off)
{
	const size_t nde = _namelen_to_nde(NULL, nlen);

	memset(dirent, 0, nde * sizeof(*dirent));
	memcpy(dirent->d_name, name, nlen);
	dirent->d_nlen = nlen;
	dirent->d_ino = tii->ino;
	dirent->d_type = IFTODT(_mode_of(tii));
	dirent->d_off = off;
}

static bool _is_active(const struct toyfs_dirent *dirent)
{
	return (dirent->d_nlen > 0) && (dirent->d_ino != 0);
}

struct toyfs_list_head *toyfs_childs_list_of(struct toyfs_inode_info *dir_tii)
{
	struct toyfs_inode *ti = dir_tii->ti;

	return &ti->list_head;
}

static int _hasname(const struct toyfs_dirent *dirent,
		    const struct zufs_str *str)
{
	return (dirent->d_nlen == str->len) &&
	       !strncmp(dirent->d_name, str->name, dirent->d_nlen);
}

static struct toyfs_dentries *_dentries_of(struct toyfs_list_head *head)
{
	return container_of(head, struct toyfs_dentries, head);
}

static struct toyfs_dirent *_next_dirent(struct toyfs_dirent *de)
{
	size_t step;

	step = _is_active(de) ? _namelen_to_nde(de, de->d_nlen) : 1;
	return de + step;
}

static size_t _count_free_de(const struct toyfs_dirent *itr,
			     const struct toyfs_dirent *end)
{
	size_t count = 0;

	while (itr < end) {
		if (itr->d_nlen)
			break;
		++count;
		++itr;
	}
	return count;
}

static struct toyfs_dirent *
_search_free(struct toyfs_dentries *dentries, size_t nlen)
{
	size_t count, required = _namelen_to_nde(NULL, nlen);
	struct toyfs_dirent *itr = &dentries->de[0];
	struct toyfs_dirent *end = itr + ARRAY_SIZE(dentries->de);

	while (itr < end) {
		count = _count_free_de(itr, end);
		if (count >= required)
			return itr;
		itr += count ? count : _namelen_to_nde(itr, itr->d_nlen);
	}
	return NULL;
}

static struct toyfs_dirent *
_find_dirent(struct toyfs_dentries *dentries, const struct zufs_str *str)
{
	struct toyfs_dirent *itr = &dentries->de[0];
	struct toyfs_dirent *end = itr + ARRAY_SIZE(dentries->de);

	while (itr < end) {
		if (_hasname(itr, str))
			return itr;
		itr = _next_dirent(itr);
	}
	return NULL;
}

static void _reset_dirent(struct toyfs_dirent *de)
{
	const size_t nde = _namelen_to_nde(de, de->d_nlen);

	toyfs_assert(de->d_nlen > 0);
	memset(de, 0, nde * sizeof(*de));
}

struct toyfs_dirent *toyfs_lookup_dirent(struct toyfs_inode_info *dir_tii,
		const struct zufs_str *str)
{
	struct toyfs_dirent *dirent;
	struct toyfs_list_head *childs, *itr;

	childs = toyfs_childs_list_of(dir_tii);
	itr = childs->next;
	while (itr != childs) {
		dirent = _find_dirent(_dentries_of(itr), str);
		if (dirent != NULL)
			return dirent;
		itr = itr->next;
	}
	return NULL;
}

static struct toyfs_dirent *
_acquire_dirent(struct toyfs_inode_info *dir_tii, size_t nlen)
{
	int64_t d_off = 2;
	struct toyfs_dirent *dirent;
	struct toyfs_list_head *childs, *itr;
	struct toyfs_pmemb *pmemb;
	struct toyfs_dentries *dentries;

	childs = toyfs_childs_list_of(dir_tii);
	itr = childs->next;
	while (itr != childs) {
		dentries = _dentries_of(itr);
		dirent = _search_free(dentries, nlen);
		if (dirent != NULL) {
			d_off += dirent - dentries->de;
			goto out;
		}
		itr = itr->next;
		d_off += (int64_t)ARRAY_SIZE(dentries->de);
	}

	pmemb = toyfs_acquire_pmemb(dir_tii->sbi);
	if (!pmemb)
		return NULL;

	dir_tii->ti->i_blocks += 1;

	dentries = (struct toyfs_dentries *)pmemb;
	toyfs_list_add_tail(&dentries->head, childs);
	dirent = dentries->de;

out:
	dirent->d_off = d_off;
	return dirent;
}

static void _add_dirent(struct toyfs_inode_info *dir_tii,
			struct toyfs_inode_info *tii, struct zufs_str *str,
			struct toyfs_dirent *dirent)
{
	_set_dirent(dirent, str->name, str->len, tii, dirent->d_off);
	/* Can not inc/dec by 1 because readdir will fail (it checks i_size) */
	dir_tii->ti->i_size += PAGE_SIZE;
	zus_std_add_dentry(dir_tii->zii.zi, tii->zii.zi);
}

int toyfs_add_dirent(struct toyfs_inode_info *dir_tii,
		     struct toyfs_inode_info *tii, struct zufs_str *str,
		     struct toyfs_dirent **out_dirent)
{
	struct toyfs_dirent *dirent;

	dirent = _acquire_dirent(dir_tii, str->len);
	if (!dirent)
		return -ENOSPC;

	_add_dirent(dir_tii, tii, str, dirent);
	*out_dirent = dirent;
	return 0;
}

int toyfs_add_dentry(struct zus_inode_info *dir_zii,
		     struct zus_inode_info *zii, struct zufs_str *str)
{
	struct toyfs_dirent *dirent;
	struct toyfs_inode_info *dir_tii = Z2II(dir_zii);
	struct toyfs_inode_info *tii = Z2II(zii);
	const ino_t dirino = dir_tii->ino;
	const ino_t ino = tii->ino;

	DBG("add_dentry: dirino=%lu %.*s ino=%lu mode=%o\n",
	    dirino, str->len, str->name, ino, _mode_of(tii));

	return toyfs_add_dirent(dir_tii, tii, str, &dirent);
}

void toyfs_remove_dirent(struct toyfs_inode_info *dir_tii,
			 struct toyfs_inode_info *tii,
			 struct toyfs_dirent *dirent)
{
	_reset_dirent(dirent);
	dir_tii->ti->i_size -= PAGE_SIZE;
	zus_std_remove_dentry(dir_tii->zii.zi, tii->zii.zi);
}

int toyfs_remove_dentry(struct zus_inode_info *dir_zii,
			struct zus_inode_info *zii, struct zufs_str *str)
{
	struct zus_inode *zi;
	struct toyfs_dirent *dirent;
	struct toyfs_inode_info *dir_tii = Z2II(dir_zii);
	struct toyfs_inode_info *tii = Z2II(zii);

	DBG("remove_dentry: dirino=%lu %.*s\n",
	    dir_tii->ino, str->len, str->name);

	dirent = toyfs_lookup_dirent(dir_tii, str);
	if (!dirent)
		return -ENOENT;

	zi = tii->zii.zi;
	if (zi_isdir(zi) && tii->ti->i_size)
		return -ENOTEMPTY;

	DBG("remove_dentry: ino=%lu mode=%o\n", dirent->d_ino, zi->i_mode);

	toyfs_remove_dirent(dir_tii, tii, dirent);

	/*
	 * XXX: Force free_inode by setting i_nlink to 0
	 * TODO: Maybe in zus? Maybe in zuf?
	 */
	if (zi_isdir(zi) && (zi->i_nlink == 1) && !tii->ti->i_size)
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
			       (uint64_t)pos, name, (uint8_t)len);
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
	bool ok;

	ok = _emit(ctx, dirent->d_name, dirent->d_nlen,
		   dirent->d_ino, dirent->d_type);
	if (ok)
		ctx->pos = (dirent->d_off + 1);
	return ok;
}

static bool _iterate_dentries(struct toyfs_dentries *dentries,
			      struct toyfs_dir_context *ctx)
{
	bool ok = true;
	struct toyfs_dirent *itr = &dentries->de[0];
	struct toyfs_dirent *end = itr + ARRAY_SIZE(dentries->de);

	while ((itr < end) && ok) {
		if (itr->d_nlen && (itr->d_off >= ctx->pos))
			ok = _emit_dirent(ctx, itr);
		itr = _next_dirent(itr);
	}
	return ok;
}

static bool _iterate_dir(struct toyfs_inode_info *dir_tii,
			 struct toyfs_dir_context *ctx)
{
	bool ok = true;
	struct toyfs_list_head *itr, *childs;
	struct toyfs_inode *dir_ti = dir_tii->ti;

	if (ctx->pos == 0) {
		ok = _emit(ctx, ".", 1, dir_ti->i_ino, DT_DIR);
		ctx->pos = 1;
	}
	if ((ctx->pos == 1) && ok) {
		ok = _emit(ctx, "..", 2, dir_ti->i_dir.parent, DT_DIR);
		ctx->pos = 2;
	}
	childs = toyfs_childs_list_of(dir_tii);
	itr = childs->next;
	while (ok && (itr != childs)) {
		ok = _iterate_dentries(_dentries_of(itr), ctx);
		if (ok)
			itr = itr->next;
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

void toyfs_release_dir(struct toyfs_inode_info *dir_tii)
{
	struct toyfs_list_head *itr, *next, *childs;
	struct toyfs_dentries *dentries;
	struct toyfs_pmemb *pmemb;

	childs = toyfs_childs_list_of(dir_tii);
	itr = childs->next;
	while (itr != childs) {
		toyfs_assert(dir_tii->ti->i_blocks > 0);

		dentries = _dentries_of(itr);
		next = itr->next;
		pmemb = (struct toyfs_pmemb *)dentries;

		toyfs_list_del(itr);
		toyfs_release_pmemb(dir_tii->sbi, pmemb);

		dir_tii->ti->i_blocks -= 1;
		itr = next;
	}

	toyfs_assert(dir_tii->ti->i_blocks == 0);
}
