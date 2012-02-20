#include <sched_ctx_hypervisor_intern.h>

unsigned imposed_resize = 0;
struct starpu_sched_ctx_hypervisor_criteria* criteria = NULL;

extern struct hypervisor_policy idle_policy;
extern struct hypervisor_policy app_driven_policy;
extern struct hypervisor_policy gflops_rate_policy;

static void idle_time_cb(unsigned sched_ctx, int worker, double idle_time);
static void pushed_task_cb(unsigned sched_ctx, int worker);
static void poped_task_cb(unsigned sched_ctx, int worker, double flops);
static void post_exec_hook_cb(unsigned sched_ctx, int taskid);
static void reset_idle_time_cb(unsigned sched_ctx, int  worker);

static void _set_elapsed_flops_per_sched_ctx(unsigned sched_ctx, double val);

static void _load_hypervisor_policy(int type)
{
	struct hypervisor_policy *policy = NULL;

	switch(type)
	{
	case IDLE_POLICY:
		policy = &idle_policy;
		break;
	case APP_DRIVEN_POLICY:
		policy = &app_driven_policy;
		break;
	case GFLOPS_RATE_POLICY:
		policy = &gflops_rate_policy;
		break;

	}

	hypervisor.policy.resize = policy->resize;
}

struct starpu_sched_ctx_hypervisor_criteria** sched_ctx_hypervisor_init(int type)
{
	hypervisor.min_tasks = 0;
	hypervisor.nsched_ctxs = 0;
	pthread_mutex_init(&act_hypervisor_mutex, NULL);
	int i;
	for(i = 0; i < STARPU_NMAX_SCHED_CTXS; i++)
	{
		hypervisor.resize[i] = 0;
		hypervisor.configurations[i] = NULL;
		hypervisor.sched_ctxs[i] = STARPU_NMAX_SCHED_CTXS;
		hypervisor.sched_ctx_w[i].sched_ctx = STARPU_NMAX_SCHED_CTXS;
		hypervisor.sched_ctx_w[i].config = NULL;
		hypervisor.sched_ctx_w[i].temp_npushed_tasks = 0;
		hypervisor.sched_ctx_w[i].temp_npoped_tasks = 0;
		hypervisor.sched_ctx_w[i].total_flops = 0.0;
		hypervisor.sched_ctx_w[i].remaining_flops = 0.0;
		hypervisor.sched_ctx_w[i].start_time = 0.0;
		hypervisor.sched_ctx_w[i].bef_res_exp_end = 0.0;
		hypervisor.sched_ctx_w[i].resize_ack.receiver_sched_ctx = -1;
		hypervisor.sched_ctx_w[i].resize_ack.moved_workers = NULL;
		hypervisor.sched_ctx_w[i].resize_ack.nmoved_workers = 0;

		int j;
		for(j = 0; j < STARPU_NMAXWORKERS; j++)
		{
			hypervisor.sched_ctx_w[i].current_idle_time[j] = 0.0;
			hypervisor.sched_ctx_w[i].pushed_tasks[j] = 0;
			hypervisor.sched_ctx_w[i].poped_tasks[j] = 0;
			hypervisor.sched_ctx_w[i].elapsed_flops[j] = 0.0;
			hypervisor.sched_ctx_w[i].total_elapsed_flops[j] = 0.0;

		}
	}

	_load_hypervisor_policy(type);

	criteria = (struct starpu_sched_ctx_hypervisor_criteria*)malloc(sizeof(struct starpu_sched_ctx_hypervisor_criteria));
	criteria->idle_time_cb = idle_time_cb;
	criteria->pushed_task_cb = pushed_task_cb;
	criteria->poped_task_cb = poped_task_cb;
	criteria->post_exec_hook_cb = post_exec_hook_cb;
	criteria->reset_idle_time_cb = reset_idle_time_cb;
	return criteria;
}

void sched_ctx_hypervisor_stop_resize(unsigned sched_ctx)
{
	imposed_resize = 1;
	hypervisor.resize[sched_ctx] = 0;
}

void sched_ctx_hypervisor_start_resize(unsigned sched_ctx)
{
	imposed_resize = 1;
	hypervisor.resize[sched_ctx] = 1;
}

