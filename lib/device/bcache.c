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

#include "lib/device/bcache.h"

#include "lib/datastruct/radix-tree.h"
#include "lib/log/lvm-logging.h"
#include "lib/log/log.h"
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/user.h>

#define SECTOR_SHIFT 9L

#define FD_TABLE_INC 1024
static int _fd_table_size = 0;
static int *_fd_table = NULL;

// Assumes the list is not empty.
static inline struct dm_list *_list_pop(struct dm_list *head)
{
	struct dm_list *l;

	l = head->n;
	dm_list_del(l);
	return l;
}

//----------------------------------------------------------------

#define MIN_BLOCKS 16
#define WRITEBACK_LOW_THRESHOLD_PERCENT 33
#define WRITEBACK_HIGH_THRESHOLD_PERCENT 66

//----------------------------------------------------------------

static void *_alloc_aligned(size_t len, size_t alignment)
{
	void *result = NULL;
	int r = posix_memalign(&result, alignment, len);
	if (r)
		return NULL;

	return result;
}

//----------------------------------------------------------------

static bool _test_flags(struct block *b, unsigned bits)
{
	return (b->flags & bits) != 0;
}

static void _set_flags(struct block *b, unsigned bits)
{
	b->flags |= bits;
}

static void _clear_flags(struct block *b, unsigned bits)
{
	b->flags &= ~bits;
}

//----------------------------------------------------------------

enum block_flags {
	BF_IO_PENDING = (1 << 0),
	BF_DIRTY = (1 << 1),
};

struct bcache {
	sector_t block_sectors;
	uint64_t nr_data_blocks;
	uint64_t nr_cache_blocks;
	unsigned max_io;

	struct io_engine *engine;

	void *raw_data;
	struct block *raw_blocks;

	/*
	 * Write clamping - limit writes to not exceed a certain byte offset
	 */
	int last_byte_di;
	uint64_t last_byte_offset;
	int last_byte_sector_size;

	/*
	 * Lists that categorize the blocks.
	 */
	unsigned nr_locked;
	unsigned nr_dirty;
	unsigned nr_io_pending;

	struct dm_list free;
	struct dm_list errored;
	struct dm_list dirty;
	struct dm_list clean;
	struct dm_list io_pending;

	struct radix_tree *rtree;

	/*
	 * Statistics
	 */
	unsigned read_hits;
	unsigned read_misses;
	unsigned write_zeroes;
	unsigned write_hits;
	unsigned write_misses;
	unsigned prefetches;
};

//----------------------------------------------------------------

struct key_parts {
	uint32_t di;
	uint64_t b;
} __attribute__ ((packed));

union key {
	struct key_parts parts;
        uint8_t bytes[12];
};

static struct block *_block_lookup(struct bcache *cache, int di, uint64_t i)
{
	union key k;
	union radix_value v;

	k.parts.di = di;
	k.parts.b = i;

	if (radix_tree_lookup(cache->rtree, k.bytes, sizeof(k.bytes), &v))
		return v.ptr;

	return NULL;
}

static bool _block_insert(struct block *b)
{
        union key k;
        union radix_value v;

        k.parts.di = b->di;
        k.parts.b = b->index;
        v.ptr = b;

	return radix_tree_insert(b->cache->rtree, k.bytes, sizeof(k.bytes), v);
}

static void _block_remove(struct block *b)
{
        union key k;

        k.parts.di = b->di;
        k.parts.b = b->index;

	(void) radix_tree_remove(b->cache->rtree, k.bytes, sizeof(k.bytes));
}

//----------------------------------------------------------------

