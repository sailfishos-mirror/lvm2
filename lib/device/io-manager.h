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

#ifndef IO_MANAGER_H
#define IO_MANAGER_H

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
	DIR_READ = 1,
	DIR_WRITE = 2
};

typedef uint64_t block_address;
typedef uint64_t sector_t;

typedef void io_complete_fn(void *context, int io_error);

enum {
        EF_READ_ONLY = 1,
        EF_EXCL = 2
};

// The io engine must support io with any sector alignment.
// For instance aio will need to fall back to sync io if the
// io is not page aligned.
struct io_engine {
	void (*destroy)(struct io_engine *e);

	int (*open)(struct io_engine *e, const char *path, unsigned flags, bool o_direct);
	void (*close)(struct io_engine *e, int fd);

	unsigned (*max_io)(struct io_engine *e);
	bool (*issue)(struct io_engine *e, enum dir d, int fd,
		      sector_t sb, sector_t se, void *data, void *context);
	bool (*wait)(struct io_engine *e, io_complete_fn fn);

	// The path is there purely for logging.
	bool (*get_size)(struct io_engine *e, const char *path, int fd, sector_t *size);
	bool (*get_block_sizes)(struct io_engine *e, const char *path, int fd,
                                unsigned *physical, unsigned *logical);
};

struct io_engine *create_async_io_engine(void);
struct io_engine *create_sync_io_engine(void);

// Same as create_async_io_engine(), except writes are not acted upon.
// Used when running with --test.
struct io_engine *create_test_io_engine(void);

/*----------------------------------------------------------------*/

struct io_manager;
struct io_dev;
struct io_dev_internal;

struct block {
	/* clients may only access these two fields */
	uint64_t index;
	void *data;

	struct io_manager *iom;
	struct io_dev_internal *dev;
	struct dm_list list;

	unsigned flags;
	unsigned ref_count;
	int error;
	enum dir io_dir;
	unsigned io_count;

        // Bits mark which sectors of the block should be written.
        uint64_t dirty_bits;
};

/*
 * Ownership of engine passes.  Engine will be destroyed even if this fails.
 * 
 * 'max_cache_devs' limits the number of devices that are held open because we
 * are caching data from them.  If to many devices are used the least recently used
 * dev will be closed, and all its data invalidated.
 */
struct io_manager *io_manager_create(sector_t block_size, unsigned nr_cache_blocks,
			     	     unsigned max_cache_devs, struct io_engine *engine,
                                     bool use_o_direct);
void io_manager_destroy(struct io_manager *iom);

// IMPORTANT: It is up to the caller to normalise the device path.  io does
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
struct io_dev *io_get_dev(struct io_manager *iom, const char *path, unsigned flags);
void io_put_dev(struct io_dev *dev);

enum io_get_flags {
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

sector_t io_block_sectors(struct io_manager *iom);
unsigned io_nr_cache_blocks(struct io_manager *iom);
unsigned io_max_prefetches(struct io_manager *iom);
unsigned io_max_cache_devs(struct io_manager *iom);

/*
 * Use the prefetch method to take advantage of asynchronous IO.  For example,
 * if you wanted to read a block from many devices concurrently you'd do
 * something like this:
 *
 * dm_list_iterate_items (dev, &devices)
 * 	io_prefetch_block(cache, dev, block);
 *
 * dm_list_iterate_items (dev, &devices) {
 *	if (!io_get_block(cache, dev, block, &b))
 *		fail();
 *
 *	process_block(b);
 * }
 *
 * It's slightly sub optimal, since you may not run the gets in the order that
 * they complete.  But we're talking a very small difference, and it's worth it
 * to keep callbacks out of this interface.
 */
void io_prefetch_block(struct io_manager *iom, struct io_dev *dev, block_address index);

/*
 * Returns true on success.
 */
bool io_get_block(struct io_manager *iom, struct io_dev *dev, block_address index,
	          unsigned flags, struct block **result);

// The mask is used to specify which sectors should be written.
// 'mask' is ignored unless the get flags are GF_ZERO or GF_DIRTY.
bool io_get_block_mask(struct io_manager *iom, struct io_dev *dev, block_address index,
	               unsigned flags, uint64_t mask, struct block **result);

void io_put_block(struct block *b);

/*
 * flush() does not attempt to writeback locked blocks.  flush will fail
 * (return false), if any unlocked dirty data cannot be written back.
 */
bool io_flush(struct io_manager *iom);
bool io_flush_dev(struct io_manager *iom, struct io_dev *dev);

/*
 * Remove blocks from the cache.
 * 
 * If the block is dirty it will be written back first.  If the writeback fails
 * false will be returned.
 * 
 * If any of the blocks are currently held, false will be returned.
 */
bool io_invalidate_block(struct io_manager *iom, struct io_dev *dev, block_address index);
bool io_invalidate_dev(struct io_manager *iom, struct io_dev *dev);
bool io_invalidate_all(struct io_manager *iom);

bool io_dev_size(struct io_dev *dev, uint64_t *sectors);
bool io_dev_block_sizes(struct io_dev *dev, unsigned *physical, unsigned *block_size);

// For testing and debug only
void *io_get_dev_context(struct io_dev *dev);
int io_get_fd(void *dev_context);
bool io_is_well_formed(struct io_manager *iom);

//----------------------------------------------------------------

// The next four functions are utilities written in terms of the above
// api.  This is simpler to use than the block based api, and I would
// expect almost all clients to use this interface in spite of the extra
// memory copying involved.
 
// Prefetches the blocks neccessary to satisfy a byte range.
void io_prefetch_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len);

// Reads, writes and zeroes bytes.  Returns false if errors occur.
bool io_read_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len, void *data);
bool io_write_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len, void *data);
bool io_zero_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len);
bool io_set_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len, uint8_t val);

//----------------------------------------------------------------

#endif
