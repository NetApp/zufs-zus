/*
 * md.c - The user-mode imp of what we need from md.h
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */

#include <linux/types.h>

#include "zus.h"
#include "md.h"

static void _init_dev_info(struct md_dev_info *mdi, struct md_dev_id *id,
			  int index, __u64 offset, void* pmem_addr)
{
	mdi->offset = offset;
	mdi->index = index;
	mdi->size = md_p2o(__dev_id_blocks(id));
	mdi->nid = __dev_id_nid(id);

	if (pmem_addr) { /* We are t1*/
		mdi->t1i.virt_addr = pmem_addr + offset;
	}
}

int md_init_from_pmem_info(struct multi_devices *md)
{
	struct md_dev_list *dev_list = &md->pmem_info.dev_list;
	ulong offset = 0;
	int i;

	md->t1_count = dev_list->t1_count;
	md->t2_count = dev_list->t2_count;

	for (i = 0; i < md->t1_count; ++i) {
		struct md_dev_info *mdi = &md->devs[i];

		_init_dev_info(mdi, &dev_list->dev_ids[i], i, offset,
			       md->p_pmem_addr);
		offset += mdi->size;
	}

	offset = 0;
	for (;i < md->t1_count + md->t2_count; ++i) {
		struct md_dev_info *mdi = &md->devs[i];

		_init_dev_info(mdi, &dev_list->dev_ids[i], i, offset, NULL);
		offset += mdi->size;
	}

	return 0;
}
