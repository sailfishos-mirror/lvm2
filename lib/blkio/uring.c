/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
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
#include <liburing.h>

#ifdef HAVE_VALGRIND
#include <valgrind/memcheck.h>
#endif

#define URING_SUBMIT_BATCH 64
#define URING_DRAIN_SUBMIT_RETRIES 64

/*
 * SQE - Submission Queue Entry: an I/O request placed on the submission queue.
 * CQE - Completion Queue Entry: a result placed on the completion queue.
 *
 * The engine pre-allocates its requests.  issue() takes one from free_list
 * and queues an SQE; wait() submits queued SQEs and reaps CQEs, returning
 * requests to free_list.  queued counts SQEs queued but not yet accepted by
 * the kernel; in_flight counts requests issued but not yet reaped.
 */
struct uring_request {
	struct io_pool_link link;	/* When free: pool link, must be first */
	struct blkio_io io;		/* The transfer it carries */
	struct iovec iov;		/* Buffer for the vectored opcode */
	void *context;			/* Caller context for the completion */
};

struct uring_engine {
	struct io_engine base;
	struct io_uring ring;
	unsigned in_flight;		/* Requests issued, not yet reaped */
	unsigned max_io;
	unsigned queued;		/* SQEs queued, not yet accepted */
	struct io_engine_pool pool;	/* Creator pid and free list */
	struct uring_request requests[];/* Pre-allocated requests */
};

static struct uring_engine *_to_uring(struct io_engine *ioe)
{
	return container_of(ioe, struct uring_engine, base);
}

static struct uring_request *_request_alloc(struct uring_engine *e, void *context)
{
	struct io_pool_link *link = blkio_pool_pop(&e->pool);
	struct uring_request *req;

	if (!link)
		return NULL;

	/* The link is the first member, so the two pointers are the same. */
	req = (struct uring_request *) link;
	e->in_flight++;
	req->context = context;

	return req;
}

static void _request_free(struct uring_engine *e, struct uring_request *req)
{
	blkio_pool_push(&e->pool, &req->link);
	e->in_flight--;
}

/*
 * io_uring_submit() returns how many SQEs the kernel accepted. That can
 * be less than the number prepared. Keep the remainder in queued so a
 * later wait() submits them. A positive return means "some", not "all".
 * Returns the io_uring_submit() result unchanged.
 */
static int _uring_submit(struct uring_engine *e)
{
	int r = io_uring_submit(&e->ring);

	if (r > 0)
		e->queued = ((unsigned) r >= e->queued) ?
			0 : e->queued - (unsigned) r;

	return r;
}

/*
 * What one round of submission achieved.  Draining and waiting differ only in
 * how they react to it, so the submit policy itself lives in one place.
 */
enum submit_result {
	SUBMIT_DONE,		/* Nothing is left queued */
	SUBMIT_FULL,		/* Stopped submitting; a completion makes room */
	SUBMIT_DEADLOCK,	/* Ring full and no request can complete */
	SUBMIT_INTERRUPTED,	/* An LVM interrupt is pending */
	SUBMIT_FAILED,		/* The submit failed and was reported */
};

/*
 * Push as much of the queued SQEs as the kernel accepts, giving up after
 * max_submits attempts (0 for no limit).  The bound exists for the drain
 * path, which must not spin at teardown time: an SQE the kernel never
 * accepted was never touched by the kernel, so dropping it there is safe.
 */
static enum submit_result _uring_submit_queued(struct uring_engine *e,
					       unsigned max_submits)
{
	unsigned i = 0;
	int r;

	while (e->queued) {
		if (sigint_caught())
			return SUBMIT_INTERRUPTED;

		if (max_submits && (i++ >= max_submits))
			return SUBMIT_FULL;

		r = _uring_submit(e);
		if ((r > 0) || (r == -EINTR))
			continue;

		if ((r < 0) && (r != -EAGAIN)) {
			blkio_log_sys_warn(&e->base, "submit", r);
			return SUBMIT_FAILED;
		}

		/*
		 * The ring is full (EAGAIN) or the kernel accepted
		 * nothing.  If nothing is in flight, or every in-flight
		 * request is still queued, waiting for a completion would
		 * block forever.
		 */
		if (!e->in_flight || (e->in_flight == e->queued))
			return SUBMIT_DEADLOCK;

		return SUBMIT_FULL;
	}

	return SUBMIT_DONE;
}

