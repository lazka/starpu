/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011, 2012  Centre National de la Recherche Scientifique
 * Copyright (C) 2011-2012  Université de Bordeaux 1
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

#include <starpu_mpi_private.h>
#include <starpu_mpi_insert_task_cache.h>
#include <common/htable64.h>

struct _starpu_mpi_clear_cache {
        starpu_data_handle_t data;
        int rank;
        int mode;
};

struct starpu_htbl64_node **sent_data = NULL;
struct starpu_htbl64_node **received_data = NULL;

void _starpu_mpi_clear_cache_callback(void *callback_arg)
{
        struct _starpu_mpi_clear_cache *clear_cache = (struct _starpu_mpi_clear_cache *)callback_arg;

        if (clear_cache->mode == _STARPU_MPI_CLEAR_SENT_DATA) {
                _STARPU_MPI_DEBUG("Clearing sent cache for data %p and rank %d\n", clear_cache->data, clear_cache->rank);
                _starpu_htbl_insert_64(&sent_data[clear_cache->rank], (uintptr_t)clear_cache->data, NULL);
        }
        else if (clear_cache->mode == _STARPU_MPI_CLEAR_RECEIVED_DATA) {
                _STARPU_MPI_DEBUG("Clearing received cache for data %p and rank %d\n", clear_cache->data, clear_cache->rank);
                _starpu_htbl_insert_64(&received_data[clear_cache->rank], (uintptr_t)clear_cache->data, NULL);
        }

        free(clear_cache);
}

double _starpu_mpi_clear_cache_cost_function(struct starpu_task *task, unsigned nimpl)
{
	return 0;
}

static struct starpu_perfmodel _starpu_mpi_clear_cache_model =
{
	.cost_function = _starpu_mpi_clear_cache_cost_function,
	.type = STARPU_COMMON,
};

static void _starpu_mpi_clear_cache_func(void *descr[] __attribute__ ((unused)), void *arg __attribute__ ((unused)))
{
}

static struct starpu_codelet _starpu_mpi_clear_cache_codelet =
{
	.where = STARPU_CPU|STARPU_CUDA|STARPU_OPENCL,
	.cpu_funcs = {_starpu_mpi_clear_cache_func, NULL},
	.cuda_funcs = {_starpu_mpi_clear_cache_func, NULL},
	.opencl_funcs = {_starpu_mpi_clear_cache_func, NULL},
	.nbuffers = 1,
	.modes = {STARPU_RW},
	.model = &_starpu_mpi_clear_cache_model
	// The model has a cost function which returns 0 so as to allow the codelet to be scheduled anywhere
};

void _starpu_mpi_clear_cache_request(starpu_data_handle_t data_handle, int rank, int mode)
{
        struct starpu_task *task = starpu_task_create();

	// We have a codelet with a empty function just to force the
	// task being created to have a dependency on data_handle
        task->cl = &_starpu_mpi_clear_cache_codelet;
        task->handles[0] = data_handle;

        struct _starpu_mpi_clear_cache *clear_cache = malloc(sizeof(struct _starpu_mpi_clear_cache));
        clear_cache->data = data_handle;
        clear_cache->rank = rank;
        clear_cache->mode = mode;

        task->callback_func = _starpu_mpi_clear_cache_callback;
        task->callback_arg = clear_cache;
        int ret = starpu_task_submit(task);
        STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
}

