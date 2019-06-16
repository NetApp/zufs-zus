/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * slab.c -- Slab-based allocator utility
 *
 * Copyright (c) 2019 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *      Shachar Sharon <sshachar@netapp.com>
 */
#define _GNU_SOURCE

#include <sys/sysinfo.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include "zus.h"
#include "_pr.h"

/* TODO: Maybe in zus.h ? */
#ifndef ZUS_BUILD_BUG_ON
#define ZUS_BUILD_BUG_ON(expr)	_Static_assert(!(expr), #expr)
#endif
#ifndef ZUS_ARRAY_SIZE
#define ZUS_ARRAY_SIZE(x_)	(sizeof(x_) / sizeof(x_[0]))
#endif

#define ZUS_BLOCK_SIZE          PAGE_SIZE
#define ZUS_MIN_SLAB_SHIFT      5
#define ZUS_MIN_SLAB_SIZE       (1 << ZUS_MIN_SLAB_SHIFT) /* 32 Bytes */
#define ZUS_MAX_SLABS_PER_BLOCK (ZUS_BLOCK_SIZE / ZUS_MIN_SLAB_SIZE)
#define ZUS_SLAB_LISTS          (PAGE_SHIFT - ZUS_MIN_SLAB_SHIFT + 1)
#define ZUS_SLAB_NFREE_WANT	1024 /* default number of wanted elements */

/* ~~~~~ SLAB allocator ~~~~~ */

struct zus_slab {
	struct zus_sb_info *sbi;
	struct zus_slab_list {
		struct a_list_head head;
		int nused; /* number elements currently owned by user */
		int nfree; /* number of elements currently in free-list */
		int nfree_want; /* threshold of min wanted elements in list */
		int _pad;
	} list[ZUS_SLAB_LISTS];
	int cpu;
	pthread_spinlock_t lock;
} __aligned(64);

struct zus_slab_elem {
	struct a_list_head list;
} __attribute__((aligned(ZUS_MIN_SLAB_SIZE)));


static void _slab_lock(struct zus_slab *slab)
{
	int err = pthread_spin_lock(&slab->lock);

	ZUS_WARN_ON(err);
}

static bool _slab_trylock(struct zus_slab *slab)
{
	int err = pthread_spin_trylock(&slab->lock);

	return (err == 0);
}

static void _slab_unlock(struct zus_slab *slab)
{
	int err = pthread_spin_unlock(&slab->lock);

	ZUS_WARN_ON(err);
}

static void _page_set_slab(struct pa_page *page, int slab_index, int cpu)
{
	page->units = slab_index + 1;
	page->sinfo.slab_cpu = cpu;
	page->sinfo.slab_uc = 0;
}

static void _page_clear_slab(struct pa_page *page)
{
	page->units = 0;
	page->sinfo.slab_cpu = 0;
	page->sinfo.slab_uc = 0;
}

static int _page_slab_index(const struct pa_page *page)
{
	return page->units - 1;
}

static int _page_slab_cpu(const struct pa_page *page)
{
	return page->sinfo.slab_cpu;
}

static void _page_slab_uc_inc(struct pa_page *page)
{
	page->sinfo.slab_uc++;
}

static int _page_slab_uc_dec(struct pa_page *page)
{
	return --page->sinfo.slab_uc;
}

/* ~~~~~ SLAB init ~~~~~ */

static void _slab_init(struct zus_slab *slab, struct zus_sb_info *sbi, int cpu)
{
	size_t i;

	ZUS_BUILD_BUG_ON(sizeof(struct zus_slab_elem) != ZUS_MIN_SLAB_SIZE);

	slab->sbi = sbi;
	slab->cpu = cpu;
	for (i = 0; i < ZUS_ARRAY_SIZE(slab->list); i++) {
		a_list_init(&slab->list[i].head);
		slab->list[i].nused = 0;
		slab->list[i].nfree = 0;
		slab->list[i].nfree_want = ZUS_SLAB_NFREE_WANT;
	}
}

static struct zus_slab_elem *
_slab_alloc_elem(struct zus_slab *slab, int slab_index)
{
	struct zus_slab_elem *se;
	struct zus_slab_list *slab_list = &slab->list[slab_index];

	if (a_list_empty(&slab_list->head))
		return NULL;

	se = a_list_first_entry(&slab_list->head, struct zus_slab_elem, list);
	a_list_del_init(&se->list);
	--slab_list->nfree;
	++slab_list->nused;

	return se;
}

