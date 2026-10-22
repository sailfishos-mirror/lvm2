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

#include "base/memory/zalloc.h"
#include "lib/datastruct/radix-tree.h"
#include "lib/log/lvm-logging.h"
#include "lib/log/log.h"
#include "lib/misc/lvm-signal.h"
#include "lib/misc/lvm-wrappers.h"

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

#define FD_TABLE_INC 1024

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
#define WRITEBACK_BATCH_BLOCKS 16

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
	BF_CLEAN = (1 << 2),
};

struct blkio {
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
	size_t last_byte_sector_size;

	/*
	 * Lists that categorize the blocks.
	 */
	unsigned nr_locked;
	unsigned nr_free;
	unsigned nr_clean;
	unsigned nr_dirty;
	unsigned nr_io_pending;

	struct dm_list free;
	struct dm_list errored;
	struct dm_list dirty;
	struct dm_list clean;
	struct dm_list io_pending;

	struct radix_tree *rtree;

	int *fd_table;
	int fd_table_size;

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

static inline int _di_valid(struct blkio *bio, int di)
{
	if (di < 0 || di >= bio->fd_table_size) {
		log_error(INTERNAL_ERROR "blkio invalid di %d (fd table size %d).", di, bio->fd_table_size);
		return 0;
	}

	return 1;
}

//----------------------------------------------------------------

struct key_parts {
	uint32_t di;
	uint64_t b;
} __attribute__ ((packed));

union key {
	struct key_parts parts;
	uint8_t bytes[sizeof(struct key_parts)];
};

static struct block *_block_lookup(struct blkio *bio, int di, uint64_t i)
{
	union key k;
	union radix_value v;

	k.parts.di = di;
	k.parts.b = i;

	if (radix_tree_lookup(bio->rtree, k.bytes, sizeof(k.bytes), &v))
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

	return radix_tree_insert(b->bio->rtree, k.bytes, sizeof(k.bytes), v);
}

static void _block_remove(struct block *b)
{
        union key k;

        k.parts.di = b->di;
        k.parts.b = b->index;

	(void) radix_tree_remove(b->bio->rtree, k.bytes, sizeof(k.bytes));
}

//----------------------------------------------------------------

static bool _init_free_list(struct blkio *bio, unsigned count, unsigned pgsize)
{
	unsigned i;
	size_t block_size = bio->block_sectors << SECTOR_SHIFT;
	unsigned char *data =
		(unsigned char *) _alloc_aligned(count * block_size, pgsize);

	/* Allocate the data for each block.  We page align the data. */
	if (!data)
		return false;

	bio->raw_blocks = malloc(count * sizeof(*bio->raw_blocks));
	if (!bio->raw_blocks) {
		free(data);
		return false;
	}

	bio->raw_data = data;

	for (i = 0; i < count; i++) {
		struct block *b = bio->raw_blocks + i;
		b->bio = bio;
		b->data = data + (block_size * i);
		dm_list_add(&bio->free, &b->list);
	}

	bio->nr_free = count;

	return true;
}

static void _exit_free_list(struct blkio *bio)
{
	free(bio->raw_data);
	free(bio->raw_blocks);
}

static struct block *_alloc_block(struct blkio *bio)
{
	struct block *b;

	if (dm_list_empty(&bio->free))
		return NULL;

