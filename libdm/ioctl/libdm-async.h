/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
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

/*
 * Internal work item: tracks one in-flight async ioctl.
 */
struct dm_work_item {
	struct dm_list  list;
	struct dm_task *dmt;
	void           *userdata;
	int             result;   /* ioctl() return value */
};

/*
 * Opaque async context exposed publicly as a forward declaration.
 * Each backend embeds this as its first member.
 */
struct dm_async_ctx {
	int  (*fn_submit)(struct dm_async_ctx *ctx,
			  struct dm_task *dmt, void *userdata);
	int  (*fn_wait)(struct dm_async_ctx *ctx,
			void **userdata_out, int *result_out);
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
int dm_ioctl_exec(int fd, unsigned command, struct dm_task *dmt,
		  struct dm_ioctl *dmi);

/* Thread-pool backend (always compiled). */
struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_parallel);

#endif /* LIBDM_ASYNC_H */
