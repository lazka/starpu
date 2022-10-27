/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2022  Université de Bordeaux, CNRS (LaBRI UMR 5800), Inria
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

#include <starpu.h>
#include <starpu_hip.h>
#include <common/config.h>
#include <core/workers.h>

#ifdef STARPU_USE_HIP
#include <hipblas.h>
#include <starpu_hipblas.h>

static int hipblas_initialized[STARPU_NMAXWORKERS];
static hipblasHandle_t hipblas_handles[STARPU_NMAXWORKERS];
static hipblasHandle_t main_handle;
static starpu_pthread_mutex_t mutex;

static unsigned get_idx(void)
{
	unsigned workerid = starpu_worker_get_id_check();
	unsigned th_per_dev = _starpu_get_machine_config()->topology.hip_th_per_dev;
	unsigned th_per_stream = _starpu_get_machine_config()->topology.hip_th_per_stream;

	if (th_per_dev)
		return starpu_worker_get_devid(workerid);
	else if (th_per_stream)
		return workerid;
	else
		/* same thread for all devices */
		return 0;
}

static void init_hipblas_func(void *args STARPU_ATTRIBUTE_UNUSED)
{
	unsigned idx = get_idx();
	STARPU_PTHREAD_MUTEX_LOCK(&mutex);
	if (!(hipblas_initialized[idx]++))
	{
		float *dA,*dB,*dC;
		float v = 0.0;
		hipMalloc(&dA, sizeof(*dA));
		hipMalloc(&dB, sizeof(*dB));
		hipMalloc(&dC, sizeof(*dC));
		hipblasStatus_t status = hipblasSgemm(main_handle,
						      HIPBLAS_OP_N, HIPBLAS_OP_N,
						      1,1,1,
						      &v, dA, 1, dB, 1,
						      &v, dC, 1);
		if (STARPU_UNLIKELY(status))
			STARPU_HIPBLAS_REPORT_ERROR(status);
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(&mutex);

	hipblasCreate(&hipblas_handles[starpu_worker_get_id_check()]);
	hipblasSetStream(hipblas_handles[starpu_worker_get_id_check()], starpu_hip_get_local_stream());
}

static void set_hipblas_stream_func(void *args STARPU_ATTRIBUTE_UNUSED)
{
	hipblasSetStream(hipblas_handles[starpu_worker_get_id_check()], starpu_hip_get_local_stream());
}

static void shutdown_hipblas_func(void *args STARPU_ATTRIBUTE_UNUSED)
{
	unsigned idx = get_idx();
	STARPU_PTHREAD_MUTEX_LOCK(&mutex);
	if (!--hipblas_initialized[idx])
		hipblasDestroy(hipblas_handles[starpu_worker_get_id_check()]);
	STARPU_PTHREAD_MUTEX_UNLOCK(&mutex);

	hipblasDestroy(hipblas_handles[starpu_worker_get_id_check()]);
}
#endif

void starpu_hipblas_init(void)
{
#ifdef STARPU_USE_HIP
	starpu_execute_on_each_worker(init_hipblas_func, NULL, STARPU_HIP);
	starpu_execute_on_each_worker(set_hipblas_stream_func, NULL, STARPU_HIP);

	if (hipblasCreate(&main_handle) != HIPBLAS_STATUS_SUCCESS)
		main_handle = NULL;
#endif
}

void starpu_hipblas_shutdown(void)
{
#ifdef STARPU_USE_HIP
	starpu_execute_on_each_worker(shutdown_hipblas_func, NULL, STARPU_HIP);

	if (main_handle)
		hipblasDestroy(main_handle);
#endif
}

void starpu_hipblas_set_stream(void)
{
#ifdef STARPU_USE_HIP
	unsigned workerid = starpu_worker_get_id_check();
	int devnum = starpu_worker_get_devnum(workerid);
	if (!_starpu_get_machine_config()->topology.hip_th_per_dev ||
		(!_starpu_get_machine_config()->topology.hip_th_per_stream &&
		 _starpu_get_machine_config()->topology.nworker[STARPU_HIP_WORKER][devnum] > 1))
		hipblasSetStream(hipblas_handles[starpu_worker_get_id_check()], starpu_hip_get_local_stream());
#endif
}

#ifdef STARPU_USE_HIP
hipblasHandle_t starpu_hipblas_get_local_handle(void)
{
	int workerid = starpu_worker_get_id();
	if (workerid >= 0)
		return hipblas_handles[workerid];
	else
		return main_handle;
}
#endif
