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
		return -errno;
	}

	return 0;
}

static int _pmem_grab(struct zus_sb_info *sbi, __u64 sb_id)
{
	struct multi_devices *md = &sbi->md;
	int err;

	md->sbi = sbi;
	err = zuf_root_open_tmp(&md->fd);
	if (unlikely(err))
		return err;

	err = zuf_grab_pmem(md->fd, sb_id, &md->pmem_info);
	if (unlikely(err))
		return err;

	err = _pmem_mmap(md);
	if (unlikely(err))
		return err;

	err = md_init_from_pmem_info(md);
	if (unlikely(err)) {
		ERROR("md_init_from_pmem_info sb_id=%llu => %d\n", sb_id, err);
		return err;
	}
	md->dev_index = md->pmem_info.dev_index;

	md->user_page_size = sbi->zfi->user_page_size;
	if (!md->user_page_size)
		return 0; /* User does not want pages */

	err = fba_alloc_align(&md->pages, md_t1_blocks(md) * md->user_page_size,
			      CONFIG_TRY_ANON_MMAP);
	return err;
}

static void _pmem_ungrab(struct zus_sb_info *sbi)
{
	/* Kernel makes free easy (close couple files) */
	fba_free(&sbi->md.pages);

	md_fini(&sbi->md, true);

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

int zus_private_mount(struct zus_fs_info *zfi, const char *options,
		      struct zufs_ioc_mount_private **zip_out)
{
	struct zufs_ioc_mount_private *zip;
	struct zus_sb_info *sbi = NULL;
	int zip_len;
	int err;

	zip_len = sizeof(*zip) + strlen(options) + 1;

	zip = calloc(1, zip_len);
	if (!zip) {
		ERROR("failed to allocate memory\n");
		return -ENOMEM;
	}

	zip->zmi.zus_zfi = zfi;

	err = zuf_root_open_tmp(&zip->mount_fd);
	if (unlikely(err))
		goto fail;

	err = zus_numa_map_init(zip->mount_fd);
	if (unlikely(err))
		goto fail_fd;

	err = zus_thread_current_init();
	if (unlikely(err))
		goto fail_fd;

	sbi = zfi->op->sbi_alloc(zfi);
	if (unlikely(!sbi)) {
		err = -ENOMEM;
		goto fail_alloc;
	}

	zip->hdr.in_len = zip_len;
	zip->zmi.po.mount_options_len = strlen(options);
	memcpy(&zip->zmi.po.mount_options, options,
		zip->zmi.po.mount_options_len);
	memcpy(&zip->rfi, &zfi->rfi, sizeof(struct register_fs_info));

	err = zuf_private_mount(zip->mount_fd, zip);
	if (unlikely(err))
		goto fail_mount;

	sbi->zfi = zip->zmi.zus_zfi;
	sbi->kern_sb_id = zip->zmi.sb_id;
	err = _pmem_grab(sbi, zip->zmi.sb_id);
	if (unlikely(err))
		goto fail_grab;

	err = sbi->zfi->op->sbi_init(sbi, &zip->zmi);
	if (unlikely(err)) {
		zus_sbi_set_flag(sbi, ZUS_SBIF_ERROR);
		goto fail_sbi_init;
	}

	zip->zmi.zus_sbi = sbi;
	zip->zmi._zi = pmem_dpp_t(md_addr_to_offset(&sbi->md, sbi->z_root->zi));
	zip->zmi.zus_ii = sbi->z_root;

	DBG("[%lld] _zi 0x%lx zus_ii=%p\n",
	    sbi->z_root->zi->i_ino, (ulong)zip->zmi._zi, zip->zmi.zus_ii);

	*zip_out = zip;

	return 0;

fail_sbi_init:
	if (sbi->z_root)
		sbi->z_root->op->evict(sbi->z_root);
	if (sbi->zfi->op->sbi_fini)
		sbi->zfi->op->sbi_fini(sbi);
	_pmem_ungrab(sbi);
fail_grab:
	zuf_private_umount(zip->mount_fd, zip);
fail_mount:
	zfi->op->sbi_free(sbi);
fail_alloc:
	zus_thread_current_fini();
fail_fd:
	close(zip->mount_fd);
fail:
	free(zip);

	return err;
}

int zus_private_umount(struct zufs_ioc_mount_private *zip)
{
	struct zus_sb_info *sbi = zip->zmi.zus_sbi;
	int err = 0;

	/* evict root inode (done by VFS on regular mount) */
	if (sbi->z_root)
		sbi->z_root->op->evict(sbi->z_root);

	if (sbi->zfi->op->sbi_fini)
		err = sbi->zfi->op->sbi_fini(sbi);
	_pmem_ungrab(sbi);
	zuf_private_umount(zip->mount_fd, zip);
	sbi->zfi->op->sbi_free(sbi);
	zus_thread_current_fini();
	close(zip->mount_fd);
	free(zip);
	return err;
}

int zus_mount(int fd, struct zufs_ioc_mount *zim)
{
	struct zus_fs_info *zfi = zim->zmi.zus_zfi;
	struct zus_sb_info *sbi;
	int err;

	sbi = zfi->op->sbi_alloc(zfi);
	if (unlikely(!sbi)) {
		zim->hdr.err = -ENOMEM;
		return zim->hdr.err;
	}
	sbi->zfi = zim->zmi.zus_zfi;
	sbi->kern_sb_id = zim->zmi.sb_id;

	err = _pmem_grab(sbi, zim->zmi.sb_id);
	if (unlikely(err))
		goto err;

	err = sbi->zfi->op->sbi_init(sbi, &zim->zmi);
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
		return sbi->zfi->op->sbi_remount(sbi, &zim->zmi);
	return 0;
}

/* ~~~ FS operations ~~~~ */

struct zus_inode_info *zus_iget(struct zus_sb_info *sbi, ulong ino)
{
	struct zus_inode_info *zii;
	int err;

	err =  sbi->op->iget(sbi, ino, &zii);
	if (err)
		return NULL;

	zii->sbi = sbi;
	return zii;
}

static int _new_inode(void *app_ptr, struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_new_inode *ioc_new = (void *)hdr;
	struct zus_sb_info *sbi = ioc_new->dir_ii->sbi;
	struct zus_inode_info *zii;
	int err;

	/* In ZUS protocol we start zero ref, add_dentry increments the refs
	 * (Kernel gave us a 1 here expect for O_TMPFILE)
	 */
	ioc_new->zi.i_nlink = 0;

	zii = sbi->op->new_inode(sbi, app_ptr, ioc_new);
	if (unlikely(!zii))
		return -EINVAL;

	ioc_new->_zi = md_addr_to_offset(&sbi->md, zii->zi);
	ioc_new->zus_ii = zii;

	if (ioc_new->flags & ZI_TMPFILE)
		return 0;

	err = ioc_new->dir_ii->sbi->op->add_dentry(ioc_new->dir_ii, zii,
						   &ioc_new->str);
	if (unlikely(err))
		goto _err_free_inode;

	return 0;

_err_free_inode:
	zii->sbi->op->free_inode(zii);
	return err;
}

static int _evict(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_evict_inode *ziei = (void *)hdr;
	struct zus_inode_info *zii = ziei->zus_ii;

	if (unlikely(!zii)) {
		ERROR("!ziei->zus_ii\n");
		return 0;
	}

	if (hdr->operation == ZUFS_OP_FREE_INODE) {
		if (likely(zii->sbi->op->free_inode))
			zii->sbi->op->free_inode(zii);
	} else { /* ZUFS_OP_EVICT_INODE */
		/* NOTE: On lookup Kernel ask's zus to allocate a new zii &&
		 * retrieve the zi, before it inserts it to inode cache, it is
		 * possible to race, and have two threads do a lookup. The
		 * loosing thread calls _evict(ZI_LOOKUP_RACE) to de-allocate
		 * the extra zii. But fs->evict need not be called. Only
		 * zii_free.
		 * (So it is possible at FS to see two fs->igets but one
		 *  fs->evict)
		 */
		if (zii->op->evict)
			zii->op->evict(zii);
	}
	return 0;
}

static int _lookup(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_lookup *lookup = (void *)hdr;
	struct zufs_str *str = &lookup->str;
	struct zus_inode_info *zii;
	ulong ino;

	if (!str->len || !str->name[0]) {
		ERROR("lookup NULL string\n");
		return  0;
	}

	if (0 == strncmp(".", str->name, str->len))
		ino = lookup->dir_ii->zi->i_ino;
	else if (0 == strncmp("..", str->name, str->len))
		ino = lookup->dir_ii->zi->i_dir.parent;
	else
		ino  = lookup->dir_ii->sbi->op->lookup(lookup->dir_ii, str);

	if (!ino) {
		DBG("[%.*s] NOT FOUND\n", lookup->str.len, lookup->str.name);
		return -ENOENT;
	}

	DBG("[%.*s] ino=%ld\n", lookup->str.len, lookup->str.name, ino);
	zii = zus_iget(lookup->dir_ii->sbi, ino);
	if (unlikely(!zii))
		return -ENOENT;

	lookup->_zi = md_addr_to_offset(&zii->sbi->md, zii->zi);
	lookup->zus_ii = zii;
	return 0;
}

static int _dentry(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_dentry *zid = (void *)hdr;
	struct zus_inode_info *dir_ii = zid->zus_dir_ii;
	struct zus_inode_info *zii = zid->zus_ii;

	if (hdr->operation == ZUFS_OP_REMOVE_DENTRY)
		return dir_ii->sbi->op->remove_dentry(dir_ii, zii, &zid->str);

	return dir_ii->sbi->op->add_dentry(dir_ii, zid->zus_ii, &zid->str);
}

static int _rename(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_rename *zir = (void *)hdr;
	struct zus_sb_info *sbi = zir->old_dir_ii->sbi;

	if (!sbi->op->rename)
		return -ENOTSUP;

	return sbi->op->rename(zir);
}

static int _readdir(void *app_ptr, struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_readdir *zir = (void *)hdr;
	struct zus_sb_info *sbi = zir->dir_ii->sbi;

	if (!sbi->op->readdir)
		return -ENOTSUP;

	return sbi->op->readdir(app_ptr, zir);
}

static int _clone(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_clone *ioc_clone = (void *)hdr;
	struct zus_sb_info *sbi = ioc_clone->src_zus_ii->sbi;

	if (!sbi->op->clone)
		return -ENOTSUP;

	return sbi->op->clone(ioc_clone);
}

static int _io_read(ulong *app_ptr, struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_IO *io = (void *)hdr;
	struct zus_inode_info *zii = io->zus_ii;

	return zii->op->read(app_ptr, io);
}

static int _io_pre_read(ulong *app_ptr, struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_IO *io = (void *)hdr;
	struct zus_inode_info *zii = io->zus_ii;

	if (!zii->op->pre_read)
		return -ENOTSUP;

	return zii->op->pre_read(app_ptr, io);
}

static int _io_write(ulong *app_ptr, struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_IO *io = (void *)hdr;
	struct zus_inode_info *zii = io->zus_ii;

	return zii->op->write(app_ptr, io);
}

static int _get_put_block(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_IO *get_block = (void *)hdr;
	struct zus_inode_info *zii = get_block->zus_ii;

	if (hdr->operation == ZUFS_OP_PUT_BLOCK) {
		if (unlikely(!zii->op->put_block))
			return 0; /* Cool put is optional */
		return zii->op->put_block(zii, get_block);
	}

	if (unlikely(!zii->op->get_block)) {
		ERROR("No get_block operation set\n");
		return -EIO;
	}

	return	zii->op->get_block(zii, get_block);
}

static int _get_put_multy(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_IO *io = (void *)hdr;
	struct zus_inode_info *zii = io->zus_ii;

	if (unlikely(!zii->op->get_put_multy)) {
		ERROR("No get_put_multy operation set\n");
		return -EIO;
	}

	return	zii->op->get_put_multy(zii, io);
}

static int _mmap_close(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_mmap_close *mmap_close = (void *)hdr;
	struct zus_inode_info *zii = mmap_close->zus_ii;

	if (unlikely(!zii->op->mmap_close))
		return 0;

	return zii->op->mmap_close(zii, mmap_close);
}

static int _symlink(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_get_link *ioc_sym = (void *)hdr;
	struct zus_inode_info *zii = ioc_sym->zus_ii;
	void *sym;
	int err;

	err = zii->op->get_symlink(zii, &sym);
	if (unlikely(err))
		return err;

	if (sym)
		ioc_sym->_link = md_addr_to_offset(&zii->sbi->md, sym);
	return 0;
}

static int _setattr(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_attr *ioc_attr = (void *)hdr;
	struct zus_inode_info *zii = ioc_attr->zus_ii;

	if (!zii->op->setattr)
		return 0; /* This is fine no flushing needed */

	return zii->op->setattr(zii, ioc_attr->zuf_attr);
}

static int _sync(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_sync *ioc_range = (void *)hdr;
	struct zus_inode_info *zii = ioc_range->zus_ii;

	if (!zii->op->sync)
		return 0; /* This is fine sync not needed */

	return zii->op->sync(zii, ioc_range);
}

static int _fallocate(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_IO *ioc_IO = (void *)hdr;
	struct zus_inode_info *zii = ioc_IO->zus_ii;

	if (!zii->op->fallocate)
		return -ENOTSUP;

	return zii->op->fallocate(zii, ioc_IO);
}

static int _seek(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_seek *ioc_seek = (void *)hdr;
	struct zus_inode_info *zii = ioc_seek->zus_ii;

	if (!zii->op->seek)
		return -ENOTSUP;

	return zii->op->seek(zii, ioc_seek);
}

static int _ioc_ioctl(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_ioctl *ioc_ioctl = (void *)hdr;
	struct zus_inode_info *zii = ioc_ioctl->zus_ii;

	if (!zii->op->ioctl)
		return -ENOTTY;

	return zii->op->ioctl(zii, ioc_ioctl);
}

static int _ioc_xattr(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_xattr *ioc_xattr = (void *)hdr;
	struct zus_inode_info *zii = ioc_xattr->zus_ii;

	if (hdr->operation == ZUFS_OP_XATTR_GET) {
		if (!zii->op->getxattr)
			return -ENOTSUP;
		return zii->op->getxattr(zii, ioc_xattr);
	} else if (hdr->operation == ZUFS_OP_XATTR_SET) {
		if (!zii->op->setxattr)
			return -ENOTSUP;
		return zii->op->setxattr(zii, ioc_xattr);
	} else if (hdr->operation == ZUFS_OP_XATTR_LIST) {
		if (!zii->op->listxattr)
			return -ENOTSUP;
		return zii->op->listxattr(zii, ioc_xattr);
	}
	ERROR("Unknown xattr operation!\n");
	return -EFAULT;
}

static int _statfs(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_statfs *ioc_statfs = (void *)hdr;
	struct zus_sb_info *sbi = ioc_statfs->zus_sbi;

	if (!sbi->op->statfs)
		return -ENOTSUP;

	return sbi->op->statfs(sbi, ioc_statfs);
}

static int _fiemap(void *app_ptr, struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_fiemap *zif = (void *)hdr;
	struct zus_inode_info *zii = zif->zus_ii;

	if (!zii->op->fiemap)
		return -ENOTSUP;

	return zii->op->fiemap(app_ptr, zif);
}

static int _show_options(struct zufs_ioc_hdr *hdr)
{
	struct zufs_ioc_mount_options *ioc_mount_options = (void *)hdr;
	struct zus_sb_info *sbi = ioc_mount_options->zus_sbi;

	if (!sbi->op->show_options)
		return 0;

	return sbi->op->show_options(sbi, ioc_mount_options);
}

const char *ZUFS_OP_name(enum e_zufs_operation op)
{
#define CASE_ENUM_NAME(e) case e: return #e
	switch (op) {
		CASE_ENUM_NAME(ZUFS_OP_NULL);
		CASE_ENUM_NAME(ZUFS_OP_STATFS);
		CASE_ENUM_NAME(ZUFS_OP_SHOW_OPTIONS);
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
		CASE_ENUM_NAME(ZUFS_OP_FIEMAP);
		CASE_ENUM_NAME(ZUFS_OP_GET_MULTY);
		CASE_ENUM_NAME(ZUFS_OP_PUT_MULTY);
		CASE_ENUM_NAME(ZUFS_OP_NOOP);
		CASE_ENUM_NAME(ZUFS_OP_BREAK);
		CASE_ENUM_NAME(ZUFS_OP_MAX_OPT);
	default:
		return "UNKNOWN";
	}
}

static void _some_pigy_put(struct zufs_ioc_hdr *hdr)
{
	while (hdr->flags & ZUFS_H_HAS_PIGY_PUT) {
		/* Kernel made sure to update hdr->in_len including the
		 * iom_n. Kernel also checks bounds.
		 */
		hdr = (void *)hdr + hdr->in_len;

		if (unlikely(hdr->operation != ZUFS_OP_PUT_MULTY)) {
			ERROR("Not yet, easily support pigy ANY operation(%s)\n",
			      ZUFS_OP_name(hdr->operation));
			break;
		}
		_get_put_multy(hdr);
	}
}

int zus_do_command(void *app_ptr, struct zufs_ioc_hdr *hdr)
{
	DBG("[%s] OP=%d off=0x%x len=0x%x\n", ZUFS_OP_name(hdr->operation),
		hdr->operation, hdr->offset, hdr->len);

	if (hdr->flags & ZUFS_H_HAS_PIGY_PUT) {
		_some_pigy_put(hdr);
		hdr->flags &= ~ZUFS_H_HAS_PIGY_PUT;
	}

	switch (hdr->operation) {
	case ZUFS_OP_NEW_INODE:
		return _new_inode(app_ptr, hdr);
	case ZUFS_OP_FREE_INODE:
	case ZUFS_OP_EVICT_INODE:
		return _evict(hdr);
	case ZUFS_OP_LOOKUP:
		return _lookup(hdr);
	case ZUFS_OP_ADD_DENTRY:
	case ZUFS_OP_REMOVE_DENTRY:
		return _dentry(hdr);
	case ZUFS_OP_RENAME:
		return _rename(hdr);
	case ZUFS_OP_READDIR:
		return _readdir(app_ptr, hdr);
	case ZUFS_OP_CLONE:
	case ZUFS_OP_COPY:
		return _clone(hdr);
	case ZUFS_OP_READ:
		return _io_read(app_ptr, hdr);
	case ZUFS_OP_PRE_READ:
		return _io_pre_read(app_ptr, hdr);
	case ZUFS_OP_WRITE:
		return _io_write(app_ptr, hdr);
	case ZUFS_OP_GET_BLOCK:
	case ZUFS_OP_PUT_BLOCK:
		return _get_put_block(hdr);
	case ZUFS_OP_MMAP_CLOSE:
		return _mmap_close(hdr);
	case ZUFS_OP_GET_SYMLINK:
		return _symlink(hdr);
	case ZUFS_OP_SETATTR:
		return _setattr(hdr);
	case ZUFS_OP_SYNC:
		return _sync(hdr);
	case ZUFS_OP_FALLOCATE:
		return _fallocate(hdr);
	case ZUFS_OP_LLSEEK:
		return _seek(hdr);
	case ZUFS_OP_IOCTL:
		return _ioc_ioctl(hdr);
	case ZUFS_OP_XATTR_GET:
	case ZUFS_OP_XATTR_SET:
	case ZUFS_OP_XATTR_LIST:
		return _ioc_xattr(hdr);
	case ZUFS_OP_STATFS:
		return _statfs(hdr);
	case ZUFS_OP_FIEMAP:
		return _fiemap(app_ptr, hdr);
	case ZUFS_OP_SHOW_OPTIONS:
		return _show_options(hdr);

	case ZUFS_OP_GET_MULTY:
	case ZUFS_OP_PUT_MULTY:
		return _get_put_multy(hdr);

	case ZUFS_OP_NOOP:
	case ZUFS_OP_BREAK:
		break;
	default:
		ERROR("Unknown OP=%d\n", hdr->operation);
	}

	return 0;
}
