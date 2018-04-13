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
#include <linux/falloc.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"

#define DBG_(fmt, ...)

#define TOYFS_ISIZE_MAX		(1ULL << 50)

/* CentOS7 workarounds */
#ifndef FALLOC_FL_INSERT_RANGE
#define FALLOC_FL_INSERT_RANGE	0x20
#endif

#ifndef FALLOC_FL_UNSHARE_RANGE
#define FALLOC_FL_UNSHARE_RANGE	0x40
#endif


static void
_drop_iblkref(struct toyfs_inode_info *tii,
	      struct toyfs_iblkref *iblkref);


static void list_add_before(struct list_head *elem, struct list_head *head)
{
	_list_add(elem, head->prev, head);
}

static struct toyfs_dblkref *_new_dblkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_page *page;
	struct toyfs_dblkref *dblkref = NULL;

	page = toyfs_acquire_page(sbi);
	if (!page)
		goto out;

	dblkref = toyfs_acquire_dblkref(sbi);
	if (!dblkref)
		goto out;

	dblkref->bn = toyfs_addr2bn(sbi, page);
	dblkref->refcnt = 1;

out:
	if (page && !dblkref)
		toyfs_release_page(sbi, page);
	return dblkref;
}

static void _free_dblkref(struct toyfs_sb_info *sbi,
			  struct toyfs_dblkref *dblkref)
{
	const size_t bn = dblkref->bn;

	toyfs_release_dblkref(sbi, dblkref);
	toyfs_release_page(sbi, toyfs_bn2page(sbi, bn));
}

static void _decref_dblkref(struct toyfs_sb_info *sbi,
			    struct toyfs_dblkref *dblkref)
{
	size_t refcnt;

	toyfs_sbi_lock(sbi);
	toyfs_assert(dblkref->refcnt > 0);
	dblkref->refcnt--;
	refcnt = dblkref->refcnt;
	toyfs_sbi_unlock(sbi);

	if (!refcnt)
		_free_dblkref(sbi, dblkref);
}

static struct toyfs_iblkref *
_new_iblkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct toyfs_dblkref *dblkref = NULL;
	struct toyfs_iblkref *iblkref = NULL;
	struct zus_inode *zi = tii->zii.zi;

	dblkref = _new_dblkref(tii->sbi);
	if (!dblkref)
		goto out;

	iblkref = toyfs_acquire_iblkref(tii->sbi);
	if (!iblkref)
		goto out;

	iblkref->dblkref = dblkref;
	iblkref->off = off;
	zi->i_blocks++;

out:
	if (!iblkref && dblkref)
		_decref_dblkref(tii->sbi, dblkref);
	return iblkref;
}

static void
_free_iblkref(struct toyfs_inode_info *tii, struct toyfs_iblkref *iblkref)
{
	struct zus_inode *zi = tii->zii.zi;

	toyfs_assert(zi->i_blocks);

	_decref_dblkref(tii->sbi, iblkref->dblkref);
	toyfs_release_iblkref(tii->sbi, iblkref);
	zi->i_blocks--;
}

static void *_advance(void *buf, size_t len)
{
	return ((char *)buf + len);
}

static loff_t _off_to_boff(loff_t off)
{
	const loff_t page_size = (loff_t)PAGE_SIZE;

	return (off / page_size) * page_size;
}

static loff_t _off_in_page(loff_t off)
{
	const loff_t page_size = (loff_t)PAGE_SIZE;

	return off % page_size;
}

static loff_t _next_page(loff_t off)
{
	const loff_t page_size = PAGE_SIZE;

	return ((off + page_size) / page_size) * page_size;
}

static bool _ispagealigned(loff_t off, size_t len)
{
	const loff_t noff = off + (loff_t)len;

	return (noff == _off_to_boff(noff));
}

static size_t _nbytes_in_range(loff_t off, loff_t next, loff_t end)
{
	return (size_t)((next < end) ? (next - off) : (end - off));
}

static void
_copy_out(void *tgt, const struct toyfs_page *page, loff_t off, size_t len)
{
	toyfs_assert(len <= sizeof(page->dat));
	toyfs_assert((size_t)off + len <= sizeof(page->dat));
	memcpy(tgt, &page->dat[off], len);
}

