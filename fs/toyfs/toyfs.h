/*
 * toyfs.h - The toyfs reference file-system implementation via zufs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Shachar Sharon <sshachar@netapp.com>
 */
#ifndef TOYFS_H_
#define TOYFS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <uuid/uuid.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <limits.h>

#include "zus.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x_)	(sizeof(x_) / sizeof(x_[0]))
#endif
#ifndef MAKESTR
#define MAKESTR(x_)	#x_
#endif
#ifndef STR
#define STR(x_)		MAKESTR(x_)
#endif


#define toyfs_panic(fmt, ...) \
	toyfs_panicf(__FILE__, __LINE__, fmt, __VA_ARGS__)
#define toyfs_panic_if_err(err, msg) \
	do { if (err) toyfs_panic("%s: %d", msg, err); } while (0)
#define toyfs_assert(cond) \
	do { if (!(cond)) toyfs_panic("assert failed: %s", #cond); } while (0)


#define TOYFS_NULL_INO		(0)
#define TOYFS_ROOT_INO		(1)
#define TOYFS_MAJOR_VERSION	(14)
#define TOYFS_MINOR_VERSION	(1)
#define TOYFS_SUPER_MAGIC	(0x5346314d)

#define Z2SBI(zsbi) toyfs_zsbi_to_sbi(zsbi)
#define Z2II(zii) toyfs_zii_to_tii(zii)


struct toyfs_page {
	uint8_t dat[PAGE_SIZE];
};

struct toyfs_pool {
	pthread_mutex_t mutex;
	union toyfs_pool_page *pages;
	struct list_head free_dblkrefs;
	struct list_head free_iblkrefs;
	struct list_head free_dirents;
	struct list_head free_inodes;
	void	*mem;
	size_t	msz;
	bool	pmem;
};

struct toyfs_itable {
	pthread_mutex_t mutex;
	size_t icount;
	struct toyfs_inode_info *imap[33377]; /* TODO: Variable size */
};

union toyfs_super_block_part {
	struct zufs_dev_table dev_table;
	uint8_t reserved[ZUFS_SB_SIZE];
};


struct toyfs_super_block {
	union toyfs_super_block_part part1;
	union toyfs_super_block_part part2;
};

struct toyfs_sb_info {
	struct zus_sb_info s_zus_sbi;
	struct statvfs s_statvfs;
	pthread_mutex_t s_mutex;
	struct toyfs_pool s_pool;
	struct toyfs_itable s_itable;
	struct toyfs_inode_info *s_root;
	ino_t s_top_ino;
};


struct toyfs_inode_dir {
	struct list_head d_childs;
	size_t d_ndentry;
	loff_t d_off_max;
};

struct toyfs_inode_reg {
	struct list_head r_iblkrefs;
	ino_t r_first_parent;
};

union toyfs_inode_symlnk {
	struct toyfs_page *sl_long;
};

struct toyfs_inode {
	struct zus_inode zi;
	ino_t i_parent_ino;
	union {
		struct toyfs_inode_dir dir;
		struct toyfs_inode_reg reg;
		union toyfs_inode_symlnk symlnk;
		uint8_t align[56];
	} ti;
};

union toyfs_inode_head {
	struct list_head head;
	struct toyfs_inode inode;
};

struct toyfs_inode_info {
	struct zus_inode_info zii;
	struct toyfs_sb_info *sbi;
	struct toyfs_inode *ti;
	struct toyfs_inode_info *next;
	ino_t ino;
	unsigned long imagic;
};

struct toyfs_dirent {
	struct list_head d_head;
	loff_t  d_off;
	ino_t   d_ino;
	size_t  d_nlen;
	mode_t	d_type;
	char    d_name[ZUFS_NAME_LEN + 1]; /* TODO: Use variable size */
};

struct toyfs_dblkref {
	struct list_head head;
	size_t refcnt;
	size_t bn;
};

struct toyfs_iblkref {
	struct list_head head;
	struct toyfs_dblkref *dblkref;
	loff_t off;
};


/* super.c */
void toyfs_check_types(void);
void toyfs_sbi_lock(struct toyfs_sb_info *sbi);
void toyfs_sbi_unlock(struct toyfs_sb_info *sbi);
int toyfs_sbi_init(struct zus_sb_info *zsbi, struct zufs_ioc_mount *zim);
int toyfs_sbi_fini(struct zus_sb_info *zsbi);
struct zus_sb_info *toyfs_sbi_alloc(struct zus_fs_info *zfi);
void toyfs_sbi_free(struct zus_sb_info *zsbi);
size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr);
void *toyfs_bn2addr(struct toyfs_sb_info *sbi, size_t bn);
struct toyfs_page *toyfs_bn2page(struct toyfs_sb_info *sbi, size_t bn);
struct toyfs_inode *toyfs_acquire_inode(struct toyfs_sb_info *sbi);
void toyfs_release_inode(struct toyfs_sb_info *sbi, struct toyfs_inode *inode);
void toyfs_track_inode(struct toyfs_inode_info *tii);
void toyfs_untrack_inode(struct toyfs_inode_info *tii);
struct toyfs_inode_info *toyfs_find_inode(struct toyfs_sb_info *sbi, ino_t ino);
struct toyfs_dirent *toyfs_acquire_dirent(struct toyfs_sb_info *sbi);
void toyfs_release_dirent(struct toyfs_sb_info *sbi,
			  struct toyfs_dirent *dirent);
struct toyfs_page *toyfs_acquire_page(struct toyfs_sb_info *sbi);
int toyfs_statfs(struct zus_sb_info *zsbi, struct zufs_ioc_statfs *ioc_statfs);
int toyfs_sync(struct zus_inode_info *zii, struct zufs_ioc_range *ioc_range);
struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi);
struct zus_inode_info *toyfs_zii_alloc(struct zus_sb_info *zsbi);
void toyfs_zii_free(struct zus_inode_info *zii);
void toyfs_release_page(struct toyfs_sb_info *sbi, struct toyfs_page *page);
struct toyfs_dblkref *toyfs_acquire_dblkref(struct toyfs_sb_info *sbi);
void toyfs_release_dblkref(struct toyfs_sb_info *sbi,
			   struct toyfs_dblkref *dblkref);
