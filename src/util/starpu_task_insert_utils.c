/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011, 2013-2014              Université Bordeaux
 * Copyright (C) 2011, 2012, 2013, 2014  Centre National de la Recherche Scientifique
 * Copyright (C) 2011, 2014        INRIA
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

#include <util/starpu_task_insert_utils.h>
#include <common/config.h>
#include <common/utils.h>
#include <core/task.h>

typedef void (*_starpu_callback_func_t)(void *);

/* Deal with callbacks. The unpack function may be called multiple times when
 * we have a parallel task, and we should not free the cl_arg parameter from
 * the callback function. */
struct _starpu_task_insert_cb_wrapper
{
	_starpu_callback_func_t callback_func;
	void *callback_arg;
};

static
void _starpu_task_insert_callback_wrapper(void *_cl_arg_wrapper)
{
	struct _starpu_task_insert_cb_wrapper *cl_arg_wrapper = (struct _starpu_task_insert_cb_wrapper *) _cl_arg_wrapper;

	/* Execute the callback specified by the application */
	if (cl_arg_wrapper->callback_func)
		cl_arg_wrapper->callback_func(cl_arg_wrapper->callback_arg);
}

void _starpu_task_insert_get_args_size(va_list varg_list, unsigned *nbuffers, size_t *cl_arg_size)
{
	int arg_type;
	size_t arg_buffer_size;
	unsigned n;

	arg_buffer_size = 0;
	n = 0;

	arg_buffer_size += sizeof(int);

	while ((arg_type = va_arg(varg_list, int)) != 0)
	{
		if (arg_type & STARPU_R || arg_type & STARPU_W || arg_type & STARPU_SCRATCH || arg_type & STARPU_REDUX)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
			n++;
		}
		else if (arg_type==STARPU_DATA_ARRAY)
		{
			(void)va_arg(varg_list, starpu_data_handle_t*);
			int nb_handles = va_arg(varg_list, int);
			n += nb_handles;
		}
		else if (arg_type==STARPU_VALUE)
		{
			(void)va_arg(varg_list, void *);
			size_t cst_size = va_arg(varg_list, size_t);

			arg_buffer_size += sizeof(size_t);
			arg_buffer_size += cst_size;
		}
		else if (arg_type==STARPU_CALLBACK)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_CALLBACK_WITH_ARG)
		{
			va_arg(varg_list, _starpu_callback_func_t);
			va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_POP)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_POP_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PRIORITY)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_NODE)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_DATA)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_EXECUTE_ON_WORKER)
		{
			va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_WORKER_ORDER)
		{
			va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_SCHED_CTX)
		{
			(void)va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_HYPERVISOR_TAG)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_POSSIBLY_PARALLEL)
		{
			(void)va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_FLOPS)
		{
			(void)va_arg(varg_list, double);
		}
		else if (arg_type==STARPU_TAG || arg_type==STARPU_TAG_ONLY)
		{
			(void)va_arg(varg_list, starpu_tag_t);
		}
		else
		{
			STARPU_ABORT_MSG("Unrecognized argument %d\n", arg_type);
		}
	}

	if (cl_arg_size)
		*cl_arg_size = arg_buffer_size;
	if (nbuffers)
		*nbuffers = n;
}

int _starpu_codelet_pack_args(void **arg_buffer, size_t arg_buffer_size, va_list varg_list)
{
	int arg_type;
	unsigned current_arg_offset = 0;
	int nargs = 0;
	char *_arg_buffer; // We would like a void* but we use a char* to allow pointer arithmetic

	/* The buffer will contain : nargs, {size, content} (x nargs)*/
	_arg_buffer = malloc(arg_buffer_size);

	/* We will begin the buffer with the number of args */
	current_arg_offset += sizeof(nargs);

	while((arg_type = va_arg(varg_list, int)) != 0)
	{
		if (arg_type & STARPU_R || arg_type & STARPU_W || arg_type & STARPU_SCRATCH || arg_type & STARPU_REDUX)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_DATA_ARRAY)
		{
			(void)va_arg(varg_list, starpu_data_handle_t*);
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_VALUE)
		{
			/* We have a constant value: this should be followed by a pointer to the cst value and the size of the constant */
			void *ptr = va_arg(varg_list, void *);
			size_t cst_size = va_arg(varg_list, size_t);

			memcpy(_arg_buffer+current_arg_offset, (void *)&cst_size, sizeof(cst_size));
			current_arg_offset += sizeof(size_t);

			memcpy(_arg_buffer+current_arg_offset, ptr, cst_size);
			current_arg_offset += cst_size;

			nargs++;
			STARPU_ASSERT(current_arg_offset <= arg_buffer_size);
		}
		else if (arg_type==STARPU_CALLBACK)
		{
			(void)va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_CALLBACK_WITH_ARG)
		{
			va_arg(varg_list, _starpu_callback_func_t);
			va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK)
		{
			va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_POP)
		{
			va_arg(varg_list, _starpu_callback_func_t);
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_POP_ARG)
		{
			(void)va_arg(varg_list, void *);
		}
		else if (arg_type==STARPU_PRIORITY)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_NODE)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_DATA)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_EXECUTE_ON_WORKER)
		{
			va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_WORKER_ORDER)
		{
			va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_SCHED_CTX)
		{
			(void)va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_HYPERVISOR_TAG)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_POSSIBLY_PARALLEL)
		{
			(void)va_arg(varg_list, unsigned);
		}
		else if (arg_type==STARPU_FLOPS)
		{
			(void)va_arg(varg_list, double);
		}
		else if (arg_type==STARPU_TAG || arg_type==STARPU_TAG_ONLY)
		{
			(void)va_arg(varg_list, starpu_tag_t);
		}
		else
		{
			STARPU_ABORT_MSG("Unrecognized argument %d\n", arg_type);
		}
	}

	if (nargs)
	{
		memcpy(_arg_buffer, (int *)&nargs, sizeof(nargs));
	}
	else
	{
		free(_arg_buffer);
		_arg_buffer = NULL;
	}

	*arg_buffer = _arg_buffer;
	return 0;
}

