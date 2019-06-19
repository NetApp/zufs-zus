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
#include <sys/mman.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "zus.h"
#include "toyfs.h"

#define DBG_(fmt, ...)

#define TOYFS_IMAGIC			(0x11E11F5)

#define TOYFS_INODES_PER_PAGE	(PAGE_SIZE / sizeof(struct toyfs_inode))
#define TOYFS_DBLKREFS_PER_PAGE	(PAGE_SIZE / sizeof(struct toyfs_dblkref))
#define TOYFS_IBLKREFS_PER_PAGE	(PAGE_SIZE / sizeof(struct toyfs_iblkref))
#define TOYFS_DIRENTS_PER_PAGE	(PAGE_SIZE / sizeof(struct toyfs_dirent))


union toyfs_pool_pmemb {
	struct toyfs_pmemb pmemb;
	union toyfs_pool_pmemb *next;
};

union toyfs_inodes_pmemb {
	struct toyfs_pmemb pmemb;
	struct toyfs_inode inodes[TOYFS_INODES_PER_PAGE];
};

union toyfs_dblkrefs_pmemb {
	struct toyfs_pmemb pmemb;
	struct toyfs_dblkref dblkrefs[TOYFS_DBLKREFS_PER_PAGE];
};

union toyfs_iblkrefs_pmemb {
	struct toyfs_pmemb pmemb;
	struct toyfs_iblkref iblkrefs[TOYFS_IBLKREFS_PER_PAGE];
};

union toyfs_dirents_pmemb {
	struct toyfs_pmemb pmemb;
	struct toyfs_dirent dirents[TOYFS_DIRENTS_PER_PAGE];
};


size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr)
{
	size_t offset;
	struct multi_devices *md = &sbi->s_zus_sbi.md;

	offset = md_addr_to_offset(md, ptr);
	return md_o2p(offset);
}

void *toyfs_bn2addr(struct toyfs_sb_info *sbi, size_t bn)
{
	struct multi_devices *md = &sbi->s_zus_sbi.md;

	return md_baddr(md, bn);
}

struct toyfs_pmemb *toyfs_bn2pmemb(struct toyfs_sb_info *sbi, size_t bn)
{
	return (struct toyfs_pmemb *)toyfs_bn2addr(sbi, bn);
}

zu_dpp_t toyfs_page2dpp(struct toyfs_sb_info *sbi, struct toyfs_pmemb *page)
{
	struct multi_devices *md = &sbi->s_zus_sbi.md;

	return pmem_dpp_t(md_addr_to_offset(md, page));
}

struct toyfs_pmemb *toyfs_dpp2pmemb(struct toyfs_sb_info *sbi, zu_dpp_t dpp)
{
	return toyfs_bn2pmemb(sbi, md_o2p(dpp));
}

void toyfs_sbi_lock(struct toyfs_sb_info *sbi)
{
	toyfs_mutex_lock(&sbi->s_mutex);
}

void toyfs_sbi_unlock(struct toyfs_sb_info *sbi)
{
	toyfs_mutex_unlock(&sbi->s_mutex);
}

void toyfs_lock_inodes(struct toyfs_sb_info *sbi)
{
	toyfs_mutex_lock(&sbi->s_inodes_lock);
}

void toyfs_unlock_inodes(struct toyfs_sb_info *sbi)
{
	toyfs_mutex_unlock(&sbi->s_inodes_lock);
}

struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi)
{
	struct toyfs_inode_info *tii = NULL;

	/* TODO: Distinguish between user types */
	toyfs_sbi_lock(sbi);
	if (!sbi->s_statvfs.f_ffree || !sbi->s_statvfs.f_favail)
		goto out;

	tii = (struct toyfs_inode_info *)zus_malloc(sizeof(*tii));
	if (!tii)
		goto out;

	memset(tii, 0, sizeof(*tii));
	tii->imagic = TOYFS_IMAGIC;
	tii->ref = 0;
	tii->valid = true;
	tii->sbi = sbi;
	tii->zii.op = &toyfs_zii_op;
	tii->zii.sbi = &sbi->s_zus_sbi;

	sbi->s_statvfs.f_ffree--;
	sbi->s_statvfs.f_favail--;

	DBG("alloc_ii tii=%p files=%lu ffree=%lu\n", tii,
	    sbi->s_statvfs.f_files, sbi->s_statvfs.f_ffree);

out:
	toyfs_sbi_unlock(sbi);
	return tii;
}

