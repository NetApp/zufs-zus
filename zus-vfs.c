/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zus-vfs.c - Abstract FS interface that calls into the um-FS
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include "zus.h"
#include "zuf_call.h"

/* ~~~ mount stuff ~~~ */

static int _pmem_mmap(struct multi_devices *md)
{
	size_t size = md_p2o(md_t1_blocks(md));
	int prot = PROT_WRITE | PROT_READ;
	int flags = MAP_SHARED;
	int err;

	if (unlikely(md->pmem_info.mdt.s_flags & MDT_F_SHADOW))
		size += size;

	md->p_pmem_addr = mmap(NULL, size, prot, flags, md->fd, 0);
	if (md->p_pmem_addr == MAP_FAILED) {
		ERROR("mmap failed=> %d: %s\n", errno, strerror(errno));
		return -(errno ?: ENOMEM);
	}

	err = madvise(md->p_pmem_addr, size, MADV_DONTDUMP);
	if (err == -1)
		ERROR("pmem madvise(DONTDUMP) failed=> %d: %s\n", errno,
		      strerror(errno));

	return 0;
}

static int _pmem_unmap(struct multi_devices *md)
{
	size_t size = md_p2o(md_t1_blocks(md));
	int err;

	if (unlikely(md->pmem_info.mdt.s_flags & MDT_F_SHADOW))
		size += size;

	err = munmap(md->p_pmem_addr, size);
	if (err == -1) {
		ERROR("munmap failed=> %d: %s\n", errno, strerror(errno));
		return errno;
	}

	return 0;
}

static int _pmem_grab(struct zus_sb_info *sbi, uint pmem_kern_id)
{
	struct multi_devices *md = &sbi->md;
	int err;

	md->sbi = sbi;
	err = zuf_root_open_tmp(&md->fd);
	if (unlikely(err))
		return err;

	err = zuf_grab_pmem(md->fd, pmem_kern_id, &md->pmem_info);
	if (unlikely(err))
		return err;

	err = _pmem_mmap(md);
	if (unlikely(err))
		return err;

	err = md_init_from_pmem_info(md);
	if (unlikely(err)) {
		ERROR("md_init_from_pmem_info pmem_kern_id=%u => %d\n",
		    pmem_kern_id, err);
		return err;
	}

	md->user_page_size = sbi->zfi->user_page_size;
	if (!md->user_page_size)
		return 0; /* User does not want pages */

	err = fba_alloc_align(&md->pages,
				md_t1_blocks(md) * md->user_page_size);
	return err;
}

static void _pmem_ungrab(struct zus_sb_info *sbi)
{
	/* Kernel makes free easy (close couple files) */
	fba_free(&sbi->md.pages);

	md_fini(&sbi->md, NULL);

	_pmem_unmap(&sbi->md);
	zuf_root_close(&sbi->md.fd);
	sbi->md.p_pmem_addr = NULL;
}

static void _zus_sbi_fini(struct zus_sb_info *sbi)
{
	// zus_iput(sbi->z_root); was this done already
	if (sbi->zfi->op->sbi_fini)
		sbi->zfi->op->sbi_fini(sbi);
	_pmem_ungrab(sbi);
	sbi->zfi->op->sbi_free(sbi);
}

int zus_mount(int fd, struct zufs_ioc_mount *zim)
{
	struct zus_fs_info *zfi = zim->zmi.zus_zfi;
	struct zus_sb_info *sbi;
	int err;

	sbi = zfi->op->sbi_alloc(zfi);
	if (unlikely(!sbi)) {
		err = -ENOMEM;
		goto err;
	}
	sbi->zfi = zim->zmi.zus_zfi;
	sbi->kern_sb_id = zim->zmi.sb_id;

	err = _pmem_grab(sbi, zim->zmi.pmem_kern_id);
	if (unlikely(err))
		goto err;

	err = sbi->zfi->op->sbi_init(sbi, zim);
	if (unlikely(err))
		goto err;

	zim->zmi.zus_sbi = sbi;
	zim->zmi._zi = pmem_dpp_t(md_addr_to_offset(&sbi->md, sbi->z_root->zi));
	zim->zmi.zus_ii = sbi->z_root;

	DBG("[%lld] _zi 0x%lx zus_ii=%p\n",
	    sbi->z_root->zi->i_ino, (ulong)zim->zmi._zi, zim->zmi.zus_ii);

	return 0;
err:
	zus_sbi_set_flag(sbi, ZUS_SBIF_ERROR);
	_zus_sbi_fini(sbi);
	zim->hdr.err = err;
	return err;
}