void _starpu_task_insert_create(void *arg_buffer, size_t arg_buffer_size, struct starpu_codelet *cl, struct starpu_task **task, va_list varg_list)
{
	int arg_type;
	unsigned current_buffer = 0;

	struct _starpu_task_insert_cb_wrapper *cl_arg_wrapper = (struct _starpu_task_insert_cb_wrapper *) malloc(sizeof(struct _starpu_task_insert_cb_wrapper));
	STARPU_ASSERT(cl_arg_wrapper);

	cl_arg_wrapper->callback_func = NULL;

	struct _starpu_task_insert_cb_wrapper *prologue_cl_arg_wrapper = (struct _starpu_task_insert_cb_wrapper *) malloc(sizeof(struct _starpu_task_insert_cb_wrapper));
	STARPU_ASSERT(prologue_cl_arg_wrapper);

	prologue_cl_arg_wrapper->callback_func = NULL;

	struct _starpu_task_insert_cb_wrapper *prologue_pop_cl_arg_wrapper = (struct _starpu_task_insert_cb_wrapper *) malloc(sizeof(struct _starpu_task_insert_cb_wrapper));
	STARPU_ASSERT(prologue_pop_cl_arg_wrapper);

	prologue_pop_cl_arg_wrapper->callback_func = NULL;

	(*task)->cl = cl;

	while((arg_type = va_arg(varg_list, int)) != 0)
	{
		if (arg_type & STARPU_R || arg_type & STARPU_W || arg_type & STARPU_SCRATCH || arg_type & STARPU_REDUX)
		{
			/* We have an access mode : we expect to find a handle */
			starpu_data_handle_t handle = va_arg(varg_list, starpu_data_handle_t);

			enum starpu_data_access_mode mode = (enum starpu_data_access_mode) arg_type;

			STARPU_ASSERT(cl != NULL);

			STARPU_TASK_SET_HANDLE((*task), handle, current_buffer);
			if (cl->nbuffers == STARPU_VARIABLE_NBUFFERS)
				STARPU_TASK_SET_MODE(*task, mode, current_buffer);
			else if (STARPU_CODELET_GET_MODE(cl, current_buffer))
			{
				STARPU_ASSERT_MSG(STARPU_CODELET_GET_MODE(cl, current_buffer) == mode,
						   "The codelet <%s> defines the access mode %d for the buffer %d which is different from the mode %d given to starpu_task_insert\n",
						  cl->name, STARPU_CODELET_GET_MODE(cl, current_buffer),
						  current_buffer, mode);
			}
			else
			{
#ifdef STARPU_DEVEL
#  warning shall we print a warning to the user
/* Morse uses it to avoid having to set it in the codelet structure */
#endif
				STARPU_CODELET_SET_MODE(cl, mode, current_buffer);
			}

			current_buffer++;
		}
		else if (arg_type == STARPU_DATA_ARRAY)
		{
			// Expect to find a array of handles and its size
			starpu_data_handle_t *handles = va_arg(varg_list, starpu_data_handle_t *);
			int nb_handles = va_arg(varg_list, int);

			int i;
			for(i=0 ; i<nb_handles ; i++)
			{
				STARPU_TASK_SET_HANDLE((*task), handles[i], current_buffer);
				current_buffer++;
			}

		}
		else if (arg_type==STARPU_VALUE)
		{
			(void)va_arg(varg_list, void *);
			(void)va_arg(varg_list, size_t);
		}
		else if (arg_type==STARPU_CALLBACK)
		{
			void (*callback_func)(void *);
			callback_func = va_arg(varg_list, _starpu_callback_func_t);
			cl_arg_wrapper->callback_func = callback_func;
		}
		else if (arg_type==STARPU_CALLBACK_WITH_ARG)
		{
			void (*callback_func)(void *);
			void *callback_arg;
			callback_func = va_arg(varg_list, _starpu_callback_func_t);
			callback_arg = va_arg(varg_list, void *);
			cl_arg_wrapper->callback_func = callback_func;
			cl_arg_wrapper->callback_arg = callback_arg;
		}
		else if (arg_type==STARPU_CALLBACK_ARG)
		{
			void *callback_arg = va_arg(varg_list, void *);
			cl_arg_wrapper->callback_arg = callback_arg;
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK)
		{
			void (*callback_func)(void *);
			callback_func = va_arg(varg_list, _starpu_callback_func_t);
			prologue_cl_arg_wrapper->callback_func = callback_func;
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_ARG)
		{
			void *callback_arg = va_arg(varg_list, void *);
			prologue_cl_arg_wrapper->callback_arg = callback_arg;
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_POP)
		{
			void (*callback_func)(void *);
			callback_func = va_arg(varg_list, _starpu_callback_func_t);
			prologue_pop_cl_arg_wrapper->callback_func = callback_func;
		}
		else if (arg_type==STARPU_PROLOGUE_CALLBACK_POP_ARG)
		{
			void *callback_arg = va_arg(varg_list, void *);
			prologue_pop_cl_arg_wrapper->callback_arg = callback_arg;
		}
		else if (arg_type==STARPU_PRIORITY)
		{
			/* Followed by a priority level */
			int prio = va_arg(varg_list, int);
			(*task)->priority = prio;
		}
		else if (arg_type==STARPU_EXECUTE_ON_NODE)
		{
			(void)va_arg(varg_list, int);
		}
		else if (arg_type==STARPU_EXECUTE_ON_DATA)
		{
			(void)va_arg(varg_list, starpu_data_handle_t);
		}
		else if (arg_type==STARPU_EXECUTE_ON_WORKER)
		{
			int worker = va_arg(varg_list, int);
			if (worker != -1)
			{
				(*task)->workerid = worker;
				(*task)->execute_on_a_specific_worker = 1;
			}
		}
		else if (arg_type==STARPU_WORKER_ORDER)
		{
			unsigned order = va_arg(varg_list, unsigned);
			if (order != 0)
			{
				STARPU_ASSERT_MSG((*task)->execute_on_a_specific_worker, "worker order only makes sense if a workerid is provided");
				(*task)->workerorder = order;
			}
		}
		else if (arg_type==STARPU_SCHED_CTX)
		{
			unsigned sched_ctx = va_arg(varg_list, unsigned);
			(*task)->sched_ctx = sched_ctx;
		}
		else if (arg_type==STARPU_HYPERVISOR_TAG)
		{
			int hypervisor_tag = va_arg(varg_list, int);
			(*task)->hypervisor_tag = hypervisor_tag;
		}
		else if (arg_type==STARPU_POSSIBLY_PARALLEL)
		{
			unsigned possibly_parallel = va_arg(varg_list, unsigned);
			(*task)->possibly_parallel = possibly_parallel;
		}
		else if (arg_type==STARPU_FLOPS)
		{
			double flops = va_arg(varg_list, double);
			(*task)->flops = flops;
		}
		else if (arg_type==STARPU_TAG)
		{
			starpu_tag_t tag = va_arg(varg_list, starpu_tag_t);
			(*task)->tag_id = tag;
			(*task)->use_tag = 1;
		}
		else if (arg_type==STARPU_TAG_ONLY)
		{
			starpu_tag_t tag = va_arg(varg_list, starpu_tag_t);
			(*task)->tag_id = tag;
		}
		else
		{
			STARPU_ABORT_MSG("Unrecognized argument %d\n", arg_type);
		}
	}

	if (cl && cl->nbuffers == STARPU_VARIABLE_NBUFFERS)
		(*task)->nbuffers = current_buffer;

	(*task)->cl_arg = arg_buffer;
	(*task)->cl_arg_size = arg_buffer_size;

	/* The callback will free the argument stack and execute the
	 * application's callback, if any. */
	(*task)->callback_func = _starpu_task_insert_callback_wrapper;
	(*task)->callback_arg = cl_arg_wrapper;
	(*task)->callback_arg_free = 1;

	(*task)->prologue_callback_func = _starpu_task_insert_callback_wrapper;
	(*task)->prologue_callback_arg = prologue_cl_arg_wrapper;
	(*task)->prologue_callback_arg_free = 1;

	(*task)->prologue_callback_pop_func = _starpu_task_insert_callback_wrapper;
	(*task)->prologue_callback_pop_arg = prologue_pop_cl_arg_wrapper;
	(*task)->prologue_callback_pop_arg_free = 1;
}
