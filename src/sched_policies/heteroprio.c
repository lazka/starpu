/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2015  INRIA
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

/* Distributed queues using performance modeling to assign tasks */

#include <starpu_config.h>
#include <starpu_scheduler.h>
#include <schedulers/starpu_heteroprio.h>

#include <common/fxt.h>
#include <core/task.h>

#include <sched_policies/fifo_queues.h>
#include <limits.h>

#ifdef HAVE_AYUDAME_H
#include <Ayudame.h>
#endif

#ifndef DBL_MIN
#define DBL_MIN __DBL_MIN__
#endif

#ifndef DBL_MAX
#define DBL_MAX __DBL_MAX__
#endif

/* A bucket corresponds to a Pair of priorities
 * When a task is pushed with a priority X, it will be stored
 * into the bucket X.
 * All the tasks stored in the fifo should be computable by the arch
 * in valid_archs.
 * For example if valid_archs = (STARPU_CPU|STARPU_CUDA)
 * Then task->task->cl->where should be at least (STARPU_CPU|STARPU_CUDA)
 */
struct _heteroprio_bucket
{
	/* The task of the current bucket */
	struct _starpu_fifo_taskq* tasks_queue;
	/* The correct arch for the current bucket */
	unsigned valid_archs;
	/* The slow factors for any archs */
	float slow_factors_per_index[STARPU_NB_TYPES];
	/* The base arch for the slow factor (the fatest arch for the current task in the bucket */
	unsigned factor_base_arch_index;
};

/* Init a bucket */
static void _heteroprio_bucket_init(struct _heteroprio_bucket* bucket)
{
	memset(bucket, 0, sizeof(*bucket));
	bucket->tasks_queue =  _starpu_create_fifo();
}

/* Release a bucket */
static void _heteroprio_bucket_release(struct _heteroprio_bucket* bucket)
{
	STARPU_ASSERT(_starpu_fifo_empty(bucket->tasks_queue) != 0);
	_starpu_destroy_fifo(bucket->tasks_queue);
}

/* A worker is mainly composed of a fifo for the tasks
 * and some direct access to worker properties.
 * The fifo is implemented with any array,
 * to read a task, access tasks_queue[tasks_queue_index]
 * to write a task, access tasks_queue[(tasks_queue_index+tasks_queue_size)%HETEROPRIO_MAX_PREFETCH]
 */
/* ANDRA_MODIF: can use starpu fifo + starpu sched_mutex*/
struct _heteroprio_worker_wrapper
{
	unsigned arch_type;
	unsigned arch_index;
	struct _starpu_fifo_taskq *tasks_queue;
};

struct _starpu_heteroprio_data
{
	starpu_pthread_mutex_t policy_mutex;
	struct starpu_bitmap *waiters;
	/* The bucket to store the tasks */
	struct _heteroprio_bucket buckets[STARPU_HETEROPRIO_MAX_PRIO];
	/* The number of buckets for each arch */
	unsigned nb_prio_per_arch_index[STARPU_NB_TYPES];
	/* The mapping to the corresponding buckets */
	unsigned prio_mapping_per_arch_index[STARPU_NB_TYPES][STARPU_HETEROPRIO_MAX_PRIO];
	/* The number of available tasks for a given arch (not prefetched) */
	unsigned nb_remaining_tasks_per_arch_index[STARPU_NB_TYPES];
	/* The total number of tasks in the bucket (not prefetched) */
	unsigned total_tasks_in_buckets;
	/* The total number of prefetched tasks for a given arch */
	unsigned nb_prefetched_tasks_per_arch_index[STARPU_NB_TYPES];
	/* The information for all the workers */
	struct _heteroprio_worker_wrapper workers_heteroprio[STARPU_NMAXWORKERS];
	/* The number of workers for a given arch */
	unsigned nb_workers_per_arch_index[STARPU_NB_TYPES];
};

