/*
 * Copyright (C) 2018-2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/device/blkio.h"
#include "lib/misc/lvm-signal.h"

#ifdef HAVE_LIBAIO_H

#include <libaio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

//----------------------------------------------------------------

#define MAX_EVENT 64

static const char _async[] = "async";

//----------------------------------------------------------------

struct control_block {
	struct dm_list list;
	void *context;
	struct iocb cb;
};

struct cb_set {
	struct dm_list free;
	struct dm_list allocated;
	struct control_block vec[];
};

static struct cb_set *_cb_set_create(unsigned nr)
{
	unsigned i;
	struct cb_set *cbs = malloc(sizeof(*cbs) + nr * sizeof(*cbs->vec));

	if (!cbs)
		return NULL;

	dm_list_init(&cbs->free);
	dm_list_init(&cbs->allocated);

	for (i = 0; i < nr; i++)
		dm_list_add(&cbs->free, &cbs->vec[i].list);

	return cbs;
}

static void _cb_set_destroy(struct cb_set *cbs)
{
	if (!cbs)
		return;

	if (!dm_list_empty(&cbs->allocated))
		log_warn("WARNING: async io engine %u control blocks still allocated at destroy.",
			 dm_list_size(&cbs->allocated));

	free(cbs);
}

static struct control_block *_cb_alloc(struct cb_set *cbs, void *context)
{
	struct control_block *cb;

	/* Remove the first element from the free list and return it. */
	dm_list_iterate_items(cb, &cbs->free) {
		dm_list_del(&cb->list);
		cb->context = context;
		dm_list_add(&cbs->allocated, &cb->list);
		return cb;
	}

	return NULL;
}

static void _cb_free(struct cb_set *cbs, struct control_block *cb)
{
	dm_list_del(&cb->list);
	dm_list_add_h(&cbs->free, &cb->list);
}

static struct control_block *_iocb_to_cb(struct iocb *icb)
{
	return dm_list_struct_base(icb, struct control_block, cb);
}

static struct control_block *_event_to_cb(struct io_event *ev)
{
	/* io_getevents() normally sets obj; guard malformed events. */
	if (!ev->obj)
		return NULL;

	return _iocb_to_cb((struct iocb *) ev->obj);
}

//----------------------------------------------------------------

struct async_engine {
	struct io_engine e;
	io_context_t aio_context;
	struct cb_set *cbs;
	unsigned max_io;
	unsigned page_mask;
	pid_t aio_context_pid; /* PID that created this AIO context */
	struct io_event event[MAX_EVENT];
};

static struct async_engine *_to_async(struct io_engine *e)
{
	return container_of(e, struct async_engine, e);
}

/*
 * Reap every in-flight request before io_destroy(). A failed flush can
 * reach destroy with I/O still outstanding: io_destroy() then fails and
 * the blkio frees buffers the kernel is still using.
 *
 * Returns true when the context is fully drained, so the buffers are
 * safe to free.  An interrupt stops the drain without blocking, so
 * teardown stays interruptible, and returns false; the caller must then
 * keep the buffers alive.  After fork() the child has no I/O of its own
 * in flight and must not reap the parent's context.
 */
static bool _async_drain(struct async_engine *e)
{
	int i, r;
	unsigned left;
	struct control_block *cb;
	struct io_event *ev;
	struct timespec no_wait = { 0, 0 };

	if (!e->aio_context || !e->cbs)
		return true;

	if (blkio_engine_is_forked(e->aio_context_pid))
		return true;

	while (!dm_list_empty(&e->cbs->allocated)) {
		left = dm_list_size(&e->cbs->allocated);

		/* Reap completions that are available without blocking. */
		r = io_getevents(e->aio_context, 0, MAX_EVENT, e->event, &no_wait);
		while (r == -EINTR && !sigint_caught())
			r = io_getevents(e->aio_context, 0, MAX_EVENT, e->event, &no_wait);

		if (r < 0) {
			if (r != -EINTR)
				log_blkio_sys_warn(_async, "io_getevents", -r);
			return false;
		}

		if (!r) {
			/*
			 * Nothing ready.  Block only while no interrupt is
			 * pending; otherwise give up so teardown can finish.
			 */
			if (sigint_caught())
				return false;

			do {
				r = io_getevents(e->aio_context, 1, MAX_EVENT,
						 e->event, NULL);
			} while (r == -EINTR && !sigint_caught());

			if (r < 0) {
				if (r != -EINTR)
					log_blkio_sys_warn(_async, "io_getevents", -r);
				return false;
			}
		}

		for (i = 0; i < r; i++) {
			ev = e->event + i;
			/*
			 * io_getevents() only reports iocbs submitted on
			 * this context, so obj is always set; skip a
			 * malformed event instead of dereferencing NULL.
			 */
			cb = _event_to_cb(ev);
			if (!cb)
				continue;

			_cb_free(e->cbs, cb);
		}

		/* No progress means the events were not ours. Stop. */
		if (dm_list_size(&e->cbs->allocated) >= left) {
			log_warn("WARNING: async io engine drain made no progress.");
			return false;
		}
	}

	return true;
}

