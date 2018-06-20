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

#ifndef BCACHE_H
#define BCACHE_H

#include "device_mapper/all.h"

#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------*/

// FIXME: move somewhere more sensible
#define container_of(v, t, head) \
    ((t *)((const char *)(v) - (const char *)&((t *) 0)->head))

/*----------------------------------------------------------------*/

enum dir {
	DIR_READ,
	DIR_WRITE
};

typedef uint64_t block_address;
typedef uint64_t sector_t;

typedef void io_complete_fn(void *context, int io_error);

enum {
        EF_READ_ONLY = 1,
        EF_EXCL = 2
};

struct io_engine {
	void (*destroy)(struct io_engine *e);

	int (*open)(struct io_engine *e, const char *path, unsigned flags);
	void (*close)(struct io_engine *e, int fd);

	unsigned (*max_io)(struct io_engine *e);
	bool (*issue)(struct io_engine *e, enum dir d, int fd,
		      sector_t sb, sector_t se, void *data, void *context);
	bool (*wait)(struct io_engine *e, io_complete_fn fn);
};

struct io_engine *create_async_io_engine(bool use_o_direct);
struct io_engine *create_sync_io_engine(bool use_o_direct);

/*----------------------------------------------------------------*/

struct bcache;
struct bcache_dev;
struct block {
	/* clients may only access these three fields */
	struct bcache_dev *dev;
	uint64_t index;
	void *data;

	struct bcache *cache;
	struct dm_list list;
	struct dm_list hash;

	unsigned flags;
	unsigned ref_count;
	int error;
	enum dir io_dir;
};

/*
 * Ownership of engine passes.  Engine will be destroyed even if this fails.
 */
struct bcache *bcache_create(sector_t block_size, unsigned nr_cache_blocks,
			     struct io_engine *engine);
void bcache_destroy(struct bcache *cache);

// IMPORTANT: It is up to the caller to normalise the device path.  bcache does
// not detect if two relative path refer to the same file, or if 2 device nodes
// refer to the same underlying dev.

// There may be more than one holder of a device at a time.  But since we cannot
// promote a dev from being opened non-exclusive to exclusive, there are some
// restrictions:
//
// - You may have concurrent non-exclusive holders.
// - You may have concurrent exclusive holders.
// - You may not have mixed holders.
// - If blocks are in the cache that were acquired by a non exclusive holder,
//   they will all be invalidated if a device is opened exclusively. 
struct bcache_dev *bcache_get_dev(struct bcache *cache, const char *path, unsigned flags);
void bcache_put_dev(struct bcache_dev *dev);

enum bcache_get_flags {
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

sector_t bcache_block_sectors(struct bcache *cache);
unsigned bcache_nr_cache_blocks(struct bcache *cache);
unsigned bcache_max_prefetches(struct bcache *cache);

/*
 * Use the prefetch method to take advantage of asynchronous IO.  For example,
 * if you wanted to read a block from many devices concurrently you'd do
 * something like this:
 *
 * dm_list_iterate_items (dev, &devices)
 * 	bcache_prefetch(cache, dev, block);
 *
 * dm_list_iterate_items (dev, &devices) {
 *	if (!bcache_get(cache, dev, block, &b))
 *		fail();
 *
 *	process_block(b);
 * }
 *
 * It's slightly sub optimal, since you may not run the gets in the order that
 * they complete.  But we're talking a very small difference, and it's worth it
 * to keep callbacks out of this interface.
 */
void bcache_prefetch(struct bcache *cache, struct bcache_dev *dev, block_address index);

/*
 * Returns true on success.
 */
bool bcache_get(struct bcache *cache, struct bcache_dev *dev, block_address index,
	        unsigned flags, struct block **result);
void bcache_put(struct block *b);

/*
 * flush() does not attempt to writeback locked blocks.  flush will fail
 * (return false), if any unlocked dirty data cannot be written back.
 */
bool bcache_flush(struct bcache *cache);
bool bcache_flush_dev(struct bcache *cache, struct bcache_dev *dev);

/*
 * Removes a block from the cache.
 * 
 * If the block is dirty it will be written back first.  If the writeback fails
 * false will be returned.
 * 
 * If the block is currently held false will be returned.
 */
bool bcache_invalidate(struct bcache *cache, struct bcache_dev *dev, block_address index);
bool bcache_invalidate_dev(struct bcache *cache, struct bcache_dev *dev);

// For debug only
bool bcache_is_well_formed(struct bcache *cache);

//----------------------------------------------------------------
// The next four functions are utilities written in terms of the above api.
 
// Prefetches the blocks neccessary to satisfy a byte range.
void bcache_prefetch_bytes(struct bcache *cache, struct bcache_dev *dev, uint64_t start, size_t len);

// Reads, writes and zeroes bytes.  Returns false if errors occur.
bool bcache_read_bytes(struct bcache *cache, struct bcache_dev *dev, uint64_t start, size_t len, void *data);
bool bcache_write_bytes(struct bcache *cache, struct bcache_dev *dev, uint64_t start, size_t len, void *data);
bool bcache_zero_bytes(struct bcache *cache, struct bcache_dev *dev, uint64_t start, size_t len);
bool bcache_set_bytes(struct bcache *cache, struct bcache_dev *dev, uint64_t start, size_t len, uint8_t val);

//----------------------------------------------------------------

#endif
