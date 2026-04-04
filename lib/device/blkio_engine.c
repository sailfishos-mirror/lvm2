/*
 * Copyright (C) 2018-2026 Red Hat, Inc. All rights reserved.
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

#include "lib/device/blkio.h"
#include "lib/log/lvm-logging.h"
#include "lib/misc/lvm-globals.h"

#include <stdbool.h>
#include <string.h>
#include <unistd.h>

//----------------------------------------------------------------

bool blkio_engine_is_forked(pid_t creator_pid)
{
	return getpid() != creator_pid;
}

//----------------------------------------------------------------

/*
 * Create an I/O engine selected by hint.
 *
 * Auto-selection: libaio (if use_aio=1) -> sync (default)
 * Explicit hints: "libaio" ("async", "aio"), "sync", "auto", or NULL
 * Fallback: sync always succeeds
 */
struct engine_hint {
	const char *name;
	const char *label;
	struct io_engine *(*create)(unsigned queue_depth);
	bool auto_select;
};

static const struct engine_hint _engine_hints[] = {
	{ "libaio", "libaio", create_async_io_engine, true },
	{ "async", "libaio", create_async_io_engine, false },
	{ "aio", "libaio", create_async_io_engine, false },
};

struct io_engine *blkio_create_io_engine(const char *hint)
{
	struct io_engine *engine = NULL;
	unsigned i;

	/* Determine engine: explicit hint or auto-selection */
	if (!hint || !*hint || !strcmp(hint, "auto")) {
		/* Auto: use libaio if use_aio=1 */
		if (use_aio())
			for (i = 0; i < DM_ARRAY_SIZE(_engine_hints); i++) {
				if (!_engine_hints[i].auto_select)
					continue;

				if ((engine = _engine_hints[i].create(0)))
					break;
			}
	} else {
		for (i = 0; i < DM_ARRAY_SIZE(_engine_hints); i++) {
			if (!strcmp(hint, _engine_hints[i].name)) {
				engine = _engine_hints[i].create(0);
				if (!engine)
					log_warn("WARNING: Requested %s I/O engine unavailable; using sync.",
						 _engine_hints[i].label);
				break;
			}
		}

		if (i == DM_ARRAY_SIZE(_engine_hints) && strcmp(hint, "sync"))
			log_warn("WARNING: Unknown I/O engine '%s', using sync.", hint);
	}

	/* Fallback to sync if creation failed or sync was requested */
	if (!engine && !(engine = create_sync_io_engine()))
		stack;

	return engine;
}
