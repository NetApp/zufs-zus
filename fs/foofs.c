/*
 * foofs.c - A do nothing example of an zuFS FS
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dirent.h>
#include <time.h>

#include "zus.h"
#include "b-minmax.h"

// #define FOO_DEF_SBI_MODE (S_IRUGO | S_IXUGO | S_IWUSR)
#define FOOFS_ROOT_NO 1
#define FOOFS_INODES_RATIO 20
#define FOOFS_INO_PER_BLOCK (PAGE_SIZE / ZUFS_INODE_SIZE)

/* FooFS uses mkfs.m1fs for the device table so
 * keep the info sync with most current m1fs.
 * NOTE: If you use a single t1 device then u do not need
 * any version or device table. FooFS is very destructive
 * to a single t1 in that case. (Silently destroy anything there)
 */
enum {
	M1FS_MAJOR_VERSION	= 15,
	M1FS_MINOR_VERSION	= 1,
	M1FS_SUPER_MAGIC	= 0x5346314d /* M1FS in BE */
};

static struct zus_inode *find_zi(struct zus_sb_info *sbi, ulong ino)
{
	struct zus_inode *zi_array = pmem_baddr(&sbi->pmem, 1);

	return &zi_array[ino];
}

static struct zus_inode *find_free_ino(struct zus_sb_info *sbi)
{
	struct zus_inode *zi_array = pmem_baddr(&sbi->pmem, 1);
	ulong max_ino = pmem_blocks(&sbi->pmem) / FOOFS_INODES_RATIO *
							FOOFS_INO_PER_BLOCK;
	ulong i;

	for (i = 1; i < max_ino; ++i) {
		if (!zi_array[i].i_mode) {
			zi_array[i].i_ino = i;
			return &zi_array[i];
		}
	}

	return NULL;
}

static ulong _get_fill(struct zus_sb_info *sbi)
{
	struct zus_inode *zi_array = pmem_baddr(&sbi->pmem, 1);
	ulong max_ino = pmem_blocks(&sbi->pmem) / FOOFS_INODES_RATIO *
							FOOFS_INO_PER_BLOCK;
	ulong used_files = 0;
	ulong i;

	for (i = 1; i < max_ino; ++i) {
		if (zi_array[i].i_mode)
			++used_files;
	}

	return used_files;
}

enum {MAX_NAME = 16};
enum {MAX_ENTS = PAGE_SIZE /  (MAX_NAME + 8)};
struct foofs_dir {
	struct __foo_dir_ent {
		ulong ino;
		char name[MAX_NAME];
	} ents[MAX_ENTS];
};

static struct foofs_dir *_foo_dir(struct zus_inode_info *dir_ii)
{
	return pmem_baddr(&dir_ii->sbi->pmem,  dir_ii->zi->i_ino + 1);
}

static struct __foo_dir_ent *_find_de(struct zus_inode_info *dir_ii,
				      struct zufs_str *str)
{
	struct foofs_dir *dir;
	int i;

	dir = _foo_dir(dir_ii);
	for (i = 0; i < MAX_ENTS; ++i)
		if (0 == strncmp(dir->ents[i].name, str->name, str->len))
			return &dir->ents[i];
	return NULL; /* NOT FOUND */
}

static struct __foo_dir_ent *_find_empty_de(struct zus_inode_info *dir_ii)
{
	struct foofs_dir *dir;
	int i;

	dir = _foo_dir(dir_ii);
	for (i = 0; i < MAX_ENTS; ++i)
		if (!dir->ents[i].ino)
			return &dir->ents[i];
	return NULL; /* ENOSPC */
}

static void _init_root(struct zus_sb_info *sbi)
{
	struct zus_inode *root = find_zi(sbi, FOOFS_ROOT_NO);
	struct timespec now;
	void *root_dir;

	memset(root, 0, sizeof(*root));

	root->i_ino = FOOFS_ROOT_NO;
	root->i_nlink = 2;
	root->i_mode = S_IFDIR | 0644;
	root->i_uid = 0;
	root->i_gid = 0;

	clock_gettime(CLOCK_REALTIME, &now);
	timespec_to_zt(&root->i_atime, &now);
	timespec_to_zt(&root->i_mtime, &now);
	timespec_to_zt(&root->i_ctime, &now);

	root->i_size = PAGE_SIZE;
	root->i_blocks = 1;
	root_dir = pmem_baddr(&sbi->pmem, FOOFS_ROOT_NO + 1);
	memset(root_dir, 0, PAGE_SIZE);
}