void sched_ctx_hypervisor_shutdown(void)
{
	printf("shutdown\n");
	int i;
	for(i = 0; i < STARPU_NMAX_SCHED_CTXS; i++)
	{
                if(hypervisor.sched_ctxs[i] != STARPU_NMAX_SCHED_CTXS && hypervisor.nsched_ctxs > 0)
		{
			sched_ctx_hypervisor_stop_resize(hypervisor.sched_ctxs[i]);
			sched_ctx_hypervisor_ignore_ctx(hypervisor.sched_ctxs[i]);
		}
	}
	criteria->idle_time_cb = NULL;
	criteria->pushed_task_cb = NULL;
	criteria->poped_task_cb = NULL;
	criteria->post_exec_hook_cb = NULL;
	criteria->reset_idle_time_cb = NULL;

	free(criteria);
	criteria = NULL;

	pthread_mutex_destroy(&act_hypervisor_mutex);
}

void sched_ctx_hypervisor_handle_ctx(unsigned sched_ctx, double total_flops)
{	
	hypervisor.configurations[sched_ctx] = (struct starpu_htbl32_node_s*)malloc(sizeof(struct starpu_htbl32_node_s));
	hypervisor.steal_requests[sched_ctx] = (struct starpu_htbl32_node_s*)malloc(sizeof(struct starpu_htbl32_node_s));
	hypervisor.resize_requests[sched_ctx] = (struct starpu_htbl32_node_s*)malloc(sizeof(struct starpu_htbl32_node_s));

	add_config(sched_ctx);
	hypervisor.sched_ctx_w[sched_ctx].sched_ctx = sched_ctx;
	hypervisor.sched_ctxs[hypervisor.nsched_ctxs++] = sched_ctx;

	hypervisor.sched_ctx_w[sched_ctx].total_flops = total_flops;
	hypervisor.sched_ctx_w[sched_ctx].remaining_flops = total_flops;
}

static int _get_first_free_sched_ctx(int *sched_ctxs, unsigned nsched_ctxs)
{
        int i;
        for(i = 0; i < nsched_ctxs; i++)
                if(sched_ctxs[i] == STARPU_NMAX_SCHED_CTXS)
                        return i;

        return STARPU_NMAX_SCHED_CTXS;
}

/* rearange array of sched_ctxs in order not to have {MAXVAL, MAXVAL, 5, MAXVAL, 7}    
   and have instead {5, 7, MAXVAL, MAXVAL, MAXVAL}                                    
   it is easier afterwards to iterate the array                           
*/
static void _rearange_sched_ctxs(int *sched_ctxs, int old_nsched_ctxs)
{
        int first_free_id = STARPU_NMAX_SCHED_CTXS;
        int i;
        for(i = 0; i < old_nsched_ctxs; i++)
        {
                if(sched_ctxs[i] != STARPU_NMAX_SCHED_CTXS)
                {
                        first_free_id = _get_first_free_sched_ctx(sched_ctxs, old_nsched_ctxs);
                        if(first_free_id != STARPU_NMAX_SCHED_CTXS)
			{
                                sched_ctxs[first_free_id] = sched_ctxs[i];
				sched_ctxs[i] = STARPU_NMAX_SCHED_CTXS;
			}
                }
	}
}

void sched_ctx_hypervisor_ignore_ctx(unsigned sched_ctx)
{
        unsigned i;
        for(i = 0; i < hypervisor.nsched_ctxs; i++)
        {
                if(hypervisor.sched_ctxs[i] == sched_ctx)
                {
                        hypervisor.sched_ctxs[i] = STARPU_NMAX_SCHED_CTXS;
			break;
                }
        }

        _rearange_sched_ctxs(hypervisor.sched_ctxs, hypervisor.nsched_ctxs);
	hypervisor.nsched_ctxs--;
	hypervisor.sched_ctx_w[sched_ctx].sched_ctx = STARPU_NMAX_SCHED_CTXS;
	remove_config(sched_ctx);

	free(hypervisor.configurations[sched_ctx]);
	free(hypervisor.steal_requests[sched_ctx]);
	free(hypervisor.resize_requests[sched_ctx]);
}

static int get_ntasks( int *tasks)
{
	int ntasks = 0;
	int j;
	for(j = 0; j < STARPU_NMAXWORKERS; j++)
	{
		ntasks += tasks[j];
	}
	return ntasks;
}