static void
_copy_in(struct toyfs_page *page, const void *src, loff_t off, size_t len)
{
	toyfs_assert(len <= sizeof(page->dat));
	toyfs_assert((size_t)off + len <= sizeof(page->dat));
	memcpy(&page->dat[off], src, len);
}

static void _copy_page(struct toyfs_page *page, const struct toyfs_page *other)
{
	_copy_in(page, other->dat, 0, sizeof(other->dat));
}

static void _fill_zeros(void *tgt, size_t len)
{
	memset(tgt, 0, len);
}

static void _assign_zeros(struct toyfs_page *page, loff_t off, size_t len)
{
	toyfs_assert(len <= sizeof(page->dat));
	toyfs_assert((size_t)off + len <= sizeof(page->dat));
	_fill_zeros(&page->dat[off], len);
}

static int _check_io(loff_t off, size_t len)
{
	const size_t uoff = (size_t)off;

	if (off < 0)
		return -EINVAL;
	if (len == 0)
		return -EINVAL; /* TODO: Ignore? */
	if (uoff > TOYFS_ISIZE_MAX)
		return -EFBIG;
	if ((uoff + len) > TOYFS_ISIZE_MAX)
		return -EFBIG;

	return 0;
}

static int _check_rw(loff_t off, size_t len)
{
	if (len > ZUS_API_MAP_MAX_SIZE) {
		ERROR("illegal: off=%ld len=%lu\n", off, len);
		return -EINVAL;
	}
	return _check_io(off, len);
}

static int _check_falloc_flags(int flags)
{
	if (flags & FALLOC_FL_NO_HIDE_STALE)
		return -ENOTSUP;
	if (flags & FALLOC_FL_COLLAPSE_RANGE)
		return -ENOTSUP;
	if (flags & FALLOC_FL_INSERT_RANGE)
		return -ENOTSUP;
	if (flags & FALLOC_FL_UNSHARE_RANGE)
		return -ENOTSUP;
	if ((flags & FALLOC_FL_PUNCH_HOLE) && !(flags & FALLOC_FL_KEEP_SIZE))
		return -ENOTSUP;
	return 0;
}

static loff_t _max_offset(loff_t off, size_t len, size_t isize)
{
	const loff_t end = off + (loff_t)len;

	return (end > (loff_t)isize) ? end : (loff_t)isize;
}

static loff_t _tin_offset(loff_t off, size_t len, size_t isize)
{
	const loff_t end = off + (loff_t)len;

	return (end < (loff_t)isize) ? end : (loff_t)isize;
}

static struct toyfs_iblkref *
_fetch_iblkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct list_head *itr;
	struct list_head *iblkrefs;
	struct toyfs_iblkref *iblkref;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	const loff_t boff = _off_to_boff(off);

	iblkrefs = &reg_ti->r_iblkrefs;
	itr = iblkrefs->next;
	while (itr != iblkrefs) {
		iblkref = container_of(itr, struct toyfs_iblkref, head);
		if (iblkref->off == boff)
			return iblkref;
		itr = itr->next;
	}
	return NULL;
}

static struct toyfs_page *_fetch_page(struct toyfs_inode_info *tii, loff_t off)
{
	struct toyfs_iblkref *iblkref;
	struct toyfs_page *page = NULL;

	iblkref = _fetch_iblkref(tii, off);
	if (iblkref)
		page = toyfs_bn2page(tii->sbi, iblkref->dblkref->bn);
	return page;
}

