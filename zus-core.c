/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * zus-core.c - A program that calls into the ZUS IOCTL server API
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boazh@netapp.com>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <asm-generic/mman.h>
#include <linux/limits.h>

#include "zus.h"
#include "zusd.h"
#include "zuf_call.h"
#include "wtz.h"
#include "iom_enc.h"

/* TODO: Where to put */
typedef unsigned int uint;

/* ~~~ zuf-root files ~~~ */
char g_zus_root_path_stor[PATH_MAX];
const char const *g_zus_root_path = g_zus_root_path_stor;

ulong g_DBGMASK = 0;

int zuf_root_open_tmp(int *fd)
{
	/* RDWR also for the mmap */
	int o_flags = O_RDWR | O_TMPFILE | O_EXCL;

	*fd = open(g_zus_root_path, o_flags, 0666);
	if (*fd < 0) {
		ERROR("Error opening <%s>: flags=0x%x, %s\n",
		      g_zus_root_path, o_flags, strerror(errno));
		return errno;
	}

	return 0;
}

void zuf_root_close(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

/* ~~~ CPU & NUMA topology by zus ~~~ */

#define __ZT_PLEASE_FREE 1
struct zus_base_thread {
	__start_routine threadfn;
	void *user_arg;

	uint one_cpu;
	uint nid;
	pthread_t thread;
	ulong flags;
	int err;
};

static pthread_key_t g_zts_id_key;
struct zufs_ioc_numa_map g_zus_numa_map;

static int _numa_map_init(int fd)
{
	return zuf_numa_map(fd, &g_zus_numa_map);
}

static inline bool ___BAD_CPU(uint cpu)
{
	/* TODO: WARN_ON_ONCE */
	if (ZUS_WARN_ON(g_zus_numa_map.online_cpus < cpu)) {
		ERROR("Bad cpu=%u\n", cpu);
		return true; /* yell, but do not crash */
	}

	return false;
}
#define BAD_CPU(cpu) (unlikely(___BAD_CPU(cpu)))

int zus_cpu_to_node(int cpu)
{
	if (BAD_CPU(cpu))
		return 0; /* Yell but don't crash */

	return g_zus_numa_map.cpu_to_node[cpu];
}

int zus_current_onecpu(void)
{
	struct zus_base_thread *zbt = pthread_getspecific(g_zts_id_key);

	if (!zbt)
		return ZUS_CPU_ALL;
	return  zbt->one_cpu;
}

static int __zus_current_cpu(bool warn)
{
	struct zus_base_thread *zbt = pthread_getspecific(g_zts_id_key);

	if (ZUS_WARN_ON(!zbt)) /* not created by us */
		return sched_getcpu();

	ZUS_WARN_ON_ONCE(warn && (zbt->one_cpu == ZUS_CPU_ALL));
	if (zbt->one_cpu == ZUS_CPU_ALL)
		return sched_getcpu();

	return zbt->one_cpu;
}

int zus_current_cpu(void)
{
	return __zus_current_cpu(true);
}

int zus_current_nid(void)
{
	struct zus_base_thread *zbt = pthread_getspecific(g_zts_id_key);

	if (ZUS_WARN_ON(!zbt)) /* not created by us */
		return zus_cpu_to_node(sched_getcpu());

	if (ZUS_WARN_ON_ONCE(zbt->nid == ZUS_NUMA_NO_NID))
		return zus_cpu_to_node(sched_getcpu());

	return zbt->nid;
}


static int zus_set_numa_affinity(cpu_set_t *affinity, int nid)
{
	uint i;

	CPU_ZERO(affinity);

	for (i = 0; i < g_zus_numa_map.online_cpus; ++i) {
		if (zus_cpu_to_node(i) == nid)
			break;
	}

	if (i == g_zus_numa_map.online_cpus) {
		ERROR("Wrong nid=%d\n", nid);
		return -EINVAL;
	}

	for (; i < g_zus_numa_map.online_cpus; ++i) {
		if (zus_cpu_to_node(i) != nid)
			break;
		CPU_SET(i, affinity);
	}
	return 0;
}

static void zus_set_onecpu_affinity(cpu_set_t *affinity, uint cpu)
{
	CPU_ZERO(affinity);
	CPU_SET(cpu, affinity);
}

static void *zus_glue_thread(void *__zt)
{
	struct zus_base_thread *zbt = __zt;
	void *ret;

	pthread_setspecific(g_zts_id_key, zbt);

	ret = zbt->threadfn(zbt->user_arg);

	pthread_setspecific(g_zts_id_key, NULL);
	if (zbt->flags & __ZT_PLEASE_FREE)
		free(zbt);

	return ret;
}

/* @zbt comes ZERO(ed), Safe zbt->flags maybe set and are untouched */
static
int __zus_thread_create(struct zus_base_thread *zbt, struct zus_thread_params *tp,
		        __start_routine fn, void *user_arg)
{
	pthread_attr_t attr;
	int err;

	zbt->threadfn = fn;
	zbt->user_arg = user_arg;
	zbt->one_cpu = ZUS_CPU_ALL;
	zbt->nid = ZUS_NUMA_NO_NID;

	err = pthread_attr_init(&attr);
	if (unlikely(err)) {
		ERROR("pthread_attr_init => %d: %s\n", err, strerror(err));
		return zbt->err = err;
	}

	err = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	if (unlikely(err)) {
		ERROR("pthread_attr_setinheritsched => %d: %s\n",
		      err, strerror(err));
		goto error;
	}

	if (tp->policy != SCHED_OTHER) {
		struct sched_param sp = {
			.__sched_priority = tp->rr_priority,
		};

		err = pthread_attr_setschedpolicy(&attr, tp->policy);
		if (unlikely(err)) {
			ERROR("pthread_attr_setschedpolicy => %d: %s\n",
			      err, strerror(err));
			goto error;
		}

		err = pthread_attr_setschedparam(&attr, &sp);
		if (unlikely(err)) {
			ERROR("pthread_attr_setschedparam => %d: %s\n",
			      err, strerror(err));
			goto error;
		}
	} /* else set nice */

	if (tp->one_cpu != ZUS_CPU_ALL || tp->nid != ZUS_NUMA_NO_NID) {
		cpu_set_t affinity;

		if (tp->one_cpu != ZUS_CPU_ALL) {
			zus_set_onecpu_affinity(&affinity, tp->one_cpu);
			zbt->one_cpu = tp->one_cpu;
			zbt->nid = zus_cpu_to_node(tp->one_cpu);
		} else {
			err = zus_set_numa_affinity(&affinity, tp->nid);
			if (unlikely(err))
				goto error;
			zbt->nid = tp->nid;
		}

		err = pthread_attr_setaffinity_np(&attr, sizeof(affinity),
						  &affinity);
		if (unlikely(err)) {
			ERROR("pthread_attr_setaffinity => %d: %s\n", err, strerror(err));
			goto error;
		}
	}

	err = pthread_create(&zbt->thread, &attr, zus_glue_thread, zbt);
	if (err)  {
		ERROR("pthread_create => %d: %s\n", err, strerror(err));
		goto error;
	}
	pthread_attr_destroy(&attr);

	if (tp->name) {
		err = pthread_setname_np(zbt->thread, tp->name);
		if (err) {
			char tname[32];
			pthread_getname_np(zbt->thread, tname, 32);
			ERROR("pthread_setname_np(%s) => %d\n", tname, err);
		}
	}
	return 0;

error:
	pthread_attr_destroy(&attr);
	zbt->thread = 0;
	return zbt->err = err;
}

int zus_thread_create(pthread_t *new_tread, struct zus_thread_params *tp,
		      __start_routine fn, void *user_arg)
{
	struct zus_base_thread *zbt = calloc(1, sizeof(*zbt));
	int err;

	if (unlikely(!zbt))
		return -ENOMEM;

	zbt->flags = __ZT_PLEASE_FREE;
	err = __zus_thread_create(zbt, tp, fn, user_arg);
	if (unlikely(err)) {
		free (zbt);
		return err;
	}

	*new_tread = zbt->thread;
	return 0;
}

/* ~~~ Zu Threads ZT(s) ~~~ */
/* These are the zus-core dispatchers threads */

struct _zu_thread {
	struct zus_base_thread zbt;

	uint no;	/* TODO: just the zbt->one_cpu above, please rename */
	uint chan;
	int fd;
	void *api_mem;
	volatile bool stop;
};

struct zt_pool {
	struct wait_til_zero wtz;
	struct fba wait_fba[ZUFS_MAX_ZT_CHANNELS];
	struct _zu_thread *zts[ZUFS_MAX_ZT_CHANNELS];
	int num_zts;
	uint max_channels;
};

static struct zt_pool g_ztp;

static int _zu_mmap(struct _zu_thread *zt)
{
	int prot = PROT_WRITE | PROT_READ;
	int flags = MAP_SHARED;

	zt->api_mem = mmap(NULL, ZUS_API_MAP_MAX_SIZE, prot, flags, zt->fd, 0);
	if (zt->api_mem == MAP_FAILED) {
		ERROR("mmap failed=> %d: %s\n", errno, strerror(errno));
		return -(errno ?: ENOMEM);
	}

	return 0;
}

static
int _do_op(struct _zu_thread *zt, struct zufs_ioc_wait_operation *op)
{
	void *app_ptr = zt->api_mem + op->hdr.offset;

	return zus_do_command(app_ptr, &op->hdr);
}

/*
 * Converts user-space error code to kernel conventions: change positive errno
 * codes to negative.
 */
static __s32 _errno_UtoK(__s32 err)
{
	return (err < 0) ? err : -err;
}

static void *_zu_thread(void *callback_info)
{
	struct _zu_thread *zt = callback_info;
	struct zufs_ioc_wait_operation *op =
				g_ztp.wait_fba[zt->chan].ptr +
						zt->no * ZUS_MAX_OP_SIZE;

	zt->zbt.err = zuf_root_open_tmp(&zt->fd);
	if (zt->zbt.err)
		return NULL;

	zt->zbt.err = zuf_zt_init(zt->fd, zt->no, zt->chan, ZUS_MAX_OP_SIZE);
	if (zt->zbt.err)
		return NULL; /* leak the file it is fine */

	zt->zbt.err = _zu_mmap(zt);
	if (zt->zbt.err)
		return NULL; /* leak the file it is fine */

	DBG("[%u] thread Init fd=%d api_mem=%p\n",
	     zt->no, zt->fd, zt->api_mem);

	wtz_release(&g_ztp.wtz);

	while(!zt->stop) {
		zt->zbt.err = zuf_wait_opt(zt->fd, op);

		if (zt->zbt.err) {
			DBG("zu_thread: err=%d\n", zt->zbt.err);
			/* Do not break continue and let zt->stop say if to exit
			 * Otherwise any kill of an app will exit the ZT
			 * and channel is stuck.
			 */
		}
		op->hdr.err = _errno_UtoK(_do_op(zt, op));
	}

	zuf_root_close(&zt->fd);

	DBG("[%u] thread Exit\n", zt->no);
	return zt;
}

static int _zus_start_chan_threads(struct zus_thread_params *tp, uint num_cpus,
				   uint chan)
{
	uint i, err;

	g_ztp.zts[chan] = calloc(num_cpus, sizeof(*g_ztp.zts[0]));
	if (!g_ztp.zts[chan])
		return -ENOMEM;

	err = fba_alloc(&g_ztp.wait_fba[chan], num_cpus * ZUS_MAX_OP_SIZE);
	if (unlikely(err)) {
		ERROR("fba_alloc => %u\n", err);
		return err;
	}

	wtz_arm(&g_ztp.wtz, num_cpus);

	for (i = 0; i < num_cpus; ++i) {
		char zt_name[32];
		struct _zu_thread *zt = &g_ztp.zts[chan][i];

		snprintf(zt_name, sizeof(zt_name), "ZT(%d.%d)", i, chan);
		tp->name = zt_name;
		zt->no = tp->one_cpu = i;
		zt->chan = chan;
		err = __zus_thread_create(&zt->zbt, tp, _zu_thread, zt);
		if (err)
			return err;
	}

	return 0;
}

/* forward declaration */
static void zus_stop_all_threads(void);

static int zus_start_all_threads(struct zus_thread_params *tp, uint num_cpus,
				 uint num_chans)
{
	uint c;
	int err;

	wtz_init(&g_ztp.wtz);
	g_ztp.num_zts = num_cpus;
	g_ztp.max_channels = num_chans;

	for (c = 0; c < g_ztp.max_channels; ++c) {
		err = _zus_start_chan_threads(tp, num_cpus, c);
		if (unlikely(err))
			goto fail;
	}

	wtz_wait(&g_ztp.wtz);
	INFO("%u ZT threads ready\n", num_cpus);
	return 0;

fail:
	zus_stop_all_threads();
	return err;
}

static void _zus_stop_chan_threads(uint chan)
{
	void *tret;
	int i;

	if (!g_ztp.zts[chan])
		return;

	for (i = 0; i < g_ztp.num_zts; ++i)
		g_ztp.zts[chan][i].stop = true;

	zuf_break_all(g_ztp.zts[chan][0].fd);

	for (i = 0; i < g_ztp.num_zts; ++i) {
		struct _zu_thread *zt = &g_ztp.zts[chan][i];

		if (zt->zbt.thread) {
			pthread_join(zt->zbt.thread, &tret);
			zt->zbt.thread = 0;
		}
	}

	fba_free(&g_ztp.wait_fba[chan]);
	free (g_ztp.zts[chan]);
	g_ztp.zts[chan] = NULL;
}

static void zus_stop_all_threads(void)
{
	uint c;

	for (c = 0; c < g_ztp.max_channels; ++c)
		_zus_stop_chan_threads(c);

	memset(&g_ztp, 0, sizeof(g_ztp));
}

/* ~~~~ mount thread ~~~~~ */
struct _zu_mount_thread {
	struct zus_base_thread zbt;
	struct zus_thread_params tp;

	int fd;
	volatile bool stop;
	struct _zu_thread mnt_th;
} g_mount = {};

static void *zus_mount_thread(void *callback_info)
{
	struct fba fba = {};
	struct zufs_ioc_mount *zim;
	int err;

	g_mount.zbt.err = fba_alloc(&fba, ZUS_MAX_OP_SIZE);
	if (unlikely(g_mount.zbt.err))
		return (void *)((long)g_mount.zbt.err);

	zim = fba.ptr;

	g_mount.zbt.err = zuf_root_open_tmp(&g_mount.fd);
	if (g_mount.zbt.err)
		goto out;

	INFO("Mount thread Running fd=%d\n", g_mount.fd);

	g_mount.zbt.err = _numa_map_init(g_mount.fd);
	if (unlikely(g_mount.zbt.err)) {
		ERROR("_numa_map_init => %d\n", g_mount.zbt.err);
		goto out;
	}

	g_mount.zbt.err = zus_register_all(g_mount.fd);
	if (g_mount.zbt.err) {
		ERROR("zus_register_all => %d\n", g_mount.zbt.err);
		goto out;
	}

	while(!g_mount.stop) {
		g_mount.zbt.err = zuf_recieve_mount(g_mount.fd, zim);
		if (g_mount.zbt.err || g_mount.stop) {
			break;
		}

		if (zim->hdr.operation == ZUS_M_MOUNT && !g_ztp.num_zts) {
			err = zus_start_all_threads(&g_mount.tp,
						    g_zus_numa_map.online_cpus,
						    zim->zmi.num_channels);
			if (unlikely(err))
				goto next;
		}
		switch (zim->hdr.operation) {
		case ZUS_M_MOUNT:
			err = zus_mount(g_mount.fd, zim);
			break;
		case ZUS_M_UMOUNT:
			err = zus_umount(g_mount.fd, zim);
			break;
		case ZUS_M_REMOUNT:
			err = zus_remount(g_mount.fd, zim);
			break;
		case ZUS_M_DDBG_RD:
			err = zus_ddbg_read(&zim->zdi);
			break;
		case ZUS_M_DDBG_WR:
			err = zus_ddbg_write(&zim->zdi);
			break;
		default:
			err = -EINVAL;
		}
next:
		zim->hdr.err = _errno_UtoK(err);
	}

	zuf_root_close(&g_mount.fd);

out:
	fba_free(&fba);
	INFO("Mount thread Exit\n");
	return (void *)((long)g_mount.zbt.err);
}

int zus_mount_thread_start(struct zus_thread_params *tp, const char* zuf_path)
{
	struct zus_thread_params mnttp = {};
	int err;

	pthread_key_create(&g_zts_id_key, NULL);
	strncpy(g_zus_root_path_stor, zuf_path,
		sizeof(g_zus_root_path_stor) - 1);
	g_mount.tp = *tp; /* this is for the _zu threads */

	ZTP_INIT(&mnttp); /* Just a Plain thread */

	mnttp.name = "zus_mounter";

	err = __zus_thread_create(&g_mount.zbt, &mnttp, &zus_mount_thread,
				  &g_mount);
	if (err)  {
		ERROR("zus_thread_create => %d: %s\n", err, strerror(errno));
		return err;
	}

	/* We assume all per_cpu objects are per super_block.
	 * so since there is only one mnt_thread and any object handling
	 * is before any zt(s) start to operate on these objects we tell
	 * the system we are cpu-0 with out any locks
	 */
	g_mount.zbt.one_cpu = 0;
	g_mount.zbt.nid = 0;

	return 0;
}

void zus_mount_thread_stop(void)
{
	void *tret;

	zus_stop_all_threads();

	g_mount.stop = true;
	pthread_join(g_mount.zbt.thread, &tret);
	g_mount.zbt.thread = 0;

	zus_unregister_all();
}

void zus_join(void)
{
	void *tret;

	pthread_join(g_mount.zbt.thread, &tret);
}

/* ~~~ callbacks from FS code into kernel ~~~ */

int __zus_iom_exec(struct zus_sb_info *sbi, struct zufs_ioc_iomap_exec *ziome,
		   bool sync)
{
	if (ZUS_WARN_ON(!ziome))
		return -EFAULT;

	ziome->sb_id = sbi->kern_sb_id;
	ziome->zus_sbi = sbi;
	ziome->wait_for_done = sync;

	DBG("ziome->sb_id=%lld, iom_n=0x%x [0x%llx, 0x%llx, 0x%llx, 0x%llx]\n",
	    ziome->sb_id, ziome->ziom.iom_n, ziome->ziom.iom_e[0],
	    ziome->ziom.iom_e[1], ziome->ziom.iom_e[2], ziome->ziom.iom_e[3]);

	return zuf_iomap_exec(g_mount.fd, ziome);
}
