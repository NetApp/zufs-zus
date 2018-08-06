/*
 * zus-vfs.c - Abstract FS interface that calls into the um-FS
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Yigal Korman <yigalk@netapp.com>
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>

#include "zus.h"
#include "zuf_call.h"

static void *dl_list[ZUS_LIBFS_MAX_NR] = {};

/*
 * TODO: support a comma separated list of names in ZUS_LIBFS_LIST_ENV
 */
static int _load_libfs(int fd)
{
	const char *libfs_env = getenv(ZUS_LIBFS_LIST_ENV);
	char libfs_path[ZUS_LIBFS_MAX_PATH];
	char libfs_register[32];
	int (*register_fs)(int);
	char *dl_err;
	int i, err;

	INFO("%s: %s\n", ZUS_LIBFS_LIST_ENV, libfs_env);
	if (!libfs_env || strnlen(libfs_env, ZUS_LIBFS_MAX_PATH) == 0)
		return 0;

	/* find vacant slot in dl_list */
	for (i = 0; i < ZUS_LIBFS_MAX_NR; ++i) {
		if (!dl_list[i])
			break;
	}
	if (ZUS_LIBFS_MAX_NR == i) {
		ERROR("Reached limit of max loaded libfs (%d)\n",
		      ZUS_LIBFS_MAX_NR);
		return -ENOMEM;
	}

	/* load production path */
	err = snprintf(libfs_path, ZUS_LIBFS_MAX_PATH, "%s/lib%s.so",
		       ZUS_LIBFS_DIR, libfs_env);
	if (err < 0)
		goto fail_load;

	dl_list[i] = dlopen(libfs_path, RTLD_NOW);
	DBG("dlopen(%s) = %p, dlerror=%s\n", libfs_path, dl_list[i], dlerror());
	if (dl_list[i])
		goto found;

	/* load development path */
	err = snprintf(libfs_path, ZUS_LIBFS_MAX_PATH, "lib%s.so", libfs_env);
	if (err < 0)
		goto fail_load;

	dl_list[i] = dlopen(libfs_path, RTLD_NOW);
	DBG("dlopen(%s) = %p, dlerror=%s\n", libfs_path, dl_list[i], dlerror());
	if (!dl_list[i])
		goto fail_load;

found:
	err = snprintf(libfs_register, 32, "%s_register_fs", libfs_env);
	if (err < 0)
		goto fail_sym;

	/* clear existing errors */
	dlerror();

	register_fs = (int (*)(int))dlsym(dl_list[i], libfs_register);
	dl_err = dlerror();
	if (dl_err)
		goto fail_sym;

	err = register_fs(fd);
	if (err) {
		ERROR("%s_register_fs failed => %d\n", libfs_env, err);
		dlclose(dl_list[i]);
		dl_list[i] = NULL;
		return err;
	}

	return 0;

fail_load:
	if (err < 0) {
		ERROR("libfs path construction failed => %d\n", err);
		return err;
	}
	ERROR("load of %s failed => %s\n", libfs_path, dlerror());
	return -ENOENT;

fail_sym:
	if (err < 0)
		ERROR("register_fs construction failed => %d\n", err);
	else
		ERROR("register_fs retrieval failed => %s\n", dl_err);
	dlclose(dl_list[i]);
	dl_list[i] = NULL;
	return err ? err : -EBADF;
}

static void _unload_libfs(void *handle)
{
	int err;

	if (handle) {
		err = dlclose(handle);
		if (err)
			ERROR("dlclose failed => %d\n", err);
	}
}

int zus_register_one(int fd, struct zus_fs_info *zfi)
{
	int err;

	err = zuf_register_fs(fd, zfi);
	if (err)
		return err;

	return 0;
}

/* TODO: We need some registry of all fss to load */
int zus_register_all(int fd)
{
	int err;

	err = foofs_register_fs(fd);
	if (err) {
		ERROR("failed to register foofs: %d\n", err);
		return err;
	}

	err = _load_libfs(fd);
	if (err) {
		ERROR("failed to load dynamic libfs modules => %d\n", err);
		return err;
	}

	return 0;
}

void zus_unregister_all(void)
{
	int i;

	for (i = 0; i < ZUS_LIBFS_MAX_NR; ++i) {
		if (dl_list[i])
			_unload_libfs(dl_list[i]);
	}
}

