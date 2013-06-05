/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2012  Inria
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


#include <string.h>
#include <pthread.h>

#include <starpu.h>
#include <datawizard/coherency.h>
#include <datawizard/interfaces/data_interface.h>
#include <drivers/mp_common/mp_common.h>

int
_starpu_src_common_sink_nbcores (const struct _starpu_mp_node *node, int *buf)
{
    // Send a request to the sink NODE for the number of cores on it.

    enum _starpu_mp_command answer;
    void *arg;
    int arg_size = sizeof (int);

    _starpu_mp_common_send_command (node, STARPU_SINK_NBCORES, NULL, 0);

    answer = _starpu_mp_common_recv_command (node, &arg, &arg_size);

    STARPU_ASSERT (answer == STARPU_ANSWER_SINK_NBCORES && arg_size == sizeof (int));

    memcpy (buf, arg, arg_size);

    return 0;
}

/* Send a request to the sink linked to NODE for the pointer to the
 * function defined by FUNC_NAME.
 * In case of success, it returns 0 and FUNC_PTR contains the pointer ;
 * else it returns -ESPIPE if the function was not found.
 */
int _starpu_src_common_lookup(struct _starpu_mp_node *node,
			      void (**func_ptr)(void), const char *func_name)
{
	enum _starpu_mp_command answer;
	void *arg;
	int arg_size;

	/* strlen ignore the terminating '\0' */
	arg_size = (strlen(func_name) + 1) * sizeof(char);

	//_STARPU_DEBUG("Looking up %s\n", func_name);
	_starpu_mp_common_send_command(node, STARPU_LOOKUP, (void *) func_name,
				       arg_size);
	answer = _starpu_mp_common_recv_command(node, (void **) &arg,
						&arg_size);

	if (answer == STARPU_ERROR_LOOKUP) {
		_STARPU_DISP("Error looking up %s\n", func_name);
		return -ESPIPE;
	}

	/* We have to be sure the device answered the right question and the
	 * answer has the right size */
	STARPU_ASSERT(answer == STARPU_ANSWER_LOOKUP &&
		      arg_size == sizeof(*func_ptr));

	memcpy(func_ptr, arg, arg_size);

	//_STARPU_DEBUG("got %p\n", *func_ptr);

	return 0;
}

 /* Send a message to the sink to execute a kernel.
 * The message sent has the form below :
 * [Function pointer on sink, number of interfaces, interfaces
 * (union _starpu_interface), cl_arg]
 */
int _starpu_src_common_execute_kernel(const struct _starpu_mp_node *node,
				      void (*kernel)(void), unsigned coreid,
				      starpu_data_handle_t *handles,
				      void **interfaces,
				      unsigned nb_interfaces,
				      void *cl_arg, size_t cl_arg_size)
{
	unsigned id;
	void *buffer, *buffer_ptr, *arg = NULL;
	int buffer_size = 0, arg_size = 0;

	/* If the user didn't give any cl_arg, there is no need to send it */
	buffer_size =
	    sizeof(kernel) + sizeof(coreid) + sizeof(nb_interfaces) +
	    nb_interfaces * sizeof(union _starpu_interface);
	if (cl_arg)
	{
		STARPU_ASSERT(cl_arg_size);
		buffer_size += cl_arg_size;
	}

	/* We give to send_command a buffer we just allocated, which contains
	 * a pointer to the function (sink-side), core on which execute this
	 * function (sink-side), number of interfaces we send,
	 * an array of generic (union) interfaces and the value of cl_arg */
	buffer_ptr = buffer = (void *) malloc(buffer_size);

	*(void(**)(void)) buffer = kernel;
	buffer_ptr += sizeof(kernel);

	*(unsigned *) buffer_ptr = coreid;
	buffer_ptr += sizeof(coreid);

	*(unsigned *) buffer_ptr = nb_interfaces;
	buffer_ptr += sizeof(nb_interfaces);

	/* Message-passing execution is a particular case as the codelet is
	 * executed on a sink with a different memory, whereas a codelet is
	 * executed on the host part for the other accelerators.
	 * Thus we need to send a copy of each interface on the MP device */
	for (id = 0; id < nb_interfaces; id++)
	{
		starpu_data_handle_t handle = handles[id];
		memcpy (buffer_ptr, interfaces[id],
			handle->ops->interface_size);
		/* The sink side has no mean to get the type of each
		 * interface, we use a union to make it generic and permit the
		 * sink to go through the array */
		buffer_ptr += sizeof(union _starpu_interface);
	}

	if (cl_arg)
		memcpy(buffer_ptr, cl_arg, cl_arg_size);

	_starpu_mp_common_send_command(node, STARPU_EXECUTE, buffer, buffer_size);
	enum _starpu_mp_command answer = _starpu_mp_common_recv_command(node, &arg, &arg_size);

	if (answer == STARPU_ERROR_EXECUTE)
		return -EINVAL;

	STARPU_ASSERT(answer == STARPU_EXECUTION_SUBMITTED);

	free(buffer);

	return 0;

}

