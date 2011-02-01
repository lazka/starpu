/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011  Université de Bordeaux 1
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

#ifdef STARPU_USE_CUDA
static void memset_cuda(void *descr[], void *arg)
{
	int *ptr = (int *)STARPU_VECTOR_GET_PTR(descr[0]);
	unsigned n = STARPU_VECTOR_GET_NX(descr[0]);

	cudaMemset(ptr, 42, n);
	cudaThreadSynchronize();
}
#endif

static void memset_cpu(void *descr[], void *arg)
{
	int *ptr = (int *)STARPU_VECTOR_GET_PTR(descr[0]);
	unsigned n = STARPU_VECTOR_GET_NX(descr[0]);

	memset(ptr, 42, n);
}

static struct starpu_perfmodel_t model = {
	.type = STARPU_NL_REGRESSION_BASED,
	.symbol = "non_linear_memset_regression_based"
};

static starpu_codelet memset_cl = 
{
	.where = STARPU_CUDA|STARPU_CPU,
#ifdef STARPU_USE_CUDA
	.cuda_func = memset_cuda,
#endif
	.cpu_func = memset_cpu,
	.model = &model,
	.nbuffers = 1
};

static void test_memset(int nelems)
{
	starpu_data_handle handle;

	starpu_vector_data_register(&handle, -1, (uintptr_t)NULL, nelems, sizeof(int));

	struct starpu_task *task = starpu_task_create();

	task->cl = &memset_cl;
	task->buffers[0].handle = handle;
	task->buffers[0].mode = STARPU_W;
	task->synchronous = 1;

	int ret = starpu_task_submit(task);
	assert(!ret);

	starpu_data_unregister(handle);
}

int main(int argc, char **argv)
{
	struct starpu_conf conf;
	starpu_conf_init(&conf);

	conf.sched_policy_name = "dm";
	conf.calibrate = 1;

	starpu_init(&conf);

	int nloops = 32;
	int loop, slog;
	for (loop = 0; loop < nloops; loop++)
	{
		for (slog = 8; slog < 25; slog++)
		{
			int size = 1 << slog;
			test_memset(size);
		}
	} 

	starpu_shutdown();

	return 0;
}
