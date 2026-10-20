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
#include <stdint.h>
#include <stdbool.h>

#include "lib/misc/lvm-sector.h"

enum dir {
	DIR_READ,
	DIR_WRITE
};

typedef uint64_t block_address;
typedef uint64_t sector_t;

typedef void io_complete_fn(void *context, int io_error);

struct io_engine {
	void (*destroy)(struct io_engine *ioe);
	bool (*issue)(struct io_engine *ioe, enum dir d, int di,
		      sector_t sb, sector_t se, void *data, void *context);
	bool (*wait)(struct io_engine *ioe, io_complete_fn fn);
	unsigned (*max_io)(struct io_engine *ioe);
};

struct io_engine *create_async_io_engine(void);
struct io_engine *create_sync_io_engine(void);

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
 *	dev->blkio_di = blkio_set_fd(dev->blkio_fd);
 *	if (dev->blkio_di == -1)
 *		fail();
 * }
 *
 * dm_list_iterate_items (dev, &devices)
 *	blkio_prefetch(bio, dev->blkio_di, block);
 *
 * dm_list_iterate_items (dev, &devices) {
 *	if (!blkio_get(bio, dev->blkio_di, block, &b))
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
 * not written.  If any blocks for di are held, then it will call
 * abort().
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

int blkio_set_fd(int fd); /* returns di, or -1 on failure */
void blkio_clear_fd(int di);
int blkio_change_fd(int di, int fd);

#endif
