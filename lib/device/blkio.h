/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

#ifndef BLKIO_H
#define BLKIO_H

#include "libdm/libdevmapper.h"
#include "base/memory/container_of.h"

#include <linux/fs.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

#include "lib/misc/lvm-sector.h"

enum dir {
	DIR_READ,
	DIR_WRITE
};

typedef uint64_t block_address;
typedef uint64_t sector_t;

struct blkio_byte_range {
	uint64_t offset;
	uint64_t len;
};

static inline struct blkio_byte_range
blkio_sector_range_bytes(sector_t sb, sector_t se)
{
	struct blkio_byte_range range;

	range.offset = sb << SECTOR_SHIFT;
	range.len = (se - sb) << SECTOR_SHIFT;
	return range;
}

typedef void io_complete_fn(void *context, int io_error);

struct io_engine {
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
	int (*issue)(struct io_engine *ioe, enum dir d, int fd,
		     sector_t sb, sector_t se, void *data, void *context);
	bool (*wait)(struct io_engine *ioe, io_complete_fn fn);
	unsigned (*max_io)(struct io_engine *ioe);
};

/*
 * Default queue depth for async and uring engines.  This value is also
 * returned via blkio_max_prefetches() which tells the caller how many
 * devices to submit I/O for concurrently.  There will be an open file
 * descriptor for each, so keep it low enough to avoid reaching the
 * default max open file limit (1024) when scanning >1024 devices.
 */
#define BLKIO_MAX_IO 256

struct io_engine *create_async_io_engine(unsigned queue_depth);
struct io_engine *create_sync_io_engine(void);
struct io_engine *create_threads_io_engine(unsigned n_threads);
struct io_engine *create_uring_io_engine(unsigned queue_depth);

/*
 * Create an I/O engine selected by hint.
 * hint: "auto", "threads", "iouring" (or "uring"), "libaio" (or "async"/"aio"), "sync", or NULL (defaults to auto)
 */
struct io_engine *blkio_create_io_engine(const char *hint);

/*
 * After fork() the child must not touch kernel state owned by the parent's
 * engine; it may only release its private copy of the engine.
 */
bool blkio_engine_is_forked(pid_t creator_pid);

#define log_blkio_sys_warn(engine, call, errnum) \
	log_warn("WARNING: %s io engine %s failed: %s.", engine, call, strerror(errnum))

/*
 * Shared completion reporting for blkio I/O engines.  Return values are
 * suitable for io_complete_fn (0, negative errno, or -ENODATA).
 */
int blkio_completion_errno(const char *engine, enum dir d,
			   uint64_t offset, uint64_t remaining,
			   int errnum);
int blkio_completion_status(const char *engine, enum dir d,
			    uint64_t offset, uint64_t requested,
			    int64_t result);

struct blkio_rw_result {
	size_t done;
	int errnum;
};

struct blkio_rw_result blkio_rw_loop(int fd, enum dir d, void *data,
				     size_t len, off_t offset,
				     bool (*stop)(void *), void *stop_arg);

/*----------------------------------------------------------------*/

struct blkio;
struct block {
	/* Public: clients may access only these fields. */
	int di;
	uint64_t index;
	void *data;

	/* Private cache state. */
	struct blkio *bio;
	struct dm_list list;

	unsigned flags;
	unsigned ref_count;
	bool write_locked;
	int error;
	enum dir io_dir;
};

/*
 * Ownership of engine passes.  Engine will be destroyed even if this fails.
 */
struct blkio *blkio_create(sector_t block_sectors,
			   unsigned nr_cache_blocks,
			   struct io_engine *engine);
void blkio_destroy(struct blkio *bio);

enum blkio_get_flags {
	/*
	 * The block will be zeroed before get_block returns it.  This
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

sector_t blkio_block_sectors(struct blkio *bio);
unsigned blkio_nr_cache_blocks(struct blkio *bio);
unsigned blkio_max_prefetches(struct blkio *bio);

/*
 * Use the prefetch method to take advantage of asynchronous IO.  For example,
 * if you wanted to read a block from many devices concurrently you'd do
 * something like this:
 *
 * dm_list_iterate_items (dev, &devices) {
 *	dev->blkio_di = blkio_set_fd(bio, dev->blkio_fd);
 *	if (dev->blkio_di == -1)
 *		fail();
 * }
 *
 * dm_list_iterate_items (dev, &devices)
 *	blkio_prefetch(bio, dev->blkio_di, block);
 *
 * dm_list_iterate_items (dev, &devices) {
 *	if (!blkio_get(bio, dev->blkio_di, block, 0, &b))
 *		fail();
 *
 *	process_block(b);
 * }
 *
 * It's slightly sub optimal, since you may not run the gets in the order that
 * they complete.  But we're talking a very small difference, and it's worth it
 * to keep callbacks out of this interface.
 */
void blkio_prefetch(struct blkio *bio, int di, block_address index);

/*
 * Returns true on success.
 *
 * GF_DIRTY and GF_ZERO take an exclusive lock on the block.  A second
 * dirty/zero get of a held block fails, as does a read of a block that is
 * currently write locked.
 */
bool blkio_get(struct blkio *bio, int di, block_address index,
	       unsigned flags, struct block **result);
void blkio_put(struct block *b);

/*
 * flush() does not attempt to writeback locked blocks.  flush will fail
 * (return false), if any unlocked dirty data cannot be written back.
 */
bool blkio_flush(struct blkio *bio);

/*
 * Removes a block from the cache.
 *
 * If the block is dirty it will be written back first.  If the writeback fails
 * false will be returned.
 *
 * If the block is currently held false will be returned.
 */
bool blkio_invalidate(struct blkio *bio, int di, block_address index);

/*
 * Invalidates all blocks on the given descriptor.  Call this before closing
 * the descriptor to make sure everything is written back.
 */
bool blkio_invalidate_di(struct blkio *bio, int di);

/*
 * Call this function if flush, or invalidate fail and you do not
 * wish to retry the writes.  This will throw away any dirty data
 * not written.  If any blocks for di are held, the cache is left
 * intact and a warning is logged.
 */
void blkio_abort_di(struct blkio *bio, int di);

/*----------------------------------------------------------------*/

/*
 * The next six functions are utilities written in terms of the above api.
 *
 * Prefetches the blocks necessary to satisfy a byte range.
 */
void blkio_prefetch_bytes(struct blkio *bio, int di, uint64_t start, size_t len);

/* Reads, writes, zeroes, sets and invalidates byte ranges.  Returns false on error. */
bool blkio_read_bytes(struct blkio *bio, int di, uint64_t start, size_t len, void *data);
bool blkio_write_bytes(struct blkio *bio, int di, uint64_t start, size_t len, const void *data);
bool blkio_zero_bytes(struct blkio *bio, int di, uint64_t start, size_t len);
bool blkio_set_bytes(struct blkio *bio, int di, uint64_t start, size_t len, uint8_t val);
bool blkio_invalidate_bytes(struct blkio *bio, int di, uint64_t start, size_t len);

void blkio_set_last_byte(struct blkio *bio, int di, uint64_t offset, size_t sector_size);
void blkio_unset_last_byte(struct blkio *bio, int di);

/*----------------------------------------------------------------*/

int blkio_set_fd(struct blkio *bio, int fd); /* returns di, or -1 on failure */
void blkio_clear_fd(struct blkio *bio, int di);
int blkio_change_fd(struct blkio *bio, int di, int fd);

#endif
