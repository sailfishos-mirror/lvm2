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

#include "lib/device/bcache.h"

#include "lib/log/lvm-logging.h"
#include "lib/misc/lvm-globals.h"

#include <errno.h>
#include <string.h>

#define BCACHE_IO_SECTOR_BYTES 512u

int bcache_io_completion_errno(const char *backend, enum dir d,
			       uint64_t offset, uint64_t remaining,
			       int errnum)
{
	const char *op = (d == DIR_READ) ? "read" : "write";

	log_warn("WARNING: bcache %s %s device offset %llu len %llu: %s.",
		 backend, op,
		 (unsigned long long) offset,
		 (unsigned long long) remaining,
		 strerror(errnum));
	return -errnum;
}

int bcache_io_completion_status(const char *backend, enum dir d,
				uint64_t offset, uint64_t requested,
				int64_t result)
{
	const char *op = (d == DIR_READ) ? "read" : "write";

	if (result < 0)
		return bcache_io_completion_errno(backend, d, offset,
						 requested, (int) -result);

	if ((uint64_t) result >= requested)
		return 0;

	if (result >= BCACHE_IO_SECTOR_BYTES) {
		log_debug_devs("bcache %s %s offset %llu requested %llu got %llu.",
			       backend, op,
			       (unsigned long long) offset,
			       (unsigned long long) requested,
			       (unsigned long long) result);
		return 0;
	}

	log_warn("WARNING: bcache %s %s offset %llu requested %llu got %llu.",
		 backend, op,
		 (unsigned long long) offset,
		 (unsigned long long) requested,
		 (unsigned long long) result);
	return -ENODATA;
}

/*
 * Create I/O engine with backend selection.
 *
 * Auto-selection: threads (default) or libaio (if use_aio=1)
 * Explicit hints: "threads", "libaio" ("async", "aio"), "sync", "auto", or NULL
 * Fallback: sync always succeeds
 */
struct io_engine *bcache_create_io_engine(const char *hint)
{
	struct io_engine *engine = NULL;

	/* Determine backend: explicit hint or auto-selection */
	if (!hint || !*hint || !strcmp(hint, "auto")) {
		/* Auto: use libaio if use_aio=1, otherwise threads */
		if (use_aio())
			engine = create_async_io_engine(0);
		if (!engine)
			engine = create_threads_io_engine(0);
	} else if (!strcmp(hint, "threads")) {
		engine = create_threads_io_engine(0);
		if (!engine)
			log_warn("WARNING: Requested threads I/O backend unavailable; using sync.");
	} else if (!strcmp(hint, "libaio") || !strcmp(hint, "async") || !strcmp(hint, "aio")) {
		engine = create_async_io_engine(0);
		if (!engine)
			log_warn("WARNING: Requested libaio I/O backend unavailable; using sync.");
	} else if (strcmp(hint, "sync")) {
		/* Unknown backend - warn and use sync */
		log_warn("WARNING: Unknown I/O backend '%s', using sync.", hint);
	}

	/* Fallback to sync if creation failed or sync was requested */
	if (!engine && !(engine = create_sync_io_engine()))
		stack;

	return engine;
}
