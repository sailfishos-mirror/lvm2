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
#define DM_ASYNC_DEFAULT_PARALLEL     32

/* Retry constants for EBUSY removes (shared with libdm-iface.c). */
#define DM_IOCTL_RETRIES              25
#define DM_RETRY_USLEEP_DELAY         200000

/* Inline in struct dm_task for parallel deptree ops */
struct dm_task_async {
	int (*complete_fn)(struct dm_task *, void *userdata);	/* completion callback */
};

/*
 * Internal work item: tracks one in-flight async ioctl.
 */
struct dm_work_item {
	struct dm_list  list;
	struct dm_task *dmt;
	void           *userdata; /* caller context, returned via wait */
};

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
	void (*fn_destroy)(struct dm_async_ctx *ctx);
	int   fd;   /* DM control fd, captured at context creation */
};

/*
 * Return the ioctl command number for a task.
 * Declared here; defined in libdm-iface.c.
 */
unsigned dm_task_get_ioctl_cmd(const struct dm_task *dmt);

/*
 * Execute the prepared ioctl for a task on the given fd.
 * dmi must be the buffer built by _dm_task_build_dmi() / dm_task_prepare().
 * Sets dmt->ioctl_errno on failure. Defined in libdm-iface.c.
 */
int dm_ioctl_exec(int fd, struct dm_task *dmt, struct dm_ioctl *dmi);

/*
 * Execute ioctl with EBUSY retry for remove operations.
 * Declared here; defined in libdm-iface.c.
 */
int dm_ioctl_exec_retry(int fd, struct dm_task *dmt);

/* Internal: wait for one completion, post-process result (main thread only). */
int dm_async_wait_completion(struct dm_async_ctx *ctx,
			     struct dm_task **dmt_out, void **userdata_out);
int dm_task_handle_completion(struct dm_task *dmt, void *userdata);

/* Thread-pool backend (always compiled). */
struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_parallel);

#endif /* LIBDM_ASYNC_H */
