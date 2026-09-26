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

#include "lib/misc/lib.h"
#include "lib/device/bcache.h"
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#define SECTOR_SHIFT 9L

//----------------------------------------------------------------

struct sync_io {
	struct dm_list list;
	void *context;
	int io_error;
};

struct sync_engine {
	struct io_engine e;
	struct dm_list complete;
};

static struct sync_engine *_to_sync(struct io_engine *e)
{
	return container_of(e, struct sync_engine, e);
}

static bool _sync_destroy(struct io_engine *ioe)
{
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *io, *tmp;

	/* issue() can queue a completion that wait() never consumed. */
	dm_list_iterate_items_safe(io, tmp, &e->complete) {
		dm_list_del(&io->list);
		free(io);
	}

	free(e);

	/* I/O is done synchronously in issue(); nothing can be in flight. */
	return true;
}

static const char _sync[] = "sync";

static bool _sync_issue(struct io_engine *ioe, enum dir d, int fd,
			sector_t sb, sector_t se, void *data, void *context)
{
	ssize_t rv;
	off_t offset;
	uint64_t pos = 0;
	uint64_t len = (se - sb) << SECTOR_SHIFT;
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *io = malloc(sizeof(*io));
	int io_error = 0;

	if (!io) {
		log_warn("WARNING: bcache sync unable to allocate io buffer.");
		return false;
	}

	offset = sb << SECTOR_SHIFT;

	while (pos < len) {
		if (d == DIR_READ)
			rv = pread(fd, (char *)data + pos, len - pos, offset + pos);
		else
			rv = pwrite(fd, (char *)data + pos, len - pos, offset + pos);

		if (rv < 0) {
			if (errno == EAGAIN)
				continue;
			if (errno == EINTR && !sigint_caught())
				continue;

			io_error = bcache_io_completion_errno(_sync, d,
							      offset + pos,
							      len - pos, errno);
			break;
		}

		if (rv == 0)
			break;

		pos += rv;
	}

	if (!io_error && pos < len)
		io_error = bcache_io_completion_status(_sync, d, offset, len, (int64_t) pos);

	/* Always queue completion - errors reported via callback in wait() */
	io->context = context;
	io->io_error = io_error;
	dm_list_add(&e->complete, &io->list);

	return true;
}

static bool _sync_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct sync_io *io, *tmp;
	struct sync_engine *e = _to_sync(ioe);

	dm_list_iterate_items_safe(io, tmp, &e->complete) {
		fn(io->context, io->io_error);
		dm_list_del(&io->list);
		free(io);
	}

	return true;
}

static unsigned _sync_max_io(struct io_engine *e)
{
	return 1;
}

struct io_engine *create_sync_io_engine(void)
{
	struct sync_engine *e = malloc(sizeof(*e));

	if (!e) {
		log_error("Failed to allocate bcache sync io engine.");
		return NULL;
	}

	e->e.destroy = _sync_destroy;
	e->e.issue = _sync_issue;
	e->e.wait = _sync_wait;
	e->e.max_io = _sync_max_io;

	dm_list_init(&e->complete);

	log_debug("Created bcache sync io engine.");

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->e;
}
