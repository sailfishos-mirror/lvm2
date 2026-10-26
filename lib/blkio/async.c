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

#include "lib/blkio/internal.h"
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <libaio.h>

#define MAX_EVENT 64

/*
 * The engine pre-allocates its requests.  issue() takes one from free_list
 * and submits its iocb; wait() reaps completion events and returns the
 * requests to free_list.  in_flight counts requests handed out but not yet
 * completed.
 */
struct async_request {
	struct async_request *next_free;	/* When free: link to next */
	void *context;				/* Caller context for the completion */
	struct blkio_io io;			/* The transfer it carries */
	struct iocb iocb;
};

struct async_engine {
	struct io_engine base;
	io_context_t aio_context;
	unsigned max_io;
	unsigned page_mask;
	unsigned in_flight;		/* Requests handed out, not completed */
	pid_t creator_pid;		/* PID that created the AIO context */
	struct async_request *free_list;	/* Head of intrusive free list */
	struct io_event event[MAX_EVENT];
	struct async_request requests[];	/* Pre-allocated requests */
};

static struct async_engine *_to_async(struct io_engine *ioe)
{
	return container_of(ioe, struct async_engine, base);
}

static struct async_request *_request_alloc(struct async_engine *e, void *context)
{
	struct async_request *req = e->free_list;

	if (!req)
		return NULL;

	e->free_list = req->next_free;
	e->in_flight++;
	req->context = context;

	return req;
}

static void _request_free(struct async_engine *e, struct async_request *req)
{
	req->next_free = e->free_list;
	e->free_list = req;
	e->in_flight--;
}

/*
 * Fetch completion events into e->event[], retrying EINTR until an LVM
 * interrupt is caught.  A NULL timeout blocks until min_nr events arrive.
 * Returns false (after reporting any real error) when no events could be
 * fetched; *nr is the number of events otherwise, possibly 0 if min_nr is 0.
 */
static bool _async_getevents(struct async_engine *e, long min_nr,
			     struct timespec *timeout, int *nr)
{
	int r;

	do {
		r = io_getevents(e->aio_context, min_nr, MAX_EVENT, e->event,
				 timeout);
	} while ((r == -EINTR) && !sigint_caught());

	if (r == -EINTR)
		return_false;

	if (r < 0) {
		blkio_log_sys_warn(&e->base, "io_getevents", r);
		return false;
	}

	*nr = r;

	return true;
}

/*
 * Return the requests of the first nr events to the free list,
 * reporting each result to fn.  A NULL fn discards the results (drain).
 * Returns false if any event was malformed and had to be skipped.
 */
static bool _async_reap(struct async_engine *e, int nr, io_complete_fn fn)
{
	struct async_request *req;
	struct io_event *ev;
	bool ret = true;
	int i;

	for (i = 0; i < nr; i++) {
		ev = e->event + i;

		/* io_getevents() normally sets obj; warn and skip malformed events. */
		if (!ev->obj) {
			log_warn("WARNING: libaio io engine malformed completion event.");
			ret = false;
			continue;
		}

		req = container_of(ev->obj, struct async_request, iocb);

		if (fn) {
			if ((int) ev->res < 0) {
				log_warn("WARNING: blkio async %s device offset %llu len %llu: %s.",
					 blkio_dir_name(req->io.d),
					 (unsigned long long)req->io.range.offset,
					 (unsigned long long)req->io.range.len,
					 strerror(-(int)ev->res));
				fn(req->context, (int) ev->res);
			} else if (ev->res >= req->io.range.len) {
				fn(req->context, 0);
			} else if (ev->res >= (1 << SECTOR_SHIFT)) {
				/* minimum acceptable read is 1 sector */
				log_debug_devs("blkio async %s offset %llu requested %llu got %llu.",
					       blkio_dir_name(req->io.d),
					       (unsigned long long)req->io.range.offset,
					       (unsigned long long)req->io.range.len,
					       (unsigned long long)ev->res);
				fn(req->context, 0);
			} else {
				log_warn("WARNING: blkio async %s offset %llu requested %llu got %llu.",
					 blkio_dir_name(req->io.d),
					 (unsigned long long)req->io.range.offset,
					 (unsigned long long)req->io.range.len,
					 (unsigned long long)ev->res);
				fn(req->context, -ENODATA);
			}
		}

		_request_free(e, req);
	}

	return ret;
}

/*
 * Reap every in-flight request before io_destroy().  A failed flush can
 * reach destroy with I/O still outstanding: io_destroy() then fails and
 * the blkio frees buffers the kernel is still using.
 * Returns true when everything was reaped; false when an error or
 * interrupt stopped the drain early.
 */