static bool _init_free_list(struct bcache *cache, unsigned count, unsigned pgsize)
{
	unsigned i;
	size_t block_size = cache->block_sectors << SECTOR_SHIFT;
	unsigned char *data =
		(unsigned char *) _alloc_aligned(count * block_size, pgsize);

	/* Allocate the data for each block.  We page align the data. */
	if (!data)
		return false;

	cache->raw_blocks = malloc(count * sizeof(*cache->raw_blocks));
	if (!cache->raw_blocks) {
		free(data);
		return false;
	}

	cache->raw_data = data;

	for (i = 0; i < count; i++) {
		struct block *b = cache->raw_blocks + i;
		b->cache = cache;
		b->data = data + (block_size * i);
		dm_list_add(&cache->free, &b->list);
	}

	return true;
}

static void _exit_free_list(struct bcache *cache)
{
	free(cache->raw_data);
	free(cache->raw_blocks);
}

static struct block *_alloc_block(struct bcache *cache)
{
	if (dm_list_empty(&cache->free))
		return NULL;

	return dm_list_struct_base(_list_pop(&cache->free), struct block, list);
}

static void _free_block(struct block *b)
{
	dm_list_add(&b->cache->free, &b->list);
}

/*----------------------------------------------------------------
 * Clean/dirty list management.
 * Always use these methods to ensure nr_dirty_ is correct.
 *--------------------------------------------------------------*/

static void _unlink_block(struct block *b)
{
	if (_test_flags(b, BF_DIRTY))
		b->cache->nr_dirty--;

	dm_list_del(&b->list);
}

static void _link_block(struct block *b)
{
	struct bcache *cache = b->cache;

	if (_test_flags(b, BF_DIRTY)) {
		dm_list_add(&cache->dirty, &b->list);
		cache->nr_dirty++;
	} else
		dm_list_add(&cache->clean, &b->list);
}

static void _relink(struct block *b)
{
	_unlink_block(b);
	_link_block(b);
}

/*----------------------------------------------------------------
 * Low level IO handling
 *
 * We cannot have two concurrent writes on the same block.
 * eg, background writeback, put with dirty, flush?
 *
 * To avoid this we introduce some restrictions:
 *
 * i)  A held block can never be written back.
 * ii) You cannot get a block until writeback has completed.
 *
 *--------------------------------------------------------------*/

static void _complete_io(void *context, int err)
{
	struct block *b = context;
	struct bcache *cache = b->cache;

	b->error = err;
	_clear_flags(b, BF_IO_PENDING);
	cache->nr_io_pending--;

	/*
	 * b is on the io_pending list, so we don't want to use unlink_block.
	 * Which would incorrectly adjust nr_dirty.
	 */
	dm_list_del(&b->list);

	if (b->error) {
		dm_list_add(&cache->errored, &b->list);

	} else {
		_clear_flags(b, BF_DIRTY);
		_link_block(b);
	}
}

/*
 * Clamp write range if last_byte limit is set for this device.
 * Returns false if write is beyond limit, true otherwise.
 * May adjust se to reflect clamped range.
 */
