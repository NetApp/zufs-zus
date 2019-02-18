/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Minimal mkfs utility for the toyfs file-system
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

#include "zus.h"
#include "toyfs.h"

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

static void toyfs_fill_dev_table(struct md_dev_table *dev_table,
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
	dev_table->s_t1_blocks = md_o2p(dev_size & ~align_mask);
	dev_table->s_dev_list.id_index = 0;
	dev_table->s_dev_list.t1_count = 1;

	dev_id = &dev_table->s_dev_list.dev_ids[0];
	memcpy(&dev_id->uuid, dev_uuid, sizeof(dev_id->uuid));
	dev_id->blocks = dev_table->s_t1_blocks;
	printf("device: uuid=%s blocks=%lu\n", uu, (size_t)dev_id->blocks);

	clock_gettime(CLOCK_REALTIME, &now);
	timespec_to_zt(&dev_table->s_wtime, &now);
	dev_table->s_sum = md_calc_csum(dev_table);
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

	rooti->i_ino = TOYFS_ROOT_INO;
	rooti->i_nlink = 2;
	rooti->i_size = 0;
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