/* ~~~~~~~~~~~~~~~~ Vectors ~~~~~~~~~~~~~~~~~~~~~*/
static const struct zus_zii_operations	foofs_zii_operations;
static const struct zus_sbi_operations	foofs_sbi_operations;
static const struct zus_zfi_operations	foofs_zfi_operations;

/* ~~~~ foofs_sbi_operations ~~~~ */
static
struct zus_sb_info *foofs_sbi_alloc(struct zus_fs_info *zfi)
{
	struct zus_sb_info *sbi = calloc(1, sizeof(struct zus_sb_info));

	if (!sbi)
		return NULL;

	sbi->op = &foofs_sbi_operations;
	return sbi;
}

static void foofs_sbi_free(struct zus_sb_info *sbi)
{
	free(sbi);
}

static
int foofs_sbi_init(struct zus_sb_info *sbi, struct zufs_ioc_mount *zim)
{
	_init_root(sbi);
	sbi->z_root = zus_iget(sbi, FOOFS_ROOT_NO);
	if (unlikely(!sbi->z_root))
		return -ENOMEM;

	return 0;
}

static int foofs_sbi_fini(struct zus_sb_info *sbi)
{
	// zus_iput(sbi->z_root); was this done already
	return 0;
}

static
struct zus_inode_info *foofs_zii_alloc(struct zus_sb_info *sbi)
{
	struct zus_inode_info *zii = calloc(1, sizeof(struct zus_inode_info));

	if (!zii)
		return NULL;

	zii->op = &foofs_zii_operations;
	return zii;
}

static
void foofs_zii_free(struct zus_inode_info *zii)
{
	free(zii);
}

static int foofs_statfs(struct zus_sb_info *sbi, struct zufs_ioc_statfs *ioc)
{
	uint num_files = _get_fill(sbi);

	ioc->statfs_out.f_type		= M1FS_SUPER_MAGIC;
	ioc->statfs_out.f_bsize		= PAGE_SIZE;

	ioc->statfs_out.f_blocks	= pmem_blocks(&sbi->pmem);
	ioc->statfs_out.f_bfree		= ioc->statfs_out.f_blocks - num_files;
	ioc->statfs_out.f_bavail	= ioc->statfs_out.f_bfree;

	ioc->statfs_out.f_files		= num_files;
	ioc->statfs_out.f_ffree		= MAX_ENTS - num_files;

// 	ioc->statfs_out.f_fsid.val[0]	= 0x17;
// 	ioc->statfs_out.f_fsid.val[1]	= 0x17;

	ioc->statfs_out.f_namelen	= MAX_NAME;

	ioc->statfs_out.f_frsize	= 0; // ???
	ioc->statfs_out.f_flags		= 0; // ????

	memset(ioc->statfs_out.f_spare, 0, sizeof(ioc->statfs_out.f_spare));
	return 0;
}

static int foofs_new_inode(struct zus_sb_info *sbi, struct zus_inode_info *zii,
			   void *app_ptr, struct zufs_ioc_new_inode *ioc_new)
{
	struct zus_inode *zi = find_free_ino(sbi);
	ulong ino;

	if (unlikely(!zi))
		return -ENOSPC;

	zii->zi = zi;

	ino = zi->i_ino;
	*zi = ioc_new->zi;
	zi->i_ino = ino;

	if (zi_isdir(zi)) {
		void *dir = _foo_dir(ioc_new->dir_ii);

		memset(dir, 0, PAGE_SIZE);
		zi->i_size = PAGE_SIZE;
		zi->i_blocks = 1;

		zus_std_new_dir(ioc_new->dir_ii->zi, zi);
	}/* else zi_issym(zi) {
		TODO: long symlink in app_ptr
	}*/

	DBG("[%lld] size=0x%llx, blocks=0x%llx ct=0x%llx mt=0x%llx link=0x%x mode=0x%x\n",
	    zi->i_ino, zi->i_size, zi->i_blocks, zi->i_ctime, zi->i_mtime,
	    zi->i_nlink, zi->i_mode);

	return 0;
}