static bool _clamp_write_range(struct bcache *cache, int di, enum dir d,
				sector_t sb, sector_t *se)
{
	sector_t offset, nbytes, limit_nbytes, extra_nbytes, orig_nbytes;

	if (d != DIR_WRITE)
		return true;

	if (!cache->last_byte_offset || di != cache->last_byte_di)
		return true;

	offset = sb << SECTOR_SHIFT;
	nbytes = (*se - sb) << SECTOR_SHIFT;

	if (offset > cache->last_byte_offset) {
		log_error("Limit write at %llu len %llu beyond last byte %llu.",
			  (unsigned long long)offset,
			  (unsigned long long)nbytes,
			  (unsigned long long)cache->last_byte_offset);
		return false;
	}

	if (offset + nbytes <= cache->last_byte_offset)
		return true;

	/* Need to clamp */
	limit_nbytes = cache->last_byte_offset - offset;

	extra_nbytes = 0;
	if (limit_nbytes % cache->last_byte_sector_size) {
		extra_nbytes = cache->last_byte_sector_size -
			       (limit_nbytes % cache->last_byte_sector_size);

		if (limit_nbytes + extra_nbytes > nbytes) {
			log_warn("WARNING: Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu.",
				 (unsigned long long)offset,
				 (unsigned long long)nbytes,
				 (unsigned long long)limit_nbytes,
				 (unsigned long long)extra_nbytes,
				 (unsigned long long)cache->last_byte_sector_size);
			extra_nbytes = 0;
		}
	}

	orig_nbytes = nbytes;

	if (extra_nbytes) {
		log_debug_devs("Limit write at %llu len %llu to len %llu rounded to %llu.",
			       (unsigned long long)offset,
			       (unsigned long long)nbytes,
			       (unsigned long long)limit_nbytes,
			       (unsigned long long)(limit_nbytes + extra_nbytes));
		nbytes = limit_nbytes + extra_nbytes;
	} else {
		log_debug_devs("Limit write at %llu len %llu to len %llu.",
			       (unsigned long long)offset,
			       (unsigned long long)nbytes,
			       (unsigned long long)limit_nbytes);
		nbytes = limit_nbytes;
	}

	if (nbytes > orig_nbytes) {
		log_error("Invalid adjusted write at %llu len %llu adjusted %llu limit %llu extra %llu sector_size %llu.",
			  (unsigned long long)offset,
			  (unsigned long long)orig_nbytes,
			  (unsigned long long)nbytes,
			  (unsigned long long)limit_nbytes,
			  (unsigned long long)extra_nbytes,
			  (unsigned long long)cache->last_byte_sector_size);
		return false;
	}

	*se = sb + (nbytes >> SECTOR_SHIFT);
	return true;
}

static bool _wait_io(struct bcache *cache)
{
	return cache->engine->wait(cache->engine, _complete_io);
}

/*
 * |b->list| should be valid (either pointing to itself, on one of the other
 * lists.
 */
static void _issue_low_level(struct block *b, enum dir d)
{
	struct bcache *cache = b->cache;
	sector_t sb = b->index * cache->block_sectors;
	sector_t se = sb + cache->block_sectors;
	int fd;

	if (_test_flags(b, BF_IO_PENDING))
		return;

	if (b->di >= _fd_table_size) {
		log_error("bcache device index %d out of range.", b->di);
		_complete_io(b, -EIO);
		return;
	}

	fd = _fd_table[b->di];
	if (fd < 0) {
		log_error("bcache no fd for device index %d.", b->di);
		_complete_io(b, -EIO);
		return;
	}

	/* Clamp write range if needed */
	if (!_clamp_write_range(cache, b->di, d, sb, &se)) {
		_complete_io(b, -EIO);
		return;
	}

	/* Drain completions if the engine queue is full */
	while (cache->nr_io_pending >= cache->max_io)
		if (!_wait_io(cache))
			break;

	b->io_dir = d;
	_set_flags(b, BF_IO_PENDING);
	cache->nr_io_pending++;

	dm_list_move(&cache->io_pending, &b->list);

	if (!cache->engine->issue(cache->engine, d, fd, sb, se, b->data, b)) {
		/* FIXME: if io_submit() set an errno, return that instead of EIO? */
		_complete_io(b, -EIO);
		return;
	}
}

static inline void _issue_read(struct block *b)
{
	_issue_low_level(b, DIR_READ);
}

static inline void _issue_write(struct block *b)
{
	_issue_low_level(b, DIR_WRITE);
}

/*----------------------------------------------------------------
 * High level IO handling
 *--------------------------------------------------------------*/

static bool _wait_all(struct bcache *cache)
{
	while (!dm_list_empty(&cache->io_pending))
		if (!_wait_io(cache))
			return false;
	return true;
}

static bool _wait_specific(struct block *b)
{
	while (_test_flags(b, BF_IO_PENDING))
		if (!_wait_io(b->cache))
			return false;
	return true;
}

