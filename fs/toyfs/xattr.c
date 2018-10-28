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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "zus.h"
#include "toyfs.h"

static bool _has_xattr(const struct toyfs_inode_info *tii)
{
	const struct toyfs_inode *ti = tii->ti;

	return (ti->i_xattr != 0);
}

static int _require_xattr(struct toyfs_inode_info *tii)
{
	struct toyfs_pmemb *pmemb;
	struct toyfs_inode *ti = tii->ti;

	if (_has_xattr(tii))
		return 0;

	pmemb = toyfs_acquire_pmemb(tii->sbi);
	if (unlikely(!pmemb))
		return -ENOSPC;

	ti->i_xattr = toyfs_addr2bn(tii->sbi, pmemb);
	return 0;
}

static struct toyfs_xattr *_xattr_of(const struct toyfs_inode_info *tii)
{
	const struct toyfs_inode *ti = tii->ti;

	return toyfs_bn2addr(tii->sbi, ti->i_xattr);
}

static bool _has_data(const struct toyfs_xattr_entry *xe)
{
	return (xe->name_len > 0);
}

static bool _has_name(const struct toyfs_xattr_entry *xe,
		      const char *name, size_t name_len)
{
	return (xe->name_len == name_len) &&
	       !memcmp(xe->data, name, name_len);
}

static struct toyfs_xattr_entry *_next_of(struct toyfs_xattr_entry *xe)
{
	void *next, *base = (void *)xe;

	next = (uint8_t *)base + xe->name_len + xe->value_size;
	return (struct toyfs_xattr_entry *)next;
}

static ssize_t _copy_value_to_buf(const struct toyfs_xattr_entry *xe,
				  void *buffer, size_t size)
{
	const uint8_t *value = xe->data + xe->name_len;

	if (xe->value_size > size)
		return -ERANGE;

	memcpy(buffer, value, xe->value_size);
	return (ssize_t)xe->value_size;
}

static struct toyfs_xattr_entry *
_find_xe(const struct toyfs_inode_info *tii, const char *name, size_t name_len)
{
	struct toyfs_xattr *xattr = _xattr_of(tii);
	struct toyfs_xattr_entry *xe = xattr->xe;
	struct toyfs_xattr_entry *xe_end =
		xattr->xe + ARRAY_SIZE(xattr->xe);

	while ((xe < xe_end) && _has_data(xe)) {
		if (_has_name(xe, name, name_len))
			return xe;
		xe = _next_of(xe);
	}
	return NULL;
}

static ssize_t _do_getxattr(const struct toyfs_inode_info *tii,
			    const char *name, size_t name_len,
			    void *buffer, size_t size)
{
	ssize_t ret = -ENODATA;
	struct toyfs_xattr_entry *xe = _find_xe(tii, name, name_len);

	if (xe)
		ret = _copy_value_to_buf(xe, buffer, size);

	return ret;
}

int toyfs_getxattr(struct zus_inode_info *zii,
		   struct zufs_ioc_xattr *ioc_xattr)
{
	ssize_t size;
	const char *name = ioc_xattr->buf;
	struct toyfs_inode_info *tii = Z2II(zii);

	if (!_has_xattr(tii))
		return -ENODATA;

	size = _do_getxattr(tii, name, strlen(name), ioc_xattr->buf,
			    ioc_xattr->user_buf_size);
	if (unlikely(size < 0))
		return size;

	if (ioc_xattr->user_buf_size)
		ioc_xattr->hdr.out_len += size;
	ioc_xattr->user_buf_size = size;
	return 0;
}

static size_t _nbytes_distance(struct toyfs_xattr_entry *beg,
			       struct toyfs_xattr_entry *end)
{
	return (uint8_t *)end - (uint8_t *)beg;
}

static int _discard_xattr(const struct toyfs_inode_info *tii,
			  struct toyfs_xattr_entry *xe)
{
	size_t cnt;
	struct toyfs_xattr *xattr = _xattr_of(tii);
	struct toyfs_xattr_entry *xe_next = _next_of(xe);
	struct toyfs_xattr_entry *xe_end =
		xattr->xe + ARRAY_SIZE(xattr->xe);

	cnt = _nbytes_distance(xe_next, xe_end);
	memcpy(xe, xe_next, cnt);
	return 0;
}