/** Tell how many prio there are for a given arch */
void starpu_heteroprio_set_nb_prios(unsigned sched_ctx_id, enum starpu_heteroprio_types arch, unsigned max_prio)
{
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	STARPU_ASSERT(max_prio < STARPU_HETEROPRIO_MAX_PRIO);

	hp->nb_prio_per_arch_index[arch] = max_prio;
}

/** Set the mapping for a given arch prio=>bucket */
inline void starpu_heteroprio_set_mapping(unsigned sched_ctx_id, enum starpu_heteroprio_types arch, unsigned source_prio, unsigned dest_bucket_id)
{
	STARPU_ASSERT(dest_bucket_id < STARPU_HETEROPRIO_MAX_PRIO);

	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	hp->prio_mapping_per_arch_index[arch][source_prio] = dest_bucket_id;

	hp->buckets[dest_bucket_id].valid_archs |= starpu_heteroprio_types_to_arch[arch];
	_STARPU_DEBUG("Adding arch %d to bucket %d\n", arch, dest_bucket_id);
}

/** Tell which arch is the faster for the tasks of a bucket (optional) */
inline void starpu_heteroprio_set_faster_arch(unsigned sched_ctx_id, enum starpu_heteroprio_types arch, unsigned bucket_id)
{
	STARPU_ASSERT(bucket_id < STARPU_HETEROPRIO_MAX_PRIO);

	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	hp->buckets[bucket_id].factor_base_arch_index = arch;

	hp->buckets[bucket_id].slow_factors_per_index[arch] = 0;
}

/** Tell how slow is a arch for the tasks of a bucket (optional) */
inline void starpu_heteroprio_set_arch_slow_factor(unsigned sched_ctx_id, enum starpu_heteroprio_types arch, unsigned bucket_id, float slow_factor)
{
	STARPU_ASSERT(bucket_id < STARPU_HETEROPRIO_MAX_PRIO);

	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	hp->buckets[bucket_id].slow_factors_per_index[arch] = slow_factor;
}


/** If the user does not provide an init callback we create a single bucket for all architectures */
static inline void default_init_sched(unsigned sched_ctx_id)
{
	// By default each type of devices uses 1 bucket and no slow factor
#ifdef STARPU_USE_CPU
	starpu_heteroprio_set_nb_prios(sched_ctx_id, STARPU_CPU_IDX, STARPU_MAX_PRIO-STARPU_MIN_PRIO+1);
#endif
#ifdef STARPU_USE_OPENCL
	starpu_heteroprio_set_nb_prios(sched_ctx_id, STARPU_OPENCL_IDX, STARPU_MAX_PRIO-STARPU_MIN_PRIO+1);
#endif
#ifdef STARPU_USE_CUDA
	starpu_heteroprio_set_nb_prios(sched_ctx_id, STARPU_CUDA_IDX, STARPU_MAX_PRIO-STARPU_MIN_PRIO+1);
#endif

	// Direct mapping
	int prio;
	for(prio=STARPU_MIN_PRIO ; prio<=STARPU_MAX_PRIO ; prio++)
	{
#ifdef STARPU_USE_CPU
		starpu_heteroprio_set_mapping(sched_ctx_id, STARPU_CPU_IDX, prio, prio);
#endif
#ifdef STARPU_USE_OPENCL
		starpu_heteroprio_set_mapping(sched_ctx_id, STARPU_OPENCL_IDX, prio, prio);
#endif
#ifdef STARPU_USE_CUDA
		starpu_heteroprio_set_mapping(sched_ctx_id, STARPU_CUDA_IDX, prio, prio);
#endif
	}
}