struct zus_inode_info *toyfs_zii_alloc(struct zus_sb_info *zsbi)
{
	struct toyfs_inode_info *tii;

	tii = toyfs_alloc_ii(Z2SBI(zsbi));
	return tii ? &tii->zii : NULL;
}

void toyfs_tii_free(struct toyfs_inode_info *tii)
{
	struct toyfs_sb_info *sbi = tii->sbi;

	toyfs_assert(tii->sbi != NULL);
	DBG("free_ii tii=%p files=%lu ffree=%lu\n", tii,
	    sbi->s_statvfs.f_files, sbi->s_statvfs.f_ffree);

	memset(tii, 0xAB, sizeof(*tii));
	tii->zii.op = NULL;
	tii->ti = NULL;
	tii->sbi = NULL;
	tii->valid = false;
	zus_free(tii);
	sbi->s_statvfs.f_ffree++;
	sbi->s_statvfs.f_favail++;
}

static void _pool_init(struct toyfs_pool *pool)
{
	pool->mem = NULL;
	pool->msz = 0;
	pool->pages = NULL;
	toyfs_list_init(&pool->free_dblkrefs);
	toyfs_list_init(&pool->free_iblkrefs);
	toyfs_list_init(&pool->free_inodes);
	toyfs_mutex_init(&pool->mutex);
}

static void _pool_setup(struct toyfs_pool *pool, void *mem, size_t msz)
{
	size_t npages, pagei;
	union toyfs_pool_pmemb *page, *next;
	union toyfs_pool_pmemb *pages_arr = mem;

	page = next = NULL;
	npages = msz / sizeof(*page);
	for (pagei = 0; pagei < npages; ++pagei) {
		page = &pages_arr[pagei];
		page->next = next;
		next = page;
	}
	pool->mem = mem;
	pool->msz = msz;
	pool->pages = page;
}

static void _pool_destroy(struct toyfs_pool *pool)
{
	pool->mem = NULL;
	pool->msz = 0;
	pool->pages = NULL;
	toyfs_mutex_destroy(&pool->mutex);
}

static void _pool_lock(struct toyfs_pool *pool)
{
	toyfs_mutex_lock(&pool->mutex);
}

static void _pool_unlock(struct toyfs_pool *pool)
{
	toyfs_mutex_unlock(&pool->mutex);
}

static struct toyfs_pmemb *_pool_pop_pmemb_without_lock(struct toyfs_pool *pool)
{
	struct toyfs_pmemb *pmemb = NULL;
	union toyfs_pool_pmemb *pp;

	if (pool->pages) {
		pp = pool->pages;
		pool->pages = pp->next;
		pp->next = NULL;
		pmemb = &pp->pmemb;
	}
	return pmemb;
}

static struct toyfs_pmemb *_pool_pop_pmemb(struct toyfs_pool *pool)
{
	struct toyfs_pmemb *pmemb;

	_pool_lock(pool);
	pmemb = _pool_pop_pmemb_without_lock(pool);
	_pool_unlock(pool);
	return pmemb;
}

static void _pool_push_pmemb(struct toyfs_pool *pool, struct toyfs_pmemb *pmemb)
{
	union toyfs_pool_pmemb *pp;

	_pool_lock(pool);
	pp = container_of(pmemb, union toyfs_pool_pmemb, pmemb);
	pp->next = pool->pages;
	pool->pages = pp;
	_pool_unlock(pool);
}

static struct toyfs_list_head *_inode_to_list_head(struct toyfs_inode *ti)
{
	return &ti->list_head;
}

static int _pool_add_free_inodes(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_pmemb *pmemb;
	union toyfs_inodes_pmemb *ipmb;
	struct toyfs_list_head *list_head;

	pmemb = _pool_pop_pmemb_without_lock(pool);
	if (!pmemb)
		return -ENOMEM;

	ipmb = (union toyfs_inodes_pmemb *)pmemb;
	for (i = 0; i < ARRAY_SIZE(ipmb->inodes); ++i) {
		list_head = _inode_to_list_head(&ipmb->inodes[i]);
		toyfs_list_add(list_head, &pool->free_inodes);
	}

	return 0;
}