static int foofs_free_inode(struct zus_inode_info *zii)
{
	DBG("\n");
	zii->zi->i_mode = 0;
	zii->zi->i_ino = 0;
	/* Do we need to clean anything */
	return 0;
}

static int foofs_iget(struct zus_sb_info *sbi, struct zus_inode_info *zii,
		      ulong ino)
{
	zii->op = &foofs_zii_operations;
	zii->zi = find_zi(sbi, ino);

	if (!zii->zi)
		return -ENOENT;

	return 0;
}

static ulong foofs_lookup(struct zus_inode_info *dir_ii, struct zufs_str *str)
{
	struct __foo_dir_ent *de;

	if (!str->len || !str->name[0]) {
		ERROR("lookup NULL string\n");
		return  0;
	}

	DBG("[%.*s]\n", str->len, str->name);

if (str->len == 1)
	DBG("[%d]\n", str->name[0]);

	if (0 == strncmp(".", str->name, str->len))
		return dir_ii->zi->i_ino;
	else if (0 == strncmp("..", str->name, str->len))
		return dir_ii->zi->i_dir.parent;

	de = _find_de(dir_ii, str);
	if (unlikely(!de))
		return 0; /* NOT FOUND */
{
ulong max_ino = pmem_blocks(&dir_ii->sbi->pmem) / FOOFS_INODES_RATIO *
						FOOFS_INO_PER_BLOCK;

	if (unlikely(de->ino > max_ino)) {
		ERROR("HWATTTTT\n");
		return 0;
	}

}

	return de->ino;
}

static int foofs_add_dentry(struct zus_inode_info *dir_ii,
			    struct zus_inode_info *zii, struct zufs_str *str)
{
	uint nl = min_t(uint, MAX_NAME-1, str->len);
	struct __foo_dir_ent *de;

	de = _find_empty_de(dir_ii);
	if (unlikely(!de)) {
		DBG("[%ld] [%.*s] ino=%ld\n",
		    zi_ino(dir_ii->zi), str->len, str->name, de->ino);
		return -ENOSPC;
	}

	memcpy(de->name, str->name, nl);
	de->name[nl]=0; /* C string for prints */
	de->ino = zii->zi->i_ino;
	zus_std_add_dentry(dir_ii->zi, zii->zi);

	DBG("[%ld] [%.*s] ino=%ld\n",
	    zi_ino(dir_ii->zi), str->len, str->name, de->ino);
	return 0;
}

static int foofs_remove_dentry(struct zus_inode_info *dir_ii,
			       struct zufs_str *str)
{
	struct __foo_dir_ent *de;

	DBG("[%ld] [%.*s]\n", zi_ino(dir_ii->zi), str->len, str->name);

	de = _find_de(dir_ii, str);
	if (unlikely(!de))
		return -ENOENT;

	zus_std_remove_dentry(dir_ii->zi, find_zi(dir_ii->sbi, de->ino));
	de->ino = 0;
	de->name[0] = 0;

	return 0;
}

static int foofs_readdir(void *app_ptr, struct zufs_ioc_readdir *zir)
{
	struct zufs_readdir_iter rdi;
	struct foofs_dir *dir;
	uint start = zir->pos / sizeof(struct __foo_dir_ent);
	uint i;

	zufs_readdir_iter_init(&rdi, zir, app_ptr);

	DBG("[0x%ld] pos 0x%lx\n", zi_ino(zir->dir_ii->zi), zir->pos);

	if (zir->pos == 0) {
		zufs_zde_emit(&rdi, zir->dir_ii->zi->i_ino, DT_DIR, 0, ".", 1);
		zir->pos = 1;
	}
	if (zir->pos == 1) {
		zufs_zde_emit(&rdi, zir->dir_ii->zi->i_ino, DT_DIR, 1, "..", 2);
		zir->pos = 2;
	}

	dir = _foo_dir(zir->dir_ii);
	for (i = start; i < MAX_ENTS; ++i) {
		struct __foo_dir_ent *de = &dir->ents[i];
		bool ok;

		zir->pos = i * sizeof(*de);
		if (!de->ino)
			continue;

		ok = zufs_zde_emit(&rdi, de->ino, 1, zir->pos,
				   de->name, strlen(de->name));
		if (unlikely(!ok)) {
			DBG("long dir\n");
			break;
		}
		DBG("	[%ld] <%s>\n", de->ino, de->name);
	}

	return 0;
}

