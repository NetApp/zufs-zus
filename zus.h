/*
 * zus.h - C Wrappers over the ZUFS_IOCTL Api
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */
#ifndef __ZUS_H__
#define __ZUS_H__

#include <stdlib.h>

/* CRAP How do we get to all this S_ISDIR and friends? */
/* FIXME: This will break if zus.h is included after  linux/stat.h somewhere */
#define SAVE___GLIBC__ __GLIBC__
#undef __GLIBC__
#include <linux/stat.h>
#define __GLIBC__ SAVE___GLIBC__

/* This is a nasty hack for getting O_TMPFILE into centos 7.4
 * it already exists on centos7.4 but with a diffrent name
 * the value is the same
 * kernel support was introduced in kernel 3.11
 */

#ifndef O_TMPFILE
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

/* Have STATX_xxx defines if not exist in <linux/stat.h> (e.g., centos7.4) */
#ifndef STATX_TYPE
#define STATX_TYPE              0x00000001U     /* Want/got stx_mode & S_IFMT */
#define STATX_MODE              0x00000002U     /* Want/got stx_mode & ~S_IFMT */
#define STATX_NLINK             0x00000004U     /* Want/got stx_nlink */
#define STATX_UID               0x00000008U     /* Want/got stx_uid */
#define STATX_GID               0x00000010U     /* Want/got stx_gid */
#define STATX_ATIME             0x00000020U     /* Want/got stx_atime */
#define STATX_MTIME             0x00000040U     /* Want/got stx_mtime */
#define STATX_CTIME             0x00000080U     /* Want/got stx_ctime */
#define STATX_INO               0x00000100U     /* Want/got stx_ino */
#define STATX_SIZE              0x00000200U     /* Want/got stx_size */
#define STATX_BLOCKS            0x00000400U     /* Want/got stx_blocks */
#define STATX_BASIC_STATS       0x000007ffU     /* The stuff in the normal stat struct */
#define STATX_BTIME             0x00000800U     /* Want/got stx_btime */
#define STATX_ALL               0x00000fffU     /* All currently supported flags */
#define STATX__RESERVED         0x80000000U     /* Reserved for future struct statx expansion */
#endif


#include "zus_api.h"
#include "_pr.h"
#include "list.h"

/* FIXME: Move to K_in_U include structure */
#ifndef likely
#define likely(x_)	__builtin_expect(!!(x_), 1)
#define unlikely(x_)	__builtin_expect(!!(x_), 0)
#endif

extern bool g_verify;
#define MAX_LFS_FILESIZE 	((loff_t)0x7fffffffffffffffLL)

/* File backed Allocator - Gives user an allocated pointer
 * which is derived from a /tmp/O_TMPFILE mmap. The size
 * is round up to 4K alignment.
 */
struct fba {
	int fd; void *ptr;
};
int  fba_alloc(struct fba *fba, size_t size);
void fba_free(struct fba *fba);

/* ~~~~ pmem ~~~~ */

/* Each FS-type can decide what size to have for the page */
struct zus_pmem_page {
	ulong flags; /* numa_id, bits*/
	ulong user_info[0];
};

/* pmem access. One for each zus_super_block */
/* use one of nv.h for movnt or cl_flush(ing) access */
struct zus_pmem {
	struct zufs_ioc_pmem pmem_info; /* As received from Kernel */

	void *p_pmem_addr;
	int fd;
	uint user_page_size;
	struct fba pages;
};

static inline ulong pmem_blocks(struct zus_pmem *pmem)
{
	return pmem->pmem_info.pmem_total_blocks;
}

static inline ulong pmem_p2o(ulong bn)
{
	return (ulong)bn << PAGE_SHIFT;
}

static inline ulong pmem_o2p(ulong offset)
{
	return offset >> PAGE_SHIFT;
}

static inline ulong pmem_o2p_up(ulong offset)
{
	return pmem_o2p(offset + PAGE_SIZE - 1);
}

static inline
void *pmem_addr(struct zus_pmem *pmem, ulong o)
{
	if (unlikely(!o))
		return NULL;

	return pmem->p_pmem_addr + o;
}

static inline void *pmem_baddr(struct zus_pmem *pmem, ulong bn)
{
	return pmem_addr(pmem, pmem_p2o(bn));
}

static inline ulong pmem_addr_2_offset(struct zus_pmem *pmem, void *ptr)
{
	ulong off = ptr - pmem->p_pmem_addr;
	/*
	if (unlikely((off < 0) || (pmem_p2o(pmem_blocks(pmem)))))
		return ~0;
	*/

	return off;
}

static inline uint pmem_numa_id(struct zus_pmem *pmem, ulong bn)
{
	return 0;
}

/* Not all users need this */
void pmem_set_numa_id_in_pages(struct zus_pmem *pmem);

static inline
zu_dpp_t pmem_dpp_t(ulong offset) { return (zu_dpp_t)offset; }


/* ~~~~ zus fs_info super_blocks inodes ~~~~ */

struct zus_fs_info;
struct zus_sb_info;