/*
 * Block for one completion, retrying EINTR until an LVM interrupt is caught.
 * Returns false (after reporting any real error) when no CQE was obtained.
 */
static bool _uring_wait_cqe(struct uring_engine *e, struct io_uring_cqe **cqe)
{
	int r;

	do {
		r = io_uring_wait_cqe(&e->ring, cqe);
	} while ((r == -EINTR) && !sigint_caught());

	if (r == -EINTR)
		return_false;

	if (r < 0) {
		blkio_log_sys_warn(&e->base, "wait_cqe", r);
		return false;
	}

	return true;
}

/*
 * Reap cqe and every further CQE available without blocking: report each
 * result to fn (NULL fn discards results) and return its request to free_list.
 * Returns false (after reporting the error) when a CQE carries no request
 * or peeking for more CQEs fails.
 */
static bool _uring_reap(struct uring_engine *e, struct io_uring_cqe *cqe,
			io_complete_fn fn)
{
	struct uring_request *req;
	int r;

	do {
		req = io_uring_cqe_get_data(cqe);

		if (!req) {
			e->in_flight--;
			io_uring_cqe_seen(&e->ring, cqe);
			log_warn("WARNING: iouring io engine null request in completion.");
			return false;
		}

		if (fn) {
#ifdef HAVE_VALGRIND
			/*
			 * Tell valgrind the buffer is now initialized after io_uring read.
			 * Valgrind doesn't track io_uring's async buffer writes, so without
			 * this annotation it reports uninitialized data even though the
			 * kernel wrote it.
			 */
			if ((cqe->res >= 0) && (req->io.d == BLKIO_DIR_READ))
				VALGRIND_MAKE_MEM_DEFINED(req->iov.iov_base, cqe->res);
#endif
			fn(req->context,
			   blkio_completion_status(&e->base, &req->io,
						   (int64_t) cqe->res));
		}

		_request_free(e, req);

		io_uring_cqe_seen(&e->ring, cqe);

		r = io_uring_peek_cqe(&e->ring, &cqe);
	} while (r == 0);

	if (r != -EAGAIN) {
		blkio_log_sys_warn(&e->base, "peek", r);
		return false;
	}

	return true;
}

/*
 * Submit what is still queued and reap every request in flight.
 * Returns true when everything was reaped; false when an error or
 * interrupt stopped the drain early.
 */
static bool _uring_drain(struct uring_engine *e)
{
	struct io_uring_cqe *cqe;
	int r;

	/*
	 * Submit what is still queued.  Whatever the outcome, drop the rest
	 * and reap what the kernel knows about.
	 */
	(void) _uring_submit_queued(e, URING_DRAIN_SUBMIT_RETRIES);

	/* in_flight - queued == requests the kernel is still working on */
	while (e->in_flight > e->queued) {
		/* Reap completions that are available without blocking. */
		r = io_uring_peek_cqe(&e->ring, &cqe);
		if (r == -EAGAIN) {
			/*
			 * Nothing ready.  Block only while no interrupt is
			 * pending; otherwise give up so teardown can finish.
			 */
			if (sigint_caught())
				return_false;

			if (!_uring_wait_cqe(e, &cqe))
				return_false;
		} else if (r < 0) {
			blkio_log_sys_warn(&e->base, "peek", r);
			return false;
		}

		if (!_uring_reap(e, cqe, NULL))
			return_false;
	}

	return true;
}

/*
 * Drain outstanding requests before io_uring_queue_exit(), which unmaps
 * the rings without waiting for the kernel to stop touching the buffers.
 * Returns true when everything in flight was reaped, so the blkio may free
 * the block data; false when an error or interrupt stopped the drain early
 * and the data must be kept.  After fork() the ring belongs to the parent
 * and the child has no I/O of its own in flight, so it must not touch it
 * (including io_uring_queue_exit()).
 */
static bool _uring_destroy(struct io_engine *ioe)
{
	struct uring_engine *e = _to_uring(ioe);
	bool drained = true;

	if (!blkio_engine_is_forked(e->pool.creator_pid)) {
		drained = _uring_drain(e);
		io_uring_queue_exit(&e->ring);
	}

	/* struct io_engine is the first member; free the caller's pointer. */
	free(ioe);

	return drained;
}

