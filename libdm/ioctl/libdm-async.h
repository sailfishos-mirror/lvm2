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

/* Default parallelism when caller passes 0 to dm_async_ctx_create(). */
#define DM_ASYNC_DEFAULT_PARALLEL     16

/*
 * Opaque async context exposed publicly as a forward declaration.
 * Each backend embeds this as its first member.
 */
struct dm_async_ctx {
	int  (*fn_submit)(struct dm_async_ctx *ctx,
			  struct dm_task *dmt, void *userdata);
	int  (*fn_wait)(struct dm_async_ctx *ctx,
			struct dm_task **dmt_out, void **userdata_out);
	int  (*fn_try_wait)(struct dm_async_ctx *ctx,
			    struct dm_task **dmt_out, void **userdata_out);
	unsigned (*fn_inflight)(struct dm_async_ctx *ctx);
	void (*fn_destroy)(struct dm_async_ctx *ctx);
	int  (*fn_get_fd)(struct dm_async_ctx *ctx);
	int   fd;   /* DM control fd, captured at context creation */
};

/*
 * Build ioctl buffer without submitting.  Normally called internally
 * by dm_async_submit(); only call directly when something must happen
 * between prepare and submit (e.g. setting dev_name for REMOVE).
 */
int dm_task_prepare(struct dm_task *dmt);

/* Internal: wait for one completion, post-process result (main thread only). */
int dm_async_wait_completion(struct dm_async_ctx *ctx,
			     struct dm_task **dmt_out, void **userdata_out);
int dm_task_handle_completion(struct dm_async_ctx *ctx, struct dm_task *dmt,
			      void *userdata);

/* Thread-pool backend (always compiled). */
struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_parallel);

#ifdef HAVE_IORING_OP_IOCTL
/* io_uring backend (requires IORING_OP_IOCTL in linux/io_uring.h, Linux 6.5+). */
struct dm_async_ctx *dm_async_ctx_alloc_uring(int fd, unsigned max_parallel);
#endif

#endif /* LIBDM_ASYNC_H */
