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
};

struct sync_engine {
	struct io_engine e;
	struct dm_list complete;
};

static struct sync_engine *_to_sync(struct io_engine *e)
{
	return container_of(e, struct sync_engine, e);
}

static void _sync_destroy(struct io_engine *ioe)
{
	struct sync_engine *e = _to_sync(ioe);
	free(e);
}

static const char *_sync_dir(enum dir d)
{
	return (d == DIR_READ) ? "read" : "write";
}

static bool _sync_issue(struct io_engine *ioe, enum dir d, int fd,
			sector_t sb, sector_t se, void *data, void *context)
{
	ssize_t rv;
	off_t offset;
	uint64_t pos = 0;
	uint64_t len = (se - sb) * 512;
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *io = malloc(sizeof(*io));

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

			log_warn("WARNING: bcache sync %s device offset %llu len %llu: %s.",
				 _sync_dir(d), (unsigned long long)offset,
				 (unsigned long long)(len - pos),
				 strerror(errno));
			free(io);
			return false;
		}

		if (rv == 0)
			break;

		pos += rv;
	}

	if (pos < len) {
		if (pos >= (1 << SECTOR_SHIFT)) {
			/* minimum acceptable read is 1 sector */
			log_debug_devs("bcache sync %s offset %llu requested %llu got %llu.",
				       _sync_dir(d), (unsigned long long)offset,
				       (unsigned long long)len, (unsigned long long)pos);
		} else {
			log_warn("WARNING: bcache sync %s offset %llu requested %llu got %llu.",
				 _sync_dir(d), (unsigned long long)offset,
				 (unsigned long long)len, (unsigned long long)pos);
			free(io);
			return false;
		}
	}

	dm_list_add(&e->complete, &io->list);
	io->context = context;

	return true;
}

static bool _sync_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct sync_io *io, *tmp;
	struct sync_engine *e = _to_sync(ioe);

	dm_list_iterate_items_safe(io, tmp, &e->complete) {
		fn(io->context, 0);
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
