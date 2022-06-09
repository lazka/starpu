/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2008-2022  Université de Bordeaux, CNRS (LaBRI UMR 5800), Inria
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

#ifndef __DRIVER_GPU_H__
#define __DRIVER_GPU_H__

/** @file */

#include <common/config.h>
#include <starpu.h>
#include <core/workers.h>

#pragma GCC visibility push(hidden)

void _starpu_gpu_set_used(int devid);

// Detect which GPU devices are already used
void _starpu_gpu_clear(struct _starpu_machine_config *config, enum starpu_worker_archtype type);

void _starpu_gpu_clean();


#pragma GCC visibility pop

#endif //  __DRIVER_GPU_H__