static void _slab_page_init(struct zus_slab *slab,
			    struct pa_page *page, int slab_index)
{
	struct zus_slab_list *slab_list = &slab->list[slab_index];
	int slabs_count = ZUS_MAX_SLABS_PER_BLOCK >> slab_index;
	int i, step = 1 << slab_index;
	struct zus_slab_elem *se;

	_page_set_slab(page, slab_index, slab->cpu);

	se = pa_page_address(slab->sbi, page);
	for (i = 0; i < slabs_count; i++) {
		a_list_add_tail(&se->list, &slab_list->head);
		++slab_list->nfree;
		se += step;
	}
}

static void _slab_page_fini(struct zus_slab *slab,
			    struct pa_page *page, int slab_index)
{
	struct zus_slab_list *slab_list = &slab->list[slab_index];
	int slabs_count = ZUS_MAX_SLABS_PER_BLOCK >> slab_index;
	int i, step = 1 << slab_index;
	struct zus_slab_elem *se;

	se = pa_page_address(slab->sbi, page);
	for (i = 0; i < slabs_count; i++) {
		a_list_del_init(&se->list);
		--slab_list->nfree;
		se += step;
	}
	_page_clear_slab(page);
}

/* ~~~~~ SLAB alloc ~~~~~ */

static int _slab_list_index(size_t size)
{
	int slab_index;

	if (unlikely(!size || (ZUS_BLOCK_SIZE < size)))
		return -EINVAL;

	if (unlikely(size <= ZUS_MIN_SLAB_SIZE))
		return 0;

	slab_index = (32 - (__builtin_clz((size - 1) >> ZUS_MIN_SLAB_SHIFT)));
	if (unlikely(ZUS_SLAB_LISTS <= slab_index))
		return -EINVAL;

	return slab_index;
}

static int _slab_check_list_index(int slab_index)
{
	return likely((slab_index >= 0) &&
		      (slab_index < ZUS_SLAB_LISTS)) ? 0 : -EINVAL;
}

static int _slab_increase(struct zus_slab *slab, int slab_index)
{
	struct pa_page *page;

	page = pa_alloc(slab->sbi);
	if (unlikely(!page))
		return -ENOMEM;

	_slab_page_init(slab, page, slab_index);
	return 0;
}

static bool _slab_iscold(const struct zus_slab *slab, size_t size)
{
	int slab_index = _slab_list_index(size);
	const struct zus_slab_list *slab_list;

	if (unlikely(_slab_check_list_index(slab_index)))
		return NULL;

	slab_list = &slab->list[slab_index];
	return !slab_list->nfree && !slab_list->nused;
}

static bool _slab_list_empty(struct zus_slab *slab, size_t size)
{
	int slab_index = _slab_list_index(size);
	struct zus_slab_list *slab_list;

	if (unlikely(_slab_check_list_index(slab_index)))
		return true;

	slab_list = &slab->list[slab_index];
	if (!slab_list->nfree) {
		ZUS_WARN_ON(!a_list_empty(&slab_list->head));
		return true;
	}
	return false;
}

static void *_slab_alloc(struct zus_slab *slab, size_t size)
{
	int slab_index = _slab_list_index(size);
	struct zus_slab_elem *se;
	struct pa_page *page;

	if (_slab_list_empty(slab, size)) {
		int err;

		err = _slab_increase(slab, slab_index);
		if (unlikely(err)) {
			DBG("failed to increase slab => %d\n", err);
			return NULL;
		}
	}
	se = _slab_alloc_elem(slab, slab_index);
	if (unlikely(!se))
		return NULL;

	page = pa_virt_to_page(slab->sbi, se);
	_page_slab_uc_inc(page);

	ZUS_WARN_ON(pa_page_count(page) != 1);
	return se;
}

/* ~~~~~ SLAB free ~~~~~ */

static void _slab_free_elem(struct zus_slab_list *slab_list,
			    struct zus_slab_elem *se)
{
	a_list_add_tail(&se->list, &slab_list->head);
	++slab_list->nfree;
	--slab_list->nused;
}

static void __slab_free(struct zus_slab *slab, int slab_index,
			struct pa_page *page, void *addr)
{
	struct zus_slab_list *slab_list = &slab->list[slab_index];
	int last;

	_slab_free_elem(slab_list, addr);

	if (_page_slab_uc_dec(page))
		return;

	if (slab_list->nfree < slab_list->nfree_want)
		return;

	_slab_page_fini(slab, page, slab_index);
	last = pa_put_page(page);
	ZUS_WARN_ON(!last);
}