static int _do_removexattr(const struct toyfs_inode_info *tii,
			   const char *name, size_t name_len)
{
	ssize_t ret = -ENODATA;
	struct toyfs_xattr_entry *xe = _find_xe(tii, name, name_len);

	if (xe)
		ret = _discard_xattr(tii, xe);
	return ret;
}

static int _append_xattr(const struct toyfs_inode_info *tii,
			 const char *name, size_t name_len,
			 const void *value, size_t size)
{
	size_t cnt;
	struct toyfs_xattr *xattr = _xattr_of(tii);
	struct toyfs_xattr_entry *xe = xattr->xe;
	struct toyfs_xattr_entry *xe_end =
		xattr->xe + ARRAY_SIZE(xattr->xe);

	while ((xe < xe_end) && _has_data(xe))
		xe = _next_of(xe);

	cnt = _nbytes_distance(xe, xe_end);
	if (cnt < (name_len + size))
		return -ENOSPC;

	memcpy(xe->data, name, name_len);
	memcpy(xe->data + name_len, value, size);
	xe->name_len = name_len;
	xe->value_size = size;
	return 0;
}

static int _do_setxattr(const struct toyfs_inode_info *tii,
			const char *name, size_t name_len,
			const void *value, size_t size, unsigned flags)
{
	struct toyfs_xattr_entry *xe = _find_xe(tii, name, name_len);

	if ((flags & XATTR_CREATE) && xe)
		return -EEXIST;
	if ((flags & XATTR_REPLACE) && !xe)
		return -ENODATA;

	/* Naive impl */
	_do_removexattr(tii, name, name_len);
	return _append_xattr(tii, name, name_len, value, size);
}

int toyfs_setxattr(struct zus_inode_info *zii,
		   struct zufs_ioc_xattr *ioc_xattr)
{
	int err;
	const void *value = NULL;
	const char *name = ioc_xattr->buf;
	struct toyfs_inode_info *tii = Z2II(zii);

	err = _require_xattr(tii);
	if (unlikely(err))
		return err;

	if (ioc_xattr->user_buf_size ||
	    (ioc_xattr->ioc_flags & ZUS_XATTR_SET_EMPTY))
		value = ioc_xattr->buf + ioc_xattr->name_len;

	if (!value)
		return _do_removexattr(tii, name, strlen(name));

	return _do_setxattr(tii, name, strlen(name),
			    value, ioc_xattr->user_buf_size, ioc_xattr->flags);
}

static void _copy_name_to_buf(const struct toyfs_xattr_entry *xe,
			      char **buf, size_t *size)
{
	memcpy(*buf, xe->data, xe->name_len);
	(*buf)[xe->name_len] = '\0';
	*size -= (xe->name_len + 1);
}

static ssize_t _do_listxattr(const struct toyfs_inode_info *tii,
			     char *buf, size_t size)
{
	ssize_t ret = 0;
	struct toyfs_xattr *xattr = _xattr_of(tii);
	struct toyfs_xattr_entry *xe = xattr->xe;
	struct toyfs_xattr_entry *xe_end =
		xattr->xe + ARRAY_SIZE(xattr->xe);

	while ((xe < xe_end) && _has_data(xe)) {
		if (size) {
			if (size <= xe->name_len)
				return -ERANGE;
			_copy_name_to_buf(xe, &buf, &size);
		}
		ret += xe->name_len + 1;
		xe = _next_of(xe);
	}
	return ret;
}

int toyfs_listxattr(struct zus_inode_info *zii,
		    struct zufs_ioc_xattr *ioc_xattr)
{
	ssize_t size;
	struct toyfs_inode_info *tii = Z2II(zii);

	if (!_has_xattr(tii))
		return -ENODATA;

	size = _do_listxattr(tii, ioc_xattr->buf, ioc_xattr->user_buf_size);

	if (unlikely(size < 0))
		return size;

	if (ioc_xattr->user_buf_size)
		ioc_xattr->hdr.out_len += size;
	ioc_xattr->user_buf_size = size;
	return 0;
}

void toyfs_drop_xattr(struct toyfs_inode_info *tii)
{
	struct toyfs_pmemb *pmemb;
	struct toyfs_inode *ti = tii->ti;

	if (_has_xattr(tii)) {
		pmemb = toyfs_bn2pmemb(tii->sbi, ti->i_xattr);
		toyfs_release_pmemb(tii->sbi, pmemb);
		ti->i_xattr = 0;
	}
}