static unsigned _writeback(struct bcache *cache, unsigned count)
{
	unsigned actual = 0;
	struct block *b, *tmp;

	dm_list_iterate_items_gen_safe (b, tmp, &cache->dirty, list) {
		if (actual == count)
			break;

		// We can't writeback anything that's still in use.
		if (!b->ref_count) {
			_issue_write(b);
			actual++;
		}
	}

	return actual;
}

/*----------------------------------------------------------------
 * High level allocation
 *--------------------------------------------------------------*/

static struct block *_find_unused_clean_block(struct bcache *cache)
{
	struct block *b;

	dm_list_iterate_items (b, &cache->clean) {
		if (!b->ref_count) {
			_unlink_block(b);
			_block_remove(b);
			return b;
		}
	}

	return NULL;
}

static struct block *_new_block(struct bcache *cache, int di, block_address i, bool can_wait)
{
	struct block *b;

	b = _alloc_block(cache);
	while (!b) {
		b = _find_unused_clean_block(cache);
		if (!b) {
			if (can_wait) {
				if (dm_list_empty(&cache->io_pending))
					_writeback(cache, 16);  // FIXME: magic number
				if (!_wait_all(cache))
					return NULL;
				if (dm_list_size(&cache->errored) >= cache->max_io) {
					log_debug("bcache no new blocks for di %d index %u with >%u errors.",
						  di, (uint32_t) i, cache->max_io);
					return NULL;
				}
			} else {
				log_debug("bcache no new blocks for di %d index %u",
					  di, (uint32_t) i);
				return NULL;
			}
		}
	}

	if (b) {
		dm_list_init(&b->list);
		b->flags = 0;
		b->di = di;
		b->index = i;
		b->ref_count = 0;
		b->error = 0;

		if (!_block_insert(b)) {
        		log_error("bcache unable to insert block in radix tree (OOM?)");
			_free_block(b);
			return NULL;
		}
	}

	return b;
}

/*----------------------------------------------------------------
 * Block reference counting
 *--------------------------------------------------------------*/
static void _zero_block(struct block *b)
{
	b->cache->write_zeroes++;
	memset(b->data, 0, b->cache->block_sectors << SECTOR_SHIFT);
	_set_flags(b, BF_DIRTY);
}

static void _hit(struct block *b, unsigned flags)
{
	struct bcache *cache = b->cache;

	if (flags & (GF_ZERO | GF_DIRTY))
		cache->write_hits++;
	else
		cache->read_hits++;

	_relink(b);
}

static void _miss(struct bcache *cache, unsigned flags)
{
	if (flags & (GF_ZERO | GF_DIRTY))
		cache->write_misses++;
	else
		cache->read_misses++;
}

static struct block *_lookup_or_read_block(struct bcache *cache,
				  	   int di, block_address i,
					   unsigned flags)
{
	struct block *b = _block_lookup(cache, di, i);

	if (b) {
		// FIXME: this is insufficient.  We need to also catch a read
		// lock of a write locked block.  Ref count needs to distinguish.
		if (b->ref_count && (flags & (GF_DIRTY | GF_ZERO))) {
			log_warn("concurrent write lock attempted");
			return NULL;
		}

		if (_test_flags(b, BF_IO_PENDING)) {
			_miss(cache, flags);
			if (!_wait_specific(b))
				return NULL;

		} else
			_hit(b, flags);

		_unlink_block(b);

		if (flags & GF_ZERO)
			_zero_block(b);

	} else {
		_miss(cache, flags);

		b = _new_block(cache, di, i, true);
		if (b) {
			if (flags & GF_ZERO)
				_zero_block(b);

			else {
				_issue_read(b);
				if (!_wait_specific(b)) {
					_unlink_block(b);
					return NULL;
				}

				// we know the block is clean and unerrored.
				_unlink_block(b);
			}
		}
	}

	if (b) {
		if (flags & (GF_DIRTY | GF_ZERO))
			_set_flags(b, BF_DIRTY);

		_link_block(b);
		return b;
	}

	return NULL;
}