static struct toyfs_inode *_list_head_to_inode(struct toyfs_list_head *head)
{
	return container_of(head, struct toyfs_inode, list_head);
}

static struct toyfs_inode *_pool_pop_free_inode(struct toyfs_pool *pool)
{
	struct toyfs_inode *ti = NULL;

	if (!toyfs_list_empty(&pool->free_inodes)) {
		ti = _list_head_to_inode(pool->free_inodes.next);
		toyfs_list_del(pool->free_inodes.next);
	}
	return ti;
}

static struct toyfs_inode *_pool_pop_inode(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_inode *ti;

	_pool_lock(pool);
	ti = _pool_pop_free_inode(pool);
	if (ti)
		goto out;

	err = _pool_add_free_inodes(pool);
	if (err)
		goto out;

	ti = _pool_pop_free_inode(pool);
out:
	_pool_unlock(pool);
	return ti;
}

static void _pool_push_inode(struct toyfs_pool *pool, struct toyfs_inode *inode)
{
	struct toyfs_list_head *list_head;

	memset(inode, 0, sizeof(*inode));
	list_head = _inode_to_list_head(inode);

	_pool_lock(pool);
	toyfs_list_add_tail(list_head, &pool->free_inodes);
	_pool_unlock(pool);
}

struct toyfs_inode *toyfs_acquire_inode(struct toyfs_sb_info *sbi)
{
	return _pool_pop_inode(&sbi->s_pool);
}

void toyfs_release_inode(struct toyfs_sb_info *sbi, struct toyfs_inode *inode)
{
	_pool_push_inode(&sbi->s_pool, inode);
}

static int _pool_add_free_dblkrefs(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_pmemb *pmemb;
	union toyfs_dblkrefs_pmemb *pp;
	struct toyfs_dblkref *dblkref;

	pmemb = _pool_pop_pmemb_without_lock(pool);
	if (!pmemb)
		return -ENOMEM;

	pp = (union toyfs_dblkrefs_pmemb *)pmemb;
	for (i = 0; i < ARRAY_SIZE(pp->dblkrefs); ++i) {
		dblkref = &pp->dblkrefs[i];
		toyfs_list_add_tail(&dblkref->head, &pool->free_dblkrefs);
	}
	return 0;
}

static struct toyfs_dblkref *_pool_pop_free_dblkref(struct toyfs_pool *pool)
{
	struct toyfs_list_head *elem;
	struct toyfs_dblkref *dblkref = NULL;

	if (!toyfs_list_empty(&pool->free_dblkrefs)) {
		elem = pool->free_dblkrefs.next;
		toyfs_list_del(elem);
		dblkref = container_of(elem, struct toyfs_dblkref, head);
	}
	return dblkref;
}

static struct toyfs_dblkref *_pool_pop_dblkref(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_dblkref *dblkref;

	_pool_lock(pool);
	dblkref = _pool_pop_free_dblkref(pool);
	if (!dblkref) {
		err = _pool_add_free_dblkrefs(pool);
		if (!err)
			dblkref = _pool_pop_free_dblkref(pool);
	}
	_pool_unlock(pool);
	return dblkref;
}

static void _pool_push_dblkref(struct toyfs_pool *pool,
			       struct toyfs_dblkref *dblkref)
{
	_pool_lock(pool);
	toyfs_list_add(&dblkref->head, &pool->free_dblkrefs);
	_pool_unlock(pool);
}

static int _pool_add_free_iblkrefs(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_pmemb *pmemb;
	union toyfs_iblkrefs_pmemb *pp;
	struct toyfs_iblkref *iblkref;

	pmemb = _pool_pop_pmemb_without_lock(pool);
	if (!pmemb)
		return -ENOMEM;

	pp = (union toyfs_iblkrefs_pmemb *)pmemb;
	for (i = 0; i < ARRAY_SIZE(pp->iblkrefs); ++i) {
		iblkref = &pp->iblkrefs[i];
		toyfs_list_add_tail(&iblkref->head, &pool->free_iblkrefs);
	}
	return 0;
}

