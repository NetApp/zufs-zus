/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * pa.c - Page Allocator
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 *	Sagi Manole <sagim@netapp.com>
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/falloc.h>

#include "zus.h"
#include "zuf_call.h"

/* PA_SIZE - allowed data held in pages; 4G by default, setup upon zusd init */
/* TODO: get this param from FS
 * TODO2: grow dynamically
 */
#define PA_SIZE		(g_pa_size.pa_size)
#define MEGA		(1UL << 20)

union _pa_size {
	const size_t pa_size;
	size_t __pa_size_wr;
};
static union _pa_size g_pa_size = { .pa_size = 1L << 32, };	/* 4GB */

int zus_setup_pa_size(size_t size)
{
	const char *env_pa_size;
	size_t avail_ram;
	long p_size, pages;

	/* user-defined size */
	if (size)
		goto out;

	env_pa_size = getenv(ZUFS_PA_SIZE);
	if (env_pa_size) {
		size = atol(env_pa_size);
		goto out;
	}

	/* defines PA_SIZE to be half of physical RAM  */
	p_size = sysconf(_SC_PAGE_SIZE);
	if (unlikely(p_size == -1))
		return -errno;
	pages = sysconf(_SC_PHYS_PAGES);
	if (unlikely(pages == -1))
		return -errno;

	avail_ram = p_size * pages;
	if (unlikely(avail_ram < (64 * MEGA)))
		return -ENOMEM;
	size = avail_ram / 2;

out:
	/* require PA_SIZE to be 2M aligned */
	g_pa_size.__pa_size_wr = size & ~((2 * MEGA) - 1);
	return 0;
}

/* ~~~~ fba ~~~~ */

/*
 * Force fba allocations to be 2M aligned. We don't care for out-of-range pages
 * as they are never touched and therefore remains unallocated.
 */
#define FBA_ALIGNSIZE	(ZUFS_ALLOC_MASK + 1)

static int _fba_alloc(struct fba *fba, size_t size, int flags)
{
	int err;

	/* Our buffers are allocated from a tmpfile so all is aligned and easy
	 */
	fba->fd = open("/dev/shm/", O_RDWR | O_TMPFILE | O_EXCL, 0666);
	if (fba->fd < 0) {
		if (!(flags & MAP_HUGETLB))
			ERROR("Error opening <%s>: %s\n","/tmp/",
			      strerror(errno));
		return errno ? -errno : -EPERM;
	}

	err = ftruncate(fba->fd, size);
	if (unlikely(err)) {
		err = -errno;
		if (!(flags & MAP_HUGETLB))
			ERROR("ftruncate failed size=0x%lx => %d\n", size, err);
		close(fba->fd);
		return err;
	}

	fba->ptr = mmap(NULL, size, PROT_WRITE | PROT_READ, flags,
			fba->fd, 0);
	if (fba->ptr == MAP_FAILED) {
		if (!(flags & MAP_HUGETLB))
			ERROR("mmap failed=> %d: %s\n", errno, strerror(errno));
		fba_free(fba);
		return errno ? -errno: -ENOMEM;
	}

	err = madvise(fba->ptr, size, MADV_DONTDUMP);
	if (err == -1 && !(flags & MAP_HUGETLB))
		ERROR("madvise(DONTDUMP) failed=> %d: %s\n", errno,
		      strerror(errno));

	fba->size = size;

	DBG("fba allocated flags=0x%x fd=%d ptr=%p size=0x%lx\n", flags,
	    fba->fd, fba->ptr, size);

	return 0;
}

int fba_alloc(struct fba *fba, size_t size)
{
	int err = _fba_alloc(fba, size, MAP_SHARED);

	if (err)
		return err;

	if (NEED_MLOCK && size < PA_SIZE) {
		err = mlock(fba->ptr, size);
		ZUS_WARN_ON(err);
	}
	return err;
}

static int _fba_alloc_huge(struct fba *fba, size_t size)
{
	int err;

	err = _fba_alloc(fba, size, MAP_ANONYMOUS | MAP_SHARED | MAP_HUGETLB);
	if (unlikely(err)) {
		/* fallback to 4k pages */
		INFO("mmap failed huge=> %d: %s\n", errno, strerror(errno));
		return _fba_alloc(fba, size, MAP_SHARED);
	}

	return 0;
}

int fba_alloc_align(struct fba *fba, size_t size, bool huge)
{
	size_t aligned_size;
	ulong addr;
	int err;

	aligned_size = ALIGN(size + FBA_ALIGNSIZE, FBA_ALIGNSIZE);

	if (huge)
		err = _fba_alloc_huge(fba, aligned_size);
	else
		err = _fba_alloc(fba, aligned_size, MAP_SHARED);

	if (unlikely(err))
		return err;

	addr = ALIGN((ulong)fba->ptr, FBA_ALIGNSIZE);
	if (fba->ptr != (void *)addr) {
		size_t start_len, end_len;

		DBG("fba: fd=%d mmap-addr=0x%lx addr=0x%lx msize=0x%lx aligned_size=0x%lx\n",
		    fba->fd, (ulong)fba->ptr, addr, size, aligned_size);

		/* unmap the unaligned edges and fix the ptr and size */
		start_len = addr - (ulong)fba->ptr;
		end_len = ((ulong)fba->ptr + aligned_size) - (addr + size) -
								start_len;

		munmap(fba->ptr, start_len);
		munmap((void *)(addr + size), end_len);

		fba->ptr = (void *)addr;
		fba->size = size;
	}

	if (NEED_MLOCK && size < PA_SIZE) {
		err = mlock(fba->ptr, size);
		ZUS_WARN_ON(err);
	}

	return 0;
}