static void initialize_heteroprio_policy(unsigned sched_ctx_id)
{
#ifdef STARPU_HAVE_HWLOC
	starpu_sched_ctx_create_worker_collection(sched_ctx_id, STARPU_WORKER_TREE);
#else
	starpu_sched_ctx_create_worker_collection(sched_ctx_id, STARPU_WORKER_LIST);
#endif
	/* Alloc the scheduler data  */
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)malloc(sizeof(struct _starpu_heteroprio_data));
	memset(hp, 0, sizeof(*hp));

	hp->waiters = starpu_bitmap_create();

	starpu_sched_ctx_set_policy_data(sched_ctx_id, (void*)hp);

	STARPU_PTHREAD_MUTEX_INIT(&hp->policy_mutex, NULL);

	unsigned idx_prio;
	for(idx_prio = 0; idx_prio < STARPU_HETEROPRIO_MAX_PRIO; ++idx_prio)
		_heteroprio_bucket_init(&hp->buckets[idx_prio]);

	void (*init_sched)(void) = starpu_sched_ctx_get_sched_policy_init(sched_ctx_id);

	if(init_sched)
		init_sched();
	else
		default_init_sched(sched_ctx_id);

	/* Ensure that information have been correctly filled */
	unsigned check_all_archs[STARPU_HETEROPRIO_MAX_PRIO];
	memset(check_all_archs, 0, sizeof(unsigned)*STARPU_HETEROPRIO_MAX_PRIO);
	unsigned arch_index;
	for(arch_index = 0; arch_index < STARPU_NB_TYPES; ++arch_index)
	{
		STARPU_ASSERT(hp->nb_prio_per_arch_index[arch_index] <= STARPU_HETEROPRIO_MAX_PRIO);

		unsigned check_archs[STARPU_HETEROPRIO_MAX_PRIO];
		memset(check_archs, 0, sizeof(unsigned)*STARPU_HETEROPRIO_MAX_PRIO);

		for(idx_prio = 0; idx_prio < hp->nb_prio_per_arch_index[arch_index]; ++idx_prio)
		{
			const unsigned mapped_prio = hp->prio_mapping_per_arch_index[arch_index][idx_prio];
			STARPU_ASSERT(mapped_prio <= STARPU_HETEROPRIO_MAX_PRIO);
			STARPU_ASSERT(hp->buckets[mapped_prio].slow_factors_per_index[arch_index] >= 0.0);
			STARPU_ASSERT(hp->buckets[mapped_prio].valid_archs & starpu_heteroprio_types_to_arch[arch_index]);
			check_archs[mapped_prio]      = 1;
			check_all_archs[mapped_prio] += 1;
		}
		for(idx_prio = 0; idx_prio < STARPU_HETEROPRIO_MAX_PRIO; ++idx_prio)
		{
			/* Ensure the current arch use a bucket or someone else can use it */
			STARPU_ASSERT(check_archs[idx_prio] == 1 || hp->buckets[idx_prio].valid_archs == 0
				      || (hp->buckets[idx_prio].valid_archs & ~starpu_heteroprio_types_to_arch[arch_index]) != 0);
		}
	}
	/* Ensure that if a valid_archs = (STARPU_CPU|STARPU_CUDA) then check_all_archs[] = 2 for example */

	for(idx_prio = 0; idx_prio < STARPU_HETEROPRIO_MAX_PRIO; ++idx_prio)
	{
		unsigned nb_arch_on_bucket = 0;
		for(arch_index = 0; arch_index < STARPU_NB_TYPES; ++arch_index)
		{
			if(hp->buckets[idx_prio].valid_archs & starpu_heteroprio_types_to_arch[arch_index])
			{
				nb_arch_on_bucket += 1;
			}
		}
		STARPU_ASSERT_MSG(check_all_archs[idx_prio] == nb_arch_on_bucket, "check_all_archs[idx_prio(%d)] = %d != nb_arch_on_bucket = %d\n", idx_prio, check_all_archs[idx_prio], nb_arch_on_bucket);
	}
}