static void reset_ntasks( int *tasks)
{
	int j;
	for(j = 0; j < STARPU_NMAXWORKERS; j++)
	{
		tasks[j] = 0;
	}
	return;
}

static void _get_cpus(int *workers, int nworkers, int *cpus, int *ncpus)
{
	int i, worker;
	*ncpus = 0;

	for(i = 0; i < nworkers; i++)
	{
		worker = workers[i];
		enum starpu_archtype arch = starpu_worker_get_type(worker);
		if(arch == STARPU_CPU_WORKER)
			cpus[(*ncpus)++] = worker;
	}
}

void sched_ctx_hypervisor_move_workers(unsigned sender_sched_ctx, unsigned receiver_sched_ctx, int* workers_to_move, unsigned nworkers_to_move)
{
	if((hypervisor.resize[sender_sched_ctx] || sched_ctx_hypervisor_get_flops_left_pct(sender_sched_ctx) == 0.0f) &&
	   nworkers_to_move > 0)
	{
		int j;
		printf("resize ctx %d with", sender_sched_ctx);
		for(j = 0; j < nworkers_to_move; j++)
			printf(" %d", workers_to_move[j]);
		printf("\n");

		hypervisor.sched_ctx_w[sender_sched_ctx].bef_res_exp_end = sched_ctx_hypervisor_get_exp_end(sender_sched_ctx);
		int *cpus = (int*) malloc(nworkers_to_move * sizeof(int));
		int ncpus;

		_get_cpus(workers_to_move, nworkers_to_move, cpus, &ncpus);

		if(ncpus != 0)
			starpu_remove_workers_from_sched_ctx(cpus, ncpus, sender_sched_ctx);

		starpu_add_workers_to_sched_ctx(workers_to_move, nworkers_to_move, receiver_sched_ctx);

		hypervisor.sched_ctx_w[sender_sched_ctx].resize_ack.receiver_sched_ctx = receiver_sched_ctx;
		hypervisor.sched_ctx_w[sender_sched_ctx].resize_ack.moved_workers = (int*)malloc(nworkers_to_move * sizeof(int));
		hypervisor.sched_ctx_w[sender_sched_ctx].resize_ack.nmoved_workers = nworkers_to_move;


		int i;
		for(i = 0; i < nworkers_to_move; i++)
		{
			hypervisor.sched_ctx_w[sender_sched_ctx].current_idle_time[workers_to_move[i]] = 0.0;
			hypervisor.sched_ctx_w[sender_sched_ctx].resize_ack.moved_workers[i] = workers_to_move[i];	
		}

		hypervisor.resize[sender_sched_ctx] = 0;
	}

	return;
}

unsigned _check_for_resize_ack(unsigned sender_sched_ctx, unsigned receiver_sched_ctx, int *moved_workers, int nmoved_workers)
{
	struct sched_ctx_wrapper *sender_sc_w = &hypervisor.sched_ctx_w[sender_sched_ctx];
	struct sched_ctx_wrapper *receiver_sc_w = &hypervisor.sched_ctx_w[receiver_sched_ctx];
	int i;
	for(i = 0; i < nmoved_workers; i++)
	{
		int worker = moved_workers[i];
		if(receiver_sc_w->elapsed_flops[worker] == 0.0f)
			return 0;
	}

	sender_sc_w->resize_ack.receiver_sched_ctx = -1;
	sender_sc_w->resize_ack.nmoved_workers = 0;
	free(sender_sc_w->resize_ack.moved_workers);

	double start_time =  starpu_timing_now();
	sender_sc_w->start_time = start_time;
	sender_sc_w->remaining_flops = sender_sc_w->remaining_flops - _get_elapsed_flops_per_sched_ctx(sender_sched_ctx);
	_set_elapsed_flops_per_sched_ctx(sender_sched_ctx, 0.0);
	
	receiver_sc_w->start_time = start_time;
	receiver_sc_w->remaining_flops = receiver_sc_w->remaining_flops - _get_elapsed_flops_per_sched_ctx(receiver_sched_ctx);
	_set_elapsed_flops_per_sched_ctx(receiver_sched_ctx, 0.0);
	
	hypervisor.resize[sender_sched_ctx] = 1;
	return 1;
}

