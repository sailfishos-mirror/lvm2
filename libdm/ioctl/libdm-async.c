/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Async DM ioctl submission backends:
 *   - thread pool (always compiled)
 *   - io_uring   (when HAVE_IORING_OP_IOCTL is defined, requires Linux 6.5+)
 */

#include "libdm-async.h"
#include "libdm-targets.h"
#include "libdm/misc/dmlib.h"

#include <pthread.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Thread-pool backend                                                  */
/* ------------------------------------------------------------------ */

struct async_threads {
	struct dm_async_ctx  base;         /* vtable + fd; must be first */
	pthread_mutex_t      lock;
	pthread_cond_t       work_cond;    /* workers wait for items */
	pthread_cond_t       done_cond;    /* main waits for completions / slots */
	struct dm_list       pending;      /* sorted by seq */
	struct dm_list       completed;
	unsigned             n_threads;
	unsigned             n_inflight;   /* pending + executing */
	unsigned             current_seq;  /* lowest seq eligible to run */
	unsigned             seq_running;  /* items at current_seq executing */
	unsigned             seq_pending;  /* items at current_seq in pending */
	int                  shutdown;
	pthread_t            threads[];    /* flex array; must be last */
};

/*
 * Advance current_seq to the next phase from the pending list.
 * Must be called under ctx->lock when seq_running == 0 && seq_pending == 0.
 */
static void _advance_seq(struct async_threads *ctx)
{
	struct dm_work_item *item;
	struct dm_list *lh;

	if (dm_list_empty(&ctx->pending))
		return;

	item = dm_list_item(dm_list_first(&ctx->pending),
			    struct dm_work_item);
	ctx->current_seq = item->seq;

	/* Count items at the new current_seq. */
	ctx->seq_pending = 0;
	dm_list_iterate(lh, &ctx->pending) {
		item = dm_list_item(lh, struct dm_work_item);
		if (item->seq != ctx->current_seq)
			break;
		ctx->seq_pending++;
	}

	pthread_cond_broadcast(&ctx->work_cond);
}

static void *_worker_fn(void *arg)
{
	struct async_threads *ctx = arg;
	struct dm_work_item *item;

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->shutdown) {
		if (dm_list_empty(&ctx->pending) ||
		    dm_list_item(dm_list_first(&ctx->pending),
				 struct dm_work_item)->seq > ctx->current_seq) {
			pthread_cond_wait(&ctx->work_cond, &ctx->lock);
			continue;
		}

		item = dm_list_item(dm_list_first(&ctx->pending),
				    struct dm_work_item);
		dm_list_del(&item->list);
		if (item->seq == ctx->current_seq) {
			ctx->seq_pending--;
			ctx->seq_running++;
		}
		pthread_mutex_unlock(&ctx->lock);

		/* Execute the ioctl outside the lock (with EBUSY retry). */
		dm_ioctl_exec_retry(ctx->base.fd, item->dmt);

		pthread_mutex_lock(&ctx->lock);
		dm_list_add(&ctx->completed, &item->list);
		if (item->seq == ctx->current_seq)
			ctx->seq_running--;

		if (!ctx->seq_running && !ctx->seq_pending)
			_advance_seq(ctx);

		pthread_cond_signal(&ctx->done_cond);
	}
	pthread_mutex_unlock(&ctx->lock);
	return NULL;
}

static int _threads_submit(struct dm_async_ctx *base,
			   struct dm_task *dmt, void *userdata)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_work_item *item;
	struct dm_list *lh;
	unsigned seq = dmt->async.seq;

	if (!(item = malloc(sizeof(*item)))) {
		log_error("Failed to allocate async work item.");
		return 0;
	}
	dm_list_init(&item->list);
	item->dmt      = dmt;
	item->userdata = userdata;
	item->seq      = seq;

	pthread_mutex_lock(&ctx->lock);

	if (ctx->shutdown) {
		pthread_mutex_unlock(&ctx->lock);
		free(item);
		return 0;
	}

	/* Insert sorted by seq (stable: append among equal seq). */
	lh = ctx->pending.p;
	while (lh != &ctx->pending &&
	       dm_list_item(lh, struct dm_work_item)->seq > seq)
		lh = lh->p;
	dm_list_add(lh, &item->list);

	ctx->n_inflight++;

	/* First item or passed phase: set current_seq. */
	if (ctx->n_inflight == 1) {
		ctx->current_seq = seq;
		ctx->seq_pending = 1;
		ctx->seq_running = 0;
	} else if (seq == ctx->current_seq)
		ctx->seq_pending++;

	pthread_cond_signal(&ctx->work_cond);
	pthread_mutex_unlock(&ctx->lock);
	return 1;
}

