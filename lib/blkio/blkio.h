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

#ifndef BLKIO_H
#define BLKIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*----------------------------------------------------------------*/

struct blkio;

/*
 * Create a cache of block_sectors sized blocks, backed by an I/O engine
 * selected by hint: "auto", "libaio" (or "async"/"aio"), "sync", or NULL
 * for auto.  Auto-selection tries libaio, then sync.  An engine that is not
 * available falls through to the next candidate, and finally to sync.  NULL
 * means the cache could not be created and the failure has been reported.
 */
struct blkio *blkio_create(const char *hint, unsigned block_sectors,
			   unsigned nr_cache_blocks);
void blkio_destroy(struct blkio *bio);

/*
 * The number of I/Os the cache can have in flight at once.  Prefetching
 * more than this would make the cache discard the earlier results to reuse
 * their blocks, so submit in batches of at most this size.
 */
unsigned blkio_max_prefetches(struct blkio *bio);

/*
 * flush() does not attempt to writeback locked blocks.  flush will fail
 * (return false), if any unlocked dirty data cannot be written back.
 */
bool blkio_flush(struct blkio *bio);

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
 *
 * Returns true if all blocks for di were dropped, false if the cache
 * was left intact.
 */
bool blkio_abort_di(struct blkio *bio, int di);

/*----------------------------------------------------------------*/

/*
 * Prefetches the data needed to satisfy a byte range.
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
