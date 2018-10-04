/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * md.c - The user-mode imp of what we need from md.h
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */

#include <linux/types.h>
#include <errno.h>

#include "zus.h"
#include "movnt.h"
#include "md.h"
#include "iom_enc.h"

/*
 * python pycrc.py --model crc-16 --algorithm table-driven --generate c
 */
static const uint16_t crc_table[256] = {
	0x0000, 0xc0c1, 0xc181, 0x0140, 0xc301, 0x03c0, 0x0280, 0xc241,
	0xc601, 0x06c0, 0x0780, 0xc741, 0x0500, 0xc5c1, 0xc481, 0x0440,
	0xcc01, 0x0cc0, 0x0d80, 0xcd41, 0x0f00, 0xcfc1, 0xce81, 0x0e40,
	0x0a00, 0xcac1, 0xcb81, 0x0b40, 0xc901, 0x09c0, 0x0880, 0xc841,
	0xd801, 0x18c0, 0x1980, 0xd941, 0x1b00, 0xdbc1, 0xda81, 0x1a40,
	0x1e00, 0xdec1, 0xdf81, 0x1f40, 0xdd01, 0x1dc0, 0x1c80, 0xdc41,
	0x1400, 0xd4c1, 0xd581, 0x1540, 0xd701, 0x17c0, 0x1680, 0xd641,
	0xd201, 0x12c0, 0x1380, 0xd341, 0x1100, 0xd1c1, 0xd081, 0x1040,
	0xf001, 0x30c0, 0x3180, 0xf141, 0x3300, 0xf3c1, 0xf281, 0x3240,
	0x3600, 0xf6c1, 0xf781, 0x3740, 0xf501, 0x35c0, 0x3480, 0xf441,
	0x3c00, 0xfcc1, 0xfd81, 0x3d40, 0xff01, 0x3fc0, 0x3e80, 0xfe41,
	0xfa01, 0x3ac0, 0x3b80, 0xfb41, 0x3900, 0xf9c1, 0xf881, 0x3840,
	0x2800, 0xe8c1, 0xe981, 0x2940, 0xeb01, 0x2bc0, 0x2a80, 0xea41,
	0xee01, 0x2ec0, 0x2f80, 0xef41, 0x2d00, 0xedc1, 0xec81, 0x2c40,
	0xe401, 0x24c0, 0x2580, 0xe541, 0x2700, 0xe7c1, 0xe681, 0x2640,
	0x2200, 0xe2c1, 0xe381, 0x2340, 0xe101, 0x21c0, 0x2080, 0xe041,
	0xa001, 0x60c0, 0x6180, 0xa141, 0x6300, 0xa3c1, 0xa281, 0x6240,
	0x6600, 0xa6c1, 0xa781, 0x6740, 0xa501, 0x65c0, 0x6480, 0xa441,
	0x6c00, 0xacc1, 0xad81, 0x6d40, 0xaf01, 0x6fc0, 0x6e80, 0xae41,
	0xaa01, 0x6ac0, 0x6b80, 0xab41, 0x6900, 0xa9c1, 0xa881, 0x6840,
	0x7800, 0xb8c1, 0xb981, 0x7940, 0xbb01, 0x7bc0, 0x7a80, 0xba41,
	0xbe01, 0x7ec0, 0x7f80, 0xbf41, 0x7d00, 0xbdc1, 0xbc81, 0x7c40,
	0xb401, 0x74c0, 0x7580, 0xb541, 0x7700, 0xb7c1, 0xb681, 0x7640,
	0x7200, 0xb2c1, 0xb381, 0x7340, 0xb101, 0x71c0, 0x7080, 0xb041,
	0x5000, 0x90c1, 0x9181, 0x5140, 0x9301, 0x53c0, 0x5280, 0x9241,
	0x9601, 0x56c0, 0x5780, 0x9741, 0x5500, 0x95c1, 0x9481, 0x5440,
	0x9c01, 0x5cc0, 0x5d80, 0x9d41, 0x5f00, 0x9fc1, 0x9e81, 0x5e40,
	0x5a00, 0x9ac1, 0x9b81, 0x5b40, 0x9901, 0x59c0, 0x5880, 0x9841,
	0x8801, 0x48c0, 0x4980, 0x8941, 0x4b00, 0x8bc1, 0x8a81, 0x4a40,
	0x4e00, 0x8ec1, 0x8f81, 0x4f40, 0x8d01, 0x4dc0, 0x4c80, 0x8c41,
	0x4400, 0x84c1, 0x8581, 0x4540, 0x8701, 0x47c0, 0x4680, 0x8641,
	0x8201, 0x42c0, 0x4380, 0x8341, 0x4100, 0x81c1, 0x8081, 0x4040
};


