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
#include "lib/misc/lvm-signal.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

/*----------------------------------------------------------------*/

struct sync_request {
	void *context;
	int io_error;
};

struct sync_engine {
	struct io_engine base;
	struct sync_request req;
	bool pending;
};

static struct sync_engine *_to_sync(struct io_engine *ioe)
{
	return container_of(ioe, struct sync_engine, base);
}

static bool _sync_destroy(struct io_engine *ioe)
{
	/* struct io_engine is the first member, and the one pending
	 * completion is embedded, so both go away with the engine.  I/O is
	 * done synchronously in issue(), so nothing can be in flight. */
	free(ioe);

	return true;
}

static bool _sync_stop(void *arg)
{
	return sigint_caught();
}

static int _sync_issue(struct io_engine *ioe, const struct blkio_io *io,
		       void *context)
{
	struct blkio_rw_result rw;
	struct sync_engine *e = _to_sync(ioe);

	/* max_io() is 1, so at most one completion may be outstanding. */
	if (e->pending)
		return -EIO;

	rw = blkio_rw_loop(io, _sync_stop, NULL);

	/* Always queue completion - errors reported via callback in wait() */
	e->req.context = context;
	e->req.io_error = blkio_completion_result(ioe, io, &rw);
	e->pending = true;

	return 0;
}

static bool _sync_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct sync_engine *e = _to_sync(ioe);

	if (e->pending) {
		e->pending = false;
		fn(e->req.context, e->req.io_error);
	}

	return true;
}

static unsigned _sync_max_io(struct io_engine *ioe)
{
	return 1;
}

struct io_engine *create_sync_io_engine(unsigned queue_depth)
{
	struct sync_engine *e = zalloc(sizeof(*e));

	if (!e) {
		log_error("Failed to allocate sync io engine.");
		return NULL;
	}

	e->base.name = "sync";
	e->base.destroy = _sync_destroy;
	e->base.issue = _sync_issue;
	e->base.wait = _sync_wait;
	e->base.max_io = _sync_max_io;

	log_debug("Created sync io engine.");

	return &e->base;
}