static struct toyfs_iblkref *_pool_pop_free_iblkref(struct toyfs_pool *pool)
{
	struct toyfs_list_head *elem;
	struct toyfs_iblkref *iblkref = NULL;

	if (!toyfs_list_empty(&pool->free_iblkrefs)) {
		elem = pool->free_iblkrefs.next;
		toyfs_list_del(elem);
		iblkref = container_of(elem, struct toyfs_iblkref, head);
	}
	return iblkref;
}

static struct toyfs_iblkref *_pool_pop_iblkref(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_iblkref *iblkref;

	_pool_lock(pool);
	iblkref = _pool_pop_free_iblkref(pool);
	if (!iblkref) {
		err = _pool_add_free_iblkrefs(pool);
		if (!err)
			iblkref = _pool_pop_free_iblkref(pool);
	}
	_pool_unlock(pool);
	return iblkref;
}

static void _pool_push_iblkref(struct toyfs_pool *pool,
			       struct toyfs_iblkref *iblkref)
{
	_pool_lock(pool);
	toyfs_list_add(&iblkref->head, &pool->free_iblkrefs);
	_pool_unlock(pool);
}

static void _itable_init(struct toyfs_itable *itable)
{
	itable->icount = 0;
	memset(itable->imap, 0x00, sizeof(itable->imap));
	toyfs_mutex_init(&itable->mutex);
}

static void _itable_destroy(struct toyfs_itable *itable)
{
	itable->icount = 0;
	memset(itable->imap, 0xff, sizeof(itable->imap));
	toyfs_mutex_destroy(&itable->mutex);
}

static void _itable_lock(struct toyfs_itable *itable)
{
	toyfs_mutex_lock(&itable->mutex);
}

static void _itable_unlock(struct toyfs_itable *itable)
{
	toyfs_mutex_unlock(&itable->mutex);
}

static size_t _itable_slot_of(const struct toyfs_itable *itable, ino_t ino)
{
	return ino % ARRAY_SIZE(itable->imap);
}

static struct toyfs_inode_ref *
_itable_find(struct toyfs_itable *itable, ino_t ino)
{
	size_t slot;
	struct toyfs_inode_ref *tir;

	_itable_lock(itable);
	slot = _itable_slot_of(itable, ino);
	tir = itable->imap[slot];
	while (tir != NULL) {
		if (tir->ino == ino)
			break;
		tir = tir->next;
	}
	_itable_unlock(itable);
	return tir;
}

static void _itable_insert(struct toyfs_itable *itable,
			   struct toyfs_inode_info *tii)
{
	size_t slot;
	struct toyfs_inode_ref **ient;
	struct toyfs_inode_ref *tir;

	tir = (struct toyfs_inode_ref *)zus_calloc(1, sizeof(*tir));
	toyfs_assert(tir != NULL);

	_itable_lock(itable);
	tir->tii = tii;
	tir->ti = tii->ti;
	tir->ino = tii->ino;

	slot = _itable_slot_of(itable, tii->ino);
	ient = &itable->imap[slot];
	tir->next = *ient;
	*ient = tir;
	itable->icount++;
	_itable_unlock(itable);
}

static void _itable_remove(struct toyfs_itable *itable,
			   struct toyfs_inode_info *tii)
{
	size_t slot;
	struct toyfs_inode_ref **pp, *tir = NULL;

	_itable_lock(itable);
	toyfs_assert(itable->icount > 0);
	slot = _itable_slot_of(itable, tii->ino);
	pp = &itable->imap[slot];
	toyfs_assert(*pp != NULL);
	while ((tir = *pp) != NULL) {
		if (tir->tii == tii)
			break;
		pp = &(*pp)->next;
	}
	toyfs_assert(tir != NULL);
	if (!tir) { /* Make clang-scan happy */
		_itable_unlock(itable);
		return;
	}
	*pp = tir->next;
	itable->icount--;
	_itable_unlock(itable);

	memset(tir, 0, sizeof(*tir));
	zus_free(tir);
}

void toyfs_i_track(struct toyfs_inode_info *tii)
{
	struct toyfs_sb_info *sbi = tii->sbi;

	_itable_insert(&sbi->s_itable, tii);
	tii->mapped = true;
}