static uint16_t crc16(uint16_t crc, const void *data, size_t data_len)
{
	const unsigned char *d = (const unsigned char *)data;
	unsigned int tbl_idx;

	while (data_len--) {
		tbl_idx = (crc ^ *d) & 0xff;
		crc = (crc_table[tbl_idx] ^ (crc >> 8)) & 0xffff;
		d++;
	}
	return crc & 0xffff;
}

static ulong _gcd(ulong _x, ulong _y)
{
	ulong tmp;

	if (_x < _y) {
		ulong c = _x;

		_x = _y;
		_y = c;
	}

	if (!_y)
		return _x;

	while ((tmp = _x % _y) != 0) {
		_x = _y;
		_y = tmp;
	}
	return _y;
}

short md_calc_csum(struct md_dev_table *mdt)
{
	uint n = MDT_STATIC_SIZE(mdt) - sizeof(mdt->s_sum);
	/* FIXME: We should skip s_version so we can change it after
	*        mount, once we start using the new structures
	*   So below should be &mdt->s_version => &mdt->s_magic
	*   PXS-240.
	*/
	return crc16(~0, (__u8 *)&mdt->s_version, n);
	return 0;
}

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

	DBG("[%d] mdi(offset=0x%lx, size=0x%lx, nid=%d) @%p\n",
	    mdi->index, mdi->offset, mdi->size, mdi->nid,
            pmem_addr ? pmem_addr + offset : 0);
}

static int _map_setup(struct multi_devices *md, ulong blocks, int dev_start,
		      struct md_dev_larray *larray)
{
	ulong map_size, bn_end;
	uint i, dev_index = dev_start;

	map_size = blocks / larray->bn_gcd;
	larray->map = calloc(map_size, sizeof(*larray->map));
	if (!larray->map) {
		md_dbg_err("failed to allocate dev map\n");
		return -ENOMEM;
	}

	bn_end = md_o2p(md->devs[dev_index].size);
	for (i = 0; i < map_size; ++i) {
		if ((i * larray->bn_gcd) >= bn_end)
			bn_end += md_o2p(md->devs[++dev_index].size);
		larray->map[i] = &md->devs[dev_index];
	}

	return 0;
}

int md_init_from_pmem_info(struct multi_devices *md)
{
	struct md_dev_list *dev_list = &md->pmem_info.dev_list;
	ulong offset = 0;
	int i, err;

	md->t1_count = dev_list->t1_count;
	md->t2_count = dev_list->t2_count;

	for (i = 0; i < md->t1_count; ++i) {
		struct md_dev_info *mdi = &md->devs[i];

		_init_dev_info(mdi, &dev_list->dev_ids[i], i, offset,
			       md->p_pmem_addr);
		offset += mdi->size;
		md->t1a.bn_gcd = _gcd(md->t1a.bn_gcd, md_o2p(mdi->size));
	}

	offset = 0;
	for (;i < md->t1_count + md->t2_count; ++i) {
		struct md_dev_info *mdi = &md->devs[i];

		_init_dev_info(mdi, &dev_list->dev_ids[i], i, offset, NULL);
		offset += mdi->size;
		md->t2a.bn_gcd = _gcd(md->t2a.bn_gcd, md_o2p(mdi->size));
	}

	if (md->t1_count) {
		err = _map_setup(md, md_t1_blocks(md), 0, &md->t1a);
		if (unlikely(err))
			return err;
	}

	if (md->t2_count) {
		err = _map_setup(md, md_t2_blocks(md),  md->t1_count, &md->t2a);
		if (unlikely(err))
			return err;
	}
	return 0;
}

