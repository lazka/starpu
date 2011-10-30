/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011  Université de Bordeaux 1
 * Copyright (C) 2011  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  Télécom-SudParis
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

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>

#include <starpu.h>
#include <starpu_perfmodel.h>
#include <core/perfmodel/perfmodel.h> // we need to browse the list associated to history-based models

#ifdef __MINGW32__
#include <windows.h>
#endif

static struct starpu_perfmodel_t model;

/* display all available models */
static int list = 0;
/* what kernel ? */
static char *symbol = NULL;
/* which architecture ? (NULL = all)*/
static char *arch = NULL;
/* Unless a FxT file is specified, we just display the model */
static int no_fxt_file = 1;

#ifdef STARPU_USE_FXT
static struct starpu_fxt_codelet_event *dumped_codelets;
static long dumped_codelets_count;
static struct starpu_fxt_options options;
#endif

#ifdef STARPU_USE_FXT
static int archtype_is_found[STARPU_NARCH_VARIATIONS];

static char data_file_name[256];
#endif
static char avg_file_name[256];
static char gnuplot_file_name[256];

static void usage(char **argv)
{
	fprintf(stderr, "Usage: %s [ options ]\n", argv[0]);
        fprintf(stderr, "\n");
	fprintf(stderr, "One must specify a symbol with the -s option or use -l\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "   -l                  display all available models\n");
        fprintf(stderr, "   -s <symbol>         specify the symbol\n");
	fprintf(stderr, "   -i <Fxt files>      input FxT files generated by StarPU\n");
        fprintf(stderr, "   -a <arch>           specify the architecture (e.g. cpu, cpu:k, cuda_k, gordon)\n");
        fprintf(stderr, "\n");
}

static void parse_args(int argc, char **argv)
{
#ifdef STARPU_USE_FXT
	/* Default options */
	starpu_fxt_options_init(&options);

	options.out_paje_path = NULL;
	options.activity_path = NULL;
	options.distrib_time_path = NULL;
	options.dag_path = NULL;

	options.dumped_codelets = &dumped_codelets;
#endif

	/* We want to support arguments such as "-i trace_*" */
	unsigned reading_input_filenames = 0;

	int i;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0) {
			symbol = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-i") == 0) {
			reading_input_filenames = 1;
#ifdef STARPU_USE_FXT
			options.filenames[options.ninputfiles++] = argv[++i];
			no_fxt_file = 0;
#else
			fprintf(stderr, "Warning: FxT support was not enabled in StarPU: FxT traces will thus be ignored!\n");
#endif
			continue;
		}

		if (strcmp(argv[i], "-l") == 0) {
			list = 1;
			continue;
		}

		if (strcmp(argv[i], "-a") == 0) {
			arch = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-h") == 0) {
			usage(argv);
		        exit(-1);
		}

		/* If the reading_input_filenames flag is set, and that the
		 * argument does not match an option, we assume this may be
		 * another filename */
		if (reading_input_filenames)
		{
#ifdef STARPU_USE_FXT
			options.filenames[options.ninputfiles++] = argv[i];
#endif
			continue;
		}
	}
}

static void print_comma(FILE *gnuplot_file, int *first)
{
	if (*first)
	{
		*first = 0;
	}
	else {
		fprintf(gnuplot_file, ",\\\n\t");
	}
}

static void display_perf_model(FILE *gnuplot_file, struct starpu_perfmodel_t *model, enum starpu_perf_archtype arch, int *first, unsigned nimpl)
{
	char arch_name[256];
	starpu_perfmodel_get_arch_name(arch, arch_name, 256, nimpl);

	struct starpu_per_arch_perfmodel_t *arch_model =
		&model->per_arch[arch][nimpl];

	if (arch_model->regression.valid || arch_model->regression.nl_valid)
		fprintf(stderr,"Arch: %s\n", arch_name);

#ifdef STARPU_USE_FXT
	if (!no_fxt_file && archtype_is_found[arch])
	{
		print_comma(gnuplot_file, first);
		fprintf(gnuplot_file, "\"< grep -w \\^%d %s\" using 2:3 title \"%s\"", arch, data_file_name, arch_name);
	}
#endif

	/* Only display the regression model if we could actually build a model */
	if (arch_model->regression.valid)
	{
		print_comma(gnuplot_file, first);
	
		fprintf(stderr, "\tLinear: y = alpha size ^ beta\n");
		fprintf(stderr, "\t\talpha = %e\n", arch_model->regression.alpha * 0.001);
		fprintf(stderr, "\t\tbeta = %e\n", arch_model->regression.beta);

		fprintf(gnuplot_file, "0.001 * %f * x ** %f title \"Linear Regression %s\"",
			arch_model->regression.alpha, arch_model->regression.beta, arch_name);
	}

	if (arch_model->regression.nl_valid)
	{
		print_comma(gnuplot_file, first);
	
		fprintf(stderr, "\tNon-Linear: y = a size ^b + c\n");
		fprintf(stderr, "\t\ta = %e\n", arch_model->regression.a * 0.001);
		fprintf(stderr, "\t\tb = %e\n", arch_model->regression.b);
		fprintf(stderr, "\t\tc = %e\n", arch_model->regression.c * 0.001);

		fprintf(gnuplot_file, "0.001 * %f * x ** %f + 0.001 * %f title \"Non-Linear Regression %s\"",
			arch_model->regression.a, arch_model->regression.b,  arch_model->regression.c, arch_name);
	}
}