static bool _async_destroy(struct io_engine *ioe)
{
	int r;
	struct async_engine *e = _to_async(ioe);
	bool drained = _async_drain(e);

	_cb_set_destroy(e->cbs);

	/*
	 * Only call io_destroy() if we're in the same process that created
	 * the AIO context. After fork(), the child inherits the parent's
	 * aio_context value but must not call io_destroy() on it.
	 */
	if (e->aio_context) {
		if (!blkio_engine_is_forked(e->aio_context_pid)) {
			log_debug_devs("Destroy AIO context.");
			r = io_destroy(e->aio_context); // really slow (~30ms)
			if (r < 0)
				log_blkio_sys_warn(_async, "io_destroy", -r);
		} else
			log_debug_devs("Skipping AIO context destroy for different pid.");
	}

	free(e);

	return drained;
}

static int _async_issue(struct io_engine *ioe, enum dir d, int fd,
			sector_t sb, sector_t se, void *data, void *context)
{
	int r;
	struct iocb *cb_array[1];
	struct control_block *cb;
	struct async_engine *e = _to_async(ioe);
	struct blkio_byte_range range;

	if (((uintptr_t) data) & e->page_mask) {
		log_warn("WARNING: async io engine misaligned data buffer.");
		return -EINVAL;
	}

	range = blkio_sector_range_bytes(sb, se);

	cb = _cb_alloc(e->cbs, context);
	if (!cb) {
		log_warn("WARNING: async io engine failed to allocate control block.");
		return -ENOMEM;
	}

	memset(&cb->cb, 0, sizeof(cb->cb));

	cb->cb.aio_fildes = (int) fd;
	cb->cb.u.c.buf = data;
	cb->cb.u.c.offset = range.offset;
	cb->cb.u.c.nbytes = range.len;
	cb->cb.aio_lio_opcode = (d == DIR_READ) ? IO_CMD_PREAD : IO_CMD_PWRITE;

	cb_array[0] = &cb->cb;
	do {
		r = io_submit(e->aio_context, 1, cb_array);
	} while (r == -EAGAIN);

	if (r < 0) {
		_cb_free(e->cbs, cb);
		return r;
	}

	return 0;
}

static bool _async_wait(struct io_engine *ioe, io_complete_fn fn)
{
	int i, r;
	enum dir d;
	int io_error;
	struct control_block *cb;
	struct io_event *ev;
	struct async_engine *e = _to_async(ioe);

	/* e->event[] zeroed at allocation; io_getevents fills 0..r-1 */

	/*
	 * Retry on EINTR from stray signals, but stop if an LVM interrupt
	 * signal (SIGINT/SIGTERM via sigint_allow()) has been caught.
	 */
	do {
		r = io_getevents(e->aio_context, 1, MAX_EVENT, e->event, NULL);
	} while (r == -EINTR && !sigint_caught());

	if (r < 0) {
		if (r == -EINTR)
			stack;
		else
			log_blkio_sys_warn(_async, "io_getevents", -r);
		return false;
	}

	for (i = 0; i < r; i++) {
		ev = e->event + i;
		cb = _event_to_cb(ev);
		if (!cb)
			continue;

		d = (cb->cb.aio_lio_opcode == IO_CMD_PREAD) ? DIR_READ : DIR_WRITE;

		io_error = blkio_completion_status(_async, d,
						   cb->cb.u.c.offset,
						   cb->cb.u.c.nbytes,
						   (int64_t) ev->res);
		fn(cb->context, io_error);

		_cb_free(e->cbs, cb);
	}

	return true;
}

static unsigned _async_max_io(struct io_engine *ioe)
{
	return _to_async(ioe)->max_io;
}

struct io_engine *create_async_io_engine(unsigned queue_depth)
{
	static int _pagesize = 0;
	int r;
	struct async_engine *e;

	if (!queue_depth)
		queue_depth = BLKIO_MAX_IO;

	if (_pagesize <= 0) {
		_pagesize = lvm_getpagesize();
		if (_pagesize <= 0) {
			log_warn("WARNING: async io engine invalid system page size.");
			return NULL;
		}
	}

	if (!(e = zalloc(sizeof(*e)))) {
		log_warn("WARNING: async io engine failed to allocate engine.");
		return NULL;
	}

	e->e.destroy = _async_destroy;
	e->e.issue = _async_issue;
	e->e.wait = _async_wait;
	e->e.max_io = _async_max_io;
	e->max_io = queue_depth;

	e->aio_context_pid = getpid();
	r = io_setup(queue_depth, &e->aio_context);
	if (r < 0) {
		log_blkio_sys_warn(_async, "io_setup", -r);
		free(e);
		return NULL;
	}

	e->cbs = _cb_set_create(queue_depth);
	if (!e->cbs) {
		log_warn("WARNING: async io engine failed to create control block set.");
		io_destroy(e->aio_context);
		free(e);
		return NULL;
	}

	e->page_mask = (unsigned) _pagesize - 1;

	log_debug("Created async io engine with queue depth %u.", queue_depth);

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->e;
}

#else /* !HAVE_LIBAIO_H */

struct io_engine *create_async_io_engine(unsigned queue_depth)
{
	log_debug("async io engine not available (built without libaio support).");
	return NULL;
}

#endif
