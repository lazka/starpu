/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2015-2021  Université de Bordeaux, CNRS (LaBRI UMR 5800), Inria
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

#ifndef __STARPU_SCHEDULER_HETEROPRIO_H__
#define __STARPU_SCHEDULER_HETEROPRIO_H__

#include <starpu.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define STARPU_HETEROPRIO_MAX_PRIO 100

#define STARPU_HETEROPRIO_MAX_PREFETCH 2
#if STARPU_HETEROPRIO_MAX_PREFETCH <= 0
#error STARPU_HETEROPRIO_MAX_PREFETCH == 1 means no prefetch so STARPU_HETEROPRIO_MAX_PREFETCH must >= 1
#endif

/** Tell how many prio there are for a given arch */
void starpu_heteroprio_set_nb_prios(unsigned sched_ctx_id, enum starpu_worker_archtype arch, unsigned max_prio);

/** Set the mapping for a given arch prio=>bucket */
void starpu_heteroprio_set_mapping(unsigned sched_ctx_id, enum starpu_worker_archtype arch, unsigned source_prio, unsigned dest_bucket_id);

/** Tell which arch is the faster for the tasks of a bucket (optional) */
void starpu_heteroprio_set_faster_arch(unsigned sched_ctx_id, enum starpu_worker_archtype arch, unsigned bucket_id);

/** Tell how slow is a arch for the tasks of a bucket (optional) */ 
void starpu_heteroprio_set_arch_slow_factor(unsigned sched_ctx_id, enum starpu_worker_archtype arch, unsigned bucket_id, float slow_factor);

#ifdef __cplusplus
}
#endif

#endif /* __STARPU_SCHEDULER_HETEROPRIO_H__ */