/* ~~~~ foofs_zii_operations ~~~~ */
static void foofs_evict(struct zus_inode_info *zii)
{
}

static int foofs_read(void *ptr, struct zufs_ioc_IO *op)
{
	struct zus_inode_info *zii = op->zus_ii;
	ulong *app_ptr = ptr;
	ulong *app_end = app_ptr + op->hdr.len / sizeof(ulong);
	ulong start = op->filepos / sizeof(ulong);

// 	INFO("READ start=0x%lx len=0x%lx offset=0x%x\n",
// 	     start, op->hdr.len / sizeof(ulong), op->hdr.offset);

	if (zii->zi->i_on_disk.a[0]) {
		*app_ptr = 0xB00DBAAD;
		return 0;
	}

	while(app_ptr < app_end)
		*app_ptr++ = start++;

	return 0;
}

static int foofs_write(void *ptr, struct zufs_ioc_IO *op)
{
	struct zus_inode_info *zii = op->zus_ii;
	ulong *app_ptr = ptr;
	ulong *app_end = app_ptr + op->hdr.len / sizeof(ulong);
	ulong start = op->filepos / sizeof(ulong);
	ulong end_pos = op->filepos + op->hdr.len;

	zii->zi->i_on_disk.a[0] = 0;

	for(;app_ptr < app_end; ++app_ptr, ++start) {
		if (*app_ptr != start) {
			if (g_verify)
				ERROR("*app_ptr(0x%lx) != start(0x%lx) offset=0x%x len=0x%x\n",
					*app_ptr, start, op->hdr.offset, op->hdr.len);
		}
	}

	if (zii->zi->i_size < end_pos)
		zii->zi->i_size = end_pos;

	return 0;
}

static int foofs_get_block(struct zus_inode_info *zii,
			   struct zufs_ioc_get_block *get_block)
{
//	get_block->pmem_bn = _file_bn(zii) + get_block->index % _foo_file_max(zii->sbi);
	/* foo-fs stands for foo-l */
	get_block->pmem_bn = zii->zi->i_ino + 1;
	return 0;
}

static const struct zus_zii_operations foofs_zii_operations = {
	.evict	= foofs_evict,
	.read	= foofs_read,
	.write	= foofs_write,
	.get_block = foofs_get_block,
};

static const struct zus_sbi_operations foofs_sbi_operations = {
	.zii_alloc	= foofs_zii_alloc,
	.zii_free	= foofs_zii_free,
	.new_inode	= foofs_new_inode,
	.free_inode	= foofs_free_inode,

	.lookup		= foofs_lookup,
	.add_dentry     = foofs_add_dentry,
	.remove_dentry  = foofs_remove_dentry,
	.iget		= foofs_iget,

// 	rename		=,
	.readdir 	= foofs_readdir,
// 	clone		=,
	.statfs		= foofs_statfs,
};

static const struct zus_zfi_operations foofs_zfi_operations = {
	.sbi_alloc = foofs_sbi_alloc,
	.sbi_free  = foofs_sbi_free,
	.sbi_init  = foofs_sbi_init,
	.sbi_fini  = foofs_sbi_fini,
};

/* Is not const because it is hanged on a list_head */
static struct zus_fs_info foo_zfi = {
	.rfi.fsname	= "foof",
	.rfi.FS_magic		= M1FS_SUPER_MAGIC,
	.rfi.FS_ver_major	= M1FS_MAJOR_VERSION,
	.rfi.FS_ver_minor	= M1FS_MINOR_VERSION,
	.rfi.dt_offset	= 0,

	.rfi.s_time_gran = 1,
	.rfi.def_mode	= /*FOO_DEF_SBI_MODE*/0755,
	.rfi.s_maxbytes = MAX_LFS_FILESIZE,

	.rfi.acl_on	= 1,

	.op		= &foofs_zfi_operations,
	.sbi_op		= &foofs_sbi_operations,
	.user_page_size = 0,
	.next_sb_id	= 0,
};

int foofs_register_fs(int fd)
{
	return zus_register_one(fd, &foo_zfi);
}
