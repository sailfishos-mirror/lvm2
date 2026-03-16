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
 * io_uring async DM ioctl backend.
 * Requires IORING_OP_IOCTL (not yet upstream as of 2026).
 * Kept as a prototype for when/if the kernel gains this opcode.
 */

#ifdef HAVE_IORING_OP_IOCTL

#include "libdm-async.h"
#include "libdm-targets.h"
#include "libdm/misc/dmlib.h"

#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

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
	unsigned             max_inflight;
	unsigned             n_inflight;
	pid_t                creator_pid;  /* for fork() detection */

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
	struct io_uring_sqe *sqe;
	unsigned tail, orig_tail, idx;
	int retry_delay = (dmt->retry_remove > 1);
	unsigned to_submit = retry_delay ? 2 : 1;

	if (ctx->n_inflight >= ctx->max_inflight) {
		log_error("io_uring async context full (%u in flight).",
			  ctx->n_inflight);
		return 0;
	}

	dmt->async_userdata = userdata;

	orig_tail = *ctx->sq_tail;
	tail = orig_tail;

	/* EBUSY retry: prepend linked timeout for kernel-side delay */
	if (retry_delay) {
		static const struct __kernel_timespec retry_ts = {
			.tv_sec  = 0,
			.tv_nsec = (long long)DM_RETRY_USLEEP_DELAY * 1000,
		};

		idx = tail & *ctx->sq_mask;
		sqe = &ctx->sqes[idx];
		memset(sqe, 0, sizeof(*sqe));
		sqe->opcode    = IORING_OP_TIMEOUT;
		sqe->addr      = (uint64_t)(uintptr_t)&retry_ts;
		sqe->len       = 1;
		sqe->flags     = IOSQE_IO_LINK;
		sqe->user_data = 0;   /* sentinel: skip in wait */
		ctx->sq_array[idx] = idx;
		tail++;
	}

	idx  = tail & *ctx->sq_mask;
	sqe  = &ctx->sqes[idx];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode    = IORING_OP_IOCTL;
	sqe->fd        = ctx->base.fd;
	sqe->off       = (uint64_t)dm_task_ioctl_cmd(dmt);
	sqe->addr      = (uint64_t)(uintptr_t)dmt->dmi.v4;
	sqe->user_data = (uint64_t)(uintptr_t)dmt;

	ctx->sq_array[idx] = idx;
	__atomic_store_n(ctx->sq_tail, tail + 1, __ATOMIC_RELEASE);

	if (_uring_enter(ctx->ring_fd, to_submit, 0, 0) < 0) {
		log_error("io_uring_enter submit failed: %s", strerror(errno));
		__atomic_store_n(ctx->sq_tail, orig_tail, __ATOMIC_RELEASE);
		return 0;
	}

	ctx->n_inflight++;
	return 1;
}

/* Consume and skip any timeout CQEs (user_data == 0). */
static void _uring_skip_timeout_cqes(struct async_uring *ctx)
{
	unsigned head = __atomic_load_n(ctx->cq_head, __ATOMIC_ACQUIRE);

	while (head != *ctx->cq_tail &&
	       ctx->cqes[head & *ctx->cq_mask].user_data == 0)
		head++;

	__atomic_store_n(ctx->cq_head, head, __ATOMIC_RELEASE);
}

static int _uring_wait(struct dm_async_ctx *base, int blocking,
		       struct dm_task **dmt_out, void **userdata_out)
{
	struct async_uring *ctx = (struct async_uring *)base;
	struct dm_task *dmt;
	struct io_uring_cqe *cqe;
	unsigned head;

	if (!ctx->n_inflight)
		return 0;