static int _uring_issue(struct io_engine *ioe, const struct blkio_io *io,
			void *context)
{
	struct uring_engine *e = _to_uring(ioe);
	struct io_uring_sqe *sqe;
	struct uring_request *req;

	/* Check free list before io_uring_get_sqe() which advances sqe_tail */
	if (!(req = _request_alloc(e, context))) {
		log_warn("WARNING: iouring io engine no free requests (queue full).");
		return -EAGAIN;
	}

	if (!(sqe = io_uring_get_sqe(&e->ring))) {
		_request_free(e, req);
		log_warn("WARNING: iouring io engine no submission queue entry available.");
		return -EAGAIN;
	}

	req->io = *io;
	req->iov.iov_base = io->data;
	req->iov.iov_len = io->range.len;

	/*
	 * Use the vectored opcodes: IORING_OP_READV/WRITEV have been
	 * supported since the first io_uring release, while the plain
	 * IORING_OP_READ/WRITE opcodes were added later.  Kernels without
	 * them would accept the queue but fail every request.
	 */
	if (io->d == BLKIO_DIR_READ)
		io_uring_prep_readv(sqe, io->fd, &req->iov, 1, io->range.offset);
	else
		io_uring_prep_writev(sqe, io->fd, &req->iov, 1, io->range.offset);

	io_uring_sqe_set_data(sqe, req);

	/*
	 * Submit in batches to give the kernel a head start on I/O while
	 * we continue queuing, balancing async overlap against syscall
	 * cost.  A short or failed submit leaves SQEs in the ring, still
	 * counted in queued; wait() submits the rest.
	 */
	if (++e->queued >= URING_SUBMIT_BATCH)
		(void) _uring_submit(e);

	return 0;
}

static bool _uring_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct uring_engine *e = _to_uring(ioe);
	struct io_uring_cqe *cqe;

	for (;;) {
		/*
		 * Submit all queued SQEs.  A short submit leaves the rest
		 * in queued; wait for a completion below to make room.
		 */
		switch (_uring_submit_queued(e, 0)) {
		case SUBMIT_INTERRUPTED:
			return_false;

		case SUBMIT_FAILED:
			return false;

		case SUBMIT_DEADLOCK:
			log_warn("WARNING: %s io engine submit deadlock: "
				 "the ring is full and no request can complete.",
				 ioe->name);
			return false;

		case SUBMIT_DONE:
		case SUBMIT_FULL:
			break;
		}

		/*
		 * Wait for at least one completion.  Do not enter a blocking
		 * wait after an LVM interrupt was already caught.
		 */
		if (sigint_caught())
			return_false;

		if (!_uring_wait_cqe(e, &cqe))
			return_false;

		if (!_uring_reap(e, cqe, fn))
			return_false;

		if (!e->queued)
			return true;
	}
}

static unsigned _uring_max_io(struct io_engine *ioe)
{
	struct uring_engine *e = _to_uring(ioe);
	return e->max_io;
}

struct io_engine *create_uring_io_engine(unsigned queue_depth)
{
	static const struct io_engine proto = {
		.name = "iouring",
		.destroy = _uring_destroy,
		.issue = _uring_issue,
		.wait = _uring_wait,
		.max_io = _uring_max_io,
	};
	static const struct io_engine_layout layout = {
		.engine_size = sizeof(struct uring_engine),
		.requests_offset = offsetof(struct uring_engine, requests),
		.pool_offset = offsetof(struct uring_engine, pool),
		.request_size = sizeof(struct uring_request),
	};
	struct uring_engine *e;
	int r;

	queue_depth = blkio_queue_depth(queue_depth);

	if (!(e = blkio_alloc_request_pool(&proto, &layout, queue_depth)))
		return_NULL;

	e->max_io = queue_depth;

	r = io_uring_queue_init(queue_depth, &e->ring, 0);
	if (r < 0) {
		free(e);

		/*
		 * A kernel without io_uring, one that refuses the ring
		 * because of a restriction, or one that is out of memory is
		 * not an error: let the caller try another engine.
		 */
		log_debug("iouring io engine not available: %s.",
			  strerror(-r));
		errno = ENODEV;
		return NULL;
	}

	log_debug("Created iouring io engine with queue depth %u.",
		  queue_depth);

	return &e->base;
}