static bool _async_drain(struct async_engine *e)
{
	struct timespec no_wait = { 0, 0 };
	unsigned left;
	int nr;

	while (e->in_flight) {
		left = e->in_flight;

		/* Reap completions that are available without blocking. */
		if (!_async_getevents(e, 0, &no_wait, &nr))
			return_false;

		if (!nr) {
			/*
			 * Nothing ready.  Block only while no interrupt is
			 * pending; otherwise give up so teardown can finish.
			 */
			if (sigint_caught())
				return_false;

			if (!_async_getevents(e, 1, NULL, &nr))
				return_false;
		}

		_async_reap(e, nr, NULL);

		/* No progress means the events were not ours. Stop. */
		if (e->in_flight >= left) {
			log_warn("WARNING: libaio io engine drain made no progress.");
			return false;
		}
	}

	return true;
}

/*
 * Drain outstanding requests before io_destroy().
 * Returns true when everything in flight was reaped, so the blkio may free
 * the block data; false when an error or interrupt stopped the drain early
 * and the data must be kept.  io_destroy() still waits for outstanding
 * I/O, so destroy itself is not interruptible.  After fork() the AIO
 * context belongs to the parent and the child has no I/O of its own in
 * flight, so it must not touch it (including io_destroy()).
 */
static bool _async_destroy(struct io_engine *ioe)
{
	struct async_engine *e = _to_async(ioe);
	bool drained = true;
	int r;

	if (!blkio_engine_is_forked(e->creator_pid)) {
		drained = _async_drain(e);

		log_debug_devs("Destroy AIO context.");
		r = io_destroy(e->aio_context);	/* really slow (~30ms) */
		if (r < 0)
			blkio_log_sys_warn(ioe, "io_destroy", r);
	} else
		log_debug_devs("Skipping AIO context destroy for different pid.");

	if (e->in_flight)
		log_warn("WARNING: libaio io engine %u requests still in flight at destroy.",
			 e->in_flight);

	/* struct io_engine is the first member; free the caller's pointer. */
	free(ioe);

	return drained;
}

static int _async_issue(struct io_engine *ioe, const struct blkio_io *io,
			void *context)
{
	struct async_engine *e = _to_async(ioe);
	struct async_request *req;
	struct iocb *icb;
	int r;

	if (((uintptr_t) io->data) & e->page_mask) {
		log_warn("WARNING: libaio io engine misaligned data buffer.");
		return -EINVAL;
	}

	req = _request_alloc(e, context);
	if (!req) {
		log_warn("WARNING: libaio io engine no free requests (queue full).");
		return -EAGAIN;
	}
	req->io = *io;

	if (io->d == BLKIO_DIR_READ)
		io_prep_pread(&req->iocb, io->fd, io->data, io->range.len,
			      (long long) io->range.offset);
	else
		io_prep_pwrite(&req->iocb, io->fd, io->data, io->range.len,
			       (long long) io->range.offset);

	icb = &req->iocb;
	r = io_submit(e->aio_context, 1, &icb);
	if (r < 0) {
		_request_free(e, req);
		return r;
	}

	return 0;
}

static bool _async_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct async_engine *e = _to_async(ioe);
	int nr;

	/*
	 * Do not enter a blocking wait after an LVM interrupt was already
	 * caught: a handled signal is not delivered again to interrupt it.
	 */
	if (sigint_caught())
		return_false;

	if (!_async_getevents(e, 1, NULL, &nr))
		return_false;

	return _async_reap(e, nr, fn);
}

static unsigned _async_max_io(struct io_engine *ioe)
{
	return _to_async(ioe)->max_io;
}

struct io_engine *create_async_io_engine(unsigned queue_depth)
{
	struct async_engine *e;
	unsigned i;
	int pagesize;
	int r;

	queue_depth = blkio_queue_depth(queue_depth);

	if ((pagesize = lvm_getpagesize()) <= 0) {
		log_error("Invalid system page size.");
		return NULL;
	}

	/* Allocate engine with pre-allocated request pool */
	if (!(e = zalloc(sizeof(*e) + queue_depth * sizeof(*e->requests)))) {
		log_warn("WARNING: libaio io engine failed to allocate engine.");
		return NULL;
	}

	e->base.name = "libaio";
	e->base.destroy = _async_destroy;
	e->base.issue = _async_issue;
	e->base.wait = _async_wait;
	e->base.max_io = _async_max_io;
	e->max_io = queue_depth;
	e->page_mask = (unsigned) pagesize - 1;
	e->creator_pid = getpid();

	/* Build intrusive free list; the last next_free stays NULL from zalloc */
	e->free_list = &e->requests[0];
	for (i = 0; i < queue_depth - 1; i++)
		e->requests[i].next_free = &e->requests[i + 1];

	r = io_setup(queue_depth, &e->aio_context);
	if (r < 0) {
		free(e);

		/*
		 * Running out of aio contexts or kernel memory is a local
		 * shortage, not a broken engine: let the caller try another
		 * one.
		 */
		log_debug("libaio io engine not available: %s.",
			  strerror(-r));
		errno = ENODEV;
		return NULL;
	}

	log_debug("Created libaio io engine with queue depth %u.",
		  queue_depth);

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->base;
}
