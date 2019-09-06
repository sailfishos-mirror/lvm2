/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef LIB_DEVICE_IO_PARALLEL_H
#define LIB_DEVICE_IO_PARALLEL_H

#include "lib/device/io-manager.h"

//----------------------------------------------------------------

// io-manager utility that let you run a common task on many
// io-manager blocks in parallel.  This doesn't use multiple threads
// but it does take care to prefetch data in parallel, so you will
// get a big speed up over a simple get/process/put loop.

struct io_processor;
typedef void (*io_task_fn)(void *context, void *data, uint64_t len);
typedef void (*io_error_fn)(void *context);


struct io_processor *io_processor_create(struct io_manager *iom,
                                         io_task_fn t, io_error_fn err);
void io_processor_destroy(struct io_processor *iop);

// path is copied. start and len are in bytes.
bool io_processor_add(struct io_processor *iop, const char *path, uint64_t start,
                      uint64_t len, void *context);
void io_processor_exec(struct io_processor *iop);


//----------------------------------------------------------------
// For unit testing
 
struct processor_ops {
	void (*destroy)(struct processor_ops *ops);
	unsigned (*batch_size)(struct processor_ops *ops);
	void *(*get_dev)(struct processor_ops *ops, const char *path, unsigned flags);
	void (*put_dev)(struct processor_ops *ops, void *dev);
	// returns the number of blocks covered
	unsigned (*prefetch_bytes)(struct processor_ops *ops, void *dev, uint64_t start, size_t len);
	bool (*read_bytes)(struct processor_ops *ops, void *dev, uint64_t start, size_t len, void *data);
};

struct io_processor *io_processor_create_internal(struct processor_ops *ops,
                                                  io_task_fn t, io_error_fn err);

//----------------------------------------------------------------

#endif
