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

#include "lib/datastruct/radix-tree.h"

#include <errno.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define FD_FREE (-1)
#define FD_RESERVED (-2)

/*----------------------------------------------------------------*/

#define MIN_BLOCKS 16
#define WRITEBACK_LOW_THRESHOLD_PERCENT 33
#define WRITEBACK_HIGH_THRESHOLD_PERCENT 66
#define WRITEBACK_BATCH_BLOCKS 16

/*----------------------------------------------------------------*/

static void *_alloc_aligned(size_t len, size_t alignment)
{
	void *result = NULL;
	int r = posix_memalign(&result, alignment, len);
	if (r)
		return NULL;

	return result;
}

/*----------------------------------------------------------------*/

static bool _test_flags(struct blkio_block *b, unsigned bits)
{
	return (b->flags & bits) != 0;
}

static void _set_flags(struct blkio_block *b, unsigned bits)
{
	b->flags |= bits;
}

static void _clear_flags(struct blkio_block *b, unsigned bits)
{
	b->flags &= ~bits;
}

/*----------------------------------------------------------------*/

enum block_flags {
	BF_IO_PENDING = (1 << 0),
	BF_DIRTY = (1 << 1),
	BF_CLEAN = (1 << 2),
};

struct blkio {
	unsigned block_sectors;
	uint64_t nr_data_blocks;
	uint64_t nr_cache_blocks;
	unsigned max_io;

	struct io_engine *engine;

	/*
	 * PID of the process that created this cache.  After fork() the
	 * child must not use the engine or kernel state owned by the
	 * parent, and must not flush I/O inherited from the parent.
	 */
	pid_t creator_pid;

	void *raw_data;
	struct blkio_block *raw_blocks;

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

static inline bool _di_valid(struct blkio *bio, int di)
{
	if (di < 0 || di >= bio->fd_table_size) {
		log_error(INTERNAL_ERROR "blkio invalid di %d (fd table size %d).", di, bio->fd_table_size);
		return false;
	}

	return true;
}

/*
 * A reserved di was closed while its cached blocks could not be dropped.
 * The slot must not be handed to another device until those blocks are gone.
 */
static inline bool _di_reserved(struct blkio *bio, int di)
{
	return bio->fd_table[di] == FD_RESERVED;
}

/*----------------------------------------------------------------*/

struct key_parts {
	uint32_t di;
	uint64_t b;
} __attribute__ ((packed));

union key {
	struct key_parts parts;
	uint8_t bytes[sizeof(struct key_parts)];
};

/*
 * The radix tree key of one block: the device index followed by the block
 * index.  Setting the block index to zero gives the prefix that selects
 * every block of a device.
 */
static void _block_key(int di, block_address_t i, union key *k)
{
	k->parts.di = di;
	k->parts.b = i;
}

static struct blkio_block *_block_lookup(struct blkio *bio, int di, uint64_t i)
{
	union key k;
	union radix_value v;

	_block_key(di, i, &k);

	if (radix_tree_lookup(bio->rtree, k.bytes, sizeof(k.bytes), &v))
		return v.ptr;

