/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * iom_enc.h - Encodes an iom_elemets array to send to Kernel
 *
 * Encoding is only done in user-mode. And decoding only in Kernel.
 * This is the encoding side. Common stuff come from zus_api.h
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#ifndef __ZUS_IOM_H__
#define __ZUS_IOM_H__

#include <errno.h>

#include "zus.h"

typedef void (*iomb_done_fn)(struct zus_iomap_build *iomb);
typedef void (*iomb_submit_fn)(struct zus_iomap_build *iomb, bool sync);
struct zus_iomap_build {
	iomb_done_fn		done;
	iomb_submit_fn		submit;
	void			*priv;
	struct zus_sb_info	*sbi;		/* needed if for ioc_exec */
	int			fd;
	int			err;

	void 			*cur_iom_e;
	void 			*end_iom_e;

	struct zufs_iomap	*ziom;
	union {
		struct zufs_ioc_iomap_exec *ioc_exec;
		struct zufs_ioc_IO *ioc_io;
	};
};

void _zus_iom_ioc_exec_submit(struct zus_iomap_build *iomb, bool sync);

static inline ulong _zus_iom_len(struct zus_iomap_build *iomb)
{
	return (__u64 *)iomb->cur_iom_e - iomb->ziom->iom_e;
}

static inline bool _zus_iom_empty(struct zus_iomap_build *iomb)
{
	return !_zus_iom_len(iomb);
}

static inline void _zus_iom_enc_type_val(__u64 *ptr, enum ZUFS_IOM_TYPE type,
					 ulong val)
{
	ZUS_WARN_ON(val & ~ZUFS_IOM_FIRST_VAL_MASK);

	*ptr = (__u64)val | ((__u64)type << ZUFS_IOM_VAL_BITS);
}

/* iomb comes ZEROed! */
static inline void _zus_iom_common_init(struct zus_iomap_build *iomb,
					struct zus_sb_info *sbi,
					struct zufs_iomap *ziom, void *end_ptr)
{
	memset(ziom, 0, sizeof(*ziom));
	ziom->iom_max = (end_ptr - (void *)ziom->iom_e) / sizeof(__u64);

	iomb->sbi = sbi;
	iomb->ziom = ziom;
	iomb->end_iom_e = end_ptr;
}

static inline void _zus_iom_init_4_ioc_exec(struct zus_iomap_build *iomb,
					struct zus_sb_info *sbi, int fd,
					struct zufs_ioc_iomap_exec *ioc_exec,
					uint max_bytes)
{
	_zus_iom_common_init(iomb, sbi, &ioc_exec->ziom,
			     (void *)ioc_exec + max_bytes);
	iomb->fd = fd;
	iomb->submit = _zus_iom_ioc_exec_submit;
	iomb->ioc_exec = ioc_exec;
}

static inline void _zus_iom_init_4_ioc_io(struct zus_iomap_build *iomb,
					    struct zus_sb_info *sbi,
					    struct zufs_ioc_IO *ioc_io,
					    uint max_bytes)
{
	_zus_iom_common_init(iomb, sbi, &ioc_io->ziom,
			     (void *)ioc_io + max_bytes);
	iomb->ioc_io = ioc_io;
}

static inline void _zus_iom_start(struct zus_iomap_build *iomb, void *priv,
				  iomb_done_fn done)
{
	iomb->cur_iom_e = iomb->ziom->iom_e;
	iomb->ziom->iom_e[0] = 0;
	iomb->done = done;
	iomb->priv = priv;
}
static inline void _zus_iom_end(struct zus_iomap_build *iomb)
{
	/* NULL terminated list */
	if (iomb->cur_iom_e < iomb->end_iom_e)
		_zus_iom_enc_type_val(iomb->cur_iom_e, 0, 0);

	if (iomb->ziom)
		iomb->ziom->iom_n = _zus_iom_len(iomb);
}

static inline int _zus_iom_enc_unmap(struct zus_iomap_build *iomb, ulong index,
				     ulong n, ulong ino)
{
	struct zufs_iom_unmap *iom_unmap = iomb->cur_iom_e;
	void *next_iom_e = iom_unmap + 1;

	if (unlikely(iomb->end_iom_e < next_iom_e))
		return -ENOSPC;

	_zus_iom_enc_type_val(&iom_unmap->unmap_index, IOM_UNMAP, index);
	iom_unmap->unmap_n = n;
	iom_unmap->ino = ino;
	iomb->cur_iom_e = next_iom_e;
	return 0;
}

static inline int _zus_iom_enc_t2_io(struct zus_iomap_build *iomb, ulong t2_bn,
				     zu_dpp_t t1_val, enum ZUFS_IOM_TYPE type)
{
	struct zufs_iom_t2_io *iom_io = iomb->cur_iom_e;
	void *next_iom_e = iom_io + 1;

	if (unlikely(iomb->end_iom_e < next_iom_e))
		return -ENOSPC;

	_zus_iom_enc_type_val(&iom_io->t2_val, type, t2_bn);
	iom_io->t1_val = t1_val;

	iomb->cur_iom_e = next_iom_e;
	return 0;
}

static inline int _zus_iom_enc_t2_write(struct zus_iomap_build *iomb, ulong t2_bn,
				     zu_dpp_t t1_val)
{
	return _zus_iom_enc_t2_io(iomb, t2_bn, t1_val, IOM_T2_WRITE);
}

static inline int _zus_iom_enc_t2_read(struct zus_iomap_build *iomb, ulong t2_bn,
				     zu_dpp_t t1_val)
{
	return _zus_iom_enc_t2_io(iomb, t2_bn, t1_val, IOM_T2_READ);
}

static inline int _zus_iom_enc_t2_zusmem_io(struct zus_iomap_build *iomb,
					    ulong t2_bn, void *ptr, ulong len,
					    enum ZUFS_IOM_TYPE type)
{
	struct zufs_iom_t2_zusmem_io *iom_io = iomb->cur_iom_e;
	void *next_iom_e = iom_io + 1;

	if (unlikely(iomb->end_iom_e < next_iom_e))
		return -ENOSPC;

	_zus_iom_enc_type_val(&iom_io->t2_val, type, t2_bn);
	iom_io->zus_mem_ptr = (__u64)ptr;
	iom_io->len = len;

	iomb->cur_iom_e = next_iom_e;
	return 0;
}

static inline int _zus_iom_enc_t2_zusmem_write(struct zus_iomap_build *iomb,
						ulong t2_bn, void *ptr,
						ulong len)
{
	return _zus_iom_enc_t2_zusmem_io(iomb, t2_bn, ptr, len,
					 IOM_T2_ZUSMEM_WRITE);
}

static inline int _zus_iom_enc_t2_zusmem_read(struct zus_iomap_build *iomb,
						ulong t2_bn, void *ptr,
						ulong len)
{
	return _zus_iom_enc_t2_zusmem_io(iomb, t2_bn, ptr, len,
					 IOM_T2_ZUSMEM_READ);
}

#endif /* define __ZUS_IOM_H__ */