void md_fini(struct multi_devices *md, struct block_device *s_bdev)
{
	if (md->t2_count)
		free(md->t2a.map);
	if (md->t1_count)
		free(md->t1a.map);
}

static bool _csum_mismatch(struct md_dev_table *mdt, int silent)
{
	ushort crc = md_calc_csum(mdt);

	if (mdt->s_sum == cpu_to_le16(crc))
		return false;

	md_warn_cnd(silent, "expected(0x%x) != s_sum(0x%x)\n",
		      cpu_to_le16(crc), mdt->s_sum);
	return true;
}

static bool _uuid_le_equal(uuid_le *uuid1, uuid_le *uuid2)
{
	return (memcmp(uuid1, uuid2, sizeof(uuid_le)) == 0);
}

static bool _mdt_compare_uuids(struct md_dev_table *mdt,
			       struct md_dev_table *main_mdt, int silent)
{
	int i, dev_count;

	if (!_uuid_le_equal(&mdt->s_uuid, &main_mdt->s_uuid)) {
		md_warn_cnd(silent, "mdt uuid (%pUb != %pUb) mismatch\n",
			      &mdt->s_uuid, &main_mdt->s_uuid);
		return false;
	}

	dev_count = mdt->s_dev_list.t1_count + mdt->s_dev_list.t2_count +
		    mdt->s_dev_list.rmem_count;
	for (i = 0; i < dev_count; ++i) {
		struct md_dev_id *dev_id1 = &mdt->s_dev_list.dev_ids[i];
		struct md_dev_id *dev_id2 = &main_mdt->s_dev_list.dev_ids[i];

		if (!_uuid_le_equal(&dev_id1->uuid, &dev_id2->uuid)) {
			md_warn_cnd(silent, "mdt dev %d uuid (%pUb != %pUb) mismatch\n",
				      i, &dev_id1->uuid, &dev_id2->uuid);
			return false;
		}

		if (dev_id1->blocks != dev_id2->blocks) {
			md_warn_cnd(silent, "mdt dev %d blocks (0x%llx != 0x%llx) mismatch\n",
				      i, le64_to_cpu(dev_id1->blocks),
				      le64_to_cpu(dev_id2->blocks));
			return false;
		}
	}

	return true;
}

bool md_mdt_check(struct md_dev_table *mdt,
		  struct md_dev_table *main_mdt, struct block_device *bdev,
		  struct mdt_check *mc)
{
	struct md_dev_table *mdt2 = (void *)mdt + MDT_SIZE;
	struct md_dev_id *dev_id;
	ulong super_size;

// 	BUILD_BUG_ON(MDT_STATIC_SIZE(mdt) & (SMP_CACHE_BYTES - 1));

	/* Do sanity checks on the superblock */
	if (le32_to_cpu(mdt->s_magic) != mc->magic) {
		if (le32_to_cpu(mdt2->s_magic) != mc->magic) {
			md_warn_cnd(mc->silent,
				     "Can't find a valid partition\n");
			return false;
		}

		md_warn_cnd(mc->silent,
			     "Magic error in super block: using copy\n");
		/* Try to auto-recover the super block */
		memcpy_to_pmem(mdt, mdt2, sizeof(*mdt));
	}

	if ((mc->major_ver != (uint)mdt_major_version(mdt)) ||
	    (mc->minor_ver < (uint)mdt_minor_version(mdt))) {
		md_warn_cnd(mc->silent,
			     "mkfs-mount versions mismatch! %d.%d != %d.%d\n",
			     mdt_major_version(mdt), mdt_minor_version(mdt),
			     mc->major_ver, mc->minor_ver);
		return false;
	}

	if (_csum_mismatch(mdt, mc->silent)) {
		if (_csum_mismatch(mdt2, mc->silent)) {
			md_warn_cnd(mc->silent, "checksum error in super block\n");
			return false;
		} else {
			md_warn_cnd(mc->silent, "crc16 error in super block: using copy\n");
			/* Try to auto-recover the super block */
			memcpy_to_pmem(mdt, mdt2, MDT_SIZE);
			/* TODO(sagi): copy fixed mdt to shadow */
		}
	}

