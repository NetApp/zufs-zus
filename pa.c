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

/* ~~~~ fba ~~~~ */

/*
 * Force fba allocations to be 2M aligned. We don't care for out-of-range pages
 * as they are never touched and therefore remains unallocated.
 */
#define FBA_ALIGNSIZE 	(ZUFS_ALLOC_MASK + 1)

int  fba_alloc(struct fba *fba, size_t size)
{
	int err;

	/* Our buffers are allocated from a tmpfile so all is aligned and easy
	 */
	fba->fd = open("/dev/shm/", O_RDWR | O_TMPFILE | O_EXCL, 0666);
	if (fba->fd < 0) {
		ERROR("Error opening <%s>: %s\n","/tmp/", strerror(errno));
		return errno;
	}

	err = ftruncate(fba->fd, size);
	if (unlikely(err)) {
		err = -errno;
		ERROR("ftruncate failed size=0x%lx => %d\n", size, err);
		close(fba->fd);
		return err;
	}

	fba->ptr = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED,
			fba->fd, 0);
	if (fba->ptr == MAP_FAILED) {
		ERROR("mmap failed=> %d: %s\n", errno, strerror(errno));
		fba_free(fba);
		return errno ?: ENOMEM;
	}

	fba->orig_ptr = fba->ptr;
	fba->size = size;

	return 0;
}
int  fba_alloc_align(struct fba *fba, size_t size)
{
	ulong addr;
	int err;

	size = ALIGN(size + FBA_ALIGNSIZE, FBA_ALIGNSIZE);
	err = fba_alloc(fba, size);
	if (unlikely(err))
		return err;

	addr = ALIGN((ulong)fba->ptr, FBA_ALIGNSIZE);
	if (fba->ptr != (void *)addr) {
		DBG("fba: fd=%d mmap-addr=0x%lx addr=0x%lx msize=0x%lx\n",
			fba->fd, (ulong)fba->ptr, addr, size);
		fba->ptr = (void *)addr;
	}
	return 0;
}

void fba_free(struct fba *fba)
{
	if (fba->fd >= 0) {
		munmap(fba->orig_ptr, fba->size);
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

/* PA_SIZE - 2GB  total allowed data held in pages */
/* TODO: get this param from FS
 * TODO2: grow dynamically
 */
#define PA_SIZE		(1UL << 31)

/* 2MB worth of pages (= 32k pages) */
#define PA_PAGES_AT_A_TIME ((1UL << 21) / sizeof(struct pa_page))

static void _init_one_page(struct zus_sb_info *sbi, struct pa *pa,
			   struct pa_page *page)
{
	a_list_init(&page->list);
	a_list_add(&page->list, &pa->head);
	pa_set_page_zone(page, POOL_NUM);
	page->owner = sbi;
}

static int _init_page_of_pages(struct zus_sb_info *sbi, struct pa *pa)
{
	struct pa_page *page;
	uint i;

	/* Better check here before we SIG_BUS on access of data */
	if (unlikely(PA_SIZE < pa->size * PAGE_SIZE)) {
		ERROR("Trouble in paradise PA_SIZE too small\n");
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

/* order - power of 2 of pages to allocate.
 * TODO: support order > 1
 */
struct pa_page *pa_alloc_order(struct zus_sb_info *sbi, int order)
{
	struct pa *pa = &sbi->pa[POOL_NUM];
	struct pa_page *page;

	if (ZUS_WARN_ON(1 < order))
		return NULL;

	pthread_spin_lock(&pa->lock);

	if (a_list_empty(&pa->head)) {
		int err = _init_page_of_pages(sbi, pa);

		if (unlikely(err)) {
			page = NULL;
			goto out;
		}
	}

	if (order == 1) {
		a_list_for_each_entry(page, &pa->head, list) {
			ulong bn = pa_page_to_bn(sbi, page);

			if (bn % 2)
				continue;
			if (_pa_is_free(page + 1)) {
				_alloc_one_page(page + 1);
				goto found;
			}
		}
		page = NULL;
		goto out;
	} else {
		page = a_list_first_entry(&pa->head, struct pa_page, list);
	}
found:
	_alloc_one_page(page);

out:
	pthread_spin_unlock(&pa->lock);

	return page;
}

#define ZUS_SBI_MASK	0x7
void __pa_free(struct pa_page *page)
{
	struct zus_sb_info *sbi = (void *)((ulong)page->owner & ~ZUS_SBI_MASK);
	struct pa *pa = &sbi->pa[POOL_NUM];

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

	pthread_spin_init(&pa->lock, PTHREAD_PROCESS_SHARED);

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

	fba_free(&pa->pages);
	fba_free(&pa->data);
	pthread_spin_destroy(&pa->lock);
}
