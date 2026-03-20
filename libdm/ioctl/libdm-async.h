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

#ifndef LIBDM_ASYNC_H
#define LIBDM_ASYNC_H

#include "libdm/libdevmapper.h"
#include "libdm/misc/dm-ioctl.h"

/* fn_wait blocking mode */
#define DM_WAIT_POLL   0
#define DM_WAIT_BLOCK  1

/* Default async concurrency when caller passes 0 to dm_async_ctx_create(). */
#define DM_ASYNC_DEFAULT_INFLIGHT     16
/* Upper bound on auto-sized thread pool (2 * CPUs, capped here). */
#define DM_ASYNC_MAX_INFLIGHT        128
/* Threads per CPU for auto-sizing (I/O-bound on kernel RCU sleeps). */
#define DM_ASYNC_THREADS_PER_CPU       2

/*
 * Opaque async context exposed publicly as a forward declaration.
 * Each backend embeds this as its first member.
 */
struct dm_async_ctx {
	int  (*fn_submit)(struct dm_async_ctx *ctx,
			  struct dm_task *dmt, void *userdata);
	/* Wait for next completed ioctl, returns 1 + task, 0 when done.
	 * blocking=1: sleep until available; blocking=0: poll only. */
	int  (*fn_wait)(struct dm_async_ctx *ctx, int blocking,
			struct dm_task **dmt_out, void **userdata_out);
	unsigned (*fn_inflight)(struct dm_async_ctx *ctx);
	void (*fn_destroy)(struct dm_async_ctx *ctx);
	int  (*fn_get_fd)(struct dm_async_ctx *ctx);
	int   fd;   /* DM control fd, captured at context creation */
	int   submit_drain_error; /* sticky: completion failed during submit back-pressure */
};

/*
 * Build ioctl buffer without submitting.  Normally called internally
 * by dm_async_submit(); only call directly when something must happen
 * between prepare and submit (e.g. setting dev_name for REMOVE).
 */
int dm_task_prepare(struct dm_task *dmt);

/* Internal: post-process completed task result (main thread only). */
int dm_async_task_completion(struct dm_async_ctx *ctx, struct dm_task *dmt,
			      void *userdata);

/* Thread-pool backend (always compiled). */
struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_inflight);

#endif /* LIBDM_ASYNC_H */