static int _slab_free(struct zus_slab *slab, void *addr)
{
	struct pa_page *page;
	int slab_index;
	int err = 0;

	_slab_lock(slab);

	page = pa_virt_to_page(slab->sbi, addr);
	slab_index = _page_slab_index(page);

	if (unlikely(_slab_check_list_index(slab_index))) {
		err = -EINVAL;
		goto out;
	}

	__slab_free(slab, slab_index, page, addr);

out:
	_slab_unlock(slab);
	return err;
}


/* ~~~~~ SLAB fini ~~~~~ */

static void _slab_fini(struct zus_slab *slab)
{
	int slab_index;

	for (slab_index = 0; slab_index < ZUS_SLAB_LISTS; ++slab_index) {
		int last;
		struct zus_slab_elem *se;
		struct pa_page *page;
		struct zus_slab_list *slab_list = &slab->list[slab_index];

		while (!a_list_empty(&slab_list->head)) {
			se = a_list_first_entry(&slab_list->head,
						struct zus_slab_elem, list);
			page = pa_virt_to_page(slab->sbi, se);
			if (ZUS_WARN_ON(page->sinfo.slab_uc)) {
				ERROR("Slab-Leak! uc=%d\n", page->sinfo.slab_uc);
				break;
			}

			_slab_page_fini(slab, page, slab_index);
			last = pa_put_page(page);
			ZUS_WARN_ON(!last);
		}
		slab->list[slab_index].nused = 0;
		slab->list[slab_index].nfree_want = 0;
	}

	slab->cpu = 0;
	slab->sbi = NULL;
}

/* ~~~~~ global volatile-memory SLAB allocator ~~~~~ */

struct zus_global_slab_allocator {
	struct zus_sb_info sbi;
	int nslabs;
	struct zus_slab slab[1];	/* at least one CPU */
};

static struct zus_global_slab_allocator *g_gsa = NULL;


/* TODO: move to pa? */
static bool __pa_addr_inrange(struct zus_sb_info *sbi, void *addr)
{
	struct pa *pa = &sbi->pa[POOL_NUM];

	return ((pa->data.ptr <= addr) &&
		(addr < (pa->data.ptr + pa->size * PAGE_SIZE)));
}

static int _zus_gsa_cpu_of(void *ptr)
{
	long addr = (long)ptr;
	const struct pa_page *page;

	if (unlikely(!addr))
		return -1;

	if (unlikely(addr & ((1 << ZUS_MIN_SLAB_SHIFT) - 1)))
		return -1;

	if (!__pa_addr_inrange(&g_gsa->sbi, ptr))
		return -1;

	page = pa_virt_to_page(&g_gsa->sbi, ptr);
	return _page_slab_cpu(page);
}

static struct zus_slab *_zus_gsa_sslab_at(int cpu, int index)
{
	return &g_gsa->slab[(cpu + index) % g_gsa->nslabs];
}

static struct zus_slab *_slab_of_cpu(int cpu)
{
	if (unlikely((cpu < 0) || (g_gsa->nslabs <= cpu)))
		return NULL;

	return &g_gsa->slab[cpu];
}

static void *_zus_gsa_malloc(size_t size)
{
	int i, cpu = zus_current_cpu_silent();
	struct zus_slab *slab;
	void *ptr;

	slab = _slab_of_cpu(cpu);
	if (unlikely(_slab_iscold(slab, size)))
		goto out;

	for (i = 0; i < g_gsa->nslabs; ++i) {
		if (_slab_trylock(slab)) {
			if (!_slab_list_empty(slab, size)) {
				ptr = _slab_alloc(slab, size);

				_slab_unlock(slab);

				return ptr;
			}
			_slab_unlock(slab);
		}
		slab = _zus_gsa_sslab_at(cpu, i + 1);
	}
out:
	_slab_lock(slab);

	ptr = _slab_alloc(slab, size);

	_slab_unlock(slab);

	return ptr;
}

static void _zus_gsa_free(void *ptr)
{
	int cpu = _zus_gsa_cpu_of(ptr);

	if (ZUS_WARN_ON(cpu < 0))
		return;

	_slab_free(_slab_of_cpu(cpu), ptr);
}

static size_t __elem_size(void *addr)
{
	struct zus_slab *slab = &g_gsa->slab[0];
	struct pa_page *page = pa_virt_to_page(slab->sbi, addr);
	int slab_index = _page_slab_index(page);

	if (unlikely(_slab_check_list_index(slab_index)))
		return -EINVAL;

	return 1 << (slab_index + ZUS_MIN_SLAB_SHIFT);
}

/* ~~~~~ malloc/free wrappers ~~~~~ */