static int _threads_wait(struct dm_async_ctx *base,
			 struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_work_item *item;

	pthread_mutex_lock(&ctx->lock);
	while (dm_list_empty(&ctx->completed) && ctx->n_inflight > 0)
		pthread_cond_wait(&ctx->done_cond, &ctx->lock);

	if (dm_list_empty(&ctx->completed)) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	item = dm_list_item(dm_list_first(&ctx->completed),
			    struct dm_work_item);
	dm_list_del(&item->list);
	ctx->n_inflight--;
	pthread_cond_signal(&ctx->done_cond);   /* wake blocked submitters */
	pthread_mutex_unlock(&ctx->lock);

	*dmt_out      = item->dmt;
	*userdata_out = item->userdata;
	free(item);
	return 1;
}

static int _threads_try_wait(struct dm_async_ctx *base,
			     struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_work_item *item;

	pthread_mutex_lock(&ctx->lock);

	if (dm_list_empty(&ctx->completed)) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	item = dm_list_item(dm_list_first(&ctx->completed),
			    struct dm_work_item);
	dm_list_del(&item->list);
	ctx->n_inflight--;
	pthread_cond_signal(&ctx->done_cond);
	pthread_mutex_unlock(&ctx->lock);

	*dmt_out      = item->dmt;
	*userdata_out = item->userdata;
	free(item);
	return 1;
}

static void _threads_destroy(struct dm_async_ctx *base)
{
	struct async_threads *ctx = (struct async_threads *)base;
	unsigned i;

	pthread_mutex_lock(&ctx->lock);
	ctx->shutdown = 1;
	pthread_cond_broadcast(&ctx->work_cond);
	pthread_cond_broadcast(&ctx->done_cond);
	pthread_mutex_unlock(&ctx->lock);

	for (i = 0; i < ctx->n_threads; i++)
		pthread_join(ctx->threads[i], NULL);

	pthread_mutex_destroy(&ctx->lock);
	pthread_cond_destroy(&ctx->work_cond);
	pthread_cond_destroy(&ctx->done_cond);
	free(ctx);
}

/*
 * Drain all deferred async completions from a context.
 * Called after the batch deactivation loop, before dm_udev_wait().
 * Does NOT destroy the ctx -- caller owns that.
 */
int dm_async_drain(struct dm_async_ctx *ctx)
{
	struct dm_task *dmt;
	void *userdata;
	int r = 1;
	unsigned count = 0;

	if (!ctx)
		return 1;

	while (dm_async_wait_completion(ctx, &dmt, &userdata)) {
		if (!dm_task_handle_completion(dmt, userdata))
			r = 0;
		dm_task_destroy(dmt);
		count++;
	}

	if (count)
		log_debug_activation("Drained %u async completion(s).",
				     count);

	return r;
}

struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_parallel)
{
	struct async_threads *ctx;
	unsigned i;

	ctx = calloc(1, sizeof(*ctx) + max_parallel * sizeof(pthread_t));
	if (!ctx) {
		log_error("Failed to allocate async thread pool context.");
		return NULL;
	}

	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _threads_submit;
	ctx->base.fn_wait      = _threads_wait;
	ctx->base.fn_try_wait  = _threads_try_wait;
	ctx->base.fn_destroy   = _threads_destroy;
	ctx->n_threads       = max_parallel;
	dm_list_init(&ctx->pending);
	dm_list_init(&ctx->completed);
	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->work_cond, NULL);
	pthread_cond_init(&ctx->done_cond, NULL);

	for (i = 0; i < max_parallel; i++) {
		if (pthread_create(&ctx->threads[i], NULL, _worker_fn, ctx)) {
			log_error("Failed to create worker thread %u.", i);
			ctx->n_threads = i;   /* only join threads we started */
			_threads_destroy(&ctx->base);
			return NULL;
		}
	}

	return &ctx->base;
}

/* ------------------------------------------------------------------ */
/* io_uring backend (Linux 6.5+, requires IORING_OP_IOCTL)            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_IORING_OP_IOCTL

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>

#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup    425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter    426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif

static int _uring_setup(unsigned entries, struct io_uring_params *p)
{
	return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int _uring_enter(int fd, unsigned to_submit,
			unsigned min_complete, unsigned flags)
{
	return (int)syscall(__NR_io_uring_enter, fd,
			    to_submit, min_complete, flags, NULL, 0);
}

static int _uring_register(int fd, unsigned opcode,
			   void *arg, unsigned nr_args)
{
	return (int)syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

struct async_uring {
	struct dm_async_ctx  base;         /* vtable + fd; must be first */
	int                  ring_fd;
	unsigned             max_parallel;
	unsigned             n_inflight;

	/* SQ ring */
	unsigned            *sq_head;
	unsigned            *sq_tail;
	unsigned            *sq_mask;
	unsigned            *sq_array;
	void                *sq_ring_ptr;
	size_t               sq_ring_sz;

	/* SQEs */
	struct io_uring_sqe *sqes;
	void                *sqe_ptr;
	size_t               sqe_sz;

	/* CQ ring */
	unsigned            *cq_head;
	unsigned            *cq_tail;
	unsigned            *cq_mask;
	struct io_uring_cqe *cqes;
	void                *cq_ring_ptr;  /* NULL if shared with sq_ring_ptr */
	size_t               cq_ring_sz;
};