static void _preemptive_writeback(struct bcache *cache)
{
	// FIXME: this ignores those blocks that are in the error state.  Track
	// nr_clean instead?
	unsigned nr_available = cache->nr_cache_blocks - (cache->nr_dirty - cache->nr_io_pending);
	if (nr_available < (WRITEBACK_LOW_THRESHOLD_PERCENT * cache->nr_cache_blocks / 100))
		_writeback(cache, (WRITEBACK_HIGH_THRESHOLD_PERCENT * cache->nr_cache_blocks / 100) - nr_available);

}

/*----------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------*/
struct bcache *bcache_create(sector_t block_sectors, unsigned nr_cache_blocks,
			     struct io_engine *engine)
{
	static long _pagesize = 0;
	struct bcache *cache;
	unsigned max_io = engine->max_io(engine);
	int i;

	if ((_pagesize <= 0) && ((_pagesize = sysconf(_SC_PAGESIZE)) < 0)) {
		log_warn("WARNING: _SC_PAGESIZE returns negative value.");
		return NULL;
	}

	if (!nr_cache_blocks) {
		log_warn("bcache must have at least one cache block");
		return NULL;
	}

	if (!block_sectors) {
		log_warn("bcache must have a non zero block size");
		return NULL;
	}

	if (block_sectors & ((_pagesize >> SECTOR_SHIFT) - 1)) {
		log_warn("bcache block size must be a multiple of page size");
		return NULL;
	}

	cache = malloc(sizeof(*cache));
	if (!cache)
		return NULL;

	cache->block_sectors = block_sectors;
	cache->nr_cache_blocks = nr_cache_blocks;
	cache->max_io = nr_cache_blocks < max_io ? nr_cache_blocks : max_io;
	cache->engine = engine;
	cache->nr_locked = 0;
	cache->nr_dirty = 0;
	cache->nr_io_pending = 0;

	cache->last_byte_di = 0;
	cache->last_byte_offset = 0;
	cache->last_byte_sector_size = 0;

	dm_list_init(&cache->free);
	dm_list_init(&cache->errored);
	dm_list_init(&cache->dirty);
	dm_list_init(&cache->clean);
	dm_list_init(&cache->io_pending);

        cache->rtree = radix_tree_create(NULL, NULL);
	if (!cache->rtree) {
		cache->engine->destroy(cache->engine);
		free(cache);
		return NULL;
	}

	cache->read_hits = 0;
	cache->read_misses = 0;
	cache->write_zeroes = 0;
	cache->write_hits = 0;
	cache->write_misses = 0;
	cache->prefetches = 0;

	if (!_init_free_list(cache, nr_cache_blocks, _pagesize)) {
		cache->engine->destroy(cache->engine);
		radix_tree_destroy(cache->rtree);
		free(cache);
		return NULL;
	}

	_fd_table_size = FD_TABLE_INC;

	if (!(_fd_table = malloc(sizeof(int) * _fd_table_size))) {
		cache->engine->destroy(cache->engine);
		radix_tree_destroy(cache->rtree);
		free(cache);
		return NULL;
	}

	for (i = 0; i < _fd_table_size; i++)
		_fd_table[i] = -1;

	return cache;
}

void bcache_destroy(struct bcache *cache)
{
	if (cache->nr_locked)
		log_warn("some blocks are still locked");

	if (!bcache_flush(cache))
		stack;
	_exit_free_list(cache);
	radix_tree_destroy(cache->rtree);
	cache->engine->destroy(cache->engine);
	free(cache);
	free(_fd_table);
	_fd_table = NULL;
	_fd_table_size = 0;
}

sector_t bcache_block_sectors(struct bcache *cache)
{
	return cache->block_sectors;
}

