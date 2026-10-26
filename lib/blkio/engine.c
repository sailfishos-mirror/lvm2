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

struct blkio_rw_result blkio_rw_loop(const struct blkio_io *io,
				     bool (*stop)(void *), void *stop_arg)
{
	struct blkio_rw_result result = { 0 };
	ssize_t rv;

	while (result.done < io->range.len) {
		if (stop && stop(stop_arg)) {
			result.errnum = EINTR;
			break;
		}

		if (io->d == BLKIO_DIR_READ)
			rv = pread(io->fd, (char *) io->data + result.done,
				   io->range.len - result.done,
				   (off_t) (io->range.offset + result.done));
		else
			rv = pwrite(io->fd, (char *) io->data + result.done,
				    io->range.len - result.done,
				    (off_t) (io->range.offset + result.done));

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

static int _completion_errno(const struct io_engine *ioe,
			     const struct blkio_io *io, int errnum)
{
	const char *op = blkio_dir_name(io->d);

	/*
	 * An interrupted scan is the normal way a SIGINT cancels
	 * outstanding I/O, so it is not worth a warning for every request.
	 */
	if (errnum == EINTR) {
		log_debug_devs("%s io engine %s interrupted at offset %llu.",
			       ioe->name, op,
			       (unsigned long long) io->range.offset);
		return -EINTR;
	}

	log_warn("WARNING: %s io engine %s device offset %llu len %llu: %s.",
		 ioe->name, op,
		 (unsigned long long) io->range.offset,
		 (unsigned long long) io->range.len,
		 strerror(errnum));
	return -errnum;
}

/*
 * A short direct read is a normal end of device condition only when it
 * still transferred a whole sector.
 */
static bool _at_least_one_sector(int64_t nr_bytes)
{
	return nr_bytes >= SECTOR_SIZE;
}

void *blkio_alloc_request_pool(const struct io_engine *proto,
			       const struct io_engine_layout *layout,
			       unsigned nr_requests)
{
	struct io_engine_pool *pool;
	struct io_pool_link *next;
	char *engine, *request;
	unsigned i;

	if (!(engine = zalloc(layout->engine_size +
			      nr_requests * layout->request_size))) {
		log_error("Failed to allocate %s io engine.", proto->name);
		return NULL;
	}

	*(struct io_engine *) engine = *proto;

	/* Thread the requests onto the free list, in allocation order. */
	next = NULL;
	for (i = nr_requests; i-- > 0;) {
		request = engine + layout->requests_offset +
			  i * layout->request_size;
		((struct io_pool_link *) request)->next = next;
		next = (struct io_pool_link *) request;
	}

	pool = (struct io_engine_pool *) (engine + layout->pool_offset);
	pool->creator_pid = getpid();
	pool->free_list = next;

	return engine;
}

int blkio_completion_status(const struct io_engine *ioe,
			    const struct blkio_io *io, int64_t result)
{
	const char *op = blkio_dir_name(io->d);

	if (result < 0)
		return _completion_errno(ioe, io, (int) -result);

	if ((uint64_t) result >= io->range.len)
		return 0;

	/*
	 * A direct read is truncated at the end of the device, so a short
	 * read is a normal end of device condition.  The caller has no way
	 * to learn about a shorter transfer, so zero fill the unread tail
	 * of the block to keep it free of stale data when the block is
	 * written back.  A transfer shorter than a whole sector means the
	 * device is not behaving as expected.  A short write means part of
	 * the dirty block was not persisted, so it is always an error.
	 */
	if ((io->d == BLKIO_DIR_READ) && _at_least_one_sector(result)) {
		memset((char *) io->data + result, 0, io->range.len - result);
		log_debug_devs("%s io engine short read zero filled %llu bytes.",
			       ioe->name,
			       (unsigned long long) (io->range.len - result));
		return 0;
	}

	log_warn("WARNING: %s io engine %s offset %llu requested %llu got %llu.",
		 ioe->name, op,
		 (unsigned long long) io->range.offset,
		 (unsigned long long) io->range.len,
		 (unsigned long long) result);
	return (io->d == BLKIO_DIR_READ) ? -ENODATA : -EIO;
}

int blkio_completion_result(const struct io_engine *ioe,
			    const struct blkio_io *io,
			    const struct blkio_rw_result *rw)
{
	if (rw->errnum) {
		/* Report the part of the transfer that failed. */
		struct blkio_io failed = *io;

		failed.range.offset += rw->done;
		failed.range.len -= rw->done;
		return _completion_errno(ioe, &failed, rw->errnum);
	}

	if (rw->done < io->range.len)
		return blkio_completion_status(ioe, io, (int64_t) rw->done);

	return 0;
}

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
		/* Auto tries the engines that are good defaults; a hint names one. */
		bool wanted = automatic ? h->auto_select : _engine_matches(h, hint);

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