	b = dm_list_struct_base(_list_pop(&bio->free), struct block, list);
	bio->nr_free--;
	return b;
}

static void _free_block(struct block *b)
{
	dm_list_add(&b->bio->free, &b->list);
	b->bio->nr_free++;
}

/*----------------------------------------------------------------
 * Clean/dirty list management.
 * Always use these methods to ensure nr_dirty and nr_clean are correct.
 *--------------------------------------------------------------*/

static void _unlink_block(struct block *b)
{
	if (_test_flags(b, BF_DIRTY))
		b->bio->nr_dirty--;

	if (_test_flags(b, BF_CLEAN)) {
		b->bio->nr_clean--;
		_clear_flags(b, BF_CLEAN);
	}

	dm_list_del(&b->list);
}

static void _link_block(struct block *b)
{
	struct blkio *bio = b->bio;

	if (_test_flags(b, BF_DIRTY)) {
		dm_list_add(&bio->dirty, &b->list);
		bio->nr_dirty++;
	} else {
		dm_list_add(&bio->clean, &b->list);
		bio->nr_clean++;
		_set_flags(b, BF_CLEAN);
	}
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
	struct blkio *bio = b->bio;

	b->error = err;
	_clear_flags(b, BF_IO_PENDING);
	bio->nr_io_pending--;

	/*
	 * b is on the io_pending list, so we don't want to use unlink_block.
	 * Which would incorrectly adjust nr_dirty.
	 */
	dm_list_del(&b->list);

	if (b->error) {
		dm_list_add(&bio->errored, &b->list);

	} else {
		if (_test_flags(b, BF_DIRTY)) {
			bio->nr_dirty--;
			_clear_flags(b, BF_DIRTY);
		}
		_link_block(b);
	}
}

struct clamped_write_range {
	bool valid;
	sector_t end;
};

/* Clamp write range if last_byte limit is set for this device. */
static struct clamped_write_range _clamp_write_range(struct blkio *bio,
						     int di, enum dir d,
						     sector_t sb, sector_t se)
{
	struct blkio_byte_range bytes = blkio_sector_range_bytes(sb, se);
	sector_t limit_nbytes, extra_nbytes, orig_nbytes;
	struct clamped_write_range range = {
		.valid = true,
		.end = se,
	};

	if (d != DIR_WRITE)
		return range;

	if (!bio->last_byte_offset || di != bio->last_byte_di)
		return range;

	/* If blkio block goes past where lvm wants to write, then clamp it. */
	if (bytes.offset > bio->last_byte_offset) {
		log_error("Limit write at %llu len %llu beyond last byte %llu.",
			  (unsigned long long)bytes.offset,
			  (unsigned long long)bytes.len,
			  (unsigned long long)bio->last_byte_offset);
		range.valid = false;
		return range;
	}

	if (bytes.offset + bytes.len <= bio->last_byte_offset)
		return range;

	/*
	 * If the blkio block offset+len goes beyond where lvm is
	 * intending to write, then reduce the len being written
	 * (which is the blkio block size) so we don't write past
	 * the limit set by lvm.  If after applying the limit, the
	 * resulting size is not a multiple of the sector size (512
	 * or 4096) then extend the reduced size to be a multiple of
	 * the sector size (we don't want to write partial sectors.)
	 */

	/* Need to clamp */
	limit_nbytes = bio->last_byte_offset - bytes.offset;

	extra_nbytes = 0;
	if (limit_nbytes % bio->last_byte_sector_size) {
		extra_nbytes = bio->last_byte_sector_size -
			       (limit_nbytes % bio->last_byte_sector_size);

		/*
		 * adding extra_nbytes to the reduced nbytes (limit_nbytes)
		 * should make the final write size a multiple of the
		 * sector size.  This should never result in a final size
		 * larger than the blkio block size (as long as the blkio
		 * block size is a multiple of the sector size).
		 */
		if (limit_nbytes + extra_nbytes > bytes.len) {
			log_warn("WARNING: Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu.",
				 (unsigned long long)bytes.offset,
				 (unsigned long long)bytes.len,
				 (unsigned long long)limit_nbytes,
				 (unsigned long long)extra_nbytes,
				 (unsigned long long)bio->last_byte_sector_size);
			extra_nbytes = 0;
		}
	}

	orig_nbytes = bytes.len;

	if (extra_nbytes) {
		log_debug_devs("Limit write at %llu len %llu to len %llu rounded to %llu.",
			       (unsigned long long)bytes.offset,
			       (unsigned long long)bytes.len,
			       (unsigned long long)limit_nbytes,
			       (unsigned long long)(limit_nbytes + extra_nbytes));
		bytes.len = limit_nbytes + extra_nbytes;
	} else {
		log_debug_devs("Limit write at %llu len %llu to len %llu.",
			       (unsigned long long)bytes.offset,
			       (unsigned long long)bytes.len,
			       (unsigned long long)limit_nbytes);
		bytes.len = limit_nbytes;
	}

	/*
	 * This shouldn't happen, the reduced+extended
	 * bytes.len value should never be larger than the
	 * blkio block size.
	 */
	if (bytes.len > orig_nbytes) {
		log_error("Invalid adjusted write at %llu len %llu adjusted %llu limit %llu extra %llu sector_size %llu.",
			  (unsigned long long)bytes.offset,
			  (unsigned long long)orig_nbytes,
			  (unsigned long long)bytes.len,
			  (unsigned long long)limit_nbytes,
			  (unsigned long long)extra_nbytes,
			  (unsigned long long)bio->last_byte_sector_size);
		range.valid = false;
		return range;
	}

	range.end = sb + (bytes.len >> SECTOR_SHIFT);
	return range;
}

static bool _wait_io(struct blkio *bio)
{
	return bio->engine->wait(bio->engine, _complete_io);
}

/*
 * |b->list| should be valid (either pointing to itself, on one of the other
 * lists.
 */
static void _issue_low_level(struct block *b, enum dir d)
{
	struct blkio *bio = b->bio;
	sector_t sb = b->index * bio->block_sectors;
	sector_t se = sb + bio->block_sectors;
	struct clamped_write_range range;
	int fd;
	int r;

	if (_test_flags(b, BF_IO_PENDING))
		return;

	/* Drain completions before counting this request as pending. */
	while (bio->nr_io_pending >= bio->max_io)
		if (!_wait_io(bio))
			break;

	/* Validation failures complete this request just like issue failures. */
	b->io_dir = d;
	_set_flags(b, BF_IO_PENDING);
	bio->nr_io_pending++;
	dm_list_move(&bio->io_pending, &b->list);

	if (!_di_valid(bio, b->di)) {
		_complete_io(b, -EIO);
		return;
	}

	fd = bio->fd_table[b->di];
	if (fd < 0) {
		log_error("blkio no fd for device index %d.", b->di);
		_complete_io(b, -EIO);
		return;
	}

	/* Clamp write range if needed */
	range = _clamp_write_range(bio, b->di, d, sb, se);
	if (!range.valid) {
		_complete_io(b, -EIO);
		return;
	}
	se = range.end;

	r = bio->engine->issue(bio->engine, d, fd, sb, se, b->data, b);
	if (r < 0) {
		_complete_io(b, r);
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

static bool _wait_all(struct blkio *bio)
{
	while (!dm_list_empty(&bio->io_pending))
		if (!_wait_io(bio))
			return false;
	return true;
}

static bool _wait_specific(struct block *b)
{
	while (_test_flags(b, BF_IO_PENDING))
		if (!_wait_io(b->bio))
			return false;
	return true;
}

static unsigned _writeback(struct blkio *bio, unsigned count)
{
	unsigned actual = 0;
	struct block *b, *tmp;

	dm_list_iterate_items_gen_safe (b, tmp, &bio->dirty, list) {
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

static struct block *_find_unused_clean_block(struct blkio *bio)
{
	struct block *b;

	dm_list_iterate_items (b, &bio->clean) {
		if (!b->ref_count) {
			_unlink_block(b);
			_block_remove(b);
			return b;
		}
	}

	return NULL;
}

static struct block *_new_block(struct blkio *bio, int di, block_address i, bool can_wait)
{
	struct block *b;

	b = _alloc_block(bio);
	while (!b) {
		b = _find_unused_clean_block(bio);
		if (!b) {
			if (can_wait) {
				if (dm_list_empty(&bio->io_pending))
					_writeback(bio, WRITEBACK_BATCH_BLOCKS);
				if (!_wait_all(bio))
					return NULL;
				if (dm_list_size(&bio->errored) >= bio->max_io) {
					log_debug("blkio no new blocks for di %d index %llu with >%u errors.",
						  di, (unsigned long long) i, bio->max_io);
					return NULL;
				}
			} else {
				log_debug("blkio no new blocks for di %d index %llu.",
					  di, (unsigned long long) i);
				return NULL;
			}
		}
	}

	dm_list_init(&b->list);
	b->flags = 0;
	b->di = di;
	b->index = i;
	b->ref_count = 0;
	b->write_locked = false;
	b->error = 0;

	if (!_block_insert(b)) {
		log_error("blkio unable to insert block in radix tree (OOM).");
		_free_block(b);
		return NULL;
	}

	return b;
}

/*----------------------------------------------------------------
 * Block reference counting
 *--------------------------------------------------------------*/
static void _zero_block(struct block *b)
{
	b->bio->write_zeroes++;
	memset(b->data, 0, b->bio->block_sectors << SECTOR_SHIFT);
	_set_flags(b, BF_DIRTY);
}

static void _hit(struct block *b, unsigned flags)
{
	struct blkio *bio = b->bio;

	if (flags & (GF_ZERO | GF_DIRTY))
		bio->write_hits++;
	else
		bio->read_hits++;
}

static void _miss(struct blkio *bio, unsigned flags)
{
	if (flags & (GF_ZERO | GF_DIRTY))
		bio->write_misses++;
	else
		bio->read_misses++;
}

static struct block *_lookup_or_read_block(struct blkio *bio,
				  	   int di, block_address i,
					   unsigned flags)
{
	struct block *b = _block_lookup(bio, di, i);

	if (b) {
		if (b->write_locked && !(flags & (GF_DIRTY | GF_ZERO))) {
			log_error(INTERNAL_ERROR "blkio concurrent read lock attempted on write locked block.");
			return NULL;
		}

		if (b->ref_count && (flags & (GF_DIRTY | GF_ZERO))) {
			log_error(INTERNAL_ERROR "blkio concurrent write lock attempted on held block.");
			return NULL;
		}

		if (_test_flags(b, BF_IO_PENDING)) {
			_miss(bio, flags);
			if (!_wait_specific(b))
				return NULL;

		} else
			_hit(b, flags);

		_unlink_block(b);

		if (flags & GF_ZERO)
			_zero_block(b);

	} else {
		_miss(bio, flags);

		b = _new_block(bio, di, i, true);
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

static unsigned _nr_available_blocks(struct blkio *bio)
{
	return bio->nr_free + bio->nr_clean;
}

static void _preemptive_writeback(struct blkio *bio)
{
	unsigned nr_available = _nr_available_blocks(bio);

	if (nr_available < (WRITEBACK_LOW_THRESHOLD_PERCENT * bio->nr_cache_blocks / 100))
		_writeback(bio, (WRITEBACK_HIGH_THRESHOLD_PERCENT * bio->nr_cache_blocks / 100) - nr_available);
}

/*----------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------*/
struct blkio *blkio_create(sector_t block_sectors, unsigned nr_cache_blocks,
			     struct io_engine *engine)
{
	static int _pagesize = 0;
	struct blkio *bio;
	unsigned max_io = engine->max_io(engine);
	int i;

	if (_pagesize <= 0) {
		_pagesize = lvm_getpagesize();
		if (_pagesize <= 0) {
			log_warn("WARNING: invalid system page size.");
			goto bad_engine;
		}
	}

	if (!nr_cache_blocks) {
		log_warn("WARNING: blkio must have at least one cache block.");
		goto bad_engine;
	}

	if (!block_sectors) {
		log_warn("WARNING: blkio must have a non zero block size.");
		goto bad_engine;
	}

	if (block_sectors & ((_pagesize >> SECTOR_SHIFT) - 1)) {
		log_warn("WARNING: blkio block size must be a multiple of page size.");
		goto bad_engine;
	}

	bio = zalloc(sizeof(*bio));
	if (!bio)
		goto bad_engine;

	bio->block_sectors = block_sectors;
	bio->nr_cache_blocks = nr_cache_blocks;
	bio->max_io = nr_cache_blocks < max_io ? nr_cache_blocks : max_io;
	bio->engine = engine;

	dm_list_init(&bio->free);
	dm_list_init(&bio->errored);
	dm_list_init(&bio->dirty);
	dm_list_init(&bio->clean);
	dm_list_init(&bio->io_pending);

        bio->rtree = radix_tree_create(NULL, NULL);
	if (!bio->rtree)
		goto bad;

	if (!_init_free_list(bio, nr_cache_blocks, _pagesize))
		goto bad;

	bio->fd_table_size = FD_TABLE_INC;

	if (!(bio->fd_table = malloc(sizeof(int) * bio->fd_table_size)))
		goto bad;

	for (i = 0; i < bio->fd_table_size; i++)
		bio->fd_table[i] = -1;

	return bio;

bad:
	_exit_free_list(bio);
	if (bio->rtree)
		radix_tree_destroy(bio->rtree);
	free(bio->fd_table);
	free(bio);

bad_engine:
	engine->destroy(engine);
	return NULL;
}

void blkio_destroy(struct blkio *bio)
{
	if (bio->nr_locked)
		log_warn("WARNING: some blocks are still locked.");

	if (!blkio_flush(bio))
		stack;

	/*
	 * A failed flush can leave I/O in flight.  Destroy the engine
	 * first so each engine drains outstanding I/O while the block
	 * data is still mapped, then free the buffers.  An engine that
	 * could not finish draining (e.g. teardown was interrupted) asks
	 * us to keep the block data, since the kernel may still use it.
	 */
	if (bio->engine->destroy(bio->engine))
		_exit_free_list(bio);
	else
		log_warn("WARNING: blkio leaked %llu blocks with I/O still in flight after interrupted destroy.",
			 (unsigned long long) bio->nr_cache_blocks);

	radix_tree_destroy(bio->rtree);
	free(bio->fd_table);
	free(bio);
}

sector_t blkio_block_sectors(struct blkio *bio)
{
	return bio->block_sectors;
}

unsigned blkio_nr_cache_blocks(struct blkio *bio)
{
	return bio->nr_cache_blocks;
}

unsigned blkio_max_prefetches(struct blkio *bio)
{
	return bio->max_io;
}

void blkio_prefetch(struct blkio *bio, int di, block_address index)
{
	struct block *b;

	if (!_di_valid(bio, di))
		return;

	b = _block_lookup(bio, di, index);

	if (!b) {
		if (bio->nr_io_pending < bio->max_io) {
			b = _new_block(bio, di, index, false);
			if (b) {
				bio->prefetches++;
				_issue_read(b);
			}
		}
	}
}

//----------------------------------------------------------------

static void _recycle_block(struct blkio *bio, struct block *b)
{
	_unlink_block(b);
	_block_remove(b);
	_free_block(b);
}

bool blkio_get(struct blkio *bio, int di, block_address index,
	        unsigned flags, struct block **result)
{
	struct block *b;

	*result = NULL;

	if (!_di_valid(bio, di))
		return false;

	b = _lookup_or_read_block(bio, di, index, flags);
	if (b) {
		if (b->error) {
			if (b->io_dir == DIR_READ) {
				// Now we know the read failed we can just forget
				// about this block, since there's no dirty data to
				// be written back.
				_recycle_block(bio, b);
			}
			return false;
		}

		if (!b->ref_count)
			bio->nr_locked++;
		b->ref_count++;

		if (flags & (GF_DIRTY | GF_ZERO))
			b->write_locked = true;

		*result = b;
		return true;
	}

	log_error("blkio failed to get block %llu di %d.", (unsigned long long) index, di);
	return false;
}

//----------------------------------------------------------------

static void _put_ref(struct block *b)
{
	if (!b->ref_count) {
		log_warn("WARNING: ref count on blkio block already zero.");
		return;
	}

	b->ref_count--;
	if (!b->ref_count) {
		b->write_locked = false;
		b->bio->nr_locked--;
	}
}

void blkio_put(struct block *b)
{
	_put_ref(b);

	if (_test_flags(b, BF_DIRTY))
		_preemptive_writeback(b->bio);
}

//----------------------------------------------------------------

bool blkio_flush(struct blkio *bio)
{
	// Only dirty data is on the errored list, since bad read blocks get
	// recycled straight away.  So we put these back on the dirty list, and
	// try and rewrite everything.
	dm_list_splice(&bio->dirty, &bio->errored);

	/*
	 * _writeback() uses safe iteration and skips locked blocks
	 * (ref_count > 0).  _issue_write() moves written blocks from
	 * dirty to io_pending via dm_list_move() in _issue_low_level().
	 * BF_IO_PENDING cannot occur here - _issue_low_level() moves
	 * the block off dirty when setting that flag, and _complete_io()
	 * clears it before moving to errored/clean.
	 */
	_writeback(bio, bio->nr_cache_blocks);

	if (!_wait_all(bio))
		return false;

	return dm_list_empty(&bio->errored);
}

//----------------------------------------------------------------
/*
 * You can safely call this with a NULL block.
 */
static bool _invalidate_block(struct blkio *bio, struct block *b)
{
	if (!b)
		return true;

	if (_test_flags(b, BF_IO_PENDING) && !_wait_specific(b))
		return false;

	if (b->ref_count) {
		log_warn("WARNING: blkio invalidate: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		return false;
	}

	if (_test_flags(b, BF_DIRTY)) {
		_issue_write(b);

		if (!_wait_specific(b))
			return false;

		if (b->error)
			return false;
	}

	_recycle_block(bio, b);

	return true;
}

bool blkio_invalidate(struct blkio *bio, int di, block_address index)
{
	if (!_di_valid(bio, di))
		return false;

	return _invalidate_block(bio, _block_lookup(bio, di, index));
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
		log_warn("WARNING: blkio invalidate: block (%d, %llu) still dirty.",
			 b->di, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	if (b->ref_count) {
		log_warn("WARNING: blkio invalidate: block (%d, %llu) still held.",
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

bool blkio_invalidate_di(struct blkio *bio, int di)
{
	union key k;
	struct invalidate_iterator it;

	k.parts.di = di;

	it.it.visit = _writeback_v;
	radix_tree_iterate(bio->rtree, k.bytes, sizeof(k.parts.di), &it.it);

	if (!_wait_all(bio))
		return false;

	it.success = true;
	it.it.visit = _invalidate_v;
	radix_tree_iterate(bio->rtree, k.bytes, sizeof(k.parts.di), &it.it);

	if (it.success)
		(void) radix_tree_remove_prefix(bio->rtree, k.bytes, sizeof(k.parts.di));

	return it.success;
}

//----------------------------------------------------------------

static bool _abort_v(struct radix_tree_iterator *it,
                     const void *kb, size_t keylen, union radix_value v)
{
	struct block *b = v.ptr;

	if (b->ref_count) {
		log_fatal("blkio destroy: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	// We can't remove the block from the radix tree yet because
	// we're in the middle of an iteration.
	return true;
}

void blkio_abort_di(struct blkio *bio, int di)
{
	union key k;
	struct radix_tree_iterator it;

	k.parts.di = di;

	it.visit = _abort_v;
	radix_tree_iterate(bio->rtree, k.bytes, sizeof(k.parts.di), &it);
	(void) radix_tree_remove_prefix(bio->rtree, k.bytes, sizeof(k.parts.di));
}

//----------------------------------------------------------------

void blkio_set_last_byte(struct blkio *bio, int di, uint64_t offset, size_t sector_size)
{
	bio->last_byte_di = di;
	bio->last_byte_offset = offset;
	bio->last_byte_sector_size = sector_size ? sector_size : SECTOR_SIZE;
}

void blkio_unset_last_byte(struct blkio *bio, int di)
{
	if (bio->last_byte_di == di) {
		bio->last_byte_di = 0;
		bio->last_byte_offset = 0;
		bio->last_byte_sector_size = 0;
	}
}

int blkio_set_fd(struct blkio *bio, int fd)
{
	int *new_table = NULL;
	int new_size = 0;
	int i;

 retry:
	for (i = 0; i < bio->fd_table_size; i++) {
		if (bio->fd_table[i] == -1) {
			bio->fd_table[i] = fd;
			return i;
		}
	}

	/* already tried once, shouldn't happen */
	if (new_size)
		return -1;

	new_size = bio->fd_table_size + FD_TABLE_INC;

	new_table = realloc(bio->fd_table, sizeof(int) * new_size);
	if (!new_table) {
		log_error("Cannot extend blkio fd table.");
		return -1;
	}

	for (i = bio->fd_table_size; i < new_size; i++)
		new_table[i] = -1;

	bio->fd_table = new_table;
	bio->fd_table_size = new_size;

	goto retry;
}

/*
 * Should we check for unflushed or in-progress io on an fd
 * prior to doing clear_fd or change_fd?  (To catch mistakes;
 * the caller should be smart enough to not do that.)
 */

void blkio_clear_fd(struct blkio *bio, int di)
{
	if (!_di_valid(bio, di))
		return;
	bio->fd_table[di] = -1;
}

int blkio_change_fd(struct blkio *bio, int di, int fd)
{
	if (!_di_valid(bio, di))
		return 0;
	bio->fd_table[di] = fd;
	return 1;
}
