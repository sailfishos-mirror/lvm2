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

#include "lib/misc/lib.h"
#include "lib/device/blkio.h"
#include "lib/misc/lvm-signal.h"

#ifdef HAVE_LIBURING_H

#include <errno.h>
#include <liburing.h>

#define URING_SUBMIT_BATCH 64

struct io_context {
	struct io_context *next_free;	/* When free: link to next free context */
	enum dir d;			/* I/O direction */
	int fd;
	sector_t sb;
	sector_t se;
	void *data;
	void *context;
};

struct uring_engine {
	struct io_engine base;
	struct io_uring ring;
	unsigned max_io;
	unsigned pending_submit;	/* Ops queued but not yet submitted */
	unsigned in_use;		/* Contexts handed out, not completed */
	pid_t creator_pid;
	struct io_context *free_list;	/* Head of intrusive free list */
	struct io_context contexts[];	/* Pre-allocated contexts */
};

static const char _uring[] = "uring";

static struct uring_engine *_to_uring(struct io_engine *e)
{
	return container_of(e, struct uring_engine, base);
}

/*
 * io_uring_submit() returns how many SQEs the kernel accepted. That can
 * be less than the number prepared. Keep the remainder in pending_submit
 * so a later wait() submits them. A positive return means "some", not "all".
 */
static void _uring_note_submit(struct uring_engine *ue, int submitted)
{
	if (submitted <= 0)
		return;

	if ((unsigned)submitted >= ue->pending_submit)
		ue->pending_submit = 0;
	else
		ue->pending_submit -= (unsigned)submitted;
}

/*
 * Drain outstanding requests before io_uring_queue_exit(), which unmaps
 * the rings without waiting for the kernel to stop touching the buffers.
 * Returns true when everything in flight was reaped, so the blkio may free
 * the block data; false when an interrupt stopped the drain early and the
 * data must be kept.  After fork() the ring belongs to the parent and the
 * child has no I/O of its own in flight.
 */
static bool _uring_destroy(struct io_engine *ioe)
{
	struct uring_engine *ue = _to_uring(ioe);
	unsigned i;
	int r;
	bool drained = true;

	/*
	 * After fork() the io_uring ring belongs to the parent process.
	 * The child must not call io_uring_queue_exit() on it.
	 */
	if (blkio_engine_is_forked(ue->creator_pid)) {
		free(ue);
		return true;
	}

	/*
	 * Submit whatever is still queued so it can be drained below.  The
	 * retries are bounded: an SQE the kernel never accepted was never
	 * touched by the kernel, so dropping it here is safe.
	 */
	for (i = 0; ue->pending_submit && i < URING_SUBMIT_BATCH; i++) {
		r = io_uring_submit(&ue->ring);
		if (r > 0) {
			_uring_note_submit(ue, r);
			continue;
		}
		if (r == -EAGAIN)
			break;
		if (r == -EINTR)
			continue;
		if (r < 0)
			log_blkio_sys_warn(_uring, "submit", -r);
		break;
	}

	/* in_use - pending_submit == requests the kernel is still working on */
	while (ue->in_use > ue->pending_submit) {
		struct io_uring_cqe *cqe;

		/* Reap completions that are available without blocking. */
		r = io_uring_peek_cqe(&ue->ring, &cqe);
		if (r == -EAGAIN) {
			/*
			 * Nothing ready.  Block only while no interrupt is
			 * pending; otherwise give up so teardown can finish.
			 */
			if (sigint_caught()) {
				drained = false;
				break;
			}

			do {
				r = io_uring_wait_cqe(&ue->ring, &cqe);
			} while (r == -EINTR && !sigint_caught());

			if (r < 0) {
				if (r != -EINTR)
					log_blkio_sys_warn(_uring, "wait_cqe", -r);
				drained = false;
				break;
			}
		} else if (r < 0) {
			log_blkio_sys_warn(_uring, "peek", -r);
			drained = false;
			break;
		}

		io_uring_cqe_seen(&ue->ring, cqe);
		ue->in_use--;
	}

	io_uring_queue_exit(&ue->ring);
	free(ue);

	return drained;
}

static int _uring_issue(struct io_engine *ioe, enum dir d, int fd,
			sector_t sb, sector_t se, void *data, void *context)
{
	struct uring_engine *ue = _to_uring(ioe);
	struct io_uring_sqe *sqe;
	struct io_context *ioc;
	struct blkio_byte_range range = blkio_sector_range_bytes(sb, se);

	/* Check free list before io_uring_get_sqe() which advances sqe_tail */
	if (!ue->free_list) {
		log_warn("WARNING: uring io engine no free io contexts (queue full).");
		return -EAGAIN;
	}

	sqe = io_uring_get_sqe(&ue->ring);
	if (!sqe) {
		log_warn("WARNING: uring io engine no submission queue entry available.");
		return -EAGAIN;
	}

	ioc = ue->free_list;
	ue->free_list = ioc->next_free;
	ue->in_use++;

	ioc->d = d;
	ioc->fd = fd;
	ioc->sb = sb;
	ioc->se = se;
	ioc->data = data;
	ioc->context = context;

	if (d == DIR_READ)
		io_uring_prep_read(sqe, fd, data, range.len, range.offset);
	else
		io_uring_prep_write(sqe, fd, data, range.len, range.offset);

	io_uring_sqe_set_data(sqe, ioc);

	/*
	 * Submit every URING_SUBMIT_BATCH operations to give kernel a
	 * head start on I/O while we continue queuing.
	 * Balances async overlap vs syscall cost.
	 */
	if (++ue->pending_submit >= URING_SUBMIT_BATCH) {
		int r = io_uring_submit(&ue->ring);

		/*
		 * A short or failed submit leaves SQEs in the ring.
		 * Keep them counted; wait() submits the rest.
		 */
		if (r > 0)
			_uring_note_submit(ue, r);
	}

	return 0;
}