	if (main_mdt) {
		if (mdt->s_dev_list.t1_count != main_mdt->s_dev_list.t1_count) {
			md_warn_cnd(mc->silent, "mdt t1 count mismatch\n");
			return false;
		}

		if (mdt->s_dev_list.t2_count != main_mdt->s_dev_list.t2_count) {
			md_warn_cnd(mc->silent, "mdt t2 count mismatch\n");
			return false;
		}

		if (mdt->s_dev_list.rmem_count != main_mdt->s_dev_list.rmem_count) {
			md_warn_cnd(mc->silent, "mdt rmem dev count mismatch\n");
			return false;
		}

		if (!_mdt_compare_uuids(mdt, main_mdt, mc->silent))
			return false;
	}

	/* check alignment */
	dev_id = &mdt->s_dev_list.dev_ids[mdt->s_dev_list.id_index];
	super_size = md_p2o(__dev_id_blocks(dev_id));
	if (unlikely(!super_size || super_size & mc->alloc_mask)) {
		md_warn_cnd(mc->silent, "super_size(0x%lx) ! 2_M aligned\n",
			      super_size);
		return false;
	}

	return true;
}

static void _done(struct zus_iomap_build *iomb)
{
}

static void _submit(struct zus_iomap_build *iomb, bool last, bool sync)
{
	ERROR("\n");
}

int md_t2_mdt_read(struct multi_devices *md, int dev_index,
		   struct md_dev_table *mdt)
{
	struct zus_iomap_build iomb = {};
	struct {
		struct zufs_ioc_iomap_exec ziome;
		struct zufs_iom_t2_zusmem_io space;
		__u64 null_term;
	} a;
	uint iom_max;

	iomb.ziom = &a.ziome.ziom;
	iom_max = sizeof(a) - offsetof(typeof(a.ziome), ziom);
	_zus_ioc_ziom_init(iomb.ziom, iom_max);
	_zus_ioc_iom_start(&iomb, md->sbi, _done, _submit, md, &a.ziome.ziom);

	_zus_iom_enc_t2_zusmem_read(&iomb, 0, mdt, PAGE_SIZE);

	_zus_ioc_iom_exec_submit(&iomb, true, true);

	return iomb.err;
}

int md_t2_mdt_write(struct multi_devices *md, struct md_dev_table *mdt)
{
	struct zus_iomap_build iomb = {};
	struct {
		struct zufs_ioc_iomap_exec ziome;
		struct zufs_iom_t2_zusmem_io space[md->t2_count];
		__u64 null_term;
	} a;
	int i;
	uint iom_max;

	iomb.ziom = &a.ziome.ziom;
	iom_max = sizeof(a) - offsetof(typeof(a.ziome), ziom);

	/* FIXME: must make copies and execute at end. one by one for now */
	for (i = 0; i < md->t2_count; ++i) {
		ulong bn = md_o2p(md_t2_dev(md, i)->offset);

		_zus_ioc_ziom_init(iomb.ziom, iom_max);
		_zus_ioc_iom_start(&iomb, md->sbi, NULL, NULL, NULL, &a.ziome.ziom);

		mdt->s_dev_list.id_index = mdt->s_dev_list.t1_count + i;
		mdt->s_sum = cpu_to_le16(md_calc_csum(mdt));

		_zus_iom_enc_t2_zusmem_write(&iomb, bn, mdt, PAGE_SIZE);
		_zus_ioc_iom_exec_submit(&iomb, true, true);
		if (iomb.err)
			break;
	}

	return iomb.err;
}

/* ~~~  _zus_iom facility (imp of iom_enc.h) ~~~ */
void _zus_ioc_iom_exec_submit(struct zus_iomap_build *iomb, bool done,
			      bool sync)
{
	struct zufs_ioc_iomap_exec *ziome =
			container_of(iomb->ziom, typeof(*ziome), ziom);
	int err;

	_zus_iom_end(iomb);

	if (ZUS_WARN_ON(!iomb->ziom))
		return;

	err = __zus_iom_exec(iomb->sbi, ziome, sync);
	iomb->err = ziome->hdr.err;
	if (unlikely(err && !iomb->err))
		iomb->err = -errno;

	if (sync && iomb->done)
		iomb->done(iomb);
}