struct zus_zii_operations {
	void (*evict)(struct zus_inode_info *zii);
	int (*read)(void *app_ptr, struct zufs_ioc_IO *io);
	int (*write)(void *app_ptr, struct zufs_ioc_IO *io);
	int (*get_block)(struct zus_inode_info *zii,
			 struct zufs_ioc_get_block *get_block);
	int (*get_symlink)(struct zus_inode_info *zii, void **symlink);
	int (*setattr)(struct zus_inode_info *zii,
		       uint enable_bits, ulong truncate_size);
	int (*sync)(struct zus_inode_info *zii,
		    struct zufs_ioc_range *ioc_range);
	int (*fallocate)(struct zus_inode_info *zii,
			 struct zufs_ioc_range *ioc_range);
	int (*seek)(struct zus_inode_info *zii, struct zufs_ioc_seek *ioc_seek);
};

struct zus_inode_info {
	const struct zus_zii_operations *op;

	struct zus_sb_info *sbi;
	struct zus_inode *zi;
};

struct zus_sbi_operations {
	struct zus_inode_info *(*zii_alloc)(struct zus_sb_info *sbi);
	void (*zii_free)(struct zus_inode_info *zii);

	int (*new_inode)(struct zus_sb_info *sbi, struct zus_inode_info *zii,
			 void *app_ptr, struct zufs_ioc_new_inode *ioc_new);
	int (*free_inode)(struct zus_inode_info *zii);

	ulong (*lookup)(struct zus_inode_info *dir_ii, struct zufs_str *str);
	int (*add_dentry)(struct zus_inode_info *dir_ii,
			  struct zus_inode_info *zii, struct zufs_str *str);
	int (*remove_dentry)(struct zus_inode_info *dir_ii,
			     struct zufs_str *str);
	int (*iget)(struct zus_sb_info *sbi, struct zus_inode_info *zii,
			   ulong ino);
	int (*rename)(struct zufs_ioc_rename *zir);
	int (*readdir)(void *app_ptr, struct zufs_ioc_readdir *zir);
	int (*clone)(struct zufs_ioc_clone *ioc_clone);
	int (*statfs)(struct zus_sb_info *sbi,
		      struct zufs_ioc_statfs *ioc_statfs);
};

struct zus_sb_info {
	struct zus_pmem		pmem;
	struct zus_fs_info	*zfi;
	const struct zus_sbi_operations *op;

	struct zus_inode_info	*z_root;
	ulong			flags;

	struct list_head	sb_list;
};

enum E_zus_sbi_flags {
	ZUS_SBIF_ERROR = 0,

	ZUS_SBIF_LAST,
};

static inline void __set_bit(uint flag, ulong *val) { *val |= (1 << flag); }
static inline
void zus_sbi_flag_set(struct zus_sb_info *sbi, int flag)
{
	__set_bit(flag, &sbi->flags);
}

struct zus_zfi_operations {
	struct zus_sb_info *(*sbi_alloc)(struct zus_fs_info *zfi);
	void (*sbi_free)(struct zus_sb_info *sbi);

	int (*sbi_init)(struct zus_sb_info *sbi, struct zufs_ioc_mount *zim);
	int (*sbi_fini)(struct zus_sb_info *sbi);
};

struct zus_fs_info {
	struct register_fs_info rfi;
	const struct zus_zfi_operations *op;
	const struct zus_sbi_operations *sbi_op;

	uint			user_page_size;
	uint			next_sb_id;
	struct list_head	sb_list; /* All mounts belong to this fs-type */
};

static inline
struct m1fs_super_block *zus_mdt(struct zus_sb_info *sbi)
{
	ulong dt_offset = sbi->zfi->rfi.dt_offset;

	if (unlikely(~0UL == dt_offset))
		return NULL;

	return (sbi->pmem.p_pmem_addr + dt_offset);
}

/* POSIX protocol helpers every one must use */

static inline bool zi_isdir(const struct zus_inode *zi)
{
	return S_ISDIR(zi->i_mode);
}
static inline bool zi_isreg(const struct zus_inode *zi)
{
	return S_ISREG(zi->i_mode);
}
static inline bool zi_islnk(const struct zus_inode *zi)
{
	return S_ISLNK(zi->i_mode);
}
static inline ulong zi_ino(const struct zus_inode *zi)
{
	return zi->i_ino;
}

/* Caller checks if (zi_isdir(zi)) */
static inline void zus_std_new_dir(struct zus_inode *dir_zi, struct zus_inode *zi)
{
	/* Directory points to itself (POSIX for you) */
	zi->i_dir.parent = dir_zi->i_ino;
	zi->i_nlink = 1;
}

static inline void zus_std_add_dentry(struct zus_inode *dir_zi,
				     struct zus_inode *zi)
{
	++zi->i_nlink;

	if (zi_isdir(zi))
		++dir_zi->i_nlink;
}

static inline void zus_std_remove_dentry(struct zus_inode *dir_zi,
					struct zus_inode *zi)
{
	if (zi_isdir(zi))
		--dir_zi->i_nlink;

	--zi->i_nlink;
}

/* zus-core */

/* Open an O_TMPFILE on the zuf-root we belong to */
int zuf_root_open_tmp(int *fd);
void zuf_root_close(int *fd);
int zus_getztno(void);

/* zus-vfs.c */
int zus_register_all(int fd);
int zus_register_one(int fd, struct zus_fs_info *p_zfi);

int zus_mount(int fd, struct zufs_ioc_mount *zim);
int zus_umount(int fd, struct zufs_ioc_mount *zim);
struct zus_inode_info *zus_iget(struct zus_sb_info *sbi, ulong ino);
int zus_do_command(void *app_ptr, struct zufs_ioc_hdr *hdr);

/* foofs.c */
int foofs_register_fs(int fd);

#endif /* define __ZUS_H__ */