static void display_history_based_perf_models(FILE *gnuplot_file, struct starpu_perfmodel_t *model, enum starpu_perf_archtype arch1, enum starpu_perf_archtype arch2, int *first)
{
	char *command;
	FILE *datafile;
	unsigned n = arch2 - arch1;
	unsigned arch;
	struct starpu_history_list_t *ptr[n], *ptrs[n];
	char archname[32];
	int col;
	int len;

	len = 10 + strlen(avg_file_name) + 1;
	command = (char *) malloc(len);
	snprintf(command, len, "sort -n > %s", avg_file_name);
	datafile = popen(command, "w");
	free(command);

	col = 2;
	unsigned implid;
	for (arch = arch1; arch < arch2; arch++) {
		for (implid = 0; implid < STARPU_MAXIMPLEMENTATIONS; implid++) {
			struct starpu_per_arch_perfmodel_t *arch_model =
				&model->per_arch[arch][implid];
			starpu_perfmodel_get_arch_name((enum starpu_perf_archtype) arch, archname, 32, implid);

			ptrs[arch-arch1] = ptr[arch-arch1] = arch_model->list;

			if (ptr[arch-arch1]) {
				print_comma(gnuplot_file, first);
				fprintf(gnuplot_file, "\"%s\" using 1:%d:%d with errorlines title \"Measured %s\"", avg_file_name, col, col+1, archname);
				col += 2;
			}
		}
	}

	while (1) {
		unsigned long minimum;
		/* Check whether there's data left */
		for (arch = arch1; arch < arch2; arch++) {
			if (ptr[arch-arch1])
				break;
		}
		if (arch == arch2)
			/* finished with archs */
			break;

		/* Get the minimum x */
		minimum = ULONG_MAX;
		for (arch = arch1; arch < arch2; arch++) {
			if (ptr[arch-arch1]) {
				struct starpu_history_entry_t *entry = ptr[arch-arch1]->entry;
				if (entry->size < minimum)
					minimum = entry->size;
			}
		}

		fprintf(stderr, "%lu ", minimum);
		fprintf(datafile, "%-15lu ", minimum);
		for (arch = arch1; arch < arch2; arch++) {
			if (ptr[arch-arch1]) {
				struct starpu_history_entry_t *entry = ptr[arch-arch1]->entry;
				if (entry->size == minimum) {
					fprintf(datafile, "\t%-15le\t%-15le", 0.001*entry->mean, 0.001*entry->deviation);
					ptr[arch-arch1] = ptr[arch-arch1]->next;
				} else
					fprintf(datafile, "\t\"\"\t\"\"");
			} else if (ptrs[arch-arch1]) {
				/* Finished for this arch only */
				fprintf(datafile, "\t\"\"\t\"\"");
			}
		}
		fprintf(datafile, "\n");
	}
	fprintf(stderr, "\n");
}

static void display_perf_models(FILE *gnuplot_file, struct starpu_perfmodel_t *model, enum starpu_perf_archtype arch1, enum starpu_perf_archtype arch2, int *first)
{
	unsigned arch;
	unsigned implid;
	for (arch = arch1; arch < arch2; arch++) {
		for (implid = 0; implid < STARPU_MAXIMPLEMENTATIONS; implid++) {
			display_perf_model(gnuplot_file, model, (enum starpu_perf_archtype) arch, first,
implid);
		}
	}
	display_history_based_perf_models(gnuplot_file, model, arch1, arch2, first);
}

#ifdef STARPU_USE_FXT
static void dump_data_file(FILE *data_file)
{
	memset(archtype_is_found, 0, STARPU_NARCH_VARIATIONS*sizeof(int));

	int i;
	for (i = 0; i < options.dumped_codelets_count; i++)
	{
		/* Dump only if the symbol matches user's request */
		if (strcmp(dumped_codelets[i].symbol, symbol) == 0) {
			enum starpu_perf_archtype archtype = dumped_codelets[i].archtype;
			archtype_is_found[archtype] = 1;

			size_t size = dumped_codelets[i].size;
			float time = dumped_codelets[i].time;

			fprintf(data_file, "%d	%f	%f\n", archtype, (float)size, time);
		}
	}
}
#endif