void fba_free(struct fba *fba)
{
	if (fba->fd >= 0) {
		munmap(fba->ptr, fba->size);
		close(fba->fd);
		fba->fd = -1;
	}
}

int fba_punch_hole(struct fba *fba, ulong index, uint nump)
{
	int ret = fallocate(fba->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			    md_p2o(index), md_p2o(nump));

	if (unlikely(ret))
		return -errno;
	return 0;
}

/* ~~~ pa - Page Allocator ~~~ */

/* 2MB worth of pages (= 32k pages) */
#define PA_PAGES_AT_A_TIME ((1UL << 21) / sizeof(struct pa_page))

static void _init_one_page(struct zus_sb_info *sbi, struct pa *pa,
			   struct pa_page *page)
{
	a_list_init(&page->list);
	a_list_add_tail(&page->list, &pa->head);
	pa_set_page_zone(page, POOL_NUM);
	page->owner = sbi;
}

static int _init_page_of_pages(struct zus_sb_info *sbi, struct pa *pa)
{
	struct pa_page *page;
	uint i;

	/* Better check here before we SIG_BUS on access of data */
	if (unlikely(PA_SIZE < ((pa->size + PA_PAGES_AT_A_TIME) * PAGE_SIZE))) {
		DBG("PA_SIZE too small pa->size=0x%lx\n", pa->size);
		return -ENOMEM;
	}

	page = pa->pages.ptr + pa->size * sizeof(*page);
	for (i = 0; i < PA_PAGES_AT_A_TIME; ++i, ++page)
		_init_one_page(sbi, pa, page);

	pa->size += PA_PAGES_AT_A_TIME;
	return 0;
}

static void _alloc_one_page(struct pa_page *page)
{
	a_list_del_init(&page->list);
	page->refcount = 1;
}

static bool _pa_is_free(struct pa_page *page)
{
	return (page->refcount == 0);
}

/* order - power of 2 of pages to allocate */
struct pa_page *pa_alloc_order(struct zus_sb_info *sbi, int order)
{
	struct pa *pa = &sbi->pa[POOL_NUM];
	struct pa_page *page;
	ushort npages = 1 << order;
	int err, i = 0;

	if (ZUS_WARN_ON(PA_MAX_ORDER < order))
		return NULL;

	pthread_spin_lock(&pa->lock);

	if (a_list_empty(&pa->head)) {
		err = _init_page_of_pages(sbi, pa);
		if (unlikely(err)) {
			page = NULL;
			goto out;
		}
	}

rescan:
	a_list_for_each_entry(page, &pa->head, list) {
		ulong bn = pa_page_to_bn(sbi, page);

		if ((bn % npages) || (bn + npages - 1 > pa->size))
			continue;

		for (i = 1; i < npages; ++i) {
			if (!_pa_is_free(page + i))
				break;
		}
		if (i == npages) {
			for (i = 0; i < npages; ++i)
				_alloc_one_page(page + i);
			goto out;
		}
	}
	page = NULL;
	err = _init_page_of_pages(sbi, pa);
	if (unlikely(err))
		goto out;
	goto rescan;

out:
	if (NEED_MLOCK && page) {
		err = mlock(pa_page_address(sbi, page), npages * PAGE_SIZE);

		if (unlikely(err))
			DBG("mlock failed pa=%p npages=%d => %d\n",
			    pa_page_address(sbi, page), (int)npages, -errno);
	}

	pthread_spin_unlock(&pa->lock);

	return page;
}

#define ZUS_SBI_MASK	0x7
void __pa_free(struct pa_page *page)
{
	struct zus_sb_info *sbi = (void *)((ulong)page->owner & ~ZUS_SBI_MASK);
	struct pa *pa = &sbi->pa[POOL_NUM];

	if (NEED_MLOCK) {
		int err = munlock(pa_page_address(sbi, page), PAGE_SIZE);

		if (unlikely(err))
			DBG("munlock failed pa=%p => %d\n",
			    pa_page_address(sbi, page), -errno);
	}
	fba_punch_hole(&pa->data, pa_page_to_bn(sbi, page), 1);

	pthread_spin_lock(&pa->lock);

	a_list_add(&page->list, &pa->head);

	pthread_spin_unlock(&pa->lock);
}

int pa_init(struct zus_sb_info *sbi)
{
	struct pa *pa = &sbi->pa[POOL_NUM];
	int err;

	pa->size = 0;
	a_list_init(&pa->head);

	err = pthread_spin_init(&pa->lock, PTHREAD_PROCESS_SHARED);
	if (unlikely(err))
		goto fail;

	err = fba_alloc(&pa->data, PA_SIZE);
	if (unlikely(err))
		goto fail;

	err = fba_alloc(&pa->pages, (PA_SIZE / PAGE_SIZE) *
						sizeof(struct pa_page));
	if (unlikely(err))
		goto fail;

	return 0;

fail:
	pa_fini(sbi);
	return err;
}

void pa_fini(struct zus_sb_info *sbi)
{
	struct pa *pa = &sbi->pa[POOL_NUM];
	struct pa_page *page;
	ulong free_p = 0;

	a_list_for_each_entry(page, &pa->head, list) {
		++free_p;
	}
	if (unlikely(free_p != pa->size))
		ERROR("pa leaks %lu pages\n", pa->size - free_p);

	fba_free(&pa->pages);
	fba_free(&pa->data);
	pthread_spin_destroy(&pa->lock);
}