/* Launch the execution of the function KERNEL points to on the sink linked
 * to NODE. Returns 0 in case of success, -EINVAL if kernel is an invalid
 * pointer.
 * Data interfaces in task are send to the sink.
 */
int _starpu_src_common_execute_kernel_from_task(const struct _starpu_mp_node *node,
						void (*kernel)(void), unsigned coreid,
						struct starpu_task *task)
{
    return _starpu_src_common_execute_kernel(node, kernel, coreid,
					     task->handles, task->interfaces, task->cl->nbuffers,
					     task->cl_arg, task->cl_arg_size);
}

/* Send a request to the sink linked to the MP_NODE to allocate SIZE bytes on
 * the sink.
 * In case of success, it returns 0 and *ADDR contains the address of the
 * allocated area ;
 * else it returns 1 if the allocation fail.
 */
int _starpu_src_common_allocate(const struct _starpu_mp_node *mp_node,
								void **addr, size_t size)
{
	enum _starpu_mp_command answer;
	void *arg;
	int arg_size;

	_starpu_mp_common_send_command(mp_node, STARPU_ALLOCATE, &size,
								   sizeof(size));

	answer = _starpu_mp_common_recv_command(mp_node, &arg, &arg_size);

	if (answer == STARPU_ERROR_ALLOCATE)
		return 1;

	STARPU_ASSERT(answer == STARPU_ANSWER_ALLOCATE &&
				  arg_size == sizeof(*addr));

	memcpy(addr, arg, arg_size);

	return 0;
}

/* Send a request to the sink linked to the MP_NODE to deallocate the memory
 * area pointed by ADDR.
 */
void _starpu_src_common_free(const struct _starpu_mp_node *mp_node,
							 void *addr)
{
	_starpu_mp_common_send_command(mp_node, STARPU_FREE, &addr, sizeof(addr));
}

/* Send SIZE bytes pointed by SRC to DST on the sink linked to the MP_NODE.
 */
int _starpu_src_common_copy_host_to_sink(const struct _starpu_mp_node *mp_node,
										 void *src, void *dst, size_t size)
{
	struct _starpu_mp_transfer_command cmd = {size, dst};

	_starpu_mp_common_send_command(mp_node, STARPU_RECV_FROM_HOST, &cmd, sizeof(cmd));
	mp_node->dt_send(mp_node, src, size);

	return 0;
}

/* Receive SIZE bytes pointed by SRC on the sink linked to the MP_NODE and store them in DST.
 */
int _starpu_src_common_copy_sink_to_host(const struct _starpu_mp_node *mp_node,
										 void *src, void *dst, size_t size)
{
	struct _starpu_mp_transfer_command cmd = {size, src};

	_starpu_mp_common_send_command(mp_node, STARPU_SEND_TO_HOST, &cmd, sizeof(cmd));
	mp_node->dt_recv(mp_node, dst, size);

	return 0;
}

/* Tell the sink linked to SRC_NODE to send SIZE bytes of data pointed by SRC
 * to the sink linked to DST_NODE. The latter store them in DST.
 */
