/*
 * zus.c - A program that calls into the ZUS IOCTL server API
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
#include <unistd.h>
#include <sys/mman.h>
#include <asm-generic/mman.h>

#include "zus.h"
#include "zusd.h"
#include "zuf_call.h"
#include "wtz.h"
#include "iom_enc.h"

const char* g_zus_root_path;

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

struct _zu_thread {
	pthread_t thread;
	int no;
	int err;
	int fd;
	void *api_mem;
	volatile bool stop;
};

/* TODO: Put all these g_xx(s) on a zus object and point to it from
 * _zu_thread. Then be Boaz Happy
 */
static struct _zu_thread *g_zts = NULL;
static int g_num_gts = 0;
static struct wait_til_zero g_wtz;
static pthread_key_t g_zts_id_key;
static struct fba g_wait_structs;

int zus_getztno(void)
{
	struct _zu_thread *zt;

	zt = (struct _zu_thread *)pthread_getspecific(g_zts_id_key);
	return likely(zt && !zt->err) ? zt->no : -1;
}

typedef unsigned int uint;

static int _zu_mmap(struct _zu_thread *zt)
{
	int prot = PROT_WRITE | PROT_READ;
	int flags = MAP_SHARED;

	zt->api_mem = mmap(NULL, ZUS_API_MAP_MAX_SIZE, prot, flags, zt->fd, 0);
	if (zt->api_mem == MAP_FAILED) {
		ERROR("mmap failed=> %d: %s\n", errno, strerror(errno));
		return errno ?: ENOMEM;
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

static void *zu_thread(void *callback_info)
{
	struct _zu_thread *zt = callback_info;
	struct zufs_ioc_wait_operation *op =
				g_wait_structs.ptr + zt->no * ZUS_MAX_OP_SIZE;

	zt->err = zuf_root_open_tmp(&zt->fd);
	if (zt->err)
		return NULL;

	zt->err = zuf_zt_init(zt->fd, zt->no, ZUS_MAX_OP_SIZE);
	if (zt->err)
		return NULL; /* leak the file it is fine */

	zt->err = _zu_mmap(zt);
	if (zt->err)
		return NULL; /* leak the file it is fine */

	INFO("[%d] thread Init fd=%d api_mem=%p\n",
	     zt->no, zt->fd, zt->api_mem);

	wtz_release(&g_wtz);

	pthread_setspecific(g_zts_id_key, zt);

	while(!zt->stop) {
		zt->err = zuf_wait_opt(zt->fd, op);

		if (zt->err) {
			INFO("zu_thread: err=%d\n", zt->err);
			break;
		}
		op->hdr.err = _errno_UtoK(_do_op(zt, op));
	}

	pthread_setspecific(g_zts_id_key, NULL);

	zuf_root_close(&zt->fd);

	INFO("[%d] thread Exit\n", zt->no);
	return zt;
}

static
int _start_one_zu_thread(struct _zu_thread *zt, struct thread_param *tp,
			 int no, cpu_set_t *affinity)
{
	pthread_attr_t attr;
	int err;

	err = pthread_attr_init(&attr);
	if (unlikely(err)) {
		ERROR("pthread_attr_init => %d: %s\n", err, strerror(err));
		goto error;
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

	err = pthread_attr_setaffinity_np(&attr,sizeof(*affinity), affinity);
	if (unlikely(err)) {
		ERROR("pthread_attr_setaffinity => %d: %s\n", err, strerror(err));
		goto error;
	}

	zt->no = no;
	err = pthread_create(&zt->thread, &attr, &zu_thread, zt);
	pthread_attr_destroy(&attr);

	if (err)  {
		ERROR("pthread_create => %d: %s\n", err, strerror(errno));
		goto error;
	}

	return 0;

error:
	zt->thread = 0;
	zt->err = err;
	return err;
}

static void _dbg_print_affinity(int cpu, cpu_set_t *affinity)
{
	long *pr_aff = (void *)affinity;
	size_t b;

	if (!g_DBG)
		return;

	DBG("cpu[%d]: ", cpu);
	for (b = 0; b < sizeof(*affinity) / sizeof(long); b++)
		if(pr_aff[b])
			DBGCONT(".%04lx", pr_aff[b]);
		else
			DBGCONT(".");
	DBGCONT("(%zd)\n", b);
}

static int zus_start_all_threads(struct thread_param *tp, uint num_cpus)
{
	uint i, err;

	wtz_init(&g_wtz);

	g_zus_root_path = tp->path;
	g_zts = calloc(num_cpus, sizeof(*g_zts));
	if (!g_zts)
		return ENOMEM;
	g_num_gts = num_cpus;
	pthread_key_create(&g_zts_id_key, NULL);

	err = fba_alloc(&g_wait_structs, num_cpus * ZUS_MAX_OP_SIZE);
	if (unlikely(err)) {
		ERROR("fba_alloc => %d\n", err);
		return err;
	}

	wtz_arm(&g_wtz, num_cpus);

	for (i = 0; i < num_cpus; ++i) {
		cpu_set_t affinity;

		CPU_ZERO(&affinity);
		CPU_SET(i, &affinity);
		_dbg_print_affinity(i, &affinity);

		err = _start_one_zu_thread(&g_zts[i], tp, i, &affinity);
		if (err)
			return err;
	}

	wtz_wait(&g_wtz);
	return 0;
}

static void zus_stop_all_threads(void)
{
	void *tret;
	int i;

	if (!g_zts)
		return;

	for (i = 0; i < g_num_gts; ++i)
		g_zts[i].stop = true;

	zuf_break_all(g_zts[0].fd);

	for (i = 0; i < g_num_gts; ++i) {
		struct _zu_thread *zt = &g_zts[i];

		if (zt->thread) {
			pthread_join(zt->thread, &tret);
			zt->thread = 0;
		}
	}

	fba_free(&g_wait_structs);
	free (g_zts);
	g_zts = NULL;
}

/* ~~~~ mount ~~~~~ */
struct _zu_mount_thread {
	struct thread_param tp;
	pthread_t thread;
	int err;
	int fd;
	volatile bool stop;
} g_mount;

struct zufs_ioc_numa_map g_zus_numa_map;

int zus_cpu_to_node(int cpu)
{
	/* TODO(sagi): put this 'if' under WARN_ON */
	if ((cpu < 0) || ((int)g_zus_numa_map.online_cpus < cpu)) {
		ERROR("Bad cpu=%d\n", cpu);
		return 0; /* yell, but do not crash */
	}

	return g_zus_numa_map.cpu_to_node[cpu];
}

int zus_set_numa_affinity(cpu_set_t *affinity, int nid)
{
	uint i;

	CPU_ZERO(affinity);

	for (i = 0; i < g_zus_numa_map.online_cpus; ++i) {
		if (zus_cpu_to_node(i) == nid)
			break;
	}

	if (i == g_zus_numa_map.online_cpus)
		return -EINVAL;

	for (; i < g_zus_numa_map.online_cpus; ++i) {
		if (zus_cpu_to_node(i) != nid)
			break;
		CPU_SET(i, affinity);
	}
	return 0;
}

static int _numa_map_init(int fd)
{
	return zuf_numa_map(fd, &g_zus_numa_map);
}

static void *zus_mount_thread(void *callback_info)
{
	struct zufs_ioc_mount zim = {};

	g_mount.err = zuf_root_open_tmp(&g_mount.fd);
	if (g_mount.err)
		return NULL;

	INFO("Mount thread Running fd=%d\n", g_mount.fd);

	zim.hdr.err = zus_register_all(g_mount.fd);
	if (zim.hdr.err) {
		ERROR("zus_register_all => %d\n", zim.hdr.err);
		return NULL;
	}

	while(!g_mount.stop) {
		g_mount.err = zuf_recieve_mount(g_mount.fd, &zim);
		if (g_mount.err || g_mount.stop) {
			break;
		}

		if (!g_zts) {
			g_mount.err = _numa_map_init(g_mount.fd);
			if (unlikely(g_mount.err))
				break;

			zus_start_all_threads(&g_mount.tp,
					      g_zus_numa_map.online_cpus);
		}

		if (zim.is_umounting) {
			zus_umount(g_mount.fd, &zim);
		} else {
			zus_mount(g_mount.fd, &zim);
		}
	}

	zuf_root_close(&g_mount.fd);

	INFO("Mount thread Exit\n");
	return &g_mount;
}

int zus_mount_thread_start(struct thread_param *tp)
{
	pthread_attr_t attr;
	int err;

	g_mount.tp = *tp;

	err = pthread_attr_init(&attr);
	if (unlikely(err)) {
		ERROR("pthread_attr_init => %d: %s\n", err, strerror(err));
		goto error;
	}

	g_zus_root_path = tp->path;
	err = pthread_create(&g_mount.thread, &attr, &zus_mount_thread,
			     &g_mount);
	pthread_attr_destroy(&attr);

	if (err)  {
		ERROR("pthread_create => %d: %s\n", err, strerror(errno));
		goto error;
	}

	return 0;

error:
	g_mount.thread = 0;
	g_mount.err = err;
	return err;
}

void zus_mount_thread_stop(void)
{
	void *tret;

	zus_stop_all_threads();

	g_mount.stop = true;
	pthread_join(g_mount.thread, &tret);
	g_mount.thread = 0;
}

void zus_join(void)
{
	void *tret;

	pthread_join(g_mount.thread, &tret);
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

	return zuf_iomap_exec(g_mount.fd, ziome);
}
