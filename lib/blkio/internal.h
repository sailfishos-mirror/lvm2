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

#ifndef BLKIO_INTERNAL_H
#define BLKIO_INTERNAL_H

/*
 * Every blkio source file includes this header first, so it pulls in the
 * library header and the public interface for them: no source file repeats
 * those includes.  blkio.h comes second so this one cannot rely on what it
 * pulls in, and anything the interface needs has to be declared there.
 */
#include "lib/misc/lib.h"
#include "lib/blkio/blkio.h"

#include "base/memory/container_of.h"
#include "lib/misc/lvm-sector.h"

#include <sys/types.h>

/*
 * A sector number or count, as opposed to a byte offset.  Only clear while
 * reading the code, since in C a typedef adds no checking; the public header
 * does not need it because its block size is a plain count.
 */
typedef uint64_t sector_t;

enum blkio_dir {
	BLKIO_DIR_READ,
	BLKIO_DIR_WRITE
};

typedef uint64_t block_address_t;

struct blkio_byte_range {
	uint64_t offset;
	size_t len;
};

static inline struct blkio_byte_range
blkio_sector_range_bytes(sector_t sb, sector_t se)
{
	struct blkio_byte_range range = {
		.offset = sb << SECTOR_SHIFT,
		.len = (size_t) ((se - sb) << SECTOR_SHIFT),
	};

	return range;
}

typedef void io_complete_fn(void *context, int io_error);

/*
 * One transfer: the direction, the device, the buffer and the byte range
 * it covers.  An engine issues it and the completion helpers report its
 * outcome, so the two always describe the same transfer.
 */
struct blkio_io {
	enum blkio_dir d;
	int fd;
	void *data;
	struct blkio_byte_range range;
};

/*
 * The transfer direction as it appears in a message.
 */
static inline const char *blkio_dir_name(enum blkio_dir d)
{
	return (d == BLKIO_DIR_READ) ? "read" : "write";
}

struct io_engine {
	/*
	 * Engine name used in messages.
	 */
	const char *name;
	/*
	 * Tear down the engine.  Returns true if every in-flight request was
	 * reaped, so the caller may free the buffers handed to issue().
	 * Returns false if outstanding I/O could not be drained (teardown was
	 * interrupted); those buffers must then be left allocated because the
	 * kernel may still be reading or writing them.
	 */
	bool (*destroy)(struct io_engine *ioe);
	/*
	 * Submit I/O.  Returns 0 on success, or a negative errno if the
	 * request could not be issued.
	 */
	int (*issue)(struct io_engine *ioe, const struct blkio_io *io,
		     void *context);
	bool (*wait)(struct io_engine *ioe, io_complete_fn fn);
	unsigned (*max_io)(struct io_engine *ioe);
};

/*
 * An engine is usable only when every method is set.  This must not call
 * any of the methods: it only tells whether they exist.
 */
static inline bool blkio_engine_usable(struct io_engine *ioe)
{
	return ioe && ioe->destroy && ioe->issue && ioe->wait &&
	       ioe->max_io;
}

/*
 * Default queue depth for the libaio engine.  This value is also returned
 * via blkio_max_prefetches() which tells the caller how many devices to
 * submit I/O for concurrently.  There will be an open file descriptor for
 * each, so keep it low enough to avoid reaching the default max open file
 * limit (1024) when scanning >1024 devices.
 */
#define BLKIO_MAX_IO 256

/*
 * Number of entries in one fd table allocation.  The table starts at this
 * size and grows by this many entries each time it is full.
 */
#define BLKIO_FD_TABLE_INC 1024

struct io_engine *create_async_io_engine(unsigned queue_depth);
struct io_engine *create_sync_io_engine(unsigned queue_depth);

/*
 * Create an I/O engine selected by hint.
 * hint: "auto", "libaio" (or "async"/"aio"), "sync", or NULL (defaults to
 * auto)
 */
struct io_engine *blkio_create_io_engine(const char *hint);

/*
 * After fork() the child must not touch kernel state owned by the parent's
 * engine; it may only release its private copy of the engine.
 */
bool blkio_engine_is_forked(pid_t creator_pid);

/*
 * Report the outcome of one completed transfer.  The return value is
 * suitable for io_complete_fn (0, negative errno, or -ENODATA).
 * io->data is the buffer the transfer used: a short read leaves the rest
 * of the block zero filled.
 */
int blkio_completion_status(const struct io_engine *ioe,
			    const struct blkio_io *io, int64_t result);

struct blkio_rw_result {
	size_t done;
	int errnum;
};

struct blkio_rw_result blkio_rw_loop(const struct blkio_io *io,
				     bool (*stop)(void *), void *stop_arg);

/*
 * Map a blkio_rw_loop() result to a value suitable for io_complete_fn.
 */
int blkio_completion_result(const struct io_engine *ioe,
			    const struct blkio_io *io,
			    const struct blkio_rw_result *rw);

/*
 * Create a cache on an already created engine.  Ownership of engine passes:
 * it is destroyed even when this fails, unless it is not usable (see
 * blkio_engine_usable()), in which case it has no destroy method and is left
 * untouched.  The public blkio_create() wraps this with hint-based engine
 * selection; it stays internal so tests can drive the cache with a mock
 * engine.
 */