void toyfs_i_untrack(struct toyfs_inode_info *tii, bool remove)
{
	struct toyfs_inode_ref *tir;
	struct toyfs_sb_info *sbi = tii->sbi;

	tir = _itable_find(&sbi->s_itable, tii->ino);
	toyfs_assert(tir != NULL);

	tii->mapped = false;
	if (remove)
		_itable_remove(&sbi->s_itable, tii);
	else
		tir->tii = NULL;
}

struct toyfs_inode_ref *
toyfs_find_inode_ref_by_ino(struct toyfs_sb_info *sbi, ino_t ino)
{
	return _itable_find(&sbi->s_itable, ino);
}

struct zus_sb_info *toyfs_sbi_alloc(struct zus_fs_info *zfi)
{
	struct toyfs_sb_info *sbi;

	INFO("sbi_alloc: zfi=%p\n", zfi);

	sbi = (struct toyfs_sb_info *)zus_malloc(sizeof(*sbi));
	if (!sbi)
		return NULL;

	memset(sbi, 0, sizeof(*sbi));
	toyfs_mutex_init(&sbi->s_mutex);
	toyfs_mutex_init(&sbi->s_inodes_lock);
	_pool_init(&sbi->s_pool);
	_itable_init(&sbi->s_itable);
	sbi->s_zus_sbi.op = &toyfs_sbi_op;
	return &sbi->s_zus_sbi;
}

void toyfs_sbi_free(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	INFO("sbi_free: sbi=%p\n", sbi);
	zus_free(sbi);
}

struct toyfs_pmemb *toyfs_acquire_pmemb(struct toyfs_sb_info *sbi)
{
	struct toyfs_pmemb *pmemb = NULL;

	/* TODO: Distinguish between user types */
	toyfs_sbi_lock(sbi);
	if (!sbi->s_statvfs.f_bfree)
		goto out;
	if (!sbi->s_statvfs.f_bavail)
		goto out;
	pmemb = _pool_pop_pmemb(&sbi->s_pool);
	if (!pmemb)
		goto out;

	memset(pmemb, 0x00, sizeof(*pmemb));
	sbi->s_statvfs.f_bfree--;
	sbi->s_statvfs.f_bavail--;
	DBG_("alloc_page: blocks=%lu bfree=%lu pmem_bn=%lu\n",
	     sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
	     toyfs_addr2bn(sbi, pmemb));
out:
	toyfs_sbi_unlock(sbi);
	return pmemb;
}

void toyfs_release_pmemb(struct toyfs_sb_info *sbi, struct toyfs_pmemb *pmemb)
{
	toyfs_sbi_lock(sbi);
	_pool_push_pmemb(&sbi->s_pool, pmemb);
	sbi->s_statvfs.f_bfree++;
	sbi->s_statvfs.f_bavail++;
	DBG_("free_page: blocks=%lu bfree=%lu pmem_bn=%lu\n",
	     sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
	     toyfs_addr2bn(sbi, pmemb));
	toyfs_sbi_unlock(sbi);
}

struct toyfs_dblkref *toyfs_acquire_dblkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_dblkref *dblkref;

	dblkref = _pool_pop_dblkref(&sbi->s_pool);
	if (dblkref) {
		dblkref->refcnt = 0;
		dblkref->bn = 0;
	}
	return dblkref;
}

void toyfs_release_dblkref(struct toyfs_sb_info *sbi,
			   struct toyfs_dblkref *dblkref)
{
	dblkref->bn = 0;
	_pool_push_dblkref(&sbi->s_pool, dblkref);
}

struct toyfs_iblkref *toyfs_acquire_iblkref(struct toyfs_sb_info *sbi)
{
	struct toyfs_iblkref *iblkref;

	iblkref = _pool_pop_iblkref(&sbi->s_pool);
	if (iblkref) {
		iblkref->off = -1;
		iblkref->dblkref = NULL;
	}
	return iblkref;
}

void toyfs_release_iblkref(struct toyfs_sb_info *sbi,
			   struct toyfs_iblkref *iblkref)
{
	iblkref->dblkref = NULL;
	iblkref->off = -1;
	_pool_push_iblkref(&sbi->s_pool, iblkref);
}

