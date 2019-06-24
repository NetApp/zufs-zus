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
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "zus.h"
#include "iom_enc.h"
#include "toyfs.h"

#define GB_WRITE 1


static uint64_t _resolve_bn(const struct toyfs_inode_info *tii,
				struct toyfs_pmemb *pmemb)
{
	return pmemb ? toyfs_addr2bn(tii->sbi, (void *)pmemb) : 0;
}

static int _get_block_rd(struct toyfs_inode_info *tii, loff_t off,
			 struct zufs_ioc_IO *get_block)
{
	struct toyfs_pmemb *pmemb;
	struct zus_iomap_build iomb = {};

	_zus_iom_init_4_ioc_io(&iomb, &tii->sbi->s_zus_sbi,
				get_block, ZUS_MAX_OP_SIZE);
	pmemb = toyfs_resolve_pmemb(tii, off);

	_zus_iom_start(&iomb, NULL, NULL);
	_ziom_enc_t1_bn(&iomb, _resolve_bn(tii, pmemb), 0);
	_zus_iom_end(&iomb);
	get_block->ret_flags = 0;
	get_block->hdr.out_len = _ioc_IO_size(1);

	return 0;
}

static int _get_block_wr(struct toyfs_inode_info *tii, loff_t off,
			 struct zufs_ioc_IO *get_block)
{
	uint64_t pmem_bn;
	struct toyfs_pmemb *pmemb;
	struct zus_iomap_build iomb = {};

	_zus_iom_init_4_ioc_io(&iomb, &tii->sbi->s_zus_sbi,
				get_block, ZUS_MAX_OP_SIZE);

	pmemb = toyfs_resolve_pmemb(tii, off);
	if (pmemb) {
		_zus_iom_start(&iomb, NULL, NULL);
		_ziom_enc_t1_bn(&iomb, _resolve_bn(tii, pmemb), 0);
		_zus_iom_end(&iomb);
		get_block->ret_flags = 0;
		get_block->hdr.out_len = _ioc_IO_size(1);

		return 0;
	}
	pmem_bn = toyfs_require_pmem_bn(tii, off);
	if (pmem_bn) {
		_zus_iom_start(&iomb, NULL, NULL);
		_ziom_enc_t1_bn(&iomb, pmem_bn, 0);
		_zus_iom_end(&iomb);
		get_block->ret_flags = ZUFS_RET_NEW;
		get_block->hdr.out_len = _ioc_IO_size(1);

		return 0;
	}
	return -ENOSPC;
}


int toyfs_get_block(struct zus_inode_info *zii,
		    struct zufs_ioc_IO *get_block)
{
	int err;
	const loff_t off = (loff_t)get_block->filepos;
	struct toyfs_inode_info *tii = Z2II(zii);

	if (!zi_isreg(tii->zii.zi))
		return -ENOTSUP;

	if (get_block->rw & GB_WRITE)
		err = _get_block_wr(tii, off, get_block);
	else
		err = _get_block_rd(tii, off, get_block);

	DBG("get_block: ino=%ld off=%ld err=%d\n",
	    (long)tii->ino, (long)get_block->filepos, err);

	return err;
}

int toyfs_put_block(struct zus_inode_info *zii, struct zufs_ioc_IO *get_block)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	DBG("put_block: ino=%ld off=%ld\n",
	    (long)tii->ino, (long)get_block->filepos);

	return 0;
}

int toyfs_mmap_close(struct zus_inode_info *zii,
		     struct zufs_ioc_mmap_close *mmap_close)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	DBG("mmap_close: ino=%ld rw=%lx\n",
	    (long)tii->ino, (long)mmap_close->rw);
	return 0;
}