static int
_read(struct toyfs_inode_info *tii, void *buf, loff_t off, size_t len)
{
	int err;
	size_t cnt = 0;
	loff_t end, nxt;
	struct toyfs_page *page;

	DBG("read: ino=%ld off=%ld len=%lu\n", tii->ino, off, len);

	err = _check_rw(off, len);
	if (err)
		return err;

	end = _tin_offset(off, len, tii->ti->zi.i_size);
	while (off < end) {
		page = _fetch_page(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		if (page)
			_copy_out(buf, page, _off_in_page(off), len);
		else
			_fill_zeros(buf, len);

		cnt += len;
		off = nxt;
		buf = _advance(buf, len);
	}

	/* TODO: Output result? */
	return 0;
}

int toyfs_read(void *buf, struct zufs_ioc_IO *ioc_io)
{
	return _read(Z2II(ioc_io->zus_ii), buf,
		     (loff_t)ioc_io->filepos, ioc_io->hdr.len);
}

int toyfs_get_block(struct zus_inode_info *zii,
		    struct zufs_ioc_get_block *get_block)
{
	loff_t off;
	const size_t blkidx = get_block->index;
	struct toyfs_page *page;
	struct toyfs_inode_info *tii = Z2II(zii);

	if (!zi_isreg(tii->zii.zi))
		return -ENOTSUP;

	off = (loff_t)(blkidx * PAGE_SIZE);
	page = _fetch_page(tii, off);
	if (page)
		get_block->pmem_bn = toyfs_addr2bn(tii->sbi, page);
	else
		get_block->pmem_bn = 0;

	DBG("get_block: ino=%ld blkidx=%lu pmem_bn=%ld\n",
	    tii->ino, blkidx, (long)get_block->pmem_bn);

	return 0;
}

static void _clone_data(struct toyfs_sb_info *sbi,
			struct toyfs_dblkref *dst_dblkref,
			const struct toyfs_dblkref *src_dblkref)
{
	struct toyfs_page *dst_page;
	const struct toyfs_page *src_page;

	toyfs_sbi_lock(sbi);
	dst_page = toyfs_bn2page(sbi, dst_dblkref->bn);
	src_page = toyfs_bn2page(sbi, src_dblkref->bn);
	_copy_page(dst_page, src_page);
	toyfs_sbi_unlock(sbi);
}

static struct toyfs_iblkref *
_require_iblkref(struct toyfs_inode_info *tii, loff_t off)
{
	struct list_head *itr;
	struct toyfs_dblkref *dblkref;
	struct toyfs_iblkref *iblkref = NULL;
	struct list_head *iblkrefs;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	const loff_t boff = _off_to_boff(off);

	iblkrefs = &reg_ti->r_iblkrefs;
	itr = iblkrefs->next;
	while (itr != iblkrefs) {
		iblkref = container_of(itr, struct toyfs_iblkref, head);
		if (iblkref->off == boff)
			break;
		if (iblkref->off > boff) {
			iblkref = NULL;
			break;
		}
		itr = itr->next;
		iblkref = NULL;
	}
	if (!iblkref) {
		iblkref = _new_iblkref(tii, boff);
		if (!iblkref)
			return NULL;
		list_add_before(&iblkref->head, itr);
	} else if (iblkref->dblkref->refcnt > 1) {
		dblkref = _new_dblkref(tii->sbi);
		if (!dblkref)
			return NULL;
		_clone_data(tii->sbi, dblkref, iblkref->dblkref);
		iblkref->dblkref = dblkref;
	}
	return iblkref;
}

static int
_write(struct toyfs_inode_info *tii, void *buf, loff_t off, size_t len)
{
	int err;
	size_t cnt = 0;
	loff_t end, nxt, from = off;
	struct toyfs_iblkref *iblkref;
	struct toyfs_page *page = NULL;

	from = off;
	DBG("write: ino=%ld off=%ld len=%lu\n", tii->ino, off, len);

	err = _check_rw(off, len);
	if (err)
		return err;

	end = off + (loff_t)len;
	while (off < end) {
		iblkref = _require_iblkref(tii, off);
		if (!iblkref)
			return -ENOSPC;
		page = toyfs_bn2page(tii->sbi, iblkref->dblkref->bn);

		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_copy_in(page, buf, _off_in_page(off), len);

		cnt += len;
		off = nxt;
		buf = _advance(buf, len);
	}
	tii->zii.zi->i_size =
		(size_t)_max_offset(from, cnt, tii->zii.zi->i_size);
	return page ? 0 : -ENOSPC;
}

int toyfs_write(void *buf, struct zufs_ioc_IO *ioc_io)
{
	return _write(Z2II(ioc_io->zus_ii), buf,
		      (loff_t)ioc_io->filepos, ioc_io->hdr.len);
}

static void _zero_range_at(struct toyfs_inode_info *tii,
			   struct toyfs_iblkref *iblkref,
			   loff_t off, size_t len)
{
	struct toyfs_page *page;

	if (iblkref) {
		DBG("zero range: ino=%lu off=%ld len=%lu bn=%lu\n",
		    tii->ino, off, len, iblkref->dblkref->bn);
		page = toyfs_bn2page(tii->sbi, iblkref->dblkref->bn);
		_assign_zeros(page, _off_in_page(off), len);
	}
}

static void _punch_hole_at(struct toyfs_inode_info *tii,
			   struct toyfs_iblkref *iblkref,
			   loff_t off, size_t len)
{
	if (iblkref) {
		if (len < PAGE_SIZE)
			_zero_range_at(tii, iblkref, off, len);
		else
			_drop_iblkref(tii, iblkref);
	}
}

static int _punch_hole(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_iblkref *iblkref;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		iblkref = _fetch_iblkref(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_punch_hole_at(tii, iblkref, off, len);

		cnt += len;
		off = nxt;
	}
	return 0;
}

static int _zero_range(struct toyfs_inode_info *tii, loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_iblkref *iblkref;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		iblkref = _fetch_iblkref(tii, off);
		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);
		_zero_range_at(tii, iblkref, off, len);

		cnt += len;
		off = nxt;
	}
	return 0;
}