void *zus_malloc(size_t size)
{
	if (unlikely(!g_gsa))
		return NULL;

	if (unlikely(!size))
		return NULL;

	if (PAGE_SIZE < size)
		return malloc(size);

	return _zus_gsa_malloc(size);
}

void zus_free(void *ptr)
{
	if (unlikely(!g_gsa))
		return;

	if (unlikely(!ptr))
		return;

	if (_zus_gsa_cpu_of(ptr) < 0) {
		free(ptr);
		return;
	}
	_zus_gsa_free(ptr);
}

void *zus_calloc(size_t nmemb, size_t elemsz)
{
	size_t size = nmemb * elemsz;
	void *ptr;

	if (unlikely(!g_gsa))
		return NULL;

	if (PAGE_SIZE < size)
		return calloc(nmemb, elemsz);

	ptr = zus_malloc(size);
	if (unlikely(!ptr))
		return NULL;

	memset(ptr, 0, size);

	return ptr;
}

void *zus_realloc(void *ptr, size_t size)
{
	void *newptr;

	if (unlikely(!g_gsa))
		return NULL;

	if (unlikely(!ptr))
		return zus_malloc(size);

	if (unlikely(!size)) {
		zus_free(ptr);
		return NULL;
	}

	if (_zus_gsa_cpu_of(ptr) < 0) {
		if (PAGE_SIZE < size)
			return realloc(ptr, size);
	} else {
		if (size <= __elem_size(ptr))
			return ptr;
	}

	newptr = zus_malloc(size);
	if (unlikely(!newptr))
		return NULL;

	memcpy(newptr, ptr, size);
	zus_free(ptr);

	return newptr;
}

struct pa_page *zus_alloc_page(int mask)
{
	void *ptr = zus_malloc(PAGE_SIZE);

	if (unlikely(!ptr))
		return NULL;
	if (mask & ZUS_ZERO)
		memset(ptr, 0, PAGE_SIZE);
	return zus_virt_to_page(ptr);
}

void zus_free_page(struct pa_page *page)
{
	int cpu = _page_slab_cpu(page);
	struct zus_slab *slab;

	if (ZUS_WARN_ON(cpu < 0))
		return;

	slab = _slab_of_cpu(cpu);
	_slab_free(slab, pa_page_address(slab->sbi, page));
}

void *zus_page_address(struct pa_page *page)
{
	return pa_page_address(&g_gsa->sbi, page);
}

void *zus_virt_to_page(void *addr)
{
	return pa_virt_to_page(&g_gsa->sbi, addr);
}

struct zus_sb_info *zus_global_sbi(void)
{
	return likely(g_gsa) ? &g_gsa->sbi : NULL;
}

/* ~~~~~ init global-allocator ~~~~~ */

int zus_slab_init(void)
{
	struct zus_global_slab_allocator *gsa;
	size_t size;
	int err, cpu, nprocs, pshared = PTHREAD_PROCESS_SHARED;

	if (unlikely(g_gsa))
		return -EINVAL;

	nprocs = get_nprocs_conf();
	size = sizeof(*gsa) + (nprocs - 1) * sizeof(gsa->slab[0]);
	err = posix_memalign((void *)&gsa, 64, size);
	if (unlikely(err)) {
		ERROR("posix_memalign failed: nprocs=%d size=0x%lx => %d\n",
		      nprocs, size, -errno);
		return err;
	}
	memset(gsa, 0, size);

	gsa->nslabs = nprocs;
	for (cpu = 0; cpu < nprocs; ++cpu) {
		err = pthread_spin_init(&gsa->slab[cpu].lock, pshared);
		if (unlikely(err)) {
			ERROR("pthread_spin_init => %d\n", err);
			goto fail;
		}
		_slab_init(&gsa->slab[cpu], &gsa->sbi, cpu);
	}
	err = pa_init(&gsa->sbi);
	if (unlikely(err)) {
		ERROR("pa_init => %d\n", err);
		goto fail;
	}
	g_gsa = gsa;
	return 0;

fail:
	free(gsa);
	return err;
}

void zus_slab_fini(void)
{
	int err, cpu;
	struct zus_global_slab_allocator *gsa = g_gsa;

	if (unlikely(!gsa))
		return;

	g_gsa = NULL;
	for (cpu = 0; cpu < gsa->nslabs; ++cpu) {
		_slab_fini(&gsa->slab[cpu]);
		err = pthread_spin_destroy(&gsa->slab[cpu].lock);
		if (unlikely(err))
			ERROR("pthread_spin_destroy => %d\n", err);
	}
	pa_fini(&gsa->sbi);
	free(gsa);
}
