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

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

//----------------------------------------------------------------

bool blkio_engine_is_forked(pid_t creator_pid)
{
	return getpid() != creator_pid;
}

//----------------------------------------------------------------

struct blkio_rw_result blkio_rw_loop(int fd, enum dir d, void *data,
				     size_t len, off_t offset,
				     bool (*stop)(void *), void *stop_arg)
{
	struct blkio_rw_result result = { 0 };
	ssize_t rv;

	while (result.done < len && !(stop && stop(stop_arg))) {
		if (d == DIR_READ)
			rv = pread(fd, (char *) data + result.done,
				   len - result.done,
				   offset + (off_t) result.done);
		else
			rv = pwrite(fd, (char *) data + result.done,
				    len - result.done,
				    offset + (off_t) result.done);

		if (rv < 0) {
			if (errno == EAGAIN)
				continue;

			if (errno == EINTR) {
				if (stop && stop(stop_arg)) {
					result.errnum = EINTR;
					break;
				}
				continue;
			}

			result.errnum = errno;
			break;
		}

		if (rv == 0)
			break;

		result.done += rv;
	}

	return result;
}

int blkio_completion_errno(const char *engine, enum dir d,
			   uint64_t offset, uint64_t remaining,
			   int errnum)
{
	const char *op = (d == DIR_READ) ? "read" : "write";

	log_warn("WARNING: %s io engine %s device offset %llu len %llu: %s.",
		 engine, op,
		 (unsigned long long) offset,
		 (unsigned long long) remaining,
		 strerror(errnum));
	return -errnum;
}

int blkio_completion_status(const char *engine, enum dir d,
			    uint64_t offset, uint64_t requested,
			    int64_t result)
{
	const char *op = (d == DIR_READ) ? "read" : "write";

	if (result < 0)
		return blkio_completion_errno(engine, d, offset,
					      requested, (int) -result);

	if ((uint64_t) result >= requested)
		return 0;

	/*
	 * A short read is acceptable as long as at least one sector was
	 * read; this is the long-standing async engine behaviour.  A short
	 * write means part of the dirty block was not persisted, so it is
	 * always an error.
	 */
	if ((d == DIR_READ) && (result >> SECTOR_SHIFT)) {
		log_debug_devs("%s io engine %s offset %llu requested %llu got %llu.",
			       engine, op,
			       (unsigned long long) offset,
			       (unsigned long long) requested,
			       (unsigned long long) result);
		return 0;
	}

	log_warn("WARNING: %s io engine %s offset %llu requested %llu got %llu.",
		 engine, op,
		 (unsigned long long) offset,
		 (unsigned long long) requested,
		 (unsigned long long) result);
	return (d == DIR_READ) ? -ENODATA : -EIO;
}

/*
 * Create an I/O engine selected by hint.
 *
 * Auto-selection: use the best async engine: iouring -> threads -> libaio.
 * Explicit hints: "iouring" ("uring"), "threads", "libaio" ("async", "aio"),
 *                 "sync", "auto", or NULL
 * Sync is used if no engine was created.
 * Engine allocation can still fail.
 */
struct engine_hint {
	const char *name;
	const char *label;
	struct io_engine *(*create)(unsigned queue_depth);
	bool auto_select;
};

static const struct engine_hint _engine_hints[] = {
	{ "iouring", "iouring", create_uring_io_engine, true },
	{ "uring", "iouring", create_uring_io_engine, false },
	{ "threads", "threads", create_threads_io_engine, true },
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