static int _falloc_range(struct toyfs_inode_info *tii,
			 loff_t from, size_t nbytes)
{
	size_t len, cnt = 0;
	loff_t off, end, nxt;
	struct toyfs_iblkref *iblkref = NULL;

	off = from;
	end = off + (loff_t)nbytes;
	while (off < end) {
		iblkref = _require_iblkref(tii, off);
		if (!iblkref)
			return -ENOSPC;

		nxt = _next_page(off);
		len = _nbytes_in_range(off, nxt, end);

		cnt += len;
		off = nxt;
	}

	tii->zii.zi->i_size =
		(size_t)_max_offset(from, cnt, tii->zii.zi->i_size);
	return 0;
}

static int
_fallocate(struct toyfs_inode_info *tii, int mode, loff_t off, size_t len)
{
	int err;

	DBG("fallocate: ino=%lu offset=%ld length=%lu flags=%d\n",
	    tii->ino, off, len, mode);

	err = _check_io(off, len);
	if (err)
		goto out;
	err = _check_falloc_flags(mode);
	if (err)
		goto out;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		err = _punch_hole(tii, off, len);
	else if (mode & FALLOC_FL_ZERO_RANGE)
		err = _zero_range(tii, off, len);
	else
		err = _falloc_range(tii, off, len);
out:
	return err;
}

int toyfs_fallocate(struct zus_inode_info *zii,
		    struct zufs_ioc_range *ioc_range)
{
	return _fallocate(Z2II(zii), (int)ioc_range->opflags,
			  (loff_t)ioc_range->offset, (size_t)ioc_range->length);
}


static int _seek_block(struct toyfs_inode_info *tii, loff_t from,
		       bool seek_exist, loff_t *out_off)
{
	loff_t off, end;
	const struct toyfs_page *page;

	off = from;
	end = tii->zii.zi->i_size;
	while (off < end) {
		page = _fetch_page(tii, off);
		if ((page && seek_exist) || (!page && !seek_exist)) {
			*out_off = off;
			break;
		}
		off = _next_page(off);
	}
	return 0;
}

static int _seek_data(struct toyfs_inode_info *tii, loff_t from, loff_t *out)
{
	return _seek_block(tii, from, true, out);
}

static int _seek_hole(struct toyfs_inode_info *tii, loff_t from, loff_t *out)
{
	return _seek_block(tii, from, false, out);
}

int toyfs_seek(struct zus_inode_info *zii, struct zufs_ioc_seek *zis)
{
	int err, whence = (int)zis->whence;
	loff_t off_in, off = -1;
	struct toyfs_inode_info *tii = Z2II(zii);

	off_in = (loff_t)zis->offset_in;
	DBG("seek: ino=%lu offset_in=%ld whence=%d\n",
	    tii->ino, off_in, whence);

	if (whence == SEEK_DATA)
		err = _seek_data(tii, off_in, &off);
	else if (whence == SEEK_HOLE)
		err = _seek_hole(tii, off_in, &off);
	else
		err = -ENOTSUP;

	zis->offset_out = (uint64_t)off;
	return err;
}