unsigned sched_ctx_hypervisor_resize(unsigned sched_ctx, int task_tag)
{
	if(task_tag == -1)
	{
		return app_driven_policy.resize(sched_ctx);
	}
	else
	{	
		unsigned *sched_ctx_pt = (unsigned*)malloc(sizeof(unsigned));
		*sched_ctx_pt = sched_ctx;
		_starpu_htbl_insert_32(&hypervisor.resize_requests[sched_ctx], (uint32_t)task_tag, (void*)sched_ctx_pt);	
		return 0;
	}
}

void get_overage_workers(unsigned sched_ctx, int *workerids, int nworkers, int *overage_workers, int *noverage_workers)
{
	struct worker_collection *workers = starpu_get_worker_collection_of_sched_ctx(sched_ctx);
	int worker, i, found = -1;

	if(workers->init_cursor)
		workers->init_cursor(workers);

	while(workers->has_next(workers))
	{
		worker = workers->get_next(workers);
		for(i = 0; i < nworkers; i++)
			if(workerids[i] == worker)
			{
				found = worker;
				break;
			}
		if(found == -1)
			overage_workers[(*noverage_workers)++]  = worker;
		found = -1;
	}

	if(workers->init_cursor)
		workers->deinit_cursor(workers);
}

void sched_ctx_hypervisor_steal_workers(unsigned sched_ctx, int *workerids, int nworkers, int task_tag)
{
	/* do it right now */
	if(task_tag == -1)	
	{
		pthread_mutex_lock(&act_hypervisor_mutex);
		
		if(hypervisor.sched_ctx_w[sched_ctx].sched_ctx != STARPU_NMAX_SCHED_CTXS)
		{
			printf("do request\n");

			int overage_workers[STARPU_NMAXWORKERS];
			int noverage_workers = 0;
			get_overage_workers(sched_ctx, workerids, nworkers, overage_workers, &noverage_workers);
			starpu_add_workers_to_sched_ctx(workerids, nworkers, sched_ctx);

			sched_ctx_hypervisor_ioctl(sched_ctx, 
						   HYPERVISOR_PRIORITY, workerids, nworkers, 1,
						   NULL);		

			if(noverage_workers > 0)
				starpu_remove_workers_from_sched_ctx(overage_workers, noverage_workers, sched_ctx);
			
			int i;
			for(i = 0; i < hypervisor.nsched_ctxs; i++)
				if(hypervisor.sched_ctxs[i] != sched_ctx && hypervisor.sched_ctxs[i] != STARPU_NMAX_SCHED_CTXS)
					starpu_remove_workers_from_sched_ctx(workerids, nworkers, hypervisor.sched_ctxs[i]);
		}
		
		pthread_mutex_unlock(&act_hypervisor_mutex);
	}
	else
	{
		struct sched_ctx_hypervisor_adjustment* adjustment = (struct sched_ctx_hypervisor_adjustment*)malloc(sizeof(struct sched_ctx_hypervisor_adjustment));
		int i;
		for(i = 0; i < nworkers; i++)
			adjustment->workerids[i] = workerids[i];
		adjustment->nworkers = nworkers;
		
		_starpu_htbl_insert_32(&hypervisor.steal_requests[sched_ctx], (uint32_t)task_tag, (void*)adjustment);	
	}

	return ;
}

double sched_ctx_hypervisor_get_idle_time(unsigned sched_ctx, int worker)
{
	return hypervisor.sched_ctx_w[sched_ctx].current_idle_time[worker];
}

double sched_ctx_hypervisor_get_bef_res_exp_end(unsigned sched_ctx)
{
	return hypervisor.sched_ctx_w[sched_ctx].bef_res_exp_end;
}

static void reset_idle_time_cb(unsigned sched_ctx, int worker)
{
	if(hypervisor.resize[sched_ctx])
		hypervisor.sched_ctx_w[sched_ctx].current_idle_time[worker] = 0.0;
}