static void deinitialize_heteroprio_policy(unsigned sched_ctx_id)
{
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	/* Ensure there are no more tasks */
	STARPU_ASSERT(hp->total_tasks_in_buckets == 0);
	unsigned arch_index;
	for(arch_index = 0; arch_index < STARPU_NB_TYPES; ++arch_index)
	{
		STARPU_ASSERT(hp->nb_remaining_tasks_per_arch_index[arch_index] == 0);
		STARPU_ASSERT(hp->nb_prefetched_tasks_per_arch_index[arch_index] == 0);
	}

	unsigned idx_prio;
	for(idx_prio = 0; idx_prio < STARPU_HETEROPRIO_MAX_PRIO; ++idx_prio)
	{
		_heteroprio_bucket_release(&hp->buckets[idx_prio]);
	}

	starpu_bitmap_destroy(hp->waiters);

	starpu_sched_ctx_delete_worker_collection(sched_ctx_id);
	STARPU_PTHREAD_MUTEX_DESTROY(&hp->policy_mutex);
	free(hp);
}

static void add_workers_heteroprio_policy(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	int workerid;
	unsigned i;
	for (i = 0; i < nworkers; i++)
	{
		workerid = workerids[i];
		memset(&hp->workers_heteroprio[workerid], 0, sizeof(hp->workers_heteroprio[workerid]));
		/* if the worker has already belonged to this context
		   the queue and the synchronization variables have been already initialized */
		if(hp->workers_heteroprio[workerid].tasks_queue == NULL)
		{
			hp->workers_heteroprio[workerid].tasks_queue = _starpu_create_fifo();
			switch(starpu_worker_get_type(workerid))
			{
#ifdef STARPU_USE_CPU
			case STARPU_CPU_WORKER:
				hp->workers_heteroprio[workerid].arch_type = STARPU_CPU;
				hp->workers_heteroprio[workerid].arch_index = STARPU_CPU_IDX;
				break;
#endif
#ifdef STARPU_USE_CUDA
			case STARPU_CUDA_WORKER:
				hp->workers_heteroprio[workerid].arch_type = STARPU_CUDA;
				hp->workers_heteroprio[workerid].arch_index = STARPU_CUDA_IDX;
				break;
#endif
#ifdef STARPU_USE_OPENCL
			case STARPU_OPENCL_WORKER:
				hp->workers_heteroprio[workerid].arch_type = STARPU_OPENCL;
				hp->workers_heteroprio[workerid].arch_index = STARPU_OPENCL_IDX;
				break;
#endif
			default:
				STARPU_ASSERT(0);
			}
		}
		hp->nb_workers_per_arch_index[hp->workers_heteroprio[workerid].arch_index]++;

	}
}

static void remove_workers_heteroprio_policy(unsigned sched_ctx_id, int *workerids, unsigned nworkers)
{
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	int workerid;
	unsigned i;
	for (i = 0; i < nworkers; i++)
	{
		workerid = workerids[i];
		if(hp->workers_heteroprio[workerid].tasks_queue != NULL)
		{
			_starpu_destroy_fifo(hp->workers_heteroprio[workerid].tasks_queue);
			hp->workers_heteroprio[workerid].tasks_queue = NULL;
		}
	}
}