unsigned bcache_nr_cache_blocks(struct bcache *cache)
{
	return cache->nr_cache_blocks;
}

unsigned bcache_max_prefetches(struct bcache *cache)
{
	return cache->max_io;
}

void bcache_prefetch(struct bcache *cache, int di, block_address i)
{
	struct block *b = _block_lookup(cache, di, i);

	if (!b) {
		if (cache->nr_io_pending < cache->max_io) {
			b = _new_block(cache, di, i, false);
			if (b) {
				cache->prefetches++;
				_issue_read(b);
			}
		}
	}
}

//----------------------------------------------------------------

static void _recycle_block(struct bcache *cache, struct block *b)
{
	_unlink_block(b);
	_block_remove(b);
	_free_block(b);
}

bool bcache_get(struct bcache *cache, int di, block_address i,
	        unsigned flags, struct block **result)
{
	struct block *b;

	if (di >= _fd_table_size)
		goto bad;

	b = _lookup_or_read_block(cache, di, i, flags);
	if (b) {
		if (b->error) {
			if (b->io_dir == DIR_READ) {
				// Now we know the read failed we can just forget
				// about this block, since there's no dirty data to
				// be written back.
				_recycle_block(cache, b);
			}
			return false;
		}

		if (!b->ref_count)
			cache->nr_locked++;
		b->ref_count++;

		*result = b;
		return true;
	}
bad:
	*result = NULL;

	log_error("bcache failed to get block %u di %d", (uint32_t) i, di);
	return false;
}

//----------------------------------------------------------------

static void _put_ref(struct block *b)
{
	if (!b->ref_count) {
		log_warn("ref count on bcache block already zero");
		return;
	}

	b->ref_count--;
	if (!b->ref_count)
		b->cache->nr_locked--;
}

void bcache_put(struct block *b)
{
	_put_ref(b);

	if (_test_flags(b, BF_DIRTY))
		_preemptive_writeback(b->cache);
}

//----------------------------------------------------------------

bool bcache_flush(struct bcache *cache)
{
	// Only dirty data is on the errored list, since bad read blocks get
	// recycled straight away.  So we put these back on the dirty list, and
	// try and rewrite everything.
	dm_list_splice(&cache->dirty, &cache->errored);

	/*
	 * _writeback() uses safe iteration and skips locked blocks
	 * (ref_count > 0).  _issue_write() moves written blocks from
	 * dirty to io_pending via dm_list_move() in _issue_low_level().
	 * BF_IO_PENDING cannot occur here - _issue_low_level() moves
	 * the block off dirty when setting that flag, and _complete_io()
	 * clears it before moving to errored/clean.
	 */
	_writeback(cache, cache->nr_cache_blocks);

	if (!_wait_all(cache))
		return false;

	return dm_list_empty(&cache->errored);
}

//----------------------------------------------------------------
/*
 * You can safely call this with a NULL block.
 */
static bool _invalidate_block(struct bcache *cache, struct block *b)
{
	if (!b)
		return true;

	if (_test_flags(b, BF_IO_PENDING))
		_wait_specific(b);

	if (b->ref_count) {
		log_warn("bcache_invalidate: block (%d, %llu) still held",
			 b->di, (unsigned long long) b->index);
		return false;
	}

	if (_test_flags(b, BF_DIRTY)) {
		_issue_write(b);
		_wait_specific(b);

		if (b->error)
        		return false;
	}

	_recycle_block(cache, b);

	return true;
}

bool bcache_invalidate(struct bcache *cache, int di, block_address i)
{
	return _invalidate_block(cache, _block_lookup(cache, di, i));
}

//----------------------------------------------------------------

struct invalidate_iterator {
	bool success;
	struct radix_tree_iterator it;
};

static bool _writeback_v(struct radix_tree_iterator *it,
                         const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;

	if (_test_flags(b, BF_DIRTY))
		_issue_write(b);

	return true;
}

