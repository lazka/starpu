/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010  Université de Bordeaux 1
 * Copyright (C) 2010  Centre National de la Recherche Scientifique
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */
#ifndef __STARPU_SPINLOCK_H__
#define __STARPU_SPINLOCK_H__

#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <common/utils.h>
#include <common/config.h>

//#ifndef STARPU_SPINLOCK_CHECK
//#define STARPU_SPINLOCK_CHECK	1
//#endif

typedef struct starpu_spinlock_s {
#ifdef STARPU_SPINLOCK_CHECK
	pthread_mutexattr_t errcheck_attr;
	pthread_mutex_t errcheck_lock;
#else
#ifdef HAVE_PTHREAD_SPIN_LOCK
	pthread_spinlock_t lock;
#else
	/* we only have a trivial implementation yet ! */
	uint32_t taken __attribute__ ((aligned(16)));
#endif
#endif
} starpu_spinlock_t;

int _starpu_spin_init(starpu_spinlock_t *lock);
int _starpu_spin_destroy(starpu_spinlock_t *lock);

int _starpu_spin_lock(starpu_spinlock_t *lock);
int _starpu_spin_trylock(starpu_spinlock_t *lock);
int _starpu_spin_unlock(starpu_spinlock_t *lock);

#endif // __STARPU_SPINLOCK_H__