static void _sbi_setup(struct toyfs_sb_info *sbi)
{
	/* TODO: FIXME */
	const size_t fssize = sbi->s_pool.msz;
	const size_t fssize_blocks = fssize / PAGE_SIZE;

	sbi->s_top_ino = TOYFS_ROOT_INO + 1;
	sbi->s_statvfs.f_bsize = PAGE_SIZE;
	sbi->s_statvfs.f_frsize = PAGE_SIZE;
	sbi->s_statvfs.f_blocks = fssize / PAGE_SIZE;
	sbi->s_statvfs.f_bfree = fssize_blocks;
	sbi->s_statvfs.f_bavail = fssize_blocks;
	sbi->s_statvfs.f_files = fssize_blocks;
	sbi->s_statvfs.f_ffree = fssize_blocks;
	sbi->s_statvfs.f_favail = fssize_blocks;
	sbi->s_statvfs.f_namemax = ZUFS_NAME_LEN;
}

static int _new_root_inode(struct toyfs_sb_info *sbi,
			   struct toyfs_inode_info **out_ii)
{
	struct toyfs_inode *root_ti;
	struct toyfs_inode_info *root_tii;

	root_tii = toyfs_alloc_ii(sbi);
	if (!root_tii)
		return -ENOMEM;

	root_ti = _pool_pop_inode(&sbi->s_pool);
	if (!root_ti) {
		toyfs_tii_free(root_tii);
		return -ENOSPC;
	}

	memset(root_ti, 0, sizeof(*root_ti));
	root_tii->ti = root_ti;
	root_tii->zii.zi = toyfs_ti2zi(root_ti);
	root_tii->ino = TOYFS_ROOT_INO;

	root_ti = root_tii->ti;
	root_ti->i_ino = TOYFS_ROOT_INO;
	root_ti->i_mode = 0755 | S_IFDIR;
	root_ti->i_nlink = 2;
	root_ti->i_uid = 0;
	root_ti->i_gid = 0;
	root_ti->i_generation = 0;
	root_ti->i_rdev = 0;
	root_ti->i_size = 0;
	root_ti->i_blocks = 0;
	toyfs_list_init(toyfs_childs_list_of(root_tii));

	_itable_insert(&sbi->s_itable, root_tii);
	*out_ii = root_tii;
	return 0;
}


static int _read_pmem_sb_first_time(struct multi_devices *md)
{
	void *pmem_addr = md->p_pmem_addr;
	const struct toyfs_super_block *sb;

	sb = (const struct toyfs_super_block *)pmem_addr;
	if (sb->head.dev_table.s_magic != TOYFS_SUPER_MAGIC) {
		ERROR("illegal magic1: %ld\n",
		      (long)sb->head.dev_table.s_magic);
		return -EINVAL;
	}
	return 0;
}

static void _read_pmem_first_time(struct multi_devices *md)
{
	size_t i, pmem_total_blocks, pmem_total_size;
	void *pmem_addr = md->p_pmem_addr;
	char *ptr;
	char buf[1024];
	const size_t buf_size = sizeof(buf);

	pmem_total_blocks = md_t1_blocks(md);
	pmem_total_size = md_p2o(pmem_total_blocks);
	ptr = (char *)pmem_addr;
	for (i = 0; i < pmem_total_size; i += buf_size) {
		memcpy(buf, ptr, buf_size);
		ptr += buf_size;
	}
}

static void _write_pmem_first_time(struct multi_devices *md)
{
	size_t i, head_size, pmem_total_blocks, pmem_total_size;
	void *pmem_addr = md->p_pmem_addr;
	char *ptr;
	char buf[1024];
	const size_t buf_size = sizeof(buf);

	pmem_total_blocks = md_t1_blocks(md);
	pmem_total_size = md_p2o(pmem_total_blocks);

	head_size = (2 * PAGE_SIZE);
	ptr = (char *)pmem_addr + head_size;
	for (i = head_size; i < pmem_total_size; i += buf_size) {
		memset(buf, i, buf_size);
		memcpy(ptr, buf, buf_size);
		ptr += buf_size;
	}
}

static int _prepare_pmem_first_time(struct multi_devices *md)
{
	int err;

	err = _read_pmem_sb_first_time(md);
	if (err)
		return err;

	_read_pmem_first_time(md);
	err = _read_pmem_sb_first_time(md);
	if (err)
		return err;

	_write_pmem_first_time(md);
	err = _read_pmem_sb_first_time(md);
	if (err)
		return err;

	return 0;
}