static void idle_time_cb(unsigned sched_ctx, int worker, double idle_time)
{
	if(hypervisor.nsched_ctxs > 1 && idle_policy.resize)
	{
		struct sched_ctx_wrapper *sc_w = &hypervisor.sched_ctx_w[sched_ctx];
		sc_w->current_idle_time[worker] += idle_time;
		if(hypervisor.resize[sched_ctx])
		{
			struct policy_config *config = sc_w->config;
			if(config != NULL &&  sc_w->current_idle_time[worker] > config->max_idle[worker])
				idle_policy.resize(sched_ctx);
		}		
		else if(sc_w->resize_ack.receiver_sched_ctx != -1)
		{
			_check_for_resize_ack(sched_ctx, sc_w->resize_ack.receiver_sched_ctx,
					      sc_w->resize_ack.moved_workers, sc_w->resize_ack.nmoved_workers);
		}
	}
	return;
}

int* sched_ctx_hypervisor_get_sched_ctxs()
{
	return hypervisor.sched_ctxs;
}

int sched_ctx_hypervisor_get_nsched_ctxs()
{
	return hypervisor.nsched_ctxs;
}

static double _get_total_elapsed_flops_per_sched_ctx(unsigned sched_ctx)
{
	double ret_val = 0.0;
	int i;
	for(i = 0; i < STARPU_NMAXWORKERS; i++)
		ret_val += hypervisor.sched_ctx_w[sched_ctx].total_elapsed_flops[i];
	return ret_val;
}

static double _get_elapsed_flops_per_sched_ctx(unsigned sched_ctx)
{
	double ret_val = 0.0;
	int i;
	for(i = 0; i < STARPU_NMAXWORKERS; i++)
		ret_val += hypervisor.sched_ctx_w[sched_ctx].elapsed_flops[i];
	return ret_val;
}

static double _get_elapsed_flops_per_cpus(unsigned sched_ctx, int *ncpus)
{
	double ret_val = 0.0;

	struct worker_collection *workers = starpu_get_worker_collection_of_sched_ctx(sched_ctx);
        int worker;

	if(workers->init_cursor)
                workers->init_cursor(workers);

        while(workers->has_next(workers))
	{
                worker = workers->get_next(workers);
                enum starpu_archtype arch = starpu_worker_get_type(worker);
                if(arch == STARPU_CPU_WORKER)
                {
			ret_val += hypervisor.sched_ctx_w[sched_ctx].elapsed_flops[worker];
			(*ncpus)++;
                }
        }

	if(workers->init_cursor)
		workers->deinit_cursor(workers);

	return ret_val;
}


static void _set_elapsed_flops_per_sched_ctx(unsigned sched_ctx, double val)
{
	int i;
	for(i = 0; i < STARPU_NMAXWORKERS; i++)
		hypervisor.sched_ctx_w[sched_ctx].elapsed_flops[i] = val;
}

double sched_ctx_hypervisor_get_exp_end(unsigned sched_ctx)
{	
	double elapsed_flops = _get_elapsed_flops_per_sched_ctx(sched_ctx);

	if( elapsed_flops != 0.0)
	{
		double curr_time = starpu_timing_now();
		double elapsed_time = curr_time - hypervisor.sched_ctx_w[sched_ctx].start_time;
		double exp_end = (elapsed_time * hypervisor.sched_ctx_w[sched_ctx].remaining_flops / 
				  elapsed_flops) + curr_time;
		return exp_end;
	}
	return -1.0;
}

double sched_ctx_hypervisor_get_ctx_velocity(unsigned sched_ctx)
{
        double elapsed_flops = _get_elapsed_flops_per_sched_ctx(sched_ctx);

        if( elapsed_flops != 0.0)
        {
                double curr_time = starpu_timing_now();
                double elapsed_time = curr_time - hypervisor.sched_ctx_w[sched_ctx].start_time;
                return elapsed_flops/elapsed_time;
        }
}

double sched_ctx_hypervisor_get_cpu_velocity(unsigned sched_ctx)
{
        int ncpus = 0;
        double elapsed_flops = _get_elapsed_flops_per_cpus(sched_ctx, &ncpus);

        if( elapsed_flops != 0.0)
        {
                double curr_time = starpu_timing_now();
                double elapsed_time = curr_time - hypervisor.sched_ctx_w[sched_ctx].start_time;
                return (elapsed_flops/elapsed_time) / ncpus;
        }

        return -1.0;
}

double sched_ctx_hypervisor_get_flops_left(unsigned sched_ctx)
{
        return hypervisor.sched_ctx_w[sched_ctx].remaining_flops;
}