/* Push a new task (simply store it and update counters) */
static int push_task_heteroprio_policy(struct starpu_task *task)
{
	unsigned sched_ctx_id = task->sched_ctx;
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

	/* One worker at a time use heteroprio */
	STARPU_PTHREAD_MUTEX_LOCK(&hp->policy_mutex);

	/* Retrieve the correct bucket */
	STARPU_ASSERT(task->priority < STARPU_HETEROPRIO_MAX_PRIO);
	struct _heteroprio_bucket* bucket = &hp->buckets[task->priority];
	/* Ensure that any worker that check that list can compute the task */
	STARPU_ASSERT_MSG(bucket->valid_archs, "The bucket %d does not have any archs\n", task->priority);
	STARPU_ASSERT(((bucket->valid_archs ^ task->cl->where) & bucket->valid_archs) == 0);

	/* save the task */
	_starpu_fifo_push_back_task(bucket->tasks_queue,task);

	/* Inc counters */
	unsigned arch_index;
	for(arch_index = 0; arch_index < STARPU_NB_TYPES; ++arch_index)
	{
		/* We test the archs on the bucket and not on task->cl->where since it is restrictive */
		if(bucket->valid_archs & starpu_heteroprio_types_to_arch[arch_index])
			hp->nb_remaining_tasks_per_arch_index[arch_index] += 1;
	}

	hp->total_tasks_in_buckets += 1;

	starpu_push_task_end(task);

	/*if there are no tasks_queue block */
	/* wake people waiting for a task */
	unsigned worker = 0;
	struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

	struct starpu_sched_ctx_iterator it;
#ifndef STARPU_NON_BLOCKING_DRIVERS
	char dowake[STARPU_NMAXWORKERS] = { 0 };
#endif

	workers->init_iterator(workers, &it);
	while(workers->has_next(workers, &it))
	{
		worker = workers->get_next(workers, &it);

#ifdef STARPU_NON_BLOCKING_DRIVERS
		if (!starpu_bitmap_get(hp->waiters, worker))
			/* This worker is not waiting for a task */
			continue;
#endif

		if (starpu_worker_can_execute_task_first_impl(worker, task, NULL))
		{
			/* It can execute this one, tell him! */
#ifdef STARPU_NON_BLOCKING_DRIVERS
			starpu_bitmap_unset(hp->waiters, worker);
			/* We really woke at least somebody, no need to wake somebody else */
			break;
#else
			dowake[worker] = 1;
#endif
		}
	}
	/* Let the task free */
	STARPU_PTHREAD_MUTEX_UNLOCK(&hp->policy_mutex);

#ifndef STARPU_NON_BLOCKING_DRIVERS
	/* Now that we have a list of potential workers, try to wake one */

	workers->init_iterator(workers, &it);
	while(workers->has_next(workers, &it))
	{
		worker = workers->get_next(workers, &it);
		if (dowake[worker])
			if (starpu_wake_worker(worker))
				break; // wake up a single worker
	}
#endif

	return 0;
}

