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

typedef void io_complete_fn(void *context, int io_error);

struct io_engine {
	void (*destroy)(struct io_engine *ioe);
	bool (*issue)(struct io_engine *ioe, enum blkio_dir d, int di,
		      sector_t sb, sector_t se, void *data, void *context);
	bool (*wait)(struct io_engine *ioe, io_complete_fn fn);
	unsigned (*max_io)(struct io_engine *ioe);
};

struct io_engine *create_async_io_engine(void);
struct io_engine *create_sync_io_engine(void);

/*
 * Create an I/O engine selected by hint: "auto", "libaio" (or "async"/"aio"),
 * "sync", or NULL for auto.  NULL means no engine could be created at all
 * and the failure has been reported.
 */
struct io_engine *blkio_create_io_engine(const char *hint);

/*
 * Create a cache on an already created engine.  Ownership of engine passes:
 * it is destroyed even when this fails.  The public blkio_create() wraps this
 * with hint-based engine selection; it stays internal so tests can drive the
 * cache with a mock engine.
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

#endif