static int _uring_submit(struct dm_async_ctx *base,
			 struct dm_task *dmt, void *userdata)
{
	struct async_uring *ctx = (struct async_uring *)base;
	struct dm_work_item *item;
	struct io_uring_sqe *sqe;
	unsigned tail, idx;

	if (ctx->n_inflight >= ctx->max_parallel) {
		log_error("io_uring async context full (%u in flight).",
			  ctx->n_inflight);
		return 0;
	}

	if (!(item = malloc(sizeof(*item)))) {
		log_error("Failed to allocate async work item.");
		return 0;
	}
	item->dmt      = dmt;
	item->userdata = userdata;

	tail = *ctx->sq_tail;
	idx  = tail & *ctx->sq_mask;
	sqe  = &ctx->sqes[idx];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode    = IORING_OP_IOCTL;
	sqe->fd        = ctx->base.fd;
	sqe->off       = (uint64_t)dm_task_get_ioctl_cmd(dmt);
	sqe->addr      = (uint64_t)(uintptr_t)dmt->dmi.v4;
	sqe->user_data = (uint64_t)(uintptr_t)item;

	ctx->sq_array[idx] = idx;
	__atomic_store_n(ctx->sq_tail, tail + 1, __ATOMIC_RELEASE);

	if (_uring_enter(ctx->ring_fd, 1, 0, 0) < 0) {
		log_error("io_uring_enter submit failed: %s", strerror(errno));
		__atomic_store_n(ctx->sq_tail, tail, __ATOMIC_RELEASE);
		free(item);
		return 0;
	}

	ctx->n_inflight++;
	return 1;
}

static int _uring_wait(struct dm_async_ctx *base,
		       struct dm_task **dmt_out, void **userdata_out)
{
	struct async_uring *ctx = (struct async_uring *)base;
	struct dm_work_item *item;
	struct io_uring_cqe *cqe;
	unsigned head;

	if (!ctx->n_inflight)
		return 0;

	if (_uring_enter(ctx->ring_fd, 0, 1, IORING_ENTER_GETEVENTS) < 0) {
		log_error("io_uring_enter wait failed: %s", strerror(errno));
		return 0;
	}

	head = __atomic_load_n(ctx->cq_head, __ATOMIC_ACQUIRE);
	if (head == *ctx->cq_tail)
		return 0;   /* spurious wake */

	cqe  = &ctx->cqes[head & *ctx->cq_mask];
	item = (struct dm_work_item *)(uintptr_t)cqe->user_data;

	item->dmt->ioctl_result = (cqe->res >= 0) ? 0 : -1;
	if (cqe->res < 0)
		item->dmt->ioctl_errno = -cqe->res;

	__atomic_store_n(ctx->cq_head, head + 1, __ATOMIC_RELEASE);
	ctx->n_inflight--;

	*dmt_out      = item->dmt;
	*userdata_out = item->userdata;
	free(item);
	return 1;
}

static int _uring_try_wait(struct dm_async_ctx *base,
			   struct dm_task **dmt_out, void **userdata_out)
{
	struct async_uring *ctx = (struct async_uring *)base;
	struct dm_work_item *item;
	struct io_uring_cqe *cqe;
	unsigned head;

	if (!ctx->n_inflight)
		return 0;

	head = __atomic_load_n(ctx->cq_head, __ATOMIC_ACQUIRE);
	if (head == *ctx->cq_tail)
		return 0;

	cqe  = &ctx->cqes[head & *ctx->cq_mask];
	item = (struct dm_work_item *)(uintptr_t)cqe->user_data;

	item->dmt->ioctl_result = (cqe->res >= 0) ? 0 : -1;
	if (cqe->res < 0)
		item->dmt->ioctl_errno = -cqe->res;

	__atomic_store_n(ctx->cq_head, head + 1, __ATOMIC_RELEASE);
	ctx->n_inflight--;

	*dmt_out      = item->dmt;
	*userdata_out = item->userdata;
	free(item);
	return 1;
}

static void _uring_destroy(struct dm_async_ctx *base)
{
	struct async_uring *ctx = (struct async_uring *)base;

	if (ctx->sq_ring_ptr && ctx->sq_ring_ptr != MAP_FAILED)
		munmap(ctx->sq_ring_ptr, ctx->sq_ring_sz);

	if (ctx->sqe_ptr && ctx->sqe_ptr != MAP_FAILED)
		munmap(ctx->sqe_ptr, ctx->sqe_sz);

	if (ctx->cq_ring_ptr && ctx->cq_ring_ptr != MAP_FAILED)
		munmap(ctx->cq_ring_ptr, ctx->cq_ring_sz);

	if (ctx->ring_fd >= 0)
		close(ctx->ring_fd);

	free(ctx);
}

