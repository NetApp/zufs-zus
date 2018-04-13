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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#include "_pr.h"
#include "list.h"
#include "zus.h"
#include "toyfs.h"

#define DBG_(fmt, ...)

#define TOYFS_IMAGIC		(0x11E11F5)
#define TOYFS_STATICASSERT(expr)	_Static_assert(expr, #expr)
#define TOYFS_STATICASSERT_EQ(a, b)	TOYFS_STATICASSERT(a == b)
#define TOYFS_STATICASSERT_SIZEOFPAGE(t) \
	TOYFS_STATICASSERT_EQ(sizeof(t), PAGE_SIZE)


#define TOYFS_INODES_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(union toyfs_inode_head))

#define TOYFS_DBLKREFS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_dblkref))

#define TOYFS_IBLKREFS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_iblkref))

#define TOYFS_DIRENTS_PER_PAGE \
	(sizeof(struct toyfs_page) / sizeof(struct toyfs_dirent))

union toyfs_pool_page {
	struct toyfs_page page;
	union toyfs_pool_page *next;
};

union toyfs_inodes_page {
	struct toyfs_page page;
	union toyfs_inode_head inodes[TOYFS_INODES_PER_PAGE];
};

union toyfs_dblkrefs_page {
	struct toyfs_page page;
	struct toyfs_dblkref dblkrefs[TOYFS_DBLKREFS_PER_PAGE];
};

union toyfs_iblkrefs_page {
	struct toyfs_page page;
	struct toyfs_iblkref iblkrefs[TOYFS_IBLKREFS_PER_PAGE];
};

union toyfs_dirents_page {
	struct toyfs_page page;
	struct toyfs_dirent dirents[TOYFS_DIRENTS_PER_PAGE];
};


size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr)
{
	size_t offset;
	struct zus_pmem *pmem = &sbi->s_zus_sbi.pmem;

	offset = pmem_addr_2_offset(pmem, ptr);
	return pmem_o2p(offset);
}

void *toyfs_bn2addr(struct toyfs_sb_info *sbi, size_t bn)
{
	struct zus_pmem *pmem = &sbi->s_zus_sbi.pmem;

	return pmem_baddr(pmem, bn);
}

struct toyfs_page *toyfs_bn2page(struct toyfs_sb_info *sbi, size_t bn)
{
	return (struct toyfs_page *)toyfs_bn2addr(sbi, bn);
}




void toyfs_sbi_lock(struct toyfs_sb_info *sbi)
{
	toyfs_mutex_lock(&sbi->s_mutex);
}

void toyfs_sbi_unlock(struct toyfs_sb_info *sbi)
{
	toyfs_mutex_unlock(&sbi->s_mutex);
}

struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi)
{
	struct toyfs_inode_info *tii = NULL;

	/* TODO: Distinguish between user types */
	toyfs_sbi_lock(sbi);
	if (!sbi->s_statvfs.f_ffree || !sbi->s_statvfs.f_favail)
		goto out;

	tii = (struct toyfs_inode_info *)malloc(sizeof(*tii));
	if (!tii)
		goto out;

	memset(tii, 0, sizeof(*tii));
	tii->imagic = TOYFS_IMAGIC;
	tii->next = NULL;
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

void toyfs_zii_free(struct zus_inode_info *zii)
{
	struct toyfs_inode_info *tii = Z2II(zii);
	struct toyfs_sb_info *sbi = tii->sbi;

	toyfs_sbi_lock(sbi);
	memset(tii, 0xAB, sizeof(*tii));
	tii->zii.op = NULL;
	tii->sbi = NULL;
	free(tii);
	sbi->s_statvfs.f_ffree++;
	sbi->s_statvfs.f_favail++;
	DBG("free_ii tii=%p files=%lu ffree=%lu\n", tii,
	    sbi->s_statvfs.f_files, sbi->s_statvfs.f_ffree);
	toyfs_sbi_unlock(sbi);
}

static int _mmap_memory(size_t msz, void **pp)
{
	int err = 0;
	void *mem;
	const int prot = PROT_WRITE | PROT_READ;
	const int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (msz < PAGE_SIZE)
		return -EINVAL;

	mem = mmap(NULL, msz, prot, flags, -1, 0);
	if (mem != MAP_FAILED) {
		*pp = mem;
		INFO("mmap ok: %p\n", mem);
	} else {
		err = -errno;
		ERROR("mmap failed: %d\n", err);
	}
	return err;
}

static void _munmap_memory(void *mem, size_t msz)
{
	if (mem) {
		INFO("munmap %p %lu\n", mem, msz);
		munmap(mem, msz);
	}
}

static void _pool_init(struct toyfs_pool *pool)
{
	pool->mem = NULL;
	pool->msz = 0;
	pool->pages = NULL;
	list_init(&pool->free_dblkrefs);
	list_init(&pool->free_iblkrefs);
	list_init(&pool->free_dirents);
	list_init(&pool->free_inodes);
	toyfs_mutex_init(&pool->mutex);
}

static void
_pool_setup(struct toyfs_pool *pool, void *mem, size_t msz, bool pmem)
{
	size_t npages, pagei;
	union toyfs_pool_page *page, *next;
	union toyfs_pool_page *pages_arr = mem;

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
	pool->pmem = pmem;
}

static void _pool_destroy(struct toyfs_pool *pool)
{
	if (pool->mem && !pool->pmem)
		_munmap_memory(pool->mem, pool->msz);
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

static struct toyfs_page *_pool_pop_page_without_lock(struct toyfs_pool *pool)
{
	struct toyfs_page *page = NULL;
	union toyfs_pool_page *ppage;

	if (pool->pages) {
		ppage = pool->pages;
		pool->pages = ppage->next;
		ppage->next = NULL;
		page = &ppage->page;
	}
	return page;
}

static struct toyfs_page *_pool_pop_page(struct toyfs_pool *pool)
{
	struct toyfs_page *page;

	_pool_lock(pool);
	page = _pool_pop_page_without_lock(pool);
	_pool_unlock(pool);
	return page;
}

static void _pool_push_page(struct toyfs_pool *pool, struct toyfs_page *page)
{
	union toyfs_pool_page *ppage;

	_pool_lock(pool);
	ppage = container_of(page, union toyfs_pool_page, page);
	ppage->next = pool->pages;
	pool->pages = ppage;
	_pool_unlock(pool);
}

static int _pool_add_free_inodes(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_inodes_page *ipage;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	ipage = (union toyfs_inodes_page *)page;
	for (i = 0; i < ARRAY_SIZE(ipage->inodes); ++i)
		list_add(&ipage->inodes[i].head, &pool->free_inodes);

	return 0;
}

static struct toyfs_inode *_list_head_to_inode(struct list_head *head)
{
	union toyfs_inode_head *ihead;

	ihead = container_of(head, union toyfs_inode_head, head);
	return &ihead->inode;
}

static struct toyfs_inode *_pool_pop_free_inode(struct toyfs_pool *pool)
{
	struct toyfs_inode *ti = NULL;

	if (!list_empty(&pool->free_inodes)) {
		ti = _list_head_to_inode(pool->free_inodes.next);
		list_del(pool->free_inodes.next);
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
	union toyfs_inode_head *ihead;

	ihead = container_of(inode, union toyfs_inode_head, inode);
	memset(ihead, 0, sizeof(*ihead));

	_pool_lock(pool);
	list_add_tail(&ihead->head, &pool->free_inodes);
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

static int _pool_add_free_dirents(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_dirents_page *dpage;
	struct toyfs_dirent *dirent;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	dpage = (union toyfs_dirents_page *)page;
	for (i = 0; i < ARRAY_SIZE(dpage->dirents); ++i) {
		dirent = &dpage->dirents[i];
		list_add_tail(&dirent->d_head, &pool->free_dirents);
	}
	return 0;
}

static struct toyfs_dirent *_pool_pop_free_dirent(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_dirent *dirent = NULL;

	if (!list_empty(&pool->free_dirents)) {
		elem = pool->free_dirents.next;
		list_del(elem);
		dirent = container_of(elem, struct toyfs_dirent, d_head);
	}
	return dirent;
}

static struct toyfs_dirent *_pool_pop_dirent(struct toyfs_pool *pool)
{
	int err;
	struct toyfs_dirent *dirent;

	_pool_lock(pool);
	dirent = _pool_pop_free_dirent(pool);
	if (!dirent) {
		err = _pool_add_free_dirents(pool);
		if (!err)
			dirent = _pool_pop_free_dirent(pool);
	}
	_pool_unlock(pool);
	return dirent;
}

static void _pool_push_dirent(struct toyfs_pool *pool,
			      struct toyfs_dirent *dirent)
{
	_pool_lock(pool);
	list_add_tail(&dirent->d_head, &pool->free_dirents);
	_pool_unlock(pool);
}

static int _pool_add_free_dblkrefs(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_dblkrefs_page *ppage;
	struct toyfs_dblkref *dblkref;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	ppage = (union toyfs_dblkrefs_page *)page;
	for (i = 0; i < ARRAY_SIZE(ppage->dblkrefs); ++i) {
		dblkref = &ppage->dblkrefs[i];
		list_add_tail(&dblkref->head, &pool->free_dblkrefs);
	}
	return 0;
}

static struct toyfs_dblkref *_pool_pop_free_dblkref(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_dblkref *dblkref = NULL;

	if (!list_empty(&pool->free_dblkrefs)) {
		elem = pool->free_dblkrefs.next;
		list_del(elem);
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
	list_add(&dblkref->head, &pool->free_dblkrefs);
	_pool_unlock(pool);
}

static int _pool_add_free_iblkrefs(struct toyfs_pool *pool)
{
	size_t i;
	struct toyfs_page *page;
	union toyfs_iblkrefs_page *bpage;
	struct toyfs_iblkref *iblkref;

	page = _pool_pop_page_without_lock(pool);
	if (!page)
		return -ENOMEM;

	bpage = (union toyfs_iblkrefs_page *)page;
	for (i = 0; i < ARRAY_SIZE(bpage->iblkrefs); ++i) {
		iblkref = &bpage->iblkrefs[i];
		list_add_tail(&iblkref->head, &pool->free_iblkrefs);
	}
	return 0;
}

static struct toyfs_iblkref *_pool_pop_free_iblkref(struct toyfs_pool *pool)
{
	struct list_head *elem;
	struct toyfs_iblkref *iblkref = NULL;

	if (!list_empty(&pool->free_iblkrefs)) {
		elem = pool->free_iblkrefs.next;
		list_del(elem);
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
	list_add(&iblkref->head, &pool->free_iblkrefs);
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

static struct toyfs_inode_info *
_itable_find(struct toyfs_itable *itable, ino_t ino)
{
	size_t slot;
	struct toyfs_inode_info *tii;

	_itable_lock(itable);
	slot = _itable_slot_of(itable, ino);
	tii = itable->imap[slot];
	while (tii != NULL) {
		if (tii->ino == ino)
			break;
		tii = tii->next;
	}
	_itable_unlock(itable);
	return tii;
}

static void _itable_insert(struct toyfs_itable *itable,
			   struct toyfs_inode_info *tii)
{
	size_t slot;
	struct toyfs_inode_info **ient;

	toyfs_assert(tii->ti);
	toyfs_assert(tii->sbi);
	toyfs_assert(!tii->next);

	_itable_lock(itable);
	slot = _itable_slot_of(itable, tii->ino);
	ient = &itable->imap[slot];
	tii->next = *ient;
	*ient = tii;
	itable->icount++;
	_itable_unlock(itable);
}

static void _itable_remove(struct toyfs_itable *itable,
			   struct toyfs_inode_info *tii)
{
	size_t slot;
	struct toyfs_inode_info **ient;

	_itable_lock(itable);
	toyfs_assert(itable->icount > 0);
	slot = _itable_slot_of(itable, tii->ino);
	ient = &itable->imap[slot];
	toyfs_assert(*ient != NULL);
	while (*ient) {
		if (*ient == tii)
			break;
		ient = &(*ient)->next;
	}
	toyfs_assert(*ient != NULL);
	*ient = tii->next;
	itable->icount--;
	_itable_unlock(itable);

	tii->next = NULL;
}

void toyfs_track_inode(struct toyfs_inode_info *tii)
{
	struct toyfs_sb_info *sbi = tii->sbi;

	_itable_insert(&sbi->s_itable, tii);
}

void toyfs_untrack_inode(struct toyfs_inode_info *tii)
{
	struct toyfs_sb_info *sbi = tii->sbi;

	_itable_remove(&sbi->s_itable, tii);
}

struct toyfs_inode_info *toyfs_find_inode(struct toyfs_sb_info *sbi, ino_t ino)
{
	return _itable_find(&sbi->s_itable, ino);
}

struct zus_sb_info *toyfs_sbi_alloc(struct zus_fs_info *zfi)
{
	struct toyfs_sb_info *sbi;

	INFO("sbi_alloc: zfi=%p\n", zfi);

	sbi = (struct toyfs_sb_info *)malloc(sizeof(*sbi));
	if (!sbi)
		return NULL;

	memset(sbi, 0, sizeof(*sbi));
	toyfs_mutex_init(&sbi->s_mutex);
	_pool_init(&sbi->s_pool);
	_itable_init(&sbi->s_itable);
	sbi->s_zus_sbi.op = &toyfs_sbi_op;
	sbi->s_zus_sbi.pmem.user_page_size = PAGE_SIZE;
	return &sbi->s_zus_sbi;
}

void toyfs_sbi_free(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	INFO("sbi_free: sbi=%p\n", sbi);
	free(sbi);
}

struct toyfs_page *toyfs_acquire_page(struct toyfs_sb_info *sbi)
{
	struct toyfs_page *page = NULL;

	/* TODO: Distinguish between user types */
	toyfs_sbi_lock(sbi);
	if (!sbi->s_statvfs.f_bfree)
		goto out;
	if (!sbi->s_statvfs.f_bavail)
		goto out;
	page = _pool_pop_page(&sbi->s_pool);
	if (!page)
		goto out;

	memset(page, 0x00, sizeof(*page));
	sbi->s_statvfs.f_bfree--;
	sbi->s_statvfs.f_bavail--;
	DBG_("alloc_page: blocks=%lu bfree=%lu pmem_bn=%lu\n",
	    sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
	    toyfs_addr2bn(sbi, page));
out:
	toyfs_sbi_unlock(sbi);
	return page;
}

void toyfs_release_page(struct toyfs_sb_info *sbi, struct toyfs_page *page)
{
	toyfs_sbi_lock(sbi);
	_pool_push_page(&sbi->s_pool, page);
	sbi->s_statvfs.f_bfree++;
	sbi->s_statvfs.f_bavail++;
	DBG_("free_page: blocks=%lu bfree=%lu pmem_bn=%lu\n",
	    sbi->s_statvfs.f_blocks, sbi->s_statvfs.f_bfree,
	    toyfs_addr2bn(sbi, page));
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

struct toyfs_dirent *toyfs_acquire_dirent(struct toyfs_sb_info *sbi)
{
	return _pool_pop_dirent(&sbi->s_pool);
}

void toyfs_release_dirent(struct toyfs_sb_info *sbi,
			  struct toyfs_dirent *dirent)
{
	return _pool_push_dirent(&sbi->s_pool, dirent);
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
	if (!root_ti)
		return -ENOSPC;

	memset(root_ti, 0, sizeof(*root_ti));
	root_tii->ti = root_ti;
	root_tii->zii.zi = &root_ti->zi;
	root_tii->ino = TOYFS_ROOT_INO;

	root_ti = root_tii->ti;
	root_ti->zi.i_ino = TOYFS_ROOT_INO;
	root_ti->zi.i_mode = 0755 | S_IFDIR;
	root_ti->zi.i_nlink = 2;
	root_ti->zi.i_uid = 0;
	root_ti->zi.i_gid = 0;
	root_ti->zi.i_generation = 0;
	root_ti->zi.i_rdev = 0;
	root_ti->zi.i_size = 0;
	/* TODO: FIXME
	 * root_i->ti_zi.i_blksize = PAGE_SIZE;
	 * root_i->ti_zi.i_blocks = 0;
	 */
	root_ti->i_parent_ino = TOYFS_ROOT_INO;
	root_ti->ti.dir.d_ndentry = 0;
	root_ti->ti.dir.d_off_max = 2;
	list_init(&root_ti->ti.dir.d_childs);

	_itable_insert(&sbi->s_itable, root_tii);
	*out_ii = root_tii;
	return 0;
}


static int _read_pmem_sb_first_time(struct zus_pmem *pmem)
{
	void *pmem_addr = pmem->p_pmem_addr;
	const struct toyfs_super_block *sb;

	sb = (const struct toyfs_super_block *)pmem_addr;
	if (sb->part1.dev_table.s_magic != TOYFS_SUPER_MAGIC) {
		ERROR("illegal magic1: %ld\n",
		      (long)sb->part1.dev_table.s_magic);
		return -EINVAL;
	}
	if (sb->part2.dev_table.s_magic != TOYFS_SUPER_MAGIC) {
		ERROR("illegal magic2: %ld\n",
		      (long)sb->part2.dev_table.s_magic);
		return -EINVAL;
	}
	return 0;
}

static void _read_pmem_first_time(struct zus_pmem *pmem)
{
	size_t i, pmem_total_blocks, pmem_total_size;
	void *pmem_addr = pmem->p_pmem_addr;
	char *ptr;
	char buf[1024];
	const size_t buf_size = sizeof(buf);

	pmem_total_blocks = pmem_blocks(pmem);
	pmem_total_size = pmem_p2o(pmem_total_blocks);
	ptr = (char *)pmem_addr;
	for (i = 0; i < pmem_total_size; i += buf_size) {
		memcpy(buf, ptr, buf_size);
		ptr += buf_size;
	}
}

static void _write_pmem_first_time(struct zus_pmem *pmem)
{
	size_t i, head_size, pmem_total_blocks, pmem_total_size;
	void *pmem_addr = pmem->p_pmem_addr;
	char *ptr;
	char buf[1024];
	const size_t buf_size = sizeof(buf);

	pmem_total_blocks = pmem_blocks(pmem);
	pmem_total_size = pmem_p2o(pmem_total_blocks);

	head_size = (2 * PAGE_SIZE);
	ptr = (char *)pmem_addr + head_size;
	for (i = head_size; i < pmem_total_size; i += buf_size) {
		memset(buf, i, buf_size);
		memcpy(ptr, buf, buf_size);
		ptr += buf_size;
	}
}

static int _prepare_pmem_first_time(struct zus_pmem *pmem)
{
	int err;

	err = _read_pmem_sb_first_time(pmem);
	if (err)
		return err;

	_read_pmem_first_time(pmem);
	err = _read_pmem_sb_first_time(pmem);
	if (err)
		return err;

	_write_pmem_first_time(pmem);
	err = _read_pmem_sb_first_time(pmem);
	if (err)
		return err;

	return 0;
}

int toyfs_sbi_init(struct zus_sb_info *zsbi, struct zufs_ioc_mount *zim)
{
	int err;
	void *mem = NULL;
	size_t pmem_total_blocks, msz = 0;
	uint32_t pmem_kernel_id;
	bool using_pmem = false;
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	INFO("sbi_init: sbi=%p\n", sbi);

	pmem_kernel_id = sbi->s_zus_sbi.pmem.pmem_info.pmem_kern_id;
	pmem_total_blocks = pmem_blocks(&sbi->s_zus_sbi.pmem);
	if ((pmem_kernel_id > 0) && (pmem_total_blocks > 2)) {
		err = _prepare_pmem_first_time(&sbi->s_zus_sbi.pmem);
		if (err)
			return err;

		msz = pmem_p2o(pmem_total_blocks - 2);
		mem = pmem_baddr(&sbi->s_zus_sbi.pmem, 2);
		using_pmem = true;
	} else {
		msz = (1ULL << 30) /* 1G */;
		err = _mmap_memory(msz, &mem);
		if (err)
			return err;
	}

	_pool_setup(&sbi->s_pool, mem, msz, using_pmem);
	_sbi_setup(sbi);

	/* TODO: Take root inode from super */

	err = _new_root_inode(sbi, &sbi->s_root);
	if (err)
		return err;

	sbi->s_zus_sbi.z_root = &sbi->s_root->zii;
	zim->zus_sbi = &sbi->s_zus_sbi;
	zim->zus_ii = sbi->s_zus_sbi.z_root;
	zim->s_blocksize_bits  = PAGE_SHIFT;

	return 0;
}

int toyfs_sbi_fini(struct zus_sb_info *zsbi)
{
	struct toyfs_sb_info *sbi = Z2SBI(zsbi);

	INFO("sbi_fini: sbi=%p\n", sbi);

	_pool_destroy(&sbi->s_pool);
	_itable_destroy(&sbi->s_itable);
	toyfs_mutex_destroy(&sbi->s_mutex);
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


/* Compile time checks only */
void toyfs_check_types(void)
{
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_pool_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_inodes_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_iblkrefs_page);
	TOYFS_STATICASSERT_SIZEOFPAGE(union toyfs_dirents_page);
}