static void display_selected_models(FILE *gnuplot_file, struct starpu_perfmodel_t *model)
{
	fprintf(gnuplot_file, "#!/usr/bin/gnuplot -persist\n");
	fprintf(gnuplot_file, "\n");
	fprintf(gnuplot_file, "set term postscript eps enhanced color\n");
	fprintf(gnuplot_file, "set output \"starpu_%s.eps\"\n", symbol);
	fprintf(gnuplot_file, "set title \"Model for codelet %s\"\n", symbol);
	fprintf(gnuplot_file, "set xlabel \"Size\"\n");
	fprintf(gnuplot_file, "set ylabel \"Time\"\n");
	fprintf(gnuplot_file, "\n");
	fprintf(gnuplot_file, "set key top left\n");
	fprintf(gnuplot_file, "set logscale x\n");
	fprintf(gnuplot_file, "set logscale y\n");
	fprintf(gnuplot_file, "\n");

	/* If no input data is given to gnuplot, we at least need to specify an
	 * arbitrary range. */
	if (no_fxt_file)
		fprintf(gnuplot_file, "set xrange [10**3:10**9]\n\n");

	int first = 1;
	fprintf(gnuplot_file, "plot\t");

	if (arch == NULL)
	{
		/* display all architectures */
		display_perf_models(gnuplot_file, model, (enum starpu_perf_archtype) 0, (enum starpu_perf_archtype) STARPU_NARCH_VARIATIONS, &first);
	}
	else {
		if (strcmp(arch, "cpu") == 0) {
			unsigned impl;
			for (impl = 0; impl < STARPU_MAXIMPLEMENTATIONS; impl++) {
				display_perf_model(gnuplot_file, model,
							STARPU_CPU_DEFAULT,
							&first, impl);
			}
			return;
		}

		int k;
		if (sscanf(arch, "cpu:%d", &k) == 1)
		{
			/* For combined CPU workers */
			if ((k < 1) || (k > STARPU_MAXCPUS))
			{
				fprintf(stderr, "Invalid CPU size\n");
				exit(-1);
			}

			display_perf_models(gnuplot_file, model, (enum starpu_perf_archtype) (STARPU_CPU_DEFAULT + k - 1), (enum starpu_perf_archtype) (STARPU_CPU_DEFAULT + k), &first);
			return;
		}

		if (strcmp(arch, "cuda") == 0) {
			display_perf_models(gnuplot_file, model, STARPU_CUDA_DEFAULT, (enum starpu_perf_archtype) (STARPU_CUDA_DEFAULT + STARPU_MAXCUDADEVS), &first);
			return;
		}

		/* There must be a cleaner way ! */
		int gpuid;
		int nmatched;
		nmatched = sscanf(arch, "cuda_%d", &gpuid);
		if (nmatched == 1)
		{
			unsigned archid = STARPU_CUDA_DEFAULT+ gpuid;
			display_perf_models(gnuplot_file, model, (enum starpu_perf_archtype) archid, (enum starpu_perf_archtype) (archid + 1), &first);
			return;
		}

		if (strcmp(arch, "gordon") == 0) {
			display_perf_models(gnuplot_file, model, STARPU_GORDON_DEFAULT, (enum starpu_perf_archtype) (STARPU_GORDON_DEFAULT + 1), &first);
			return;
		}

		fprintf(stderr, "Unknown architecture requested, aborting.\n");
		exit(-1);
	}
}

int main(int argc, char **argv)
{
	int ret;

#ifdef __MINGW32__
	WSADATA wsadata;
	WSAStartup(MAKEWORD(1,0), &wsadata);
#endif

	parse_args(argc, argv);

        if (list) {
                int ret = starpu_list_models();
                if (ret) {
                        fprintf(stderr, "The performance model directory is invalid\n");
                        return 1;
                }
		return 0;
        }

	/* We need at least a symbol name */
	if (!symbol)
	{
		fprintf(stderr, "No symbol was specified\n");
		return 1;
	}

	/* Load the performance model associated to the symbol */
	ret = starpu_load_history_debug(symbol, &model);
	if (ret == 1)
	{
		fprintf(stderr, "The performance model could not be loaded\n");
		return 1;
	}

	/* If some FxT input was specified, we put the points on the graph */
#ifdef STARPU_USE_FXT
	if (!no_fxt_file)
	{
		starpu_fxt_generate_trace(&options);

		snprintf(data_file_name, 256, "starpu_%s.data", symbol);

		FILE *data_file = fopen(data_file_name, "w+");
		STARPU_ASSERT(data_file);
		dump_data_file(data_file);
		fclose(data_file);
	}
#endif

	snprintf(gnuplot_file_name, 256, "starpu_%s.gp", symbol);

	snprintf(avg_file_name, 256, "starpu_%s_avg.data", symbol);

	FILE *gnuplot_file = fopen(gnuplot_file_name, "w+");
	STARPU_ASSERT(gnuplot_file);
	display_selected_models(gnuplot_file, &model);
	fclose(gnuplot_file);

	/* Retrieve the current mode of the gnuplot executable */
	struct stat sb;
	ret = stat(gnuplot_file_name, &sb);
	if (ret)
	{
		perror("stat");
		STARPU_ABORT();
	}

	/* Make the gnuplot scrit executable for the owner */
	ret = chmod(gnuplot_file_name, sb.st_mode|S_IXUSR);
	if (ret)
	{
		perror("chmod");
		STARPU_ABORT();
	}

	return 0;
}