static struct starpu_task *pop_task_heteroprio_policy(unsigned sched_ctx_id)
{
	const unsigned workerid = starpu_worker_get_id();
	struct _starpu_heteroprio_data *hp = (struct _starpu_heteroprio_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
	struct _heteroprio_worker_wrapper* worker = &hp->workers_heteroprio[workerid];

	/* If no tasks available, no tasks in worker queue or some arch worker queue just return NULL */
	if ((hp->total_tasks_in_buckets == 0 || hp->nb_remaining_tasks_per_arch_index[worker->arch_index] == 0)
            && worker->tasks_queue->ntasks == 0 && hp->nb_prefetched_tasks_per_arch_index[worker->arch_index] == 0){
		return NULL;
	}

#ifdef STARPU_NON_BLOCKING_DRIVERS
	if (starpu_bitmap_get(hp->waiters, workerid))
	{
		/* Nobody woke us, avoid bothering the mutex */
		return NULL;
	}
#endif
	starpu_pthread_mutex_t *worker_sched_mutex;
	starpu_pthread_cond_t *worker_sched_cond;
	starpu_worker_get_sched_condition(workerid, &worker_sched_mutex, &worker_sched_cond);

	/* Note: Releasing this mutex before taking the victim mutex, to avoid interlock*/
	STARPU_PTHREAD_MUTEX_UNLOCK(worker_sched_mutex);

	STARPU_PTHREAD_MUTEX_LOCK(&hp->policy_mutex);

	/* keep track of the new added task to perfom real prefetch on node */
	unsigned nb_added_tasks = 0;

	/* Check that some tasks are available for the current worker arch */
	if( hp->nb_remaining_tasks_per_arch_index[worker->arch_index] != 0 )
	{
		/* Ideally we would like to fill the prefetch array */
		unsigned nb_tasks_to_prefetch = (STARPU_HETEROPRIO_MAX_PREFETCH-worker->tasks_queue->ntasks);
		/* But there are maybe less tasks than that! */
		if(nb_tasks_to_prefetch > hp->nb_remaining_tasks_per_arch_index[worker->arch_index])
		{
			nb_tasks_to_prefetch = hp->nb_remaining_tasks_per_arch_index[worker->arch_index];
		}
		/* But in case there are less tasks than worker we take the minimum */
		if(hp->nb_remaining_tasks_per_arch_index[worker->arch_index] < starpu_sched_ctx_get_nworkers(sched_ctx_id))
		{
			if(worker->tasks_queue->ntasks == 0)
				nb_tasks_to_prefetch = 1;
			else
				nb_tasks_to_prefetch = 0;
		}

		unsigned idx_prio, arch_index;
		/* We iterate until we found all the tasks we need */
		for(idx_prio = 0; nb_tasks_to_prefetch && idx_prio < hp->nb_prio_per_arch_index[worker->arch_index]; ++idx_prio)
		{
			/* Retrieve the bucket using the mapping */
			struct _heteroprio_bucket* bucket = &hp->buckets[hp->prio_mapping_per_arch_index[worker->arch_index][idx_prio]];
			/* Ensure we can compute task from this bucket */
			STARPU_ASSERT(bucket->valid_archs & worker->arch_type);
			/* Take nb_tasks_to_prefetch tasks if possible */
			while(!_starpu_fifo_empty(bucket->tasks_queue) && nb_tasks_to_prefetch &&
			      (bucket->factor_base_arch_index == 0 ||
			       worker->arch_index == bucket->factor_base_arch_index ||
			       (((float)bucket->tasks_queue->ntasks)/((float)hp->nb_workers_per_arch_index[bucket->factor_base_arch_index])) >= bucket->slow_factors_per_index[worker->arch_index]))
			{
				struct starpu_task* task = _starpu_fifo_pop_local_task(bucket->tasks_queue);
				STARPU_ASSERT(starpu_worker_can_execute_task(workerid, task, 0));
				/* Save the task */
				_starpu_fifo_push_task(worker->tasks_queue, task);

				/* Update general counter */
				hp->nb_prefetched_tasks_per_arch_index[worker->arch_index] += 1;
				hp->total_tasks_in_buckets -= 1;

				for(arch_index = 0; arch_index < STARPU_NB_TYPES; ++arch_index)
				{
					/* We test the archs on the bucket and not on task->cl->where since it is restrictive */
					if(bucket->valid_archs & starpu_heteroprio_types_to_arch[arch_index])
					{
						hp->nb_remaining_tasks_per_arch_index[arch_index] -= 1;
					}
				}
				/* Decrease the number of tasks to found */
				nb_tasks_to_prefetch -= 1;
				nb_added_tasks       += 1;
				// TODO starpu_prefetch_task_input_on_node(task, workerid);
			}
		}		
	}

	struct starpu_task* task = NULL;

	/* The worker has some tasks in its queue */
	if(worker->tasks_queue->ntasks)
	{
		task = _starpu_fifo_pop_task(worker->tasks_queue, workerid);
		hp->nb_prefetched_tasks_per_arch_index[worker->arch_index] -= 1;
	}
	/* Otherwise look if we can steal some work */
	else if(hp->nb_prefetched_tasks_per_arch_index[worker->arch_index])
	{
		/* If HETEROPRIO_MAX_PREFETCH==1 it should not be possible to steal work */
		STARPU_ASSERT(STARPU_HETEROPRIO_MAX_PREFETCH != 1);

		struct starpu_worker_collection *workers = starpu_sched_ctx_get_worker_collection(sched_ctx_id);

		struct starpu_sched_ctx_iterator it;

		workers->init_iterator(workers, &it);
		unsigned victim = workerid;
		unsigned current_worker;
		while(workers->has_next(workers, &it))
		{
			current_worker = workers->get_next(workers, &it);
			if(current_worker == victim)
				break;
		}

		/* circular loop */
		while(1)
		{
			while(workers->has_next(workers, &it))
			{
				victim = workers->get_next(workers, &it);
				if(victim == workerid)
					continue;

				/* If it is the same arch and there is a task to steal */
				if(hp->workers_heteroprio[victim].arch_index == worker->arch_index
				   && hp->workers_heteroprio[victim].tasks_queue->ntasks){
					starpu_pthread_mutex_t *victim_sched_mutex;
					starpu_pthread_cond_t *victim_sched_cond;
					starpu_worker_get_sched_condition(victim, &victim_sched_mutex, &victim_sched_cond);

					/* ensure the worker is not currently prefetching its data */
					STARPU_PTHREAD_MUTEX_LOCK(victim_sched_mutex);

					if(hp->workers_heteroprio[victim].arch_index == worker->arch_index
					   && hp->workers_heteroprio[victim].tasks_queue->ntasks)
					{
						/* steal the last added task */
						task = starpu_task_list_pop_back(&hp->workers_heteroprio[victim].tasks_queue->taskq);
						/* we steal a task update global counter */
						hp->nb_prefetched_tasks_per_arch_index[hp->workers_heteroprio[victim].arch_index] -= 1;

						STARPU_PTHREAD_MUTEX_UNLOCK(victim_sched_mutex);
						break;
					}
					STARPU_PTHREAD_MUTEX_UNLOCK(victim_sched_mutex);
				}
			}
		}
	}

	if (!task)
	{
		/* Tell pushers that we are waiting for tasks_queue for us */
		starpu_bitmap_set(hp->waiters, workerid);
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(&hp->policy_mutex);

	STARPU_PTHREAD_MUTEX_LOCK(worker_sched_mutex);
	if(task)
	{
		unsigned child_sched_ctx = starpu_sched_ctx_worker_is_master_for_child_ctx(workerid, sched_ctx_id);
		if(child_sched_ctx != STARPU_NMAX_SCHED_CTXS)
		{
			starpu_sched_ctx_move_task_to_ctx(task, child_sched_ctx, 1);
			starpu_sched_ctx_revert_task_counters(sched_ctx_id, task->flops);
			return NULL;
		}
	}

	/* if we have task (task) me way have some in the queue (worker->tasks_queue_size) that was freshly addeed (nb_added_tasks) */
	if(task && worker->tasks_queue->ntasks && nb_added_tasks && starpu_get_prefetch_flag())
	{
		const unsigned memory_node = starpu_worker_get_memory_node(workerid);

/* TOTO berenger: iterate in the other sense */
		struct starpu_task *task_to_prefetch = NULL;
		for (task_to_prefetch  = starpu_task_list_begin(&worker->tasks_queue->taskq);
		     (task_to_prefetch != starpu_task_list_end(&worker->tasks_queue->taskq) &&
		      nb_added_tasks && hp->nb_remaining_tasks_per_arch_index[worker->arch_index] != 0);
		     task_to_prefetch  = starpu_task_list_next(task_to_prefetch))
		{
			/* prefetch from closest to end task */
			starpu_prefetch_task_input_on_node(task_to_prefetch, memory_node);
			nb_added_tasks -= 1;
		}
	}

	return task;
}

struct starpu_sched_policy _starpu_sched_heteroprio_policy =
{
        .init_sched = initialize_heteroprio_policy,
        .deinit_sched = deinitialize_heteroprio_policy,
        .add_workers = add_workers_heteroprio_policy,
        .remove_workers = remove_workers_heteroprio_policy,
        .push_task = push_task_heteroprio_policy,
	.simulate_push_task = NULL,
        .push_task_notify = NULL,
	.pop_task = pop_task_heteroprio_policy,
	.pre_exec_hook = NULL,
        .post_exec_hook = NULL,
	.pop_every_task = NULL,
        .policy_name = "heteroprio",
        .policy_description = "heteroprio"
};