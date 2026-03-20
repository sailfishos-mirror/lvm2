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
 * Synchronous single-slot async backend.
 * Used when max_parallel == 1; executes the ioctl inline in submit().
 */

#include "libdm-sync.h"
#include "libdm-targets.h"
#include "libdm/misc/dmlib.h"

#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

struct async_sync {
	struct dm_async_ctx  base;
	struct dm_task      *dmt;
	int                  has_result;
};

static int _sync_submit(struct dm_async_ctx *base,
			struct dm_task *dmt, void *userdata)
{
	struct async_sync *ctx = (struct async_sync *)base;

	/* EBUSY retry resubmission -- delay before re-issuing ioctl */
	if (dmt->retry_remove > 1)
		usleep(DM_RETRY_USLEEP_DELAY);

	dm_ioctl_exec(base->fd, dmt, dmt->dmi.v4);

	dmt->async_userdata = userdata;
	ctx->dmt        = dmt;
	ctx->has_result = 1;
	return 1;
}

static int _sync_wait(struct dm_async_ctx *base,
		      struct dm_task **dmt_out, void **userdata_out)
{
	struct async_sync *ctx = (struct async_sync *)base;

	if (!ctx->has_result)
		return 0;

	*dmt_out      = ctx->dmt;
	*userdata_out = ctx->dmt->async_userdata;
	ctx->has_result = 0;
	return 1;
}

static int _sync_try_wait(struct dm_async_ctx *base,
			  struct dm_task **dmt_out, void **userdata_out)
{
	/* Sync backend: result is consumed by _sync_wait, never pending */
	return _sync_wait(base, dmt_out, userdata_out);
}

static unsigned _sync_inflight(struct dm_async_ctx *base)
{
	return ((struct async_sync *)base)->has_result ? 1 : 0;
}

static int _sync_get_fd(struct dm_async_ctx *base)
{
	return -1;   /* no pollable fd for sync backend */
}

static void _sync_destroy(struct dm_async_ctx *base)
{
	struct async_sync *ctx = (struct async_sync *)base;

	if (ctx->has_result) {
		log_warn("WARNING: Destroying sync async context with undrained task.");
		if (ctx->dmt->async_complete_fn)
			(void) ctx->dmt->async_complete_fn(base, ctx->dmt, 0,
							   ctx->dmt->async_userdata);
		dm_task_destroy(ctx->dmt);
	}

	dm_free(base);
}

struct dm_async_ctx *dm_async_ctx_alloc_sync(int fd)
{
	struct async_sync *ctx;

	if (!(ctx = dm_zalloc(sizeof(*ctx)))) {
		log_error("Failed to allocate sync async context.");
		return NULL;
	}

	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _sync_submit;
	ctx->base.fn_wait      = _sync_wait;
	ctx->base.fn_try_wait  = _sync_try_wait;
	ctx->base.fn_inflight  = _sync_inflight;
	ctx->base.fn_destroy   = _sync_destroy;
	ctx->base.fn_get_fd    = _sync_get_fd;
	return &ctx->base;
}
