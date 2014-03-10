/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2010, 2012, 2014  Université de Bordeaux 1
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
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

#include <config.h>
#include <starpu.h>
#include <limits.h>
#include <unistd.h>
#include "../helper.h"

#define N	1000
#define VECTORSIZE	1024

void codelet_null(void *descr[], STARPU_ATTRIBUTE_UNUSED void *_args)
{
	STARPU_SKIP_IF_VALGRIND;

	int worker_size = starpu_combined_worker_get_size();
	STARPU_ASSERT(worker_size > 0);
	usleep(1000/worker_size);
#if 1
	int id = starpu_worker_get_id();
	int combined_id = starpu_combined_worker_get_id();
	FPRINTF(stderr, "worker id %d - combined id %d - worker size %d\n", id, combined_id, worker_size);
#endif
}

static struct starpu_codelet cl =
{
	.type = STARPU_FORKJOIN,
	.max_parallelism = INT_MAX,
	.cpu_funcs = {codelet_null, NULL},
	.cpu_funcs_name = {"codelet_null", NULL},
	.cuda_funcs = {codelet_null, NULL},
        .opencl_funcs = {codelet_null, NULL},
	.nbuffers = 1,
	.modes = {STARPU_R}
};


int main(int argc, char **argv)
{
	starpu_data_handle_t v_handle;
	unsigned *v;
	int ret;

	struct starpu_conf conf;
	ret = starpu_conf_init(&conf);
	conf.sched_policy_name = "pheft";
	conf.calibrate = 1;

	ret = starpu_init(&conf);
	if (ret == -ENODEV) return STARPU_TEST_SKIPPED;
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_init");

	starpu_malloc((void **)&v, VECTORSIZE*sizeof(unsigned));
	starpu_vector_data_register(&v_handle, STARPU_MAIN_RAM, (uintptr_t)v, VECTORSIZE, sizeof(unsigned));

	unsigned nworker = starpu_worker_get_count() + starpu_combined_worker_get_count();

	unsigned iter, worker;
	for (iter = 0; iter < N; iter++)
	{
		for (worker = 0; worker < nworker; worker++)
		{
			/* execute a task on that worker */
			struct starpu_task *task = starpu_task_create();
			task->cl = &cl;

			task->handles[0] = v_handle;

			task->execute_on_a_specific_worker = 1;
			task->workerid = worker;

			ret = starpu_task_submit(task);
			if (ret == -ENODEV) goto enodev;
			STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_submit");
		}
	}

	ret = starpu_task_wait_for_all();
	STARPU_CHECK_RETURN_VALUE(ret, "starpu_task_wait_for_all");

	starpu_data_unregister(v_handle);
	starpu_free(v);
	starpu_shutdown();

	return EXIT_SUCCESS;

enodev:
	starpu_data_unregister(v_handle);
	starpu_free(v);
	fprintf(stderr, "WARNING: No one can execute this task\n");
	/* yes, we do not perform the computation but we did detect that no one
 	 * could perform the kernel, so this is not an error from StarPU */
	starpu_shutdown();
	return STARPU_TEST_SKIPPED;
}
