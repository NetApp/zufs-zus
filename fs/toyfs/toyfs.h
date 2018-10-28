/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * toyfs.h - The toyfs reference file-system implementation via zufs
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *      Shachar Sharon <sshachar@netapp.com>
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
#define ARRAY_SIZE(x_)  (sizeof(x_) / sizeof(x_[0]))
#endif
#ifndef MAKESTR
#define MAKESTR(x_)     #x_
#endif
#ifndef STR
#define STR(x_)         MAKESTR(x_)
#endif
#ifndef container_of
#define container_of(ptr, type, member) \
	(type *)((void *)((char *)ptr - offsetof(type, member)))
#endif

#define TOYFS_STATICASSERT(expr)	_Static_assert(expr, #expr)
#define TOYFS_STATICASSERT_EQ(a, b)	TOYFS_STATICASSERT(a == b)
#define TOYFS_BUILD_BUG_ON(expr)	TOYFS_STATICASSERT(expr)

#define toyfs_panic(fmt, ...) \
	toyfs_panicf(__FILE__, __LINE__, fmt, __VA_ARGS__)
#define toyfs_panic_if_err(err, msg) \
	do { if (err) toyfs_panic("%s: %d", msg, err); } while (0)
#define toyfs_assert(cond) \
	do { if (!(cond)) toyfs_panic("assert failed: %s", #cond); } while (0)


#define TOYFS_NULL_INO          (0)
#define TOYFS_ROOT_INO          (1)
#define TOYFS_MAJOR_VERSION     (14)
#define TOYFS_MINOR_VERSION     (1)
#define TOYFS_SUPER_MAGIC       (0x5346314d)

#define Z2SBI(zsbi) toyfs_zsbi_to_sbi(zsbi)
#define Z2II(zii) toyfs_zii_to_tii(zii)

struct toyfs_list_head {
	struct toyfs_list_head *next;
	struct toyfs_list_head *prev;
};

void toyfs_list_init(struct toyfs_list_head *list);

void toyfs_list_add(struct toyfs_list_head *elem, struct toyfs_list_head *head);

void toyfs_list_del(struct toyfs_list_head *entry);

int toyfs_list_empty(const struct toyfs_list_head *head);

void toyfs_list_add_tail(struct toyfs_list_head *elem,
			 struct toyfs_list_head *head);

void toyfs_list_add_before(struct toyfs_list_head *elem,
			   struct toyfs_list_head *head);


/* "raw" 4k pmem page/block */
struct toyfs_pmemb {
	uint8_t dat[PAGE_SIZE];
};

struct toyfs_pool {
	pthread_mutex_t mutex;
	union toyfs_pool_pmemb *pages;
	struct toyfs_list_head free_dblkrefs;
	struct toyfs_list_head free_iblkrefs;
	struct toyfs_list_head free_inodes;
	void    *mem;
	size_t  msz;
};

struct toyfs_inode_ref {
	struct toyfs_inode_ref *next;
	struct toyfs_inode_info *tii;
	struct toyfs_inode *ti;
	ino_t ino;
};

struct toyfs_itable {
	pthread_mutex_t mutex;
	size_t icount;
	struct toyfs_inode_ref *imap[33377]; /* TODO: Variable size */
};

union toyfs_super_block_part {
	struct md_dev_table dev_table;
	uint8_t reserved[MDT_SIZE];
};


struct toyfs_super_block {
	union toyfs_super_block_part part1;
	union toyfs_super_block_part part2;
};

struct toyfs_sb_info {
	struct zus_sb_info s_zus_sbi;
	struct statvfs s_statvfs;
	pthread_mutex_t s_mutex;
	pthread_mutex_t s_inodes_lock;
	struct toyfs_pool s_pool;
	struct toyfs_itable s_itable;
	struct toyfs_inode_info *s_root;
	ino_t s_top_ino;
};

struct toyfs_inode {
	uint16_t i_flags;
	uint16_t i_mode;
	uint32_t i_nlink;
	uint64_t i_size;
	struct toyfs_list_head list_head;
	uint64_t i_blocks;
	uint64_t i_mtime;
	uint64_t i_ctime;
	uint64_t i_atime;
	uint64_t i_ino;
	uint32_t i_uid;
	uint32_t i_gid;
	uint64_t i_xattr;
	uint64_t i_generation;
	union {
		uint32_t i_rdev;
		uint8_t	 i_symlink[32];
		uint64_t i_sym_dpp;
		struct  _t_dir {
			uint64_t reserved;
			uint64_t parent;
		} i_dir;
	};
};

struct toyfs_inode_info {
	struct zus_inode_info zii;
	struct toyfs_sb_info *sbi;
	struct toyfs_inode *ti;
	ino_t ino;
	unsigned long imagic;
	int ref;
	bool mapped;
	bool valid;
};

struct toyfs_dirent {
	int64_t  d_off;
	uint64_t d_ino;
	uint8_t  d_type;
	uint8_t  d_nlen;
	char d_name[14];
};

struct toyfs_dentries {
	struct toyfs_list_head head;
	uint8_t reserved[16];
	struct toyfs_dirent de[127];
};

struct toyfs_dblkref {
	struct toyfs_list_head head;
	size_t refcnt;
	size_t bn;
};

struct toyfs_iblkref {
	struct toyfs_list_head head;
	struct toyfs_dblkref *dblkref;
	loff_t off;
};

struct toyfs_xattr_entry {
	uint16_t value_size;
	uint8_t  name_len;
	uint8_t  data[1];
};

struct toyfs_xattr {
	struct toyfs_xattr_entry xe[1024];
};

/* super.c */
void toyfs_check_types(void);
struct zus_inode *toyfs_ti2zi(struct toyfs_inode *ti);
void toyfs_sbi_lock(struct toyfs_sb_info *sbi);
void toyfs_sbi_unlock(struct toyfs_sb_info *sbi);
void toyfs_lock_inodes(struct toyfs_sb_info *sbi);
void toyfs_unlock_inodes(struct toyfs_sb_info *sbi);
int toyfs_sbi_init(struct zus_sb_info *zsbi, struct zufs_ioc_mount *zim);
int toyfs_sbi_fini(struct zus_sb_info *zsbi);
struct zus_sb_info *toyfs_sbi_alloc(struct zus_fs_info *zfi);
void toyfs_sbi_free(struct zus_sb_info *zsbi);
size_t toyfs_addr2bn(struct toyfs_sb_info *sbi, void *ptr);
void *toyfs_bn2addr(struct toyfs_sb_info *sbi, size_t bn);
struct toyfs_pmemb *toyfs_bn2pmemb(struct toyfs_sb_info *sbi, size_t bn);
zu_dpp_t toyfs_page2dpp(struct toyfs_sb_info *sbi, struct toyfs_pmemb *);
struct toyfs_pmemb *toyfs_dpp2pmemb(struct toyfs_sb_info *sbi, zu_dpp_t dpp);
struct toyfs_inode *toyfs_acquire_inode(struct toyfs_sb_info *sbi);
void toyfs_release_inode(struct toyfs_sb_info *sbi, struct toyfs_inode *inode);
void toyfs_i_track(struct toyfs_inode_info *tii);
void toyfs_i_untrack(struct toyfs_inode_info *tii, bool);
struct toyfs_inode_ref *
toyfs_find_inode_ref_by_ino(struct toyfs_sb_info *sbi, ino_t ino);
struct toyfs_dirent *toyfs_acquire_dirent(struct toyfs_sb_info *sbi);
struct toyfs_pmemb *toyfs_acquire_pmemb(struct toyfs_sb_info *sbi);
int toyfs_statfs(struct zus_sb_info *zsbi, struct zufs_ioc_statfs *ioc_statfs);
int toyfs_sync(struct zus_inode_info *zii, struct zufs_ioc_range *ioc_range);
struct toyfs_inode_info *toyfs_alloc_ii(struct toyfs_sb_info *sbi);
struct zus_inode_info *toyfs_zii_alloc(struct zus_sb_info *zsbi);
void toyfs_tii_free(struct toyfs_inode_info *zii);
void toyfs_release_pmemb(struct toyfs_sb_info *sbi, struct toyfs_pmemb *);
struct toyfs_dblkref *toyfs_acquire_dblkref(struct toyfs_sb_info *sbi);
void toyfs_release_dblkref(struct toyfs_sb_info *sbi,
			   struct toyfs_dblkref *dblkref);
struct toyfs_iblkref *toyfs_acquire_iblkref(struct toyfs_sb_info *sbi);
void toyfs_release_iblkref(struct toyfs_sb_info *sbi,
			   struct toyfs_iblkref *iblkref);

/* inode.c */
void toyfs_evict(struct zus_inode_info *zii);
struct zus_inode_info *
toyfs_new_inode(struct zus_sb_info *zsbi,
		void *app_ptr, struct zufs_ioc_new_inode *ioc_new);
void toyfs_free_inode(struct toyfs_inode_info *zii);
int toyfs_iget(struct zus_sb_info *zsbi, ulong ino,
	       struct zus_inode_info **zii);
int toyfs_setattr(struct zus_inode_info *zii,
		  uint enable_bits, ulong truncate_size);

/* dir.c */
int toyfs_add_dirent(struct toyfs_inode_info *dir_tii,
		     struct toyfs_inode_info *tii, struct zufs_str *str,
		     struct toyfs_dirent **out_dirent);
void toyfs_remove_dirent(struct toyfs_inode_info *dir_tii,
			 struct toyfs_inode_info *tii,
			 struct toyfs_dirent *dirent);
int toyfs_add_dentry(struct zus_inode_info *dir_zii,
		     struct zus_inode_info *zii, struct zufs_str *str);
int toyfs_remove_dentry(struct zus_inode_info *dir_zii,
			struct zus_inode_info *zii, struct zufs_str *str);
int toyfs_readdir(void *app_ptr, struct zufs_ioc_readdir *zir);
int toyfs_iterate_dir(struct toyfs_inode_info *dir_tii,
		      struct zufs_ioc_readdir *zir, void *buf);
void toyfs_release_dir(struct toyfs_inode_info *dir_tii);

struct toyfs_dirent *
toyfs_lookup_dirent(struct toyfs_inode_info *dir_ii, const struct zufs_str *);
struct toyfs_list_head *toyfs_childs_list_of(struct toyfs_inode_info *dir_tii);

/* file.c */
int toyfs_read(void *buf, struct zufs_ioc_IO *ioc_io);
int toyfs_pre_read(void *buf, struct zufs_ioc_IO *ioc_io);
int toyfs_write(void *buf, struct zufs_ioc_IO *ioc_io);
int toyfs_fallocate(struct zus_inode_info *zii,
		    struct zufs_ioc_range *ioc_range);
int toyfs_seek(struct zus_inode_info *zii, struct zufs_ioc_seek *zis);
int toyfs_truncate(struct toyfs_inode_info *tii, size_t size);
int toyfs_clone(struct zufs_ioc_clone *ioc_clone);
struct toyfs_list_head *toyfs_iblkrefs_list_of(struct toyfs_inode_info *tii);
struct toyfs_pmemb *toyfs_resolve_pmemb(struct toyfs_inode_info *tii,
					loff_t off);
uint64_t toyfs_require_pmem_bn(struct toyfs_inode_info *tii, loff_t off);

/* symlink.c */
void toyfs_release_symlink(struct toyfs_inode_info *tii);
int toyfs_get_symlink(struct zus_inode_info *zii, void **symlink);

/* namei.c */
ulong toyfs_lookup(struct zus_inode_info *dir_ii, struct zufs_str *str);
int toyfs_rename(struct zufs_ioc_rename *zir);

/* xattr.c */
int toyfs_getxattr(struct zus_inode_info *zii, struct zufs_ioc_xattr *);
int toyfs_setxattr(struct zus_inode_info *zii, struct zufs_ioc_xattr *);
int toyfs_listxattr(struct zus_inode_info *zii, struct zufs_ioc_xattr *);
void toyfs_drop_xattr(struct toyfs_inode_info *tii);

/* mmap.c */
int toyfs_get_block(struct zus_inode_info *zii, struct zufs_ioc_IO *get_block);
int toyfs_put_block(struct zus_inode_info *zii, struct zufs_ioc_IO *get_block);
int toyfs_mmap_close(struct zus_inode_info *zii,
		     struct zufs_ioc_mmap_close *mmap_close);

/* common.c */
void toyfs_panicf(const char *file, int line, const char *fmt, ...);
void toyfs_mutex_init(pthread_mutex_t *mutex);
void toyfs_mutex_destroy(pthread_mutex_t *mutex);
void toyfs_mutex_lock(pthread_mutex_t *mutex);
void toyfs_mutex_unlock(pthread_mutex_t *mutex);
struct toyfs_sb_info *toyfs_zsbi_to_sbi(struct zus_sb_info *zsbi);
struct toyfs_inode_info *toyfs_zii_to_tii(struct zus_inode_info *zii);
extern const struct zus_sbi_operations toyfs_sbi_op;
extern const struct zus_zii_operations toyfs_zii_op;

#endif /* TOYFS_H_*/