struct dm_async_ctx *dm_async_ctx_alloc_uring(int fd, unsigned max_parallel)
{
	struct async_uring *ctx;
	struct io_uring_params params;
	struct io_uring_probe *probe;
	size_t probe_sz, sq_sz, sqe_sz, cq_sz;
	int ring_fd;
	void *sq_ptr, *sqe_ptr, *cq_ptr;

	/* Probe for IORING_OP_IOCTL support before committing resources. */
	probe_sz = sizeof(*probe) + IORING_OP_LAST * sizeof(probe->ops[0]);
	if (!(probe = calloc(1, probe_sz)))
		return NULL;

	memset(&params, 0, sizeof(params));
	ring_fd = _uring_setup(max_parallel, &params);
	if (ring_fd < 0) {
		free(probe);
		return NULL;
	}

	if (_uring_register(ring_fd, IORING_REGISTER_PROBE,
			    probe, IORING_OP_LAST) < 0 ||
	    !(probe->ops[IORING_OP_IOCTL].flags & IO_URING_OP_SUPPORTED)) {
		log_debug_activation("IORING_OP_IOCTL not supported by kernel.");
		free(probe);
		close(ring_fd);
		return NULL;
	}
	free(probe);

	if (!(ctx = calloc(1, sizeof(*ctx)))) {
		close(ring_fd);
		return NULL;
	}
	ctx->ring_fd         = ring_fd;
	ctx->max_parallel    = max_parallel;
	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _uring_submit;
	ctx->base.fn_wait      = _uring_wait;
	ctx->base.fn_try_wait  = _uring_try_wait;
	ctx->base.fn_destroy   = _uring_destroy;

	/* Map SQ ring. */
	sq_sz  = params.sq_off.array + params.sq_entries * sizeof(unsigned);
	sq_ptr = mmap(NULL, sq_sz, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
	if (sq_ptr == MAP_FAILED)
		goto err;
	ctx->sq_ring_ptr = sq_ptr;
	ctx->sq_ring_sz  = sq_sz;
	ctx->sq_head  = (unsigned *)((char *)sq_ptr + params.sq_off.head);
	ctx->sq_tail  = (unsigned *)((char *)sq_ptr + params.sq_off.tail);
	ctx->sq_mask  = (unsigned *)((char *)sq_ptr + params.sq_off.ring_mask);
	ctx->sq_array = (unsigned *)((char *)sq_ptr + params.sq_off.array);

	/* Map SQEs. */
	sqe_sz  = params.sq_entries * sizeof(struct io_uring_sqe);
	sqe_ptr = mmap(NULL, sqe_sz, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQES);
	if (sqe_ptr == MAP_FAILED)
		goto err;
	ctx->sqe_ptr = sqe_ptr;
	ctx->sqe_sz  = sqe_sz;
	ctx->sqes    = (struct io_uring_sqe *)sqe_ptr;

	/* Map CQ ring (shared with SQ on kernels with IORING_FEAT_SINGLE_MMAP). */
	if (params.features & IORING_FEAT_SINGLE_MMAP) {
		ctx->cq_ring_ptr = sq_ptr;
		ctx->cq_ring_sz  = 0;   /* shared; do not unmap separately */
	} else {
		cq_sz  = params.cq_off.cqes +
			 params.cq_entries * sizeof(struct io_uring_cqe);
		cq_ptr = mmap(NULL, cq_sz, PROT_READ | PROT_WRITE,
			      MAP_SHARED | MAP_POPULATE,
			      ring_fd, IORING_OFF_CQ_RING);
		if (cq_ptr == MAP_FAILED)
			goto err;
		ctx->cq_ring_ptr = cq_ptr;
		ctx->cq_ring_sz  = cq_sz;
	}

	ctx->cq_head = (unsigned *)((char *)ctx->cq_ring_ptr + params.cq_off.head);
	ctx->cq_tail = (unsigned *)((char *)ctx->cq_ring_ptr + params.cq_off.tail);
	ctx->cq_mask = (unsigned *)((char *)ctx->cq_ring_ptr + params.cq_off.ring_mask);
	ctx->cqes    = (struct io_uring_cqe *)
		       ((char *)ctx->cq_ring_ptr + params.cq_off.cqes);

	return &ctx->base;
err:
	log_error("Failed to map io_uring rings: %s", strerror(errno));
	_uring_destroy(&ctx->base);
	return NULL;
}

#endif  /* HAVE_IORING_OP_IOCTL */