int _starpu_src_common_copy_sink_to_sink(const struct _starpu_mp_node *src_node,
		const struct _starpu_mp_node *dst_node, void *src, void *dst, size_t size)
{
	enum _starpu_mp_command answer;
	void *arg;
	int arg_size;

	struct _starpu_mp_transfer_command_to_device cmd = {dst_node->peer_id, size, src};

	/* Tell source to send data to dest. */
	_starpu_mp_common_send_command(src_node, STARPU_SEND_TO_SINK, &cmd, sizeof(cmd));

	cmd.devid = src_node->peer_id;
	cmd.size = size;
	cmd.addr = dst;

	/* Tell dest to receive data from source. */
	_starpu_mp_common_send_command(dst_node, STARPU_RECV_FROM_SINK, &cmd, sizeof(cmd));

	/* Wait for answer from dest to know wether transfer is finished. */
	answer = _starpu_mp_common_recv_command(dst_node, &arg, &arg_size);

	STARPU_ASSERT(answer == STARPU_TRANSFER_COMPLETE);

	return 0;
}

/* 5 functions to determine the executable to run on the device (MIC, SCC,
 * MPI).
 */
static void _starpu_src_common_cat_3(char *final, const char *first, const char *second,
										  const char *third)
{
	strcpy(final, first);
	strcat(final, second);
	strcat(final, third);
}

static void _starpu_src_common_cat_2(char *final, const char *first, const char *second)
{
	_starpu_src_common_cat_3(final, first, second, "");
}

static void _starpu_src_common_dir_cat(char *final, const char *dir, const char *file)
{
	if (file[0] == '/')
		++file;

	size_t size = strlen(dir);
	if (dir[size - 1] == '/')
		_starpu_src_common_cat_2(final, dir, file);
	else
		_starpu_src_common_cat_3(final, dir, "/", file);
}

static int _starpu_src_common_test_suffixes(char *located_file_name, const char *base, const char **suffixes)
{
	unsigned int i;
	for (i = 0; suffixes[i] != NULL; ++i)
	{
		_starpu_src_common_cat_2(located_file_name, base, suffixes[i]);
		if (access(located_file_name, R_OK) == 0)
			return 0;
	}

	return 1;
}

int _starpu_src_common_locate_file(char *located_file_name,
							const char *env_file_name, const char *env_mic_path,
							const char *config_file_name, const char *actual_file_name,
							const char **suffixes)
{
	if (env_file_name != NULL)
	{
		if (access(env_file_name, R_OK) == 0)
		{
			strcpy(located_file_name, env_file_name);
			return 0;
		}
		else if(env_mic_path != NULL)
		{
			_starpu_src_common_dir_cat(located_file_name, env_mic_path, env_file_name);

			return access(located_file_name, R_OK);
		}
	}
	else if (config_file_name != NULL)
	{
		if (access(config_file_name, R_OK) == 0)
		{
			strcpy(located_file_name, config_file_name);
			return 0;
		}
		else if (env_mic_path != NULL)
		{
			_starpu_src_common_dir_cat(located_file_name, env_mic_path, config_file_name);

			return access(located_file_name, R_OK);
		}
	}
	else if (actual_file_name != NULL)
	{
		if (_starpu_src_common_test_suffixes(located_file_name, actual_file_name, suffixes) == 0)
			return 0;

		if (env_mic_path != NULL)
		{
			char actual_cpy[1024];
			strcpy(actual_cpy, actual_file_name);

			char *last =  strrchr(actual_cpy, '/');
			while (last != NULL)
			{
				char tmp[1024];

				_starpu_src_common_dir_cat(tmp, env_mic_path, last);

				if (access(tmp, R_OK) == 0)
				{
					strcpy(located_file_name, tmp);
					return 0;
				}

				if (_starpu_src_common_test_suffixes(located_file_name, tmp, suffixes) == 0)
					return 0;

				*last = '\0';
				char *last_tmp = strrchr(actual_cpy, '/');
				*last = '/';
				last = last_tmp;
			}
		}
	}

	return 1;
}