static int _sbi_init(struct toyfs_sb_info *sbi)
{
	int err;
	void *mem = NULL;
	size_t pmem_total_blocks, msz = 0;
	uint32_t pmem_kernel_id;

	INFO("sbi_init: sbi=%p\n", sbi);
	pmem_kernel_id = sbi->s_zus_sbi.md.pmem_info.pmem_kern_id;
	if (!pmem_kernel_id) {
		ERROR("pmem_kernel_id=%ld\n", (long)pmem_kernel_id);
		return -EINVAL;
	}
	pmem_total_blocks = md_t1_blocks(&sbi->s_zus_sbi.md);
	if (pmem_total_blocks < 1024) {
		ERROR("pmem_total_blocks=%ld\n", (long)pmem_total_blocks);
		return -EINVAL;
	}
	err = _prepare_pmem_first_time(&sbi->s_zus_sbi.md);
	if (err)
		return err;

	msz = md_p2o(pmem_total_blocks - 2);
	mem = md_baddr(&sbi->s_zus_sbi.md, 2);
	_pool_setup(&sbi->s_pool, mem, msz);
	_sbi_setup(sbi);

	/* TODO: Take root inode from super */
	err = _new_root_inode(sbi, &sbi->s_root);
	if (err)
		return err;

	sbi->s_zus_sbi.z_root = &sbi->s_root->zii;
	return 0;
}

int toyfs_sbi_init(struct zus_sb_info *zsbi, struct zufs_mount_info *zmi)
{
	int err;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	err = _sbi_init(sbi);
	if (err)
		return err;

	zmi->zus_sbi = &sbi->s_zus_sbi;
	zmi->zus_ii = sbi->s_zus_sbi.z_root;
	zmi->s_blocksize_bits = PAGE_SHIFT;
	zmi->acl_on = 1;

	return 0;
}

int toyfs_sbi_fini(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	INFO("sbi_fini: sbi=%p\n", sbi);

	_pool_destroy(&sbi->s_pool);
	_itable_destroy(&sbi->s_itable);
	toyfs_mutex_destroy(&sbi->s_mutex);
	toyfs_mutex_destroy(&sbi->s_inodes_lock);
	sbi->s_root = NULL;
	return 0;
}

int toyfs_statfs(struct zus_sb_info *zsbi, struct zufs_ioc_statfs *ioc_statfs)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);
	struct statfs64 *out = &ioc_statfs->statfs_out;
	const struct statvfs *stvfs = &sbi->s_statvfs;

	DBG("statfs sbi=%p\n", sbi);

	toyfs_sbi_lock(sbi);
	out->f_bsize = stvfs->f_bsize;
	out->f_blocks = stvfs->f_blocks;
	out->f_bfree = stvfs->f_bfree;
	out->f_bavail = stvfs->f_bavail;
	out->f_files = stvfs->f_files;
	out->f_ffree = stvfs->f_ffree;
	out->f_namelen = stvfs->f_namemax;
	out->f_frsize = stvfs->f_frsize;
	out->f_flags = stvfs->f_flag;
	toyfs_sbi_unlock(sbi);

	DBG("statfs: bsize=%ld blocks=%ld bfree=%ld bavail=%ld "
	    "files=%ld ffree=%ld\n", (long)out->f_bsize,
	    (long)out->f_blocks, (long)out->f_bfree, (long)out->f_bavail,
	    (long)out->f_files, (long)out->f_ffree);

	return 0;
}

int toyfs_sync(struct zus_inode_info *zii, struct zufs_ioc_range *ioc_range)
{
	struct toyfs_inode_info *tii = Z2II(zii);

	DBG("sync: ino=%lu offset=%lu length=%lu opflags=%u\n",
	    tii->ino, (size_t)ioc_range->offset,
	    (size_t)ioc_range->length, ioc_range->opflags);

	/* TODO: CL_FLUSH for relevant pages */
	return 0;
}


#define BUILD_BUG_ON_EQ(a, b) TOYFS_STATICASSERT_EQ(a, b)