static bool _uring_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct uring_engine *ue = _to_uring(ioe);
	struct io_uring_cqe *cqe;
	struct io_context *ioc;
	struct blkio_byte_range range;
	uint64_t expected;
	int r;
	int io_error;
	bool ret = true;

	/*
	 * Submit any remaining queued operations.
	 * Some may have been submitted already in issue() for early start.
	 * Retry EAGAIN and stray EINTR. Stop on an LVM interrupt.
	 * Do not treat a short submit as "all done": leftovers must be
	 * submitted again or wait_cqe blocks on a completion that never
	 * arrives.
	 */
	while (ue->pending_submit) {
		do {
			r = io_uring_submit(&ue->ring);
		} while (r == -EAGAIN || (r == -EINTR && !sigint_caught()));

		if (r < 0) {
			if (r == -EINTR)
				stack;
			else
				log_blkio_sys_warn(_uring, "submit", -r);
			return false;
		}

		if (!r) {
			log_blkio_sys_warn(_uring, "submit", EAGAIN);
			return false;
		}

		_uring_note_submit(ue, r);
	}

	/* Wait for at least one completion */
	do {
		r = io_uring_wait_cqe(&ue->ring, &cqe);
	} while (r == -EINTR && !sigint_caught());

	if (r < 0) {
		if (r == -EINTR)
			stack;
		else
			log_blkio_sys_warn(_uring, "wait_cqe", -r);
		return false;
	}

	while (cqe) {
		ioc = io_uring_cqe_get_data(cqe);
		ue->in_use--;
		if (!ioc) {
			log_warn("WARNING: uring io engine null context in completion.");
			io_uring_cqe_seen(&ue->ring, cqe);
			ret = false;
			goto next;
		}

		range = blkio_sector_range_bytes(ioc->sb, ioc->se);
		expected = range.len;

		io_error = blkio_completion_status(_uring, ioc->d,
						       range.offset,
						       expected,
						       (int64_t) cqe->res);

		fn(ioc->context, io_error);

		/* Push context back onto intrusive free list */
		ioc->next_free = ue->free_list;
		ue->free_list = ioc;
		io_uring_cqe_seen(&ue->ring, cqe);

next:
		/* Check for more completions without blocking */
		r = io_uring_peek_cqe(&ue->ring, &cqe);
		if (r == -EAGAIN)
			break;
		if (r < 0) {
			log_blkio_sys_warn(_uring, "peek", -r);
			ret = false;
			break;
		}
	}

	return ret;
}

static unsigned _uring_max_io(struct io_engine *ioe)
{
	struct uring_engine *ue = _to_uring(ioe);
	return ue->max_io;
}

struct io_engine *create_uring_io_engine(unsigned queue_depth)
{
	struct uring_engine *ue;
	unsigned i;
	int r;

	if (!queue_depth)
		queue_depth = BLKIO_MAX_IO;

	/* Allocate engine with pre-allocated context pool */
	ue = zalloc(sizeof(*ue) + queue_depth * sizeof(struct io_context));
	if (!ue) {
		log_warn("WARNING: uring io engine failed to allocate engine.");
		return NULL;
	}

	ue->base.destroy = _uring_destroy;
	ue->base.issue = _uring_issue;
	ue->base.wait = _uring_wait;
	ue->base.max_io = _uring_max_io;
	ue->max_io = queue_depth;
	ue->creator_pid = getpid();

	/* Build intrusive free list - link all contexts */
	ue->free_list = &ue->contexts[0];
	for (i = 0; i < queue_depth - 1; i++)
		ue->contexts[i].next_free = &ue->contexts[i + 1];
	ue->contexts[queue_depth - 1].next_free = NULL;

	r = io_uring_queue_init(queue_depth, &ue->ring, 0);
	if (r < 0) {
		log_debug("uring io engine queue_init failed: %s.", strerror(-r));
		free(ue);
		return NULL;
	}

	log_debug("Created uring io engine with queue depth %u.", queue_depth);

	return &ue->base;
}

#else /* !HAVE_LIBURING_H */

struct io_engine *create_uring_io_engine(unsigned queue_depth)
{
	log_debug("uring io engine not available (built without liburing support).");
	return NULL;
}

#endif