int zus_umount(int fd, struct zufs_ioc_mount *zim)
{
	_zus_sbi_fini(zim->zmi.zus_sbi);
	return 0;
}

int zus_remount(int fd, struct zufs_ioc_mount *zim)
{
	struct zus_sb_info *sbi = zim->zmi.zus_sbi;

	if (sbi->zfi->op->sbi_remount)
		return sbi->zfi->op->sbi_remount(sbi, zim);
	return 0;
}

const char *ZUFS_OP_name(enum e_zufs_operation op)
{
#define CASE_ENUM_NAME(e) case e: return #e
	switch (op) {
		CASE_ENUM_NAME(ZUFS_OP_NULL);
		CASE_ENUM_NAME(ZUFS_OP_STATFS);
		CASE_ENUM_NAME(ZUFS_OP_NEW_INODE);
		CASE_ENUM_NAME(ZUFS_OP_FREE_INODE);
		CASE_ENUM_NAME(ZUFS_OP_EVICT_INODE);
		CASE_ENUM_NAME(ZUFS_OP_LOOKUP);
		CASE_ENUM_NAME(ZUFS_OP_ADD_DENTRY);
		CASE_ENUM_NAME(ZUFS_OP_REMOVE_DENTRY);
		CASE_ENUM_NAME(ZUFS_OP_RENAME);
		CASE_ENUM_NAME(ZUFS_OP_READDIR);
		CASE_ENUM_NAME(ZUFS_OP_CLONE);
		CASE_ENUM_NAME(ZUFS_OP_COPY);
		CASE_ENUM_NAME(ZUFS_OP_READ);
		CASE_ENUM_NAME(ZUFS_OP_PRE_READ);
		CASE_ENUM_NAME(ZUFS_OP_WRITE);
		CASE_ENUM_NAME(ZUFS_OP_GET_BLOCK);
		CASE_ENUM_NAME(ZUFS_OP_PUT_BLOCK);
		CASE_ENUM_NAME(ZUFS_OP_MMAP_CLOSE);
		CASE_ENUM_NAME(ZUFS_OP_GET_SYMLINK);
		CASE_ENUM_NAME(ZUFS_OP_SETATTR);
		CASE_ENUM_NAME(ZUFS_OP_SYNC);
		CASE_ENUM_NAME(ZUFS_OP_FALLOCATE);
		CASE_ENUM_NAME(ZUFS_OP_LLSEEK);
		CASE_ENUM_NAME(ZUFS_OP_IOCTL);
		CASE_ENUM_NAME(ZUFS_OP_XATTR_GET);
		CASE_ENUM_NAME(ZUFS_OP_XATTR_SET);
		CASE_ENUM_NAME(ZUFS_OP_XATTR_LIST);
		CASE_ENUM_NAME(ZUFS_OP_BREAK);
		CASE_ENUM_NAME(ZUFS_OP_MAX_OPT);
	default:
		return "UNKNOWN";
	}
}

int zus_do_command(void *app_ptr, struct zufs_ioc_hdr *hdr)
{
	DBG("[%s] OP=%d off=0x%x len=0x%x\n", ZUFS_OP_name(hdr->operation),
		hdr->operation, hdr->offset, hdr->len);

	switch (hdr->operation) {
	case ZUFS_OP_NEW_INODE:
	case ZUFS_OP_FREE_INODE:
	case ZUFS_OP_EVICT_INODE:
	case ZUFS_OP_LOOKUP:
	case ZUFS_OP_ADD_DENTRY:
	case ZUFS_OP_REMOVE_DENTRY:
	case ZUFS_OP_RENAME:
	case ZUFS_OP_READDIR:
	case ZUFS_OP_CLONE:
	case ZUFS_OP_COPY:
	case ZUFS_OP_READ:
	case ZUFS_OP_PRE_READ:
	case ZUFS_OP_WRITE:
	case ZUFS_OP_GET_BLOCK:
	case ZUFS_OP_PUT_BLOCK:
	case ZUFS_OP_MMAP_CLOSE:
	case ZUFS_OP_GET_SYMLINK:
	case ZUFS_OP_SETATTR:
	case ZUFS_OP_SYNC:
	case ZUFS_OP_FALLOCATE:
	case ZUFS_OP_LLSEEK:
	case ZUFS_OP_IOCTL:
	case ZUFS_OP_XATTR_GET:
	case ZUFS_OP_XATTR_SET:
	case ZUFS_OP_XATTR_LIST:
	case ZUFS_OP_STATFS:
		return -ENOTSUP;
	case ZUFS_OP_BREAK:
		break;
	default:
		ERROR("Unknown OP=%d\n", hdr->operation);
	}

	return 0;
}