static void _drop_iblkref(struct toyfs_inode_info *tii,
			  struct toyfs_iblkref *iblkref)
{
	if (iblkref) {
		DBG_("drop page: ino=%lu off=%ld bn=%lu\n",
		    tii->ino, iblkref->off, iblkref->dblkref->bn);
		list_del(&iblkref->head);
		_free_iblkref(tii, iblkref);
	}
}

static void _drop_range(struct toyfs_inode_info *tii, loff_t pos)
{
	struct list_head *itr;
	struct toyfs_iblkref *iblkref = NULL;
	struct toyfs_inode_reg *reg_ti = &tii->ti->ti.reg;
	struct list_head *iblkrefs = &reg_ti->r_iblkrefs;

	if (pos % PAGE_SIZE)
		pos = _next_page(pos);

	itr = iblkrefs->next;
	while (itr != iblkrefs) {
		iblkref = container_of(itr, struct toyfs_iblkref, head);
		itr = itr->next;

		if (iblkref->off >= pos)
			_drop_iblkref(tii, iblkref);
	}
}

int toyfs_truncate(struct toyfs_inode_info *tii, size_t size)
{
	struct zus_inode *zi = tii->zii.zi;

	if (S_ISDIR(zi->i_mode))
		return -EISDIR;

	if (!S_ISREG(zi->i_mode))
		return -EINVAL;

	if (size < zi->i_size)
		_drop_range(tii, (loff_t)size);

	zi->i_size = size;
	return 0;
}

static int _clone_entire_file_range(struct toyfs_inode_info *src_tii,
				    struct toyfs_inode_info *dst_tii)
{
	struct list_head *itr;
	struct toyfs_iblkref *src_iblkref, *dst_iblkref;
	struct zus_inode *src_zi = src_tii->zii.zi;
	struct zus_inode *dst_zi = dst_tii->zii.zi;
	struct list_head *src_iblkrefs = &src_tii->ti->ti.reg.r_iblkrefs;
	struct list_head *dst_iblkrefs = &dst_tii->ti->ti.reg.r_iblkrefs;

	_drop_range(dst_tii, 0);

	toyfs_sbi_lock(dst_tii->sbi);
	itr = src_iblkrefs->next;
	while (itr != src_iblkrefs) {
		src_iblkref = container_of(itr, struct toyfs_iblkref, head);
		itr = itr->next;

		dst_iblkref = toyfs_acquire_iblkref(dst_tii->sbi);
		if (!dst_iblkref) {
			toyfs_sbi_unlock(dst_tii->sbi);
			return -ENOSPC;
		}
		dst_iblkref->off = src_iblkref->off;
		dst_iblkref->dblkref = src_iblkref->dblkref;
		dst_iblkref->dblkref->refcnt++;
		list_add_tail(&dst_iblkref->head, dst_iblkrefs);
		dst_zi->i_blocks++;
	}
	toyfs_sbi_unlock(dst_tii->sbi);
	dst_zi->i_size = src_zi->i_size;
	return 0;
}

static struct toyfs_page *
_unique_page(struct toyfs_sb_info *sbi, struct toyfs_iblkref *iblkref)
{
	struct toyfs_page *page, *new_page;
	struct toyfs_dblkref *dblkref = iblkref->dblkref;

	page = toyfs_bn2page(sbi, dblkref->bn);
	if (dblkref->refcnt > 1) {
		dblkref = toyfs_acquire_dblkref(sbi);
		if (!dblkref)
			return NULL;
		new_page = toyfs_bn2page(sbi, dblkref->bn);
		_copy_page(new_page, page);

		iblkref->dblkref->refcnt--;
		iblkref->dblkref = dblkref;
		page = new_page;
	}
	return page;
}

static void _share_page(struct toyfs_sb_info *sbi,
			struct toyfs_iblkref *src_iblkref,
			struct toyfs_iblkref *dst_iblkref)
{
	struct toyfs_dblkref *dblkref = dst_iblkref->dblkref;

	if (dblkref) {
		dblkref->refcnt--;
		if (!dblkref->refcnt)
			_free_dblkref(sbi, dblkref);
	}
	dblkref = dst_iblkref->dblkref = src_iblkref->dblkref;
	dblkref->refcnt++;
}