double sched_ctx_hypervisor_get_flops_left_pct(unsigned sched_ctx)
{
	struct sched_ctx_wrapper *wrapper = &hypervisor.sched_ctx_w[sched_ctx];
	double total_elapsed_flops = _get_total_elapsed_flops_per_sched_ctx(sched_ctx);
	if(wrapper->total_flops == total_elapsed_flops || total_elapsed_flops > wrapper->total_flops)
		return 0.0;
       
	return (wrapper->total_flops - total_elapsed_flops)/wrapper->total_flops;
}

static void pushed_task_cb(unsigned sched_ctx, int worker)
{	
	hypervisor.sched_ctx_w[sched_ctx].pushed_tasks[worker]++;
	if(hypervisor.sched_ctx_w[sched_ctx].total_flops != 0.0 && hypervisor.sched_ctx_w[sched_ctx].start_time == 0.0)
		hypervisor.sched_ctx_w[sched_ctx].start_time = starpu_timing_now();
	
	int ntasks = get_ntasks(hypervisor.sched_ctx_w[sched_ctx].pushed_tasks);
	
	if(!imposed_resize && ntasks == hypervisor.min_tasks)
		hypervisor.resize[sched_ctx] = 1;
}

static void poped_task_cb(unsigned sched_ctx, int worker, double elapsed_flops)
{
	hypervisor.sched_ctx_w[sched_ctx].poped_tasks[worker]++;
	hypervisor.sched_ctx_w[sched_ctx].elapsed_flops[worker] += elapsed_flops;
	hypervisor.sched_ctx_w[sched_ctx].total_elapsed_flops[worker] += elapsed_flops;

	if(hypervisor.nsched_ctxs > 1)
	{
		struct sched_ctx_wrapper *sc_w = &hypervisor.sched_ctx_w[sched_ctx];
		if(hypervisor.resize[sched_ctx] || sched_ctx_hypervisor_get_flops_left_pct(sched_ctx) == 0.0f)
			gflops_rate_policy.resize(sched_ctx);
		else if(sc_w->resize_ack.receiver_sched_ctx != -1)
		{
			_check_for_resize_ack(sched_ctx, sc_w->resize_ack.receiver_sched_ctx,
					      sc_w->resize_ack.moved_workers, sc_w->resize_ack.nmoved_workers);
		}
	}
}

static void post_exec_hook_cb(unsigned sched_ctx, int task_tag)
{
	STARPU_ASSERT(task_tag > 0);

//	if(hypervisor.nsched_ctxs > 1 && hypervisor.resize[sched_ctx])
	if(hypervisor.nsched_ctxs > 1)
	{
		unsigned conf_sched_ctx;
		int i;
		for(i = 0; i < hypervisor.nsched_ctxs; i++)
		{
			conf_sched_ctx = hypervisor.sched_ctxs[i];
			void *config = _starpu_htbl_search_32(hypervisor.configurations[conf_sched_ctx], (uint32_t)task_tag);
			if(config && config != hypervisor.configurations[conf_sched_ctx])
			{
				sched_ctx_hypervisor_set_config(conf_sched_ctx, config);
				free(config);
				_starpu_htbl_insert_32(&hypervisor.configurations[sched_ctx], (uint32_t)task_tag, NULL);
			}
		}	
		
		struct sched_ctx_hypervisor_adjustment *adjustment = (struct sched_ctx_hypervisor_adjustment*) _starpu_htbl_search_32(hypervisor.steal_requests[sched_ctx], (uint32_t)task_tag);
		if(adjustment && adjustment != hypervisor.steal_requests[sched_ctx])
		{
			sched_ctx_hypervisor_steal_workers(sched_ctx, adjustment->workerids, adjustment->nworkers, -1);
			free(adjustment);
			_starpu_htbl_insert_32(&hypervisor.steal_requests[sched_ctx], (uint32_t)task_tag, NULL);
		}
		
		unsigned *sched_ctx_pt = _starpu_htbl_search_32(hypervisor.resize_requests[sched_ctx], (uint32_t)task_tag);
		if(sched_ctx_pt && sched_ctx_pt != hypervisor.resize_requests[sched_ctx])
		{
			sched_ctx_hypervisor_resize(sched_ctx, -1);
			free(sched_ctx_pt);
			_starpu_htbl_insert_32(&hypervisor.resize_requests[sched_ctx], (uint32_t)task_tag, NULL);
		}
	}
}