	return NULL;
}

static bool _block_insert(struct blkio_block *b)
{
	union key k;
	union radix_value v;

	_block_key(b->di, b->index, &k);
	v.ptr = b;

	return radix_tree_insert(b->bio->rtree, k.bytes, sizeof(k.bytes), v);
}

static void _block_remove(struct blkio_block *b)
{
	union key k;

	_block_key(b->di, b->index, &k);

	(void) radix_tree_remove(b->bio->rtree, k.bytes, sizeof(k.bytes));
}

/*----------------------------------------------------------------*/

static bool _init_free_list(struct blkio *bio, unsigned count, unsigned pgsize)
{
	unsigned i;
	size_t block_bytes = (size_t) bio->block_sectors << SECTOR_SHIFT;
	unsigned char *data =
		(unsigned char *) _alloc_aligned(count * block_bytes, pgsize);

	/* Allocate the data for each block.  We page align the data. */
	if (!data)
		return_false;

	bio->raw_blocks = malloc(count * sizeof(*bio->raw_blocks));
	if (!bio->raw_blocks) {
		free(data);
		return_false;
	}

	bio->raw_data = data;

	for (i = 0; i < count; i++) {
		struct blkio_block *b = bio->raw_blocks + i;
		b->bio = bio;
		b->data = data + (block_bytes * i);
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

static struct blkio_block *_alloc_block(struct blkio *bio)
{
	struct blkio_block *b;
	struct dm_list *l;

	/* Remove the first element from the free list and return it. */
	if (!(l = dm_list_first(&bio->free)))
		return NULL;

	b = dm_list_item(l, struct blkio_block);
	dm_list_del(&b->list);
	bio->nr_free--;

	return b;
}

static void _free_block(struct blkio_block *b)
{
	dm_list_add(&b->bio->free, &b->list);
	b->bio->nr_free++;
}

/*----------------------------------------------------------------
 * Clean/dirty list management.
 * Always use these methods to ensure nr_dirty and nr_clean are correct.
 *--------------------------------------------------------------*/

static void _unlink_block(struct blkio_block *b)
{
	if (_test_flags(b, BF_DIRTY))
		b->bio->nr_dirty--;

	if (_test_flags(b, BF_CLEAN)) {
		b->bio->nr_clean--;
		_clear_flags(b, BF_CLEAN);
	}

	dm_list_del(&b->list);
}

static void _link_block(struct blkio_block *b)
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
	struct blkio_block *b = context;
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

/*
 * Round a byte count up to a whole sector so a clamped write does not end
 * in the middle of a sector.  A count that is already sector aligned is
 * returned unchanged.
 */
static uint64_t _round_up_to_sector(uint64_t nr_bytes, sector_t sector_size)
{
	uint64_t remainder = nr_bytes % sector_size;

	return remainder ? nr_bytes + (sector_size - remainder) : nr_bytes;
}

struct clamped_write_range {
	bool valid;
	sector_t end;
};

/* Clamp write range if last_byte limit is set for this device. */
static struct clamped_write_range _clamp_write_range(struct blkio *bio, int di,
						     const struct blkio_io *io)
{
	struct blkio_byte_range bytes = io->range;
	size_t limit_nbytes, rounded_nbytes, block_nbytes;
	struct clamped_write_range range = {
		.valid = true,
		.end = (io->range.offset + io->range.len) >> SECTOR_SHIFT,
	};

	if (io->d != BLKIO_DIR_WRITE)
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
	 * the limit set by lvm.  A partial sector is never written,
	 * so the reduced len is rounded up to a whole sector when
	 * that does not take it past the end of the block, which is
	 * possible because the block size is a multiple of it.
	 */
	limit_nbytes = bio->last_byte_offset - bytes.offset;
	block_nbytes = bytes.len;

	rounded_nbytes = _round_up_to_sector(limit_nbytes,
					     bio->last_byte_sector_size);
	if (rounded_nbytes > block_nbytes) {
		log_warn("WARNING: Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu.",
			 (unsigned long long)bytes.offset,
			 (unsigned long long)block_nbytes,
			 (unsigned long long)limit_nbytes,
			 (unsigned long long)(rounded_nbytes - limit_nbytes),
			 (unsigned long long)bio->last_byte_sector_size);
		rounded_nbytes = limit_nbytes;
	}

	log_debug_devs("Limit write at %llu len %llu to len %llu.",
		       (unsigned long long)bytes.offset,
		       (unsigned long long)block_nbytes,
		       (unsigned long long)rounded_nbytes);

	bytes.len = rounded_nbytes;
	range.end = (bytes.offset + bytes.len) >> SECTOR_SHIFT;
	return range;
}

static bool _wait_io(struct blkio *bio)
{
	return bio->engine->wait(bio->engine, _complete_io);
}

/*
 * Drain completions before counting a new request as pending.  A break
 * here means the wait was interrupted; the caller notices when the
 * pending count then exceeds max_io.
 */
static void _drain_before_issue(struct blkio *bio)
{
	while (bio->nr_io_pending >= bio->max_io)
		if (!_wait_io(bio))
			break;
}

/*
 * The engine queue is full.  Withdraw this request from the pending
 * count (but keep it linked so _complete_io can retire it), reap a
 * completion to make room, and ask the caller to retry.  Without the
 * withdrawal the wait could block forever on a request the kernel never
 * received.
 *
 * Returns false when there is nothing to wait for or the wait was
 * interrupted.  The request is then back in the pending count and has
 * been completed with the engine's error.
 */
static bool _make_room_for_retry(struct blkio_block *b, int err)
{
	struct blkio *bio = b->bio;

	_clear_flags(b, BF_IO_PENDING);
	bio->nr_io_pending--;

	if (!bio->nr_io_pending || !_wait_io(bio)) {
		/* Put it back before retiring it as an error. */
		bio->nr_io_pending++;
		_set_flags(b, BF_IO_PENDING);
		_complete_io(b, err);
		return false;
	}

	return true;
}

/*
 * |b->list| should be valid (either pointing to itself, on one of the other
 * lists.
 */
static void _issue_low_level(struct blkio_block *b, enum blkio_dir d)
{
	struct blkio *bio = b->bio;
	struct clamped_write_range range;
	struct blkio_io io;
	int r;

	if (_test_flags(b, BF_IO_PENDING))
		return;

	_drain_before_issue(bio);

retry:
	/* Validation failures complete this request just like issue failures. */
	b->io_dir = d;
	_set_flags(b, BF_IO_PENDING);
	bio->nr_io_pending++;
	dm_list_move(&bio->io_pending, &b->list);

	if (!_di_valid(bio, b->di)) {
		_complete_io(b, -EIO);
		return;
	}

	io.d = d;
	io.fd = bio->fd_table[b->di];
	io.data = b->data;
	io.range = blkio_sector_range_bytes(b->index * bio->block_sectors,
					    (b->index + 1) * bio->block_sectors);

	if (io.fd < 0) {
		log_error("blkio no fd for device index %d.", b->di);
		_complete_io(b, -EIO);
		return;
	}

	range = _clamp_write_range(bio, b->di, &io);
	if (!range.valid) {
		_complete_io(b, -EIO);
		return;
	}
	io.range.len = (range.end << SECTOR_SHIFT) - io.range.offset;

	r = bio->engine->issue(bio->engine, &io, b);
	if (r == -EAGAIN) {
		if (!_make_room_for_retry(b, r))
			return;

		goto retry;
	}

	if (r < 0) {
		_complete_io(b, r);
		return;
	}
}

static inline void _issue_read(struct blkio_block *b)
{
	_issue_low_level(b, BLKIO_DIR_READ);
}

static inline void _issue_write(struct blkio_block *b)
{
	_issue_low_level(b, BLKIO_DIR_WRITE);
}

/*----------------------------------------------------------------
 * High level IO handling
 *--------------------------------------------------------------*/

static bool _wait_all(struct blkio *bio)
{
	while (!dm_list_empty(&bio->io_pending))
		if (!_wait_io(bio))
			return_false;
	return true;
}

/*
 * A failed wait may be just an interrupted wait, so report success if
 * the completion still arrived.  If b is still pending, the caller must
 * leave it on io_pending; its completion will unlink it.
 */
static bool _wait_specific(struct blkio_block *b)
{
	while (_test_flags(b, BF_IO_PENDING))
		if (!_wait_io(b->bio))
			break;
	return !_test_flags(b, BF_IO_PENDING);
}

static unsigned _writeback(struct blkio *bio, unsigned count)
{
	unsigned actual = 0;
	struct blkio_block *b, *tmp;

	dm_list_iterate_items_gen_safe (b, tmp, &bio->dirty, list) {
		if (actual == count)
			break;

		/*
		 * We can't writeback anything that's not dirty or still in
		 * use.
		 */
		if (_test_flags(b, BF_DIRTY) && !b->ref_count) {
			_issue_write(b);
			actual++;
		}
	}

	return actual;
}

/*----------------------------------------------------------------
 * High level allocation
 *--------------------------------------------------------------*/

static struct blkio_block *_find_unused_clean_block(struct blkio *bio)
{
	struct blkio_block *b;

	dm_list_iterate_items (b, &bio->clean) {
		if (!b->ref_count) {
			_unlink_block(b);
			_block_remove(b);
			return b;
		}
	}

	return NULL;
}

static struct blkio_block *_claim_free_block(struct blkio *bio)
{
	struct blkio_block *b;

	if ((b = _alloc_block(bio)))
		return b;

	return _find_unused_clean_block(bio);
}

/*
 * Reset a claimed block and add it to the radix tree.  Returns NULL after
 * returning the block to the free list when the tree cannot take it.
 */
static struct blkio_block *_init_block(struct blkio_block *b, int di,
				       block_address_t i)
{
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

/*
 * Take a block only if one is already free.  Used by prefetch, which must
 * not evict or wait for writeback on behalf of I/O nobody asked for yet.
 */
static struct blkio_block *_try_new_block(struct blkio *bio, int di,
					  block_address_t i)
{
	struct blkio_block *b;

	if (!(b = _claim_free_block(bio))) {
		log_debug("blkio no new blocks for di %d index %llu.",
			  di, (unsigned long long) i);
		return NULL;
	}

	return _init_block(b, di, i);
}

/*
 * Take a block, evicting clean blocks and waiting for writeback as needed.
 * Returns NULL when memory or I/O pressure leaves none.
 */
static struct blkio_block *_new_block(struct blkio *bio, int di, block_address_t i)
{
	struct blkio_block *b;

	for (;;) {
		if ((b = _claim_free_block(bio)))
			return _init_block(b, di, i);

		if (dm_list_empty(&bio->io_pending) &&
		    !_writeback(bio, WRITEBACK_BATCH_BLOCKS)) {
			log_debug("blkio no new blocks for di %d index %llu.",
				  di, (unsigned long long) i);
			return NULL;
		}

		if (!_wait_all(bio))
			return_NULL;

		if (dm_list_size(&bio->errored) >= bio->max_io) {
			log_debug("blkio no new blocks for di %d index %llu with >%u errors.",
				  di, (unsigned long long) i, bio->max_io);
			return NULL;
		}
	}
}

/*----------------------------------------------------------------
 * Block reference counting
 *--------------------------------------------------------------*/
static void _zero_block(struct blkio_block *b)
{
	b->bio->write_zeroes++;
	memset(b->data, 0, (uint64_t) b->bio->block_sectors << SECTOR_SHIFT);
	_set_flags(b, BF_DIRTY);
}

/*
 * A get that carries either of these flags takes the block for writing.
 */
static inline bool _is_write(unsigned flags)
{
	return (flags & (GF_DIRTY | GF_ZERO)) != 0;
}

static void _hit(struct blkio_block *b, unsigned flags)
{
	if (_is_write(flags))
		b->bio->write_hits++;
	else
		b->bio->read_hits++;
}

static void _miss(struct blkio *bio, unsigned flags)
{
	if (_is_write(flags))
		bio->write_misses++;
	else
		bio->read_misses++;
}

static struct blkio_block *_lookup_or_read_block(struct blkio *bio,
				       int di, block_address_t i,
				       unsigned flags)
{
	bool is_write = _is_write(flags);
	struct blkio_block *b = _block_lookup(bio, di, i);

	if (b) {
		if (b->write_locked && !is_write) {
			log_error(INTERNAL_ERROR "blkio concurrent read lock attempted on write locked block.");
			return NULL;
		}

		if (b->ref_count && is_write) {
			log_error(INTERNAL_ERROR "blkio concurrent write lock attempted on held block.");
			return NULL;
		}

		if (_test_flags(b, BF_IO_PENDING))
			_miss(bio, flags);
		else
			_hit(b, flags);

	} else {
		_miss(bio, flags);

		if (!(b = _new_block(bio, di, i)))
			return_NULL;

		/* A block that is going to be zeroed needs no read. */
		if (!(flags & GF_ZERO))
			_issue_read(b);
	}

	/*
	 * An I/O may still be in flight, either from a prefetch or from the
	 * read just issued.  While it is pending its completion owns the
	 * block and will unlink it, so wait before touching the data.  A
	 * failed read is not reported here: once the block is no longer
	 * pending blkio_get() inspects b->error.
	 */
	if (!_wait_specific(b))
		return_NULL;

	_unlink_block(b);

	/* _zero_block() marks the block dirty itself. */
	if (flags & GF_ZERO)
		_zero_block(b);
	else if (flags & GF_DIRTY)
		_set_flags(b, BF_DIRTY);

	_link_block(b);

	return b;
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
/*
 * Return the system page size, or 0 after reporting the problem.
 */
static int _get_pagesize(void)
{
	static int _pagesize = 0;

	if (_pagesize <= 0) {
		_pagesize = lvm_getpagesize();
		if (_pagesize <= 0)
			log_error("Invalid system page size.");
	}

	return _pagesize;
}

/*
 * Check the cache geometry arguments against the page size the blocks have
 * to align to.  Returns 1 when they are usable, or 0 after reporting the
 * rejected argument; the caller still owns the engine.
 */
static int _check_geometry(sector_t block_sectors, unsigned nr_cache_blocks,
			   int pagesize)
{
	if (!nr_cache_blocks) {
		log_error("blkio must have at least one cache block.");
		return 0;
	}

	if (!block_sectors) {
		log_error("blkio must have a non zero block size.");
		return 0;
	}

	if (block_sectors & ((pagesize >> SECTOR_SHIFT) - 1)) {
		log_error("blkio block size must be a multiple of page size.");
		return 0;
	}

	return 1;
}

struct blkio *blkio_create_engine(unsigned block_sectors,
				  unsigned nr_cache_blocks,
				  struct io_engine *engine)
{
	struct blkio *bio;
	unsigned max_io;
	int pagesize;
	int i;

	if (!blkio_engine_usable(engine)) {
		log_error("Cannot use an unavailable io engine.");
		return NULL;
	}

	max_io = engine->max_io(engine);
	if (!max_io) {
		log_error("The %s io engine accepts no outstanding I/O.",
			  engine->name);
		goto bad_engine;
	}

	if (!(pagesize = _get_pagesize()) ||
	    !_check_geometry(block_sectors, nr_cache_blocks, pagesize)) {
		stack;
		goto bad_engine;
	}

	bio = zalloc(sizeof(*bio));
	if (!bio) {
		stack;
		goto bad_engine;
	}

	bio->creator_pid = getpid();
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
		goto_bad;

	if (!_init_free_list(bio, nr_cache_blocks, pagesize))
		goto_bad;

	bio->fd_table_size = BLKIO_FD_TABLE_INC;

	if (!(bio->fd_table = malloc(sizeof(int) * bio->fd_table_size)))
		goto_bad;

	for (i = 0; i < bio->fd_table_size; i++)
		bio->fd_table[i] = FD_FREE;

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

struct blkio *blkio_create(const char *hint, unsigned block_sectors,
			   unsigned nr_cache_blocks)
{
	struct io_engine *engine;

	if (!(engine = blkio_create_io_engine(hint)))
		return_NULL;

	return blkio_create_engine(block_sectors, nr_cache_blocks, engine);
}

void blkio_destroy(struct blkio *bio)
{
	bool forked = blkio_engine_is_forked(bio->creator_pid);

	if (bio->nr_locked)
		log_warn("WARNING: some blocks are still locked.");

	/*
	 * After fork() the cache and its engine belong to the parent.
	 * The child must not submit a flush or otherwise touch the
	 * inherited kernel state; it only releases its private copies.
	 * The fork check must happen before the flush.
	 */
	if (!forked && !blkio_flush(bio))
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

unsigned blkio_block_sectors(struct blkio *bio)
{
	return bio->block_sectors;
}

unsigned blkio_max_prefetches(struct blkio *bio)
{
	return bio->max_io;
}

void blkio_prefetch(struct blkio *bio, int di, block_address_t index)
{
	struct blkio_block *b;

	if (!_di_valid(bio, di))
		return;

	b = _block_lookup(bio, di, index);

	if (!b) {
		if (bio->nr_io_pending < bio->max_io) {
			b = _try_new_block(bio, di, index);
			if (b) {
				bio->prefetches++;
				_issue_read(b);
			}
		}
	}
}

/*----------------------------------------------------------------*/

static void _recycle_block(struct blkio_block *b)
{
	_unlink_block(b);
	_block_remove(b);
	_free_block(b);
}

bool blkio_get(struct blkio *bio, int di, block_address_t index,
	        unsigned flags, struct blkio_block **result)
{
	struct blkio_block *b;

	*result = NULL;

	if (!_di_valid(bio, di))
		return_false;

	if (!(b = _lookup_or_read_block(bio, di, index, flags))) {
		log_error("blkio failed to get block %llu di %d.", (unsigned long long) index, di);
		return false;
	}

	if (b->error) {
		if (b->io_dir == BLKIO_DIR_READ) {
			/* Now we know the read failed we can just forget
			 * about this block, since there's no dirty data to
			 * be written back. */
			_recycle_block(b);
		}
		return_false;
	}

	if (!b->ref_count)
		bio->nr_locked++;
	b->ref_count++;

	if (_is_write(flags))
		b->write_locked = true;

	*result = b;
	return true;
}

/*----------------------------------------------------------------*/

static void _put_ref(struct blkio_block *b)
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

void blkio_put(struct blkio_block *b)
{
	_put_ref(b);

	if (_test_flags(b, BF_DIRTY))
		_preemptive_writeback(b->bio);
}

/*----------------------------------------------------------------*/

bool blkio_flush(struct blkio *bio)
{
	struct blkio_block *b, *tmp;

	/*
	 * Failed reads have no dirty data to write back, so recycle them.
	 * Failed writes are still dirty; put them back on the dirty list
	 * and retry.
	 */
	dm_list_iterate_items_safe (b, tmp, &bio->errored) {
		if (!_test_flags(b, BF_DIRTY))
			_recycle_block(b);
	}

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
		return_false;

	return dm_list_empty(&bio->errored);
}

/*----------------------------------------------------------------*/
/*
 * You can safely call this with a NULL block.
 */
static bool _invalidate_block(struct blkio_block *b)
{
	if (!b)
		return true;

	if (_test_flags(b, BF_IO_PENDING) && !_wait_specific(b))
		return_false;

	if (b->ref_count) {
		log_warn("WARNING: blkio invalidate: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		return false;
	}

	if (_test_flags(b, BF_DIRTY)) {
		_issue_write(b);

		if (!_wait_specific(b))
			return_false;

		if (b->error)
			return_false;
	}

	_recycle_block(b);

	return true;
}

bool blkio_invalidate(struct blkio *bio, int di, block_address_t index)
{
	if (!_di_valid(bio, di))
		return_false;

	return _invalidate_block(_block_lookup(bio, di, index));
}

/*----------------------------------------------------------------*/

#define DROP_HELD	(1 << 0)
#define DROP_DIRTY	(1 << 1)

struct block_iterator {
	struct radix_tree_iterator it;
	unsigned reasons;	/* DROP_* flags accepted as a reason to fail */
	const char *what;	/* operation name used in messages */
	bool failed;
};

static bool _writeback_v(struct radix_tree_iterator *it,
                         const void *kb, size_t keylen, union radix_value v)
{
	struct blkio_block *b = v.ptr;

	if (_test_flags(b, BF_DIRTY) && !b->ref_count)
		_issue_write(b);

	return true;
}

static bool _check_droppable_v(struct radix_tree_iterator *it,
			       const void *kb, size_t keylen, union radix_value v)
{
	struct blkio_block *b = v.ptr;
	struct block_iterator *bit = container_of(it, struct block_iterator, it);

	if (b->ref_count) {
		if (bit->reasons & DROP_HELD) {
			log_warn("WARNING: blkio %s: block (%d, %llu) still held.",
				 bit->what, b->di, (unsigned long long) b->index);
			bit->failed = true;
		}

		return true;
	}

	if ((bit->reasons & DROP_DIRTY) && _test_flags(b, BF_DIRTY)) {
		log_warn("WARNING: blkio %s: block (%d, %llu) still dirty.",
			 bit->what, b->di, (unsigned long long) b->index);
		bit->failed = true;
	}

	return true;
}

static bool _drop_block_v(struct radix_tree_iterator *it,
			  const void *kb, size_t keylen, union radix_value v)
{
	struct blkio_block *b = v.ptr;

	_unlink_block(b);
	_free_block(b);

	/*
	 * We can't remove the block from the radix tree yet because
	 * we're in the middle of an iteration.
	 */
	return true;
}

static void _drop_di(struct blkio *bio, const union key *k)
{
	struct radix_tree_iterator it;

	it.visit = _drop_block_v;
	radix_tree_iterate(bio->rtree, k->bytes, sizeof(k->parts.di), &it);
	(void) radix_tree_remove_prefix(bio->rtree, k->bytes, sizeof(k->parts.di));

	/*
	 * A reserved di was closed while its blocks could not be dropped.
	 * Now that they are gone the slot no longer aliases a closed
	 * device and can be handed to the next blkio_set_fd().
	 */
	if (_di_reserved(bio, k->parts.di))
		bio->fd_table[k->parts.di] = FD_FREE;
}

/*
 * Validate di and build the radix tree key that selects all of its blocks.
 */
static bool _di_key(struct blkio *bio, int di, union key *k)
{
	if (!_di_valid(bio, di))
		return_false;

	_block_key(di, 0, k);

	return true;
}

/*
 * The two operations that drop a whole di, which differ only in the block
 * state that makes the drop fail and in the name used in the messages.
 */
enum drop_op {
	DROP_INVALIDATE,
	DROP_ABORT,
};

/*
 * Drop every block cached for the di selected by k.  Reports the reason
 * itself, so a caller that needs to say more about the device does so
 * after the false return.
 */
static bool _drop_di_blocks(struct blkio *bio, const union key *k,
			    enum drop_op op)
{
	struct block_iterator it = {
		.reasons = (op == DROP_INVALIDATE) ?
			   DROP_HELD | DROP_DIRTY : DROP_HELD,
		.what = (op == DROP_INVALIDATE) ? "invalidate" : "abort",
	};

	/*
	 * Check for held blocks before freeing anything.  Once _drop_block_v()
	 * unlinks a block the radix tree still points at it, so the prefix
	 * cannot be removed unless every block is dropped.
	 */
	it.it.visit = _check_droppable_v;
	radix_tree_iterate(bio->rtree, k->bytes, sizeof(k->parts.di), &it.it);

	if (it.failed)
		return_false;

	_drop_di(bio, k);

	return true;
}

bool blkio_invalidate_di(struct blkio *bio, int di)
{
	union key k;
	struct radix_tree_iterator it;

	if (!_di_key(bio, di, &k))
		return_false;

	it.visit = _writeback_v;
	radix_tree_iterate(bio->rtree, k.bytes, sizeof(k.parts.di), &it);

	if (!_wait_all(bio))
		return_false;

	return _drop_di_blocks(bio, &k, DROP_INVALIDATE);
}

/*----------------------------------------------------------------*/

bool blkio_abort_di(struct blkio *bio, int di)
{
	union key k;

	if (!_di_key(bio, di, &k))
		return_false;

	/*
	 * An in-flight request still references its block.  Leave the cache
	 * intact if the I/O cannot be drained; blkio_destroy() will retain
	 * the buffers when the engine reports an interrupted teardown.
	 */
	if (!_wait_all(bio)) {
		log_warn("WARNING: blkio abort: I/O still in flight for di %d.", di);
		return false;
	}

	if (!_drop_di_blocks(bio, &k, DROP_ABORT)) {
		log_warn("WARNING: blkio abort: cache for di %d left intact.", di);
		return false;
	}

	return true;
}

/*----------------------------------------------------------------*/

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
	int *new_table;
	int old_size, new_size;
	int i;

	old_size = bio->fd_table_size;
	new_size = old_size + BLKIO_FD_TABLE_INC;

	for (i = 0; i < old_size; i++) {
		if (bio->fd_table[i] == FD_FREE) {
			bio->fd_table[i] = fd;
			return i;
		}
	}

	new_table = realloc(bio->fd_table, sizeof(int) * new_size);
	if (!new_table) {
		log_error("Cannot extend blkio fd table.");
		return -1;
	}

	new_table[old_size] = fd;

	for (i = old_size + 1; i < new_size; i++)
		new_table[i] = FD_FREE;
	bio->fd_table = new_table;
	bio->fd_table_size = new_size;

	return old_size;
}

void blkio_clear_fd(struct blkio *bio, int di)
{
	if (!_di_valid(bio, di))
		return;

	/* A reserved di still owns cached blocks and must not be reused. */
	if (_di_reserved(bio, di))
		return;

	bio->fd_table[di] = FD_FREE;
}

void blkio_reserve_fd(struct blkio *bio, int di)
{
	if (!_di_valid(bio, di))
		return;
	bio->fd_table[di] = FD_RESERVED;
}

bool blkio_change_fd(struct blkio *bio, int di, int fd)
{
	if (!_di_valid(bio, di))
		return_false;

	/* A reserved di has no fd to change and must not be reused. */
	if (_di_reserved(bio, di)) {
		log_error("Can't change fd for reserved blkio di %d.", di);
		return false;
	}

	bio->fd_table[di] = fd;
	return true;
}
