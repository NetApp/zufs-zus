/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zus.h - C Wrappers over the ZUFS_IOCTL Api
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 *	Sagi Manole <sagim@netapp.com>
 */

#include <pthread.h>

#include "zus.h"

/* TODO: get this param from FS... */
#define PA_SIZE		(1UL << 31)	/* 2GB */
//#define PA_NEW_ALLOC	(1UL << 27)	/* 128MB */
#define PA_PAGES_SIZE	(1UL << 21)	/* 2MB */

static void _init_one_page(struct zus_sb_info *sbi, struct pa *pa,
			   struct pa_page *page)
{
	a_list_init(&page->list);
	a_list_add(&page->list, &pa->head);
	pa_set_page_zone(page, POOL_NUM);
	page->owner = sbi;
}

struct pa_page *pa_alloc(struct zus_sb_info *sbi)
{
	struct pa *pa = &sbi->pa[POOL_NUM];
	struct pa_page *page;

	pthread_spin_lock(&pa->lock);

	if (a_list_empty(&pa->head)) {
		uint i;

		page = pa->pages.ptr + pa->size;
		for (i = 0; i < PA_PAGES_SIZE / sizeof(*page); ++i, ++page)
			_init_one_page(sbi, pa, page);
		pa->size += PA_PAGES_SIZE;
	}
	page = a_list_first_entry(&pa->head, struct pa_page, list);
	a_list_del_init(&page->list);

	page->refcount = 1;

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
