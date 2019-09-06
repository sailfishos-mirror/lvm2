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

#include "lib/device/io-processor.h"

#include "base/data-struct/list.h"
#include "base/memory/zalloc.h"
#include "lib/log/lvm-logging.h"
#include "lib/log/log.h"

//----------------------------------------------------------------

struct job {
	struct dm_list list;

	char *path;
	uint64_t start;
	uint64_t len;
	void *context;
};

struct io_processor {
	struct processor_ops *ops;
	io_task_fn task;
	io_error_fn err;
	struct dm_list jobs;

	size_t buffer_size;
	void *buffer;
};

static void _free_job(struct job *j)
{
	free(j->path);
	free(j);
}

struct io_processor *io_processor_create_internal(struct processor_ops *ops,
                                                  io_task_fn t, io_error_fn err)
{
	struct io_processor *iop = zalloc(sizeof(*iop));

	if (iop) {
		iop->ops= ops;
		iop->task = t;
		iop->err = err;
		dm_list_init(&iop->jobs);
	}

	return iop;
}

void io_processor_destroy(struct io_processor *iop)
{
	struct job *j, *tmp;

	iop->ops->destroy(iop->ops);

	dm_list_iterate_items_safe(j, tmp, &iop->jobs)
		_free_job(j);

	free(iop->buffer);
	free(iop);
}

static bool _ensure_buffer(struct io_processor *iop, uint64_t sectors)
{
	uint64_t size = sectors * 512;
	void *buffer;

	// is the existing buffer big enough?
	if (size <= iop->buffer_size)
		return true;

	if (posix_memalign(&buffer, 8, size)) {
		log_error("unable to allocate job buffer");
		return false;
	}

	if (iop->buffer)
		free(iop->buffer);

	iop->buffer = buffer;
	iop->buffer_size = size;

	return true;
}

bool io_processor_add(struct io_processor *iop, const char *path,
                      uint64_t start, uint64_t len,
                      void *context)
{
	struct job *j = zalloc(sizeof(*j));

	if (!j)
		return false;

	j->path = strdup(path);
	if (!j->path) {
		free(j);
		return false;
	}

	j->start = start;
	j->len = len;
	j->context = context;

	if (!_ensure_buffer(iop, len)) {
		free(j->path);
		free(j);
		return false;
	}

	dm_list_add(&iop->jobs, &j->list);
	return true;
}

static void _fail_job(struct io_processor *iop, struct job *j)
{
	iop->err(j->context);
	dm_list_del(&j->list);
	_free_job(j);
}

static uint64_t min(uint64_t lhs, uint64_t rhs)
{
	if (lhs < rhs)
		return lhs;

	if (rhs < lhs)
		return rhs;

	return lhs;
}

static void _batch(struct io_processor *iop, unsigned count)
{
	unsigned blocks_covered;
	struct job *j, *tmp;
	struct dm_list batch;
	void *dev;

	dm_list_init(&batch);

	// prefetch
	dm_list_iterate_items_safe(j, tmp, &iop->jobs) {
		if (!count)
			break;

		dev = iop->ops->get_dev(iop->ops, j->path, EF_READ_ONLY);
		if (!dev) {
			_fail_job(iop, j);
			continue;
		}

		blocks_covered = iop->ops->prefetch_bytes(iop->ops, dev, j->start, j->len);
		iop->ops->put_dev(iop->ops, dev);

		count -= min(count, blocks_covered);
		dm_list_move(&batch, &j->list);
	}

	// read
	dm_list_iterate_items_safe(j, tmp, &batch) {
		dev = iop->ops->get_dev(iop->ops, j->path, EF_READ_ONLY);
		if (!dev) {
			_fail_job(iop, j);
			continue;
		}

		if (!iop->ops->read_bytes(iop->ops, dev, j->start, j->len, iop->buffer)) {
			iop->ops->put_dev(iop->ops, dev);
			_fail_job(iop, j);
			continue;
		}

		iop->ops->put_dev(iop->ops, dev);

		iop->task(j->context, iop->buffer, j->len);
		dm_list_del(&j->list);
		_free_job(j);
	}
}

void io_processor_exec(struct io_processor *iop)
{
	unsigned batch_size = iop->ops->batch_size(iop->ops);

	while (!dm_list_empty(&iop->jobs))
		_batch(iop, batch_size);
}

//----------------------------------------------------------------

struct iom_ops {
	struct processor_ops ops;
	struct io_manager *iom;
};

// How many blocks does a byte range cover?
static unsigned _blocks_covered(struct io_manager *iom, uint64_t start, uint64_t len)
{
	uint64_t bs = io_block_sectors(iom) * 512;
	uint64_t b = start / bs;
	uint64_t e = (start + len + bs - 1) / bs;

	return e - b;
}

static void _iom_destroy(struct processor_ops *ops)
{
	struct iom_ops *p = container_of(ops, struct iom_ops, ops);
	free(p);
}

static unsigned _iom_batch_size(struct processor_ops *ops)
{
	struct io_manager *iom = container_of(ops, struct iom_ops, ops)->iom;
	return io_max_prefetches(iom);
}

static void *_iom_get_dev(struct processor_ops *ops, const char *path, unsigned flags)
{
	struct io_manager *iom = container_of(ops, struct iom_ops, ops)->iom;
	return io_get_dev(iom, path, flags);
}

static void _iom_put_dev(struct processor_ops *ops, void *dev)
{
	return io_put_dev(dev);
}

static unsigned _iom_prefetch_bytes(struct processor_ops *ops, void *dev, uint64_t start, size_t len)
{
	struct io_manager *iom = container_of(ops, struct iom_ops, ops)->iom;
	io_prefetch_bytes(iom, dev, start, len);

	return _blocks_covered(iom, start, len);
}

static bool _iom_read_bytes(struct processor_ops *ops, void *dev, uint64_t start, size_t len, void *data)
{
	struct io_manager *iom = container_of(ops, struct iom_ops, ops)->iom;
	return io_read_bytes(iom, dev, start, len, data);
}

struct io_processor *io_processor_create(struct io_manager *iom,
                                         io_task_fn t, io_error_fn err)
{
	struct io_processor *iop;
	struct iom_ops *p = zalloc(sizeof(*p));

	p->ops.destroy = _iom_destroy;
	p->ops.batch_size = _iom_batch_size;
	p->ops.get_dev = _iom_get_dev;
	p->ops.put_dev = _iom_put_dev;
	p->ops.prefetch_bytes = _iom_prefetch_bytes;
	p->ops.read_bytes = _iom_read_bytes;
	p->iom = iom;

	iop = io_processor_create_internal(&p->ops, t, err);
	if (!iop)
		free(p);

	return iop;
}

//----------------------------------------------------------------
