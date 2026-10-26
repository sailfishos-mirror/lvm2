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

#include "lib/blkio/internal.h"
#include "lib/misc/lvm-globals.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

/*----------------------------------------------------------------*/

bool blkio_engine_is_forked(pid_t creator_pid)
{
	return getpid() != creator_pid;
}

/*----------------------------------------------------------------*/

/*
 * An engine name or alias is stored inline rather than pointed to, so the
 * row stays one object.  The array is sized for the longest name in the
 * table, and a name that does not fit is a compiler warning.
 */
#define ENGINE_NAME_SIZE 8

/*
 * One selectable engine: the name the hint uses, up to two aliases for it
 * and its create function.  A NULL create means the engine was not built
 * in, so the name is still known but cannot be selected.  auto_select
 * marks the entries automatic selection tries.
 */
struct engine_hint {
	char name[ENGINE_NAME_SIZE];
	char alias[2][ENGINE_NAME_SIZE];
	struct io_engine *(*create)(unsigned queue_depth);
	bool auto_select;
};

static const struct engine_hint _engine_hints[] = {
	{ "libaio",  { "async", "aio" }, create_async_io_engine,   true },
	{ "sync",    { "" },             create_sync_io_engine,    false },
};

static bool _engine_matches(const struct engine_hint *h, const char *name)
{
	unsigned i;

	if (!strcmp(name, h->name))
		return true;

	for (i = 0; i < DM_ARRAY_SIZE(h->alias) && h->alias[i][0]; i++)
		if (!strcmp(name, h->alias[i]))
			return true;

	return false;
}

/*
 * Create an I/O engine selected by hint.  A NULL return always means
 * creation failed and has been reported, except when errno is ENODEV: the
 * engine exists but this system cannot set it up, so the walk passes it
 * over and tries the next candidate.
 */
struct io_engine *blkio_create_io_engine(const char *hint)
{
	const struct engine_hint *h;
	struct io_engine *engine;
	bool automatic = !hint || !*hint || !strcmp(hint, "auto");
	bool matched = false;

	for (h = _engine_hints;
	     h < _engine_hints + DM_ARRAY_SIZE(_engine_hints); h++) {
		/*
		 * Auto tries the engines that are good defaults; a hint names
		 * one.  The deprecated use_aio=0 asks for sync I/O.
		 */
		bool wanted = automatic ?
			(h->auto_select && use_aio()) : _engine_matches(h, hint);

		if (!wanted)
			continue;

		/* The name is known even when the engine was not built in. */
		matched = true;

		if (!h->create) {
			if (automatic) {
				log_debug("%s io engine not compiled in.",
					  h->name);
				continue;
			}

			log_warn("WARNING: Requested %s I/O engine is not "
				 "compiled in.", h->name);
			break;
		}

		/* A stale errno must not make a failure look unavailable. */
		errno = 0;
		if ((engine = h->create(BLKIO_ENGINE_DEFAULT)))
			return engine;

		if (errno != ENODEV)
			return_NULL;

		if (automatic)
			log_debug("%s io engine not available.", h->name);
		else {
			log_warn("WARNING: Requested %s I/O engine is not "
				 "available.", h->name);
			break;
		}
	}

	/* An explicit hint that matched no engine at all is a typo. */
	if (!automatic && !matched)
		log_warn("WARNING: Unknown I/O engine \"%s\".", hint);

	if (!(engine = create_sync_io_engine(BLKIO_ENGINE_DEFAULT)))
		return_NULL;

	return engine;
}