static bool _invalidate_v(struct radix_tree_iterator *it,
                          const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;
	struct invalidate_iterator *iit = container_of(it, struct invalidate_iterator, it);

	if (b->error || _test_flags(b, BF_DIRTY)) {
		log_warn("WARNING: bcache_invalidate: block (%d, %llu) still dirty.",
			 b->di, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	if (b->ref_count) {
		log_warn("WARNING: bcache_invalidate: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	// We can't remove the block from the radix tree yet because
	// we're in the middle of an iteration.
	return true;
}

bool bcache_invalidate_di(struct bcache *cache, int di)
{
	union key k;
	struct invalidate_iterator it;

	k.parts.di = di;

	it.it.visit = _writeback_v;
	radix_tree_iterate(cache->rtree, k.bytes, sizeof(k.parts.di), &it.it);

	if (!_wait_all(cache))
		return false;

	it.success = true;
	it.it.visit = _invalidate_v;
	radix_tree_iterate(cache->rtree, k.bytes, sizeof(k.parts.di), &it.it);

	if (it.success)
		(void) radix_tree_remove_prefix(cache->rtree, k.bytes, sizeof(k.parts.di));

	return it.success;
}

//----------------------------------------------------------------

static bool _abort_v(struct radix_tree_iterator *it,
                     const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;

	if (b->ref_count) {
		log_fatal("bcache_abort: block (%d, %llu) still held",
			 b->di, (unsigned long long) b->index);
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	// We can't remove the block from the radix tree yet because
	// we're in the middle of an iteration.
	return true;
}

void bcache_abort_di(struct bcache *cache, int di)
{
	union key k;
	struct radix_tree_iterator it;

	k.parts.di = di;

	it.visit = _abort_v;
	radix_tree_iterate(cache->rtree, k.bytes, sizeof(k.parts.di), &it);
	(void) radix_tree_remove_prefix(cache->rtree, k.bytes, sizeof(k.parts.di));
}

//----------------------------------------------------------------

void bcache_set_last_byte(struct bcache *cache, int di, uint64_t offset, int sector_size)
{
	cache->last_byte_di = di;
	cache->last_byte_offset = offset;
	cache->last_byte_sector_size = sector_size ? sector_size : 512;
}

void bcache_unset_last_byte(struct bcache *cache, int di)
{
	if (cache->last_byte_di == di) {
		cache->last_byte_di = 0;
		cache->last_byte_offset = 0;
		cache->last_byte_sector_size = 0;
	}
}

int bcache_set_fd(int fd)
{
	int *new_table = NULL;
	int new_size = 0;
	int i;

 retry:
	for (i = 0; i < _fd_table_size; i++) {
		if (_fd_table[i] == -1) {
			_fd_table[i] = fd;
			return i;
		}
	}

	/* already tried once, shouldn't happen */
	if (new_size)
		return -1;

	new_size = _fd_table_size + FD_TABLE_INC;

	new_table = realloc(_fd_table, sizeof(int) * new_size);
	if (!new_table) {
		log_error("Cannot extend bcache fd table");
		return -1;
	}

	for (i = _fd_table_size; i < new_size; i++)
		new_table[i] = -1;

	_fd_table = new_table;
	_fd_table_size = new_size;

	goto retry;
}

/*
 * Should we check for unflushed or in-progress io on an fd
 * prior to doing clear_fd or change_fd?  (To catch mistakes;
 * the caller should be smart enough to not do that.)
 */

void bcache_clear_fd(int di)
{
	if (di >= _fd_table_size)
		return;
	_fd_table[di] = -1;
}

int bcache_change_fd(int di, int fd)
{
	if (di >= _fd_table_size)
		return 0;
	if (di < 0) {
		log_error(INTERNAL_ERROR "Cannot change not opened DI with FD:%d", fd);
		return 0;
	}
	_fd_table[di] = fd;
	return 1;
}
