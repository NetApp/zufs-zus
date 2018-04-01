/*
 * Minimal mkfs utility for the toyfs file-system
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
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <time.h>
#include <uuid/uuid.h>

#include "_pr.h"
#include "zus.h"
#include "toyfs.h"


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


static uint16_t crc_update(uint16_t crc, const void *data, size_t data_len)
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

static uint16_t crc(const void *data, size_t data_len)
{
	return crc_update(~0, data, data_len);
}


static uint16_t toyfs_calc_csum(struct zufs_dev_table *dev_table)
{
#ifndef u64
#define u64 uint64_t
#endif

	uint32_t n = ZUFS_SB_STATIC_SIZE(dev_table) - sizeof(dev_table->s_sum);

	return crc(&dev_table->s_version, n);
}

static int toyfs_open_blkdev(const char *path, loff_t *sz)
{
	int fd, err;
	size_t bdev_size = 0, min_size = 1UL << 20;
	struct stat st;

	fd = open(path, O_RDWR);
	if (fd <= 0)
		error(EXIT_FAILURE, -errno, "open failed: %s", path);

	err = fstat(fd, &st);
	if (err)
		error(EXIT_FAILURE, -errno, "fstat failed: %s", path);

	if (!S_ISBLK(st.st_mode) && !S_ISREG(st.st_mode))
		error(EXIT_FAILURE, -1, "not block or regualr file: %s", path);

	if (S_ISBLK(st.st_mode)) {
		err = ioctl(fd, BLKGETSIZE64, &bdev_size);
		if (err)
			error(EXIT_FAILURE, err,
			      "ioctl(BLKGETSIZE64) failed: %s", path);
		if (bdev_size < min_size)
			error(EXIT_FAILURE, 0,
			      "illegal device size: %s %lu", path, bdev_size);
		*sz = (loff_t)bdev_size;
	} else {
		if (st.st_size < (loff_t)min_size)
			error(EXIT_FAILURE, 0,
			      "illegal size: %s %ld", path, st.st_size);
		*sz = st.st_size;
	}
	printf("open device: %s size=%ld fd=%d\n", path, *sz, fd);
	return fd;
}

static void toyfs_close_blkdev(const char *path, int fd)
{
	printf("close device: %s fd=%d\n", path, fd);
	close(fd);
}

static void toyfs_fill_dev_table(struct zufs_dev_table *dev_table,
				 loff_t dev_size, const char *uu)
{
	int err;
	struct timespec now;
	uuid_t super_uuid, dev_uuid;
	struct md_dev_id *dev_id;
	uint64_t align_mask = ZUFS_ALLOC_MASK;

	uuid_generate(super_uuid);
	err = uuid_parse(uu, dev_uuid);
	if (err)
		error(EXIT_FAILURE, 0, "illegal uuid: %s", uu);

	memset(dev_table, 0, sizeof(*dev_table));
	memcpy(&dev_table->s_uuid, super_uuid, sizeof(dev_table->s_uuid));
	dev_table->s_version = (TOYFS_MAJOR_VERSION * ZUFS_MINORS_PER_MAJOR) +
			       TOYFS_MINOR_VERSION;
	dev_table->s_magic = TOYFS_SUPER_MAGIC;
	dev_table->s_flags = 0;
	dev_table->s_t1_blocks = pmem_o2p(dev_size & ~align_mask);
	dev_table->s_dev_list.id_index = 0;
	dev_table->s_dev_list.t1_count = 1;

	dev_id = &dev_table->s_dev_list.dev_ids[0];
	memcpy(&dev_id->uuid, dev_uuid, sizeof(dev_id->uuid));
	dev_id->blocks = dev_table->s_t1_blocks;
	printf("device: uuid=%s blocks=%lu\n", uu, (size_t)dev_id->blocks);

	clock_gettime(CLOCK_REALTIME, &now);
	timespec_to_mt(&dev_table->s_wtime, &now);
	dev_table->s_sum = toyfs_calc_csum(dev_table);
}

static void toyfs_mirror_parts(struct toyfs_super_block *super_block)
{
	union toyfs_super_block_part *part1 = &super_block->part1;
	union toyfs_super_block_part *part2 = &super_block->part2;

	memcpy(part2, part1, sizeof(*part2));
}

static void
toyfs_write_super_block(int fd, struct toyfs_super_block *super_block)
{
	int err;
	loff_t off;

	off = lseek(fd, 0, SEEK_SET);
	if (off != 0)
		error(EXIT_FAILURE, -errno,
		      "failed to lseek to offset=%ld", off);

	err = write(fd, super_block, sizeof(*super_block));
	if (err != (int)sizeof(*super_block))
		error(EXIT_FAILURE, -errno, "failed to write super block");

	err = fsync(fd);
	if (err)
		error(EXIT_FAILURE, -errno, "failed to fsync");
}

static void toyfs_fill_root_inode(struct toyfs_inode *rooti)
{
	memset(rooti, 0, sizeof(*rooti));

	rooti->zi.i_ino = TOYFS_ROOT_INO;
	rooti->zi.i_nlink = 2;
	rooti->zi.i_size = 0;
	rooti->i_parent_ino = TOYFS_ROOT_INO;
	rooti->ti.dir.d_off_max = 2;
}

static void toyfs_write_root_inode(int fd, struct toyfs_inode *rooti)
{
	int err;
	loff_t off;

	off = lseek(fd, PAGE_SIZE, SEEK_SET);
	if (off != PAGE_SIZE)
		error(EXIT_FAILURE, -errno,
		      "failed to lseek to offset=%ld", off);

	err = write(fd, rooti, sizeof(*rooti));
	if (err != (int)sizeof(*rooti))
		error(EXIT_FAILURE, -errno, "failed to write root inode");

	err = fsync(fd);
	if (err)
		error(EXIT_FAILURE, -errno, "failed to fsync");
}


static struct toyfs_super_block g_super_block;
static struct toyfs_inode g_root_inode;

int main(int argc, char *argv[])
{
	int fd;
	loff_t dev_size = 0;
	struct toyfs_super_block *sb = &g_super_block;
	struct toyfs_inode *rooti = &g_root_inode;

	if (argc != 3)
		error(EXIT_FAILURE, -1, "usage: mkfs <uuid> <device-path>");

	fd = toyfs_open_blkdev(argv[2], &dev_size);
	toyfs_fill_dev_table(&sb->part1.dev_table, dev_size, argv[1]);
	toyfs_mirror_parts(sb);
	toyfs_fill_root_inode(rooti);
	toyfs_write_super_block(fd, sb);
	toyfs_write_root_inode(fd, rooti);
	toyfs_close_blkdev(argv[1], fd);
	return 0;
}
