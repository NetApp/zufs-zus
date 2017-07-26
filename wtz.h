/*
 * wtz.h - Wait Til Zero wait object
 *
 * This is opposite of a semaphore. It arms the object with a count
 * and only the last arrival releases the waiter. Usually used
 * for a barrier, where main thread needs to wait for all workers
 * to finish a stage.
 *
 * TODO: where is the MPI true barrier where also all the workers
 * then wait for the barrier to finish? Probably we can do that with
 * the semaphore or another one, I need to calculate that
 *
 * Copyright (c) 2018 NetApp, Inc. All rights reserved.
 *
 * ZUFS-License: BSD-3-Clause. See module.c for LICENSE details.
 *
 * Authors:
 *	Boaz Harrosh <boaz@plexistor.com>
 */
#ifndef __WTZ_H__
#define __WTZ_H__

#include <semaphore.h>

struct wait_til_zero {
	int acnt;
	sem_t sem;
};

static void wtz_init(struct wait_til_zero *wtz)
{
	__atomic_store_n(&wtz->acnt, 0, __ATOMIC_RELAXED);
	sem_init(&wtz->sem, 0, 0);
}

static int wtz_arm(struct wait_til_zero *wtz, int c)
{
	int prev = __atomic_fetch_add(&wtz->acnt, c,
						__ATOMIC_RELAXED);

	return prev;
}

/* Release one at a time sorry ;-) */
static int wtz_release(struct wait_til_zero *wtz)
{
	int prev = __atomic_fetch_sub(&wtz->acnt, 1,
						__ATOMIC_RELAXED);
	if (prev == 1)
		sem_post(&wtz->sem);

	return prev - 1;
}

/* Wait all arms are released */
static void wtz_wait(struct wait_til_zero *wtz)
{
	sem_wait(&wtz->sem);
}

#endif /* define __WTZ_H__ */