	for (;;) {
		_uring_skip_timeout_cqes(ctx);

		head = __atomic_load_n(ctx->cq_head, __ATOMIC_ACQUIRE);
		if (head != *ctx->cq_tail)
			break;

		if (!blocking)
			return 0;

		/* No real CQE yet -- wait for more */
		if (_uring_enter(ctx->ring_fd, 0, 1,
				 IORING_ENTER_GETEVENTS) < 0) {
			log_error("io_uring_enter wait failed: %s",
				  strerror(errno));
			return 0;
		}
	}

	cqe = &ctx->cqes[head & *ctx->cq_mask];
	dmt = (struct dm_task *)(uintptr_t)cqe->user_data;

	dmt->ioctl_result = (cqe->res >= 0) ? 0 : -1;
	if (cqe->res < 0)
		dmt->ioctl_errno = -cqe->res;

	__atomic_store_n(ctx->cq_head, head + 1, __ATOMIC_RELEASE);
	ctx->n_inflight--;

	*dmt_out      = dmt;
	*userdata_out = dmt->async_userdata;
	return 1;
}

static unsigned _uring_inflight(struct dm_async_ctx *base)
{
	return ((struct async_uring *)base)->n_inflight;
}

static int _uring_get_fd(struct dm_async_ctx *base)
{
	return ((struct async_uring *)base)->ring_fd;
}

static void _uring_destroy(struct dm_async_ctx *base)
{
	struct async_uring *ctx = (struct async_uring *)base;

	/*
	 * After fork() the io_uring ring belongs to the parent process.
	 * The child must not unmap or close it.
	 */
	if (getpid() != ctx->creator_pid) {
		dm_free(ctx);
		return;
	}

	if (ctx->sq_ring_ptr && ctx->sq_ring_ptr != MAP_FAILED)
		munmap(ctx->sq_ring_ptr, ctx->sq_ring_sz);

	if (ctx->sqe_ptr && ctx->sqe_ptr != MAP_FAILED)
		munmap(ctx->sqe_ptr, ctx->sqe_sz);

	if (ctx->cq_ring_ptr && ctx->cq_ring_ptr != MAP_FAILED)
		munmap(ctx->cq_ring_ptr, ctx->cq_ring_sz);

	if (ctx->ring_fd >= 0)
		close(ctx->ring_fd);

	dm_free(ctx);
}

struct dm_async_ctx *dm_async_ctx_alloc_uring(int fd, unsigned max_inflight)
{
	struct async_uring *ctx;
	struct io_uring_params params;
	struct io_uring_probe *probe;
	size_t probe_sz, sq_sz, sqe_sz, cq_sz;
	int ring_fd;
	void *sq_ptr, *sqe_ptr, *cq_ptr;

	/* Probe for IORING_OP_IOCTL support before committing resources. */
	probe_sz = sizeof(*probe) + IORING_OP_LAST * sizeof(probe->ops[0]);
	if (!(probe = dm_zalloc(probe_sz)))
		return NULL;

	memset(&params, 0, sizeof(params));
	ring_fd = _uring_setup(max_inflight, &params);
	if (ring_fd < 0) {
		dm_free(probe);
		return NULL;
	}

	if (_uring_register(ring_fd, IORING_REGISTER_PROBE,
			    probe, IORING_OP_LAST) < 0 ||
	    !(probe->ops[IORING_OP_IOCTL].flags & IO_URING_OP_SUPPORTED)) {
		log_debug_activation("IORING_OP_IOCTL not supported by kernel.");
		dm_free(probe);
		close(ring_fd);
		return NULL;
	}
	dm_free(probe);

	if (!(ctx = dm_zalloc(sizeof(*ctx)))) {
		close(ring_fd);
		return NULL;
	}
	ctx->ring_fd         = ring_fd;
	ctx->max_inflight    = max_inflight;
	ctx->creator_pid     = getpid();
	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _uring_submit;
	ctx->base.fn_wait      = _uring_wait;
	ctx->base.fn_inflight  = _uring_inflight;
	ctx->base.fn_destroy   = _uring_destroy;
	ctx->base.fn_get_fd    = _uring_get_fd;

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