#define BUILD_BUG_ON_SIZEOFTYPE(t, n) \
	BUILD_BUG_ON_EQ(sizeof(t), (n))

#define BUILD_BUG_ON_SIZEOFPAGE(t) \
	BUILD_BUG_ON_SIZEOFTYPE(t, PAGE_SIZE)

#define BUILD_BUG_ON_OFFSET(a, b, mem) \
	BUILD_BUG_ON_EQ(offsetof(typeof(*a), mem), offsetof(typeof(*b), mem))

#define BUILD_BUG_ON_SIZE(a, b, mem) \
	BUILD_BUG_ON_EQ(sizeof(a->mem), sizeof(b->mem))

/* Compile time checks only */
static void _check_inode(void)
{
	const struct toyfs_inode *ti = NULL;
	const struct zus_inode *zi = NULL;

	BUILD_BUG_ON_EQ(sizeof(*ti), sizeof(*zi));

	BUILD_BUG_ON_SIZE(ti, zi, i_flags);
	BUILD_BUG_ON_SIZE(ti, zi, i_mode);
	BUILD_BUG_ON_SIZE(ti, zi, i_nlink);
	BUILD_BUG_ON_SIZE(ti, zi, i_size);
	BUILD_BUG_ON_SIZE(ti, zi, i_blocks);
	BUILD_BUG_ON_SIZE(ti, zi, i_mtime);
	BUILD_BUG_ON_SIZE(ti, zi, i_ctime);
	BUILD_BUG_ON_SIZE(ti, zi, i_atime);
	BUILD_BUG_ON_SIZE(ti, zi, i_ino);
	BUILD_BUG_ON_SIZE(ti, zi, i_uid);
	BUILD_BUG_ON_SIZE(ti, zi, i_gid);
	BUILD_BUG_ON_SIZE(ti, zi, i_xattr);
	BUILD_BUG_ON_SIZE(ti, zi, i_symlink);

	BUILD_BUG_ON_OFFSET(ti, zi, i_flags);
	BUILD_BUG_ON_OFFSET(ti, zi, i_mode);
	BUILD_BUG_ON_OFFSET(ti, zi, i_nlink);
	BUILD_BUG_ON_OFFSET(ti, zi, i_size);
	BUILD_BUG_ON_OFFSET(ti, zi, i_mtime);
	BUILD_BUG_ON_OFFSET(ti, zi, i_ctime);
	BUILD_BUG_ON_OFFSET(ti, zi, i_atime);
	BUILD_BUG_ON_OFFSET(ti, zi, i_ino);
	BUILD_BUG_ON_OFFSET(ti, zi, i_uid);
	BUILD_BUG_ON_OFFSET(ti, zi, i_gid);
	BUILD_BUG_ON_OFFSET(ti, zi, i_xattr);
	BUILD_BUG_ON_OFFSET(ti, zi, i_generation);
	BUILD_BUG_ON_OFFSET(ti, zi, i_rdev);
	BUILD_BUG_ON_OFFSET(ti, zi, i_symlink);
}

static void _check_typesizes(void)
{
	BUILD_BUG_ON_SIZEOFTYPE(struct toyfs_pmemb, PAGE_SIZE);
	BUILD_BUG_ON_SIZEOFTYPE(struct toyfs_dirent, 32);

	BUILD_BUG_ON_SIZEOFPAGE(union toyfs_pool_pmemb);
	BUILD_BUG_ON_SIZEOFPAGE(union toyfs_inodes_pmemb);
	BUILD_BUG_ON_SIZEOFPAGE(union toyfs_iblkrefs_pmemb);
	BUILD_BUG_ON_SIZEOFPAGE(union toyfs_dirents_pmemb);
	BUILD_BUG_ON_SIZEOFPAGE(struct toyfs_xattr);
	BUILD_BUG_ON_SIZEOFPAGE(struct toyfs_dentries);
}

void toyfs_check_types(void)
{
	_check_inode();
	_check_typesizes();
}

struct zus_inode *toyfs_ti2zi(struct toyfs_inode *ti)
{
	void *p = (void *)ti;
	struct zus_inode *zi = p;

	TOYFS_STATICASSERT_EQ(sizeof(*zi), sizeof(*ti));
	return zi;
}

