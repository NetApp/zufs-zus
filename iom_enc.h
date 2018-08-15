/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * iom_enc.h - Encodes an iom_elemets array to send to Kernel
 *
 * Encoding is only done in user-mode. And decoding only in Kernel.
 * This is the encoding side. Common stuff come from zus_api.h
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */
#ifndef __ZUS_IOM_H__
#define __ZUS_IOM_H__

#include <errno.h>

#include "zus.h"

typedef void (*iomb_done_fn)(struct zus_iomap_build *iomb);
typedef void (*iomb_submit_fn)(struct zus_iomap_build *iomb, bool last,
			       bool sync);
struct zus_iomap_build {
	iomb_done_fn		done;
	iomb_submit_fn		submit;
	void			*priv;
	int			err;

	__u64 			*start_iom_e;
	void 			*cur_iom_e;
	void 			*end_iom_e;

	/* Optional */
	struct zus_sb_info	*sbi;
	struct zufs_iomap	*ziom;
};

static inline ulong _zus_iom_len(struct zus_iomap_build *iomb)
{
	return (__u64 *)iomb->cur_iom_e - iomb->start_iom_e;
}

static inline void _zus_iom_enc_type_val(__u64 *ptr, enum ZUFS_IOM_TYPE type,
					 ulong val)
{
	ZUS_WARN_ON(val & ~ZUFS_IOM_FIRST_VAL_MASK);

	*ptr = (__u64)val | ((__u64)type << ZUFS_IOM_VAL_BITS);
}

static inline void _zus_ioc_ziom_init(struct zufs_iomap *ziom, uint bytes)
{
	memset(ziom, 0, sizeof(*ziom));
	ziom->iom_max = (bytes - sizeof(*ziom)) / sizeof(__u64);
}

static inline void _zus_iom_start(struct zus_iomap_build *iomb, __u64 *iom_e,
				  uint max, struct zufs_iomap *ziom)
{
	iomb->cur_iom_e = iomb->start_iom_e = iom_e;
	iomb->end_iom_e = iom_e + max;
	iom_e[0] = 0;
	iomb->ziom = ziom;
}

/* ziom must come zero(ed) and @ziom->max_list denoting
 * how much space is available in @ziom->iom_e[].
 */
static inline void _zus_ioc_iom_start(struct zus_iomap_build *iomb,
				      struct zus_sb_info *sbi,
				      iomb_done_fn done, iomb_submit_fn submit,
				      void* priv, struct zufs_iomap *ziom)
{
	_zus_iom_start(iomb, ziom->iom_e, ziom->iom_max, ziom);
	iomb->sbi = sbi;
	iomb->done = done;
	iomb->submit = submit;
	iomb->priv = priv;
	ziom->iomb = iomb;
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

/* Two services for submit */
void _zus_ioc_iom_exec_submit(struct zus_iomap_build *iomb, bool done, bool sync);
void _zus_iom_read_submit(struct zus_iomap_build *iomb, bool done, bool sync);

#endif /* define __ZUS_IOM_H__ */
