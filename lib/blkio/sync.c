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

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/*----------------------------------------------------------------*/

struct sync_io {
	struct dm_list list;
	void *context;
	int io_error;
};

struct sync_engine {
	struct io_engine base;
	struct dm_list complete;
};

static struct sync_engine *_to_sync(struct io_engine *e)
{
	return container_of(e, struct sync_engine, base);
}

static bool _sync_destroy(struct io_engine *ioe)
{
	/* struct io_engine is the first member; free the caller's pointer. */
	free(ioe);

	/* I/O is done synchronously in issue(); nothing can be in flight. */
	return true;
}

static int _sync_issue(struct io_engine *ioe, const struct blkio_io *io,
		       void *context)
{
	ssize_t rv;
	off_t offset;
	uint64_t pos = 0;
	uint64_t len;
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *sio = malloc(sizeof(*sio));
	int io_error = 0;

	if (!sio) {
		log_warn("WARNING: sync io engine unable to allocate io buffer.");
		return -ENOMEM;
	}

	offset = io->range.offset;
	len = io->range.len;

	while (pos < len) {
		if (io->d == BLKIO_DIR_READ)
			rv = pread(io->fd, (char *)io->data + pos, len - pos, offset + pos);
		else
			rv = pwrite(io->fd, (char *)io->data + pos, len - pos, offset + pos);

		if (rv < 0) {
			if (errno == EAGAIN)
				continue;
			if (errno == EINTR && !sigint_caught())
				continue;

			log_warn("WARNING: blkio sync %s device offset %llu len %llu: %s.",
				 blkio_dir_name(io->d), (unsigned long long)offset,
				 (unsigned long long)(len - pos),
				 strerror(errno));
			io_error = -errno;
			break;
		}

		if (rv == 0)
			break;

		pos += rv;
	}

	if (!io_error && pos < len) {
		if (pos >= (1 << SECTOR_SHIFT)) {
			/* minimum acceptable read is 1 sector */
			log_debug_devs("blkio sync %s offset %llu requested %llu got %llu.",
				       blkio_dir_name(io->d), (unsigned long long)offset,
				       (unsigned long long)len, (unsigned long long)pos);
		} else {
			log_warn("WARNING: blkio sync %s offset %llu requested %llu got %llu.",
				 blkio_dir_name(io->d), (unsigned long long)offset,
				 (unsigned long long)len, (unsigned long long)pos);
			io_error = -ENODATA;
		}
	}

	/* Always queue completion - errors reported via callback in wait() */
	sio->context = context;
	sio->io_error = io_error;
	dm_list_add(&e->complete, &sio->list);

	return 0;
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

	e->base.destroy = _sync_destroy;
	e->base.issue = _sync_issue;
	e->base.wait = _sync_wait;
	e->base.max_io = _sync_max_io;

	dm_list_init(&e->complete);

	log_debug("Created sync io engine.");

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->base;
}