static bool _is_entire_page(loff_t src_off, loff_t dst_off, size_t len)
{
	return ((len == PAGE_SIZE) &&
		(_off_in_page(src_off) == 0) &&
		(_off_in_page(dst_off) == 0));
}

static int _clone_range(struct toyfs_inode_info *src_tii,
			struct toyfs_inode_info *dst_tii,
			loff_t src_off, loff_t dst_off, size_t len)
{
	size_t size;
	struct toyfs_page *dst_page;
	struct toyfs_iblkref *dst_iblkref, *src_iblkref;
	struct toyfs_sb_info *sbi = dst_tii->sbi;
	struct zus_inode *dst_zi = dst_tii->zii.zi;

	toyfs_assert(_is_entire_page(src_off, dst_off, len));
	src_iblkref = _fetch_iblkref(src_tii, src_off);
	dst_iblkref = _fetch_iblkref(dst_tii, dst_off);

	if (src_iblkref) {
		dst_iblkref = _require_iblkref(dst_tii, dst_off);
		if (!dst_iblkref)
			return -ENOSPC;
		_share_page(sbi, src_iblkref, dst_iblkref);
	} else {
		dst_iblkref = _fetch_iblkref(dst_tii, dst_off);
		if (!dst_iblkref)
			return 0;
		dst_page = _unique_page(sbi, dst_iblkref);
		if (!dst_page)
			return -ENOSPC;
		_assign_zeros(dst_page, _off_in_page(dst_off), len);
	}
	size = (size_t)dst_off + len;
	if (size > dst_zi->i_size)
		dst_zi->i_size = size;

	return 0;
}

static int _clone_sub_file_range(struct toyfs_inode_info *src_tii,
				 struct toyfs_inode_info *dst_tii,
				 loff_t src_pos, loff_t dst_pos, size_t nbytes)
{
	int err;
	size_t src_len, dst_len, len;
	loff_t src_off, src_end, src_nxt;
	loff_t dst_off, dst_end, dst_nxt;

	src_off = src_pos;
	src_end = src_off + (loff_t)nbytes;
	dst_off = dst_pos;
	dst_end = dst_off + (loff_t)nbytes;
	while ((src_off < src_end) && (dst_off < dst_end)) {
		src_nxt = _next_page(src_off);
		src_len = _nbytes_in_range(src_off, src_nxt, src_end);

		dst_nxt = _next_page(dst_off);
		dst_len = _nbytes_in_range(dst_off, dst_nxt, dst_end);

		len = src_len < dst_len ? src_len : dst_len;
		err = _clone_range(src_tii, dst_tii, src_off, dst_off, len);
		if (err)
			return err;

		src_off += len;
		dst_off += len;
	}
	return 0;
}

static int _clone(struct toyfs_inode_info *src_tii,
		  struct toyfs_inode_info *dst_tii,
		  loff_t src_pos, loff_t dst_pos, size_t len)
{
	struct zus_inode *src_zi = src_tii->zii.zi;
	struct zus_inode *dst_zi = dst_tii->zii.zi;

	DBG("clone: src_ino=%ld dst_ino=%ld pos_in=%ld pos_out=%ld len=%lu\n",
	    src_tii->ino, dst_tii->ino, src_pos, dst_pos, len);

	if (!S_ISREG(src_zi->i_mode) || !S_ISREG(dst_zi->i_mode))
		return -ENOTSUP;

	if (src_tii == dst_tii)
		return 0;

	if (!src_pos && !len && !dst_pos)
		return _clone_entire_file_range(src_tii, dst_tii);

	/* Follow XFS: only reflink if we're aligned to page boundaries */
	if (!_ispagealigned(src_pos, 0) || !_ispagealigned(src_pos, len) ||
	    !_ispagealigned(dst_pos, 0) || !_ispagealigned(dst_pos, len))
		return -ENOTSUP;

	return _clone_sub_file_range(src_tii, dst_tii, src_pos, dst_pos, len);
}

int toyfs_clone(struct zufs_ioc_clone *ioc_clone)
{
	return _clone(Z2II(ioc_clone->src_zus_ii),
		      Z2II(ioc_clone->dst_zus_ii),
		      (loff_t)ioc_clone->pos_in,
		      (loff_t)ioc_clone->pos_out,
		      (size_t)ioc_clone->len);
}