struct toyfs_iblkref *toyfs_acquire_iblkref(struct toyfs_sb_info *sbi);
void toyfs_release_iblkref(struct toyfs_sb_info *sbi,
			   struct toyfs_iblkref *iblkref);

/* inode.c */
void toyfs_evict(struct zus_inode_info *zii);
int toyfs_new_inode(struct zus_sb_info *zsbi, struct zus_inode_info *zii,
		    void *app_ptr, struct zufs_ioc_new_inode *ioc_new);
int toyfs_free_inode(struct zus_inode_info *zii);
int toyfs_iget(struct zus_sb_info *zsbi, struct zus_inode_info *zii, ulong ino);
int toyfs_setattr(struct zus_inode_info *zii,
		  uint enable_bits, ulong truncate_size);

/* dir.c */
int toyfs_add_dentry(struct zus_inode_info *dir_zii,
		     struct zus_inode_info *zii, struct zufs_str *str);
int toyfs_remove_dentry(struct zus_inode_info *dir_zii, struct zufs_str *str);
int toyfs_readdir(void *app_ptr, struct zufs_ioc_readdir *zir);
int toyfs_iterate_dir(struct toyfs_inode_info *dir_tii,
		      struct zufs_ioc_readdir *zir, void *buf);

/* file.c */
int toyfs_read(void *buf, struct zufs_ioc_IO *ioc_io);
int toyfs_write(void *buf, struct zufs_ioc_IO *ioc_io);
int toyfs_get_block(struct zus_inode_info *zii,
		    struct zufs_ioc_get_block *get_block);
int toyfs_fallocate(struct zus_inode_info *zii,
		    struct zufs_ioc_range *ioc_range);
int toyfs_seek(struct zus_inode_info *zii, struct zufs_ioc_seek *zis);
int toyfs_truncate(struct toyfs_inode_info *tii, size_t size);
int toyfs_clone(struct zufs_ioc_clone *ioc_clone);

/* symlink.c */
void toyfs_release_symlink(struct toyfs_inode_info *tii);
const char *toyfs_symlink_value(const struct toyfs_inode_info *tii);
int toyfs_get_symlink(struct zus_inode_info *zii, void **symlink);

/* namei.c */
ulong toyfs_lookup(struct zus_inode_info *dir_ii, struct zufs_str *str);
int toyfs_rename(struct zufs_ioc_rename *zir);

/* common.c */
void toyfs_panicf(const char *file, int line, const char *fmt, ...);
void toyfs_mutex_init(pthread_mutex_t *mutex);
void toyfs_mutex_destroy(pthread_mutex_t *mutex);
void toyfs_mutex_lock(pthread_mutex_t *mutex);
void toyfs_mutex_unlock(pthread_mutex_t *mutex);
struct toyfs_sb_info *toyfs_zsbi_to_sbi(struct zus_sb_info *zsbi);
struct toyfs_inode_info *toyfs_zii_to_tii(struct zus_inode_info *zii);
int toyfs_register_fs(int fd);
extern const struct zus_zii_operations toyfs_zii_op;
extern const struct zus_zfi_operations toyfs_zfi_op;
extern const struct zus_sbi_operations toyfs_sbi_op;

#endif /* TOYFS_H_*/