struct blkio *blkio_create_engine(unsigned block_sectors,
				  unsigned nr_cache_blocks,
				  struct io_engine *engine);

/*----------------------------------------------------------------*/

/*
 * The raw block cache API.  blkio.h exposes only the byte range helpers,
 * which are written in terms of these.  They stay internal to keep the
 * interface small; a caller that really needs block access can be given a
 * public entry point later.
 */
/*
 * A cache block.  di, index and data are read outside blkio.c by the byte
 * range helpers; everything below them is cache state only blkio.c touches.
 */
struct blkio_block {
	int di;
	uint64_t index;
	void *data;

	struct blkio *bio;
	struct dm_list list;

	unsigned flags;
	unsigned ref_count;
	bool write_locked;
	int error;
	enum blkio_dir io_dir;
};

/*
 * Flags for blkio_get().  A client only ever passes 0, while the byte range
 * helpers in byterange.c use these to ask for a zero filled or a writable
 * block.
 */
enum blkio_get_flags {
	/*
	 * The block will be zeroed before blkio_get returns it.  This
	 * potentially avoids a read if the block is not already in the cache.
	 * GF_DIRTY is implicit.
	 */
	GF_ZERO = (1 << 0),

	/*
	 * Indicates the caller is intending to change the data in the block, a
	 * writeback will occur after the block is released.
	 */
	GF_DIRTY = (1 << 1)
};

unsigned blkio_block_sectors(struct blkio *bio);

/*
 * Submit a read of one block without waiting for it.  A later blkio_get()
 * of the same block finds the data, or waits for the read to complete.
 */
void blkio_prefetch(struct blkio *bio, int di, block_address_t index);

/*
 * Returns true on success.
 *
 * flags is 0 for a read-only reference, or a set of enum blkio_get_flags
 * asking for a zero filled or dirty block.  Those take an exclusive lock on
 * the block: a second dirty/zero get of a held block fails, as does a read
 * of a block that is currently write locked.
 */
bool blkio_get(struct blkio *bio, int di, block_address_t index,
	       unsigned flags, struct blkio_block **result);
void blkio_put(struct blkio_block *b);

/*
 * Removes a block from the cache.
 *
 * If the block is dirty it will be written back first.  If the writeback fails
 * false will be returned.
 *
 * If the block is currently held false will be returned.
 */
bool blkio_invalidate(struct blkio *bio, int di, block_address_t index);

/*----------------------------------------------------------------*/

/*
 * Intrusive free-list link.  A request embeds one as its first member, so a
 * link pointer is also a pointer to the request and one pool can thread
 * requests of any engine type.
 */
struct io_pool_link {
	struct io_pool_link *next;
};

/*
 * Bookkeeping shared by the engines that pre-allocate a request pool: the pid
 * the engine was created in and the head of the intrusive free list threaded
 * through the requests.  An engine embeds it and points at it from its own
 * create function, so the pool code needs no knowledge of the engine type.
 */
struct io_engine_pool {
	pid_t creator_pid;
	struct io_pool_link *free_list;
};

/*
 * Take one request from the pool, or NULL when it is exhausted.
 */
static inline struct io_pool_link *blkio_pool_pop(struct io_engine_pool *pool)
{
	struct io_pool_link *link = pool->free_list;

	if (!link)
		return NULL;

	pool->free_list = link->next;

	return link;
}

/*
 * Return a request to the pool.
 */
static inline void blkio_pool_push(struct io_engine_pool *pool,
				   struct io_pool_link *link)
{
	link->next = pool->free_list;
	pool->free_list = link;
}

/*
 * Layout of an engine with a pre-allocated request pool, so one allocator can
 * serve engines of different types.  request_size is the size of one request,
 * whose first member must be the free-list pointer; requests_offset and
 * pool_offset are the offsetof() of the flexible requests[] member and of the
 * embedded struct io_engine_pool.
 */
struct io_engine_layout {
	size_t engine_size;
	size_t requests_offset;
	size_t pool_offset;
	size_t request_size;
};

/*
 * Passing this as the queue depth asks the engine for its own default.
 */
#define BLKIO_ENGINE_DEFAULT 0

/*
 * The queue depth to use when the caller does not ask for one.
 */
static inline unsigned blkio_queue_depth(unsigned queue_depth)
{
	return queue_depth == BLKIO_ENGINE_DEFAULT ? BLKIO_MAX_IO : queue_depth;
}

/*
 * Allocate an engine and its nr_requests pre-allocated requests.  The vtable
 * is copied from proto, the requests are threaded onto the pool free list in
 * order and the pool records the creating pid for the fork checks.  Returns
 * the engine, whose first member is the struct io_engine from proto, or NULL
 * after reporting the failed allocation.
 */
void *blkio_alloc_request_pool(const struct io_engine *proto,
			       const struct io_engine_layout *layout,
			       unsigned nr_requests);

/*
 * Report a failed system call.  rc is the value the call returned, so a
 * negative errno is passed as is, just like the call site received it.
 */
#define blkio_log_sys_warn(ioe, call, rc) \
	log_warn("WARNING: %s io engine %s failed: %s.", (ioe)->name, call, strerror(-(rc)))

#endif
