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

#include <string.h>

/*
 * Create I/O engine with backend selection.
 *
 * Auto-selection: uring -> threads (default) or async (if use_aio=1)
 * Explicit hints: "uring", "threads", "async", "aio", "sync", "auto", or NULL
 * Fallback: sync always succeeds
 */
struct io_engine *bcache_create_io_engine(const char *hint)
{
	struct io_engine *engine = NULL;

	/* Determine backend: explicit hint or auto-selection */
	if (!hint || !*hint || !strcmp(hint, "auto")) {
		/* Auto: use async if use_aio=1, otherwise try uring first, then threads */
		if (use_aio())
			engine = create_async_io_engine(0);
		if (!engine)
			engine = create_uring_io_engine(0);
		if (!engine)
			engine = create_threads_io_engine(0);
	} else if (!strcmp(hint, "uring")) {
		engine = create_uring_io_engine(0);
	} else if (!strcmp(hint, "threads")) {
		engine = create_threads_io_engine(0);
	} else if (!strcmp(hint, "async") || !strcmp(hint, "aio")) {
		engine = create_async_io_engine(0);
	} else if (strcmp(hint, "sync")) {
		/* Unknown backend - warn and use sync */
		log_warn("WARNING: Unknown I/O backend '%s', using sync.", hint);
	}

	/* Fallback to sync if creation failed or sync was requested */
	if (!engine && !(engine = create_sync_io_engine()))
		stack;

	return engine;
}
