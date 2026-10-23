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
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libaio.h>
#include <unistd.h>
#include <sys/user.h>

#define SECTOR_SHIFT 9L

#define FD_TABLE_INC 1024
static int _fd_table_size = 0;
static int *_fd_table = NULL;

static inline int _di_valid(int di)
{
	if (di < 0 || di >= _fd_table_size) {
		log_error(INTERNAL_ERROR "blkio invalid di %d (fd table size %d).", di, _fd_table_size);
		return 0;
	}

	return 1;
}


/*----------------------------------------------------------------*/

static void _log_sys_warn(const char *call, int err)
{
	log_warn("WARNING: %s failed: %s.", call, strerror(err));
}

// Assumes the list is not empty.
static inline struct dm_list *_list_pop(struct dm_list *head)
{
	struct dm_list *l;

	l = head->n;
	dm_list_del(l);
	return l;
}

/*----------------------------------------------------------------*/

struct control_block {
	struct dm_list list;
	void *context;
	struct iocb cb;
};

struct cb_set {
	struct dm_list free;
	struct dm_list allocated;
	struct control_block vec[];
};

static struct cb_set *_cb_set_create(unsigned nr)
{
	unsigned i;
	struct cb_set *cbs = malloc(sizeof(*cbs) + nr * sizeof(*cbs->vec));

	if (!cbs)
		return NULL;

	dm_list_init(&cbs->free);
	dm_list_init(&cbs->allocated);

	for (i = 0; i < nr; i++)
		dm_list_add(&cbs->free, &cbs->vec[i].list);

	return cbs;
}

static void _cb_set_destroy(struct cb_set *cbs)
{
	// We know this is always called after a wait_all.  So there should
	// never be in flight IO.
	if (!dm_list_empty(&cbs->allocated)) {
		// bail out
		log_warn("WARNING: Async io still in flight.");
		return;
	}

	free(cbs);
}

static struct control_block *_cb_alloc(struct cb_set *cbs, void *context)
{
	struct control_block *cb;

	if (dm_list_empty(&cbs->free))
		return NULL;

	cb = dm_list_item(_list_pop(&cbs->free), struct control_block);
	cb->context = context;
	dm_list_add(&cbs->allocated, &cb->list);

	return cb;
}

static void _cb_free(struct cb_set *cbs, struct control_block *cb)
{
	dm_list_del(&cb->list);
	dm_list_add_h(&cbs->free, &cb->list);
}

static struct control_block *_iocb_to_cb(struct iocb *icb)
{
	return dm_list_struct_base(icb, struct control_block, cb);
}

/*----------------------------------------------------------------*/

struct async_engine {
	struct io_engine base;
	io_context_t aio_context;
	struct cb_set *cbs;
	unsigned page_mask;
	pid_t aio_context_pid; /* PID that created this AIO context */
};

static struct async_engine *_to_async(struct io_engine *e)
{
	return container_of(e, struct async_engine, base);
}

static void _async_destroy(struct io_engine *ioe)
{
	int r;
	struct async_engine *e = _to_async(ioe);

	_cb_set_destroy(e->cbs);

	/*
	 * Only call io_destroy() if we're in the same process that created
	 * the AIO context. After fork(), the child inherits the parent's
	 * aio_context value but must not call io_destroy() on it.
	 */
	if (e->aio_context) {
		if (e->aio_context_pid != getpid())
			log_debug_devs("Skipping AIO context destroy for different pid.");
		else {
			log_debug_devs("Destroy AIO context.");
			r = io_destroy(e->aio_context); // really slow (~40ms)
			if (r < 0)
				_log_sys_warn("io_destroy", -r);
		}
	}

	free(e);
}

static int _last_byte_di;
static uint64_t _last_byte_offset;
static size_t _last_byte_sector_size;

static bool _async_issue(struct io_engine *ioe, enum blkio_dir d, int di,
			 sector_t sb, sector_t se, void *data, void *context)
{
	int r;
	struct iocb *cb_array[1];
	struct control_block *cb;
	struct async_engine *e = _to_async(ioe);
	sector_t offset;
	sector_t nbytes;
	sector_t limit_nbytes;
	sector_t orig_nbytes;
	sector_t extra_nbytes = 0;

	if (((uintptr_t) data) & e->page_mask) {
		log_warn("misaligned data buffer");
		return false;
	}

	offset = sb << SECTOR_SHIFT;
	nbytes = (se - sb) << SECTOR_SHIFT;

	/*
	 * If blkio block goes past where lvm wants to write, then clamp it.
	 */
	if ((d == BLKIO_DIR_WRITE) && _last_byte_offset && (di == _last_byte_di)) {
		if (offset > _last_byte_offset) {
			log_error("Limit write at %llu len %llu beyond last byte %llu",
				  (unsigned long long)offset,
				  (unsigned long long)nbytes,
				  (unsigned long long)_last_byte_offset);
			return false;
		}

		/*
		 * If the blkio block offset+len goes beyond where lvm is
		 * intending to write, then reduce the len being written
		 * (which is the blkio block size) so we don't write past
		 * the limit set by lvm.  If after applying the limit, the
		 * resulting size is not a multiple of the sector size (512
		 * or 4096) then extend the reduced size to be a multiple of
		 * the sector size (we don't want to write partial sectors.)
		 */
		if (offset + nbytes > _last_byte_offset) {
			limit_nbytes = _last_byte_offset - offset;

			if (limit_nbytes % _last_byte_sector_size) {
				extra_nbytes = _last_byte_sector_size - (limit_nbytes % _last_byte_sector_size);

				/*
				 * adding extra_nbytes to the reduced nbytes (limit_nbytes)
				 * should make the final write size a multiple of the
				 * sector size.  This should never result in a final size
				 * larger than the blkio block size (as long as the blkio
				 * block size is a multiple of the sector size).
				 */
				if (limit_nbytes + extra_nbytes > nbytes) {
					log_warn("Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu",
						 (unsigned long long)offset,
						 (unsigned long long)nbytes,
						 (unsigned long long)limit_nbytes,
						 (unsigned long long)extra_nbytes,
						 (unsigned long long)_last_byte_sector_size);
					extra_nbytes = 0;
				}
			}

			orig_nbytes = nbytes;

			if (extra_nbytes) {
				log_debug("Limit write at %llu len %llu to len %llu rounded to %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)(limit_nbytes + extra_nbytes));
				nbytes = limit_nbytes + extra_nbytes;
			} else {
				log_debug("Limit write at %llu len %llu to len %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes);
				nbytes = limit_nbytes;
			}

			/*
			 * This shouldn't happen, the reduced+extended
			 * nbytes value should never be larger than the
			 * blkio block size.
			 */
			if (nbytes > orig_nbytes) {
				log_error("Invalid adjusted write at %llu len %llu adjusted %llu limit %llu extra %llu sector_size %llu",
					  (unsigned long long)offset,
					  (unsigned long long)orig_nbytes,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)extra_nbytes,
					  (unsigned long long)_last_byte_sector_size);
				return false;
			}
		}
	}

	cb = _cb_alloc(e->cbs, context);
	if (!cb) {
		log_warn("couldn't allocate control block");
		return false;
	}

	memset(&cb->cb, 0, sizeof(cb->cb));

	cb->cb.aio_fildes = (int) _fd_table[di];
	cb->cb.u.c.buf = data;
	cb->cb.u.c.offset = offset;
	cb->cb.u.c.nbytes = nbytes;
	cb->cb.aio_lio_opcode = (d == BLKIO_DIR_READ) ? IO_CMD_PREAD : IO_CMD_PWRITE;

#if 0
	if (d == BLKIO_DIR_READ) {
		log_debug("io R off %llu bytes %llu di %d fd %d",
			  (unsigned long long)cb->cb.u.c.offset,
			  (unsigned long long)cb->cb.u.c.nbytes,
			  di, _fd_table[di]);
	} else {
		log_debug("io W off %llu bytes %llu di %d fd %d",
			  (unsigned long long)cb->cb.u.c.offset,
			  (unsigned long long)cb->cb.u.c.nbytes,
			  di, _fd_table[di]);
	}
#endif

	cb_array[0] = &cb->cb;
	do {
		r = io_submit(e->aio_context, 1, cb_array);
	} while (r == -EAGAIN);

	if (r < 0) {
		_cb_free(e->cbs, cb);
		return false;
	}

	return true;
}

/*
 * MAX_IO is returned to the layer above via blkio_max_prefetches() which
 * tells the caller how many devices to submit io for concurrently.  There will
 * be an open file descriptor for each of these, so keep it low enough to avoid
 * reaching the default max open file limit (1024) when there are over 1024
 * devices being scanned.
 */

#define MAX_IO 256
#define MAX_EVENT 64

static bool _async_wait(struct io_engine *ioe, io_complete_fn fn)
{
	int i, r;
	struct io_event event[MAX_EVENT] = { };
	struct control_block *cb;
	struct async_engine *e = _to_async(ioe);

	/*
	 * Retry on EINTR from stray signals, but stop if an LVM interrupt
	 * signal (SIGINT/SIGTERM via sigint_allow()) has been caught.
	 */
	do {
		r = io_getevents(e->aio_context, 1, MAX_EVENT, event, NULL);
	} while (r == -EINTR && !sigint_caught());

	if (r < 0) {
		if (r == -EINTR)
			stack;
		else
			_log_sys_warn("io_getevents", -r);
		return false;
	}

	for (i = 0; i < r; i++) {
		struct io_event *ev = event + i;

		cb = _iocb_to_cb((struct iocb *) ev->obj);

		if (ev->res == cb->cb.u.c.nbytes)
			fn((void *) cb->context, 0);

		else if ((int) ev->res < 0)
			fn(cb->context, (int) ev->res);

		// FIXME: dct added this. a short read is ok?!
		else if (ev->res >= (1 << SECTOR_SHIFT)) {
			/* minimum acceptable read is 1 sector */
			fn((void *) cb->context, 0);

		} else {
			fn(cb->context, -ENODATA);
		}

		_cb_free(e->cbs, cb);
	}

	return true;
}

static unsigned _async_max_io(struct io_engine *ioe)
{
	return MAX_IO;
}

struct io_engine *create_async_io_engine(void)
{
	static int _pagesize = 0;
	int r;
	struct async_engine *e;

	if ((_pagesize <= 0) && (_pagesize = sysconf(_SC_PAGESIZE)) < 0) {
		log_warn("_SC_PAGESIZE returns negative value.");
		return NULL;
	}

	if (!(e = malloc(sizeof(*e))))
		return NULL;

	e->base.destroy = _async_destroy;
	e->base.issue = _async_issue;
	e->base.wait = _async_wait;
	e->base.max_io = _async_max_io;

	e->aio_context = 0;
	e->aio_context_pid = getpid();
	r = io_setup(MAX_IO, &e->aio_context);
	if (r < 0) {
		log_debug("io_setup failed %d", r);
		free(e);
		return NULL;
	}

	e->cbs = _cb_set_create(MAX_IO);
	if (!e->cbs) {
		log_warn("couldn't create control block set");
		free(e);
		return NULL;
	}

	e->page_mask = (unsigned) _pagesize - 1;

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->base;
}

/*----------------------------------------------------------------*/

struct sync_io {
        struct dm_list list;
	void *context;
};

struct sync_engine {
	struct io_engine base;
	struct dm_list complete;
};

static struct sync_engine *_to_sync(struct io_engine *e)
{
        return container_of(e, struct sync_engine, base);
}

static void _sync_destroy(struct io_engine *ioe)
{
        struct sync_engine *e = _to_sync(ioe);
        free(e);
}

static bool _sync_issue(struct io_engine *ioe, enum blkio_dir d, int di,
                        sector_t sb, sector_t se, void *data, void *context)
{
	ssize_t rv;
	off_t off;
	uint64_t where;
	uint64_t pos = 0;
	uint64_t len = (se - sb) * 512;
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *io = malloc(sizeof(*io));
	if (!io) {
		log_warn("unable to allocate sync_io");
        	return false;
	}

	where = sb * 512;
	off = lseek(_fd_table[di], where, SEEK_SET);
	if (off == (off_t) -1) {
		log_warn("Device seek error %d for offset %llu", errno, (unsigned long long)where);
		free(io);
		return false;
	}
	if (off != (off_t) where) {
		log_warn("Device seek failed for offset %llu", (unsigned long long)where);
		free(io);
		return false;
	}

	/*
	 * If blkio block goes past where lvm wants to write, then clamp it.
	 */
	if ((d == BLKIO_DIR_WRITE) && _last_byte_offset && (di == _last_byte_di)) {
		uint64_t offset = where;
		uint64_t nbytes = len;
		sector_t limit_nbytes = 0;
		sector_t extra_nbytes = 0;
		sector_t orig_nbytes = 0;

		if (offset > _last_byte_offset) {
			log_error("Limit write at %llu len %llu beyond last byte %llu",
				  (unsigned long long)offset,
				  (unsigned long long)nbytes,
				  (unsigned long long)_last_byte_offset);
			free(io);
			return false;
		}

		if (offset + nbytes > _last_byte_offset) {
			limit_nbytes = _last_byte_offset - offset;

			if (limit_nbytes % _last_byte_sector_size) {
				extra_nbytes = _last_byte_sector_size - (limit_nbytes % _last_byte_sector_size);

				/*
				 * adding extra_nbytes to the reduced nbytes (limit_nbytes)
				 * should make the final write size a multiple of the
				 * sector size.  This should never result in a final size
				 * larger than the blkio block size (as long as the blkio
				 * block size is a multiple of the sector size).
				 */
				if (limit_nbytes + extra_nbytes > nbytes) {
					log_warn("Skip extending write at %llu len %llu limit %llu extra %llu sector_size %llu",
						 (unsigned long long)offset,
						 (unsigned long long)nbytes,
						 (unsigned long long)limit_nbytes,
						 (unsigned long long)extra_nbytes,
						 (unsigned long long)_last_byte_sector_size);
					extra_nbytes = 0;
				}
			}

			orig_nbytes = nbytes;

			if (extra_nbytes) {
				log_debug("Limit write at %llu len %llu to len %llu rounded to %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)(limit_nbytes + extra_nbytes));
				nbytes = limit_nbytes + extra_nbytes;
			} else {
				log_debug("Limit write at %llu len %llu to len %llu",
					  (unsigned long long)offset,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes);
				nbytes = limit_nbytes;
			}

			/*
			 * This shouldn't happen, the reduced+extended
			 * nbytes value should never be larger than the
			 * blkio block size.
			 */
			if (nbytes > orig_nbytes) {
				log_error("Invalid adjusted write at %llu len %llu adjusted %llu limit %llu extra %llu sector_size %llu",
					  (unsigned long long)offset,
					  (unsigned long long)orig_nbytes,
					  (unsigned long long)nbytes,
					  (unsigned long long)limit_nbytes,
					  (unsigned long long)extra_nbytes,
					  (unsigned long long)_last_byte_sector_size);
				free(io);
				return false;
			}
		}

		len = nbytes;
	}

	while (pos < len) {
		if (d == BLKIO_DIR_READ)
			rv = read(_fd_table[di], (char *)data + pos, len - pos);
		else
			rv = write(_fd_table[di], (char *)data + pos, len - pos);

		if (rv == -1 && errno == EINTR)
			continue;
		if (rv == -1 && errno == EAGAIN)
			continue;

		if (!rv)
			break;

		if (rv < 0) {
			if (d == BLKIO_DIR_READ)
				log_debug("Device read error %d offset %llu len %llu", errno,
					  (unsigned long long)(where + pos),
					  (unsigned long long)(len - pos));
			else
				log_debug("Device write error %d offset %llu len %llu", errno,
					  (unsigned long long)(where + pos),
					  (unsigned long long)(len - pos));
			free(io);
			return false;
		}
		pos += rv;
	}

	if (pos < len) {
		if (d == BLKIO_DIR_READ)
			log_warn("Device read short %u bytes remaining", (unsigned)(len - pos));
		else
			log_warn("Device write short %u bytes remaining", (unsigned)(len - pos));
		/*
        	free(io);
        	return false;
		*/
	}


	dm_list_add(&e->complete, &io->list);
	io->context = context;

	return true;
}

static bool _sync_wait(struct io_engine *ioe, io_complete_fn fn)
{
        struct sync_io *io, *tmp;
	struct sync_engine *e = _to_sync(ioe);

	dm_list_iterate_items_safe(io, tmp, &e->complete) {
		fn(io->context, 0);
		dm_list_del(&io->list);
		free(io);
	}

	return true;
}

static unsigned _sync_max_io(struct io_engine *ioe)
{
        return 1;
}

struct io_engine *create_sync_io_engine(void)
{
	struct sync_engine *e = malloc(sizeof(*e));

	if (!e)
        	return NULL;

        e->base.destroy = _sync_destroy;
        e->base.issue = _sync_issue;
        e->base.wait = _sync_wait;
        e->base.max_io = _sync_max_io;

	dm_list_init(&e->complete);
	/* coverity[leaked_storage] 'e' is not leaking */
        return &e->base;
}

/*----------------------------------------------------------------*/

#define MIN_BLOCKS 16
#define WRITEBACK_LOW_THRESHOLD_PERCENT 33
#define WRITEBACK_HIGH_THRESHOLD_PERCENT 66

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
};

struct blkio {
	unsigned block_sectors;
	uint64_t nr_data_blocks;
	uint64_t nr_cache_blocks;
	unsigned max_io;

	struct io_engine *engine;

	void *raw_data;
	struct blkio_block *raw_blocks;

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

/*----------------------------------------------------------------*/

struct key_parts {
	uint32_t di;
	uint64_t b;
} __attribute__ ((packed));

union key {
	struct key_parts parts;
	uint8_t bytes[sizeof(struct key_parts)];
};

static struct blkio_block *_block_lookup(struct blkio *bio, int di, uint64_t i)
{
	union key k;
	union radix_value v;

	k.parts.di = di;
	k.parts.b = i;

	if (radix_tree_lookup(bio->rtree, k.bytes, sizeof(k.bytes), &v))
		return v.ptr;

	return NULL;
}

static bool _block_insert(struct blkio_block *b)
{
	union key k;
	union radix_value v;

	k.parts.di = b->di;
	k.parts.b = b->index;
	v.ptr = b;

	return radix_tree_insert(b->bio->rtree, k.bytes, sizeof(k.bytes), v);
}

static void _block_remove(struct blkio_block *b)
{
	union key k;

	k.parts.di = b->di;
	k.parts.b = b->index;

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

	return true;
}

static void _exit_free_list(struct blkio *bio)
{
	free(bio->raw_data);
	free(bio->raw_blocks);
}

static struct blkio_block *_alloc_block(struct blkio *bio)
{
	if (dm_list_empty(&bio->free))
		return NULL;

	return dm_list_struct_base(_list_pop(&bio->free), struct blkio_block, list);
}

static void _free_block(struct blkio_block *b)
{
	dm_list_add(&b->bio->free, &b->list);
}

/*----------------------------------------------------------------
 * Clean/dirty list management.
 * Always use these methods to ensure nr_dirty_ is correct.
 *--------------------------------------------------------------*/

static void _unlink_block(struct blkio_block *b)
{
	if (_test_flags(b, BF_DIRTY))
		b->bio->nr_dirty--;

	dm_list_del(&b->list);
}

static void _link_block(struct blkio_block *b)
{
	struct blkio *bio = b->bio;

	if (_test_flags(b, BF_DIRTY)) {
		dm_list_add(&bio->dirty, &b->list);
		bio->nr_dirty++;
	} else
		dm_list_add(&bio->clean, &b->list);
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
 * |b->list| should be valid (either pointing to itself, on one of the other
 * lists.
 */
static void _issue_low_level(struct blkio_block *b, enum blkio_dir d)
{
	struct blkio *bio = b->bio;
	sector_t sb = b->index * bio->block_sectors;
	sector_t se = sb + bio->block_sectors;

	if (_test_flags(b, BF_IO_PENDING))
		return;

	b->io_dir = d;
	_set_flags(b, BF_IO_PENDING);
	bio->nr_io_pending++;

	dm_list_move(&bio->io_pending, &b->list);

	if (!bio->engine->issue(bio->engine, d, b->di, sb, se, b->data, b)) {
		/* FIXME: if io_submit() set an errno, return that instead of EIO? */
		_complete_io(b, -EIO);
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

static bool _wait_io(struct blkio *bio)
{
	return bio->engine->wait(bio->engine, _complete_io);
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

static bool _wait_specific(struct blkio_block *b)
{
	while (_test_flags(b, BF_IO_PENDING))
		if (!_wait_io(b->bio))
			return false;
	return true;
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

		if (dm_list_empty(&bio->io_pending))
			_writeback(bio, 16);  // FIXME: magic number

		if (!_wait_all(bio))
			return NULL;

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

static void _hit(struct blkio_block *b, unsigned flags)
{
	if (flags & (GF_ZERO | GF_DIRTY))
		b->bio->write_hits++;
	else
		b->bio->read_hits++;
}

static void _miss(struct blkio *bio, unsigned flags)
{
	if (flags & (GF_ZERO | GF_DIRTY))
		bio->write_misses++;
	else
		bio->read_misses++;
}

static struct blkio_block *_lookup_or_read_block(struct blkio *bio,
				       int di, block_address_t i,
				       unsigned flags)
{
	struct blkio_block *b = _block_lookup(bio, di, i);

	if (b) {
		// FIXME: this is insufficient.  We need to also catch a read
		// lock of a write locked block.  Ref count needs to distinguish.
		if (b->ref_count && (flags & (GF_DIRTY | GF_ZERO))) {
			log_warn("concurrent write lock attempted.");
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

		b = _new_block(bio, di, i);
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

static void _preemptive_writeback(struct blkio *bio)
{
	// FIXME: this ignores those blocks that are in the error state.  Track
	// nr_clean instead?
	unsigned nr_available;

	if (bio->nr_dirty <= bio->nr_io_pending)
		return;

	nr_available = bio->nr_cache_blocks - (bio->nr_dirty - bio->nr_io_pending);
	if (nr_available < (WRITEBACK_LOW_THRESHOLD_PERCENT * bio->nr_cache_blocks / 100))
		_writeback(bio, (WRITEBACK_HIGH_THRESHOLD_PERCENT * bio->nr_cache_blocks / 100) - nr_available);

}

/*----------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------*/
struct blkio *blkio_create_engine(unsigned block_sectors,
				  unsigned nr_cache_blocks,
				  struct io_engine *engine)
{
	static long _pagesize = 0;
	struct blkio *bio;
	unsigned max_io = engine->max_io(engine);
	int i;

	if ((_pagesize <= 0) && ((_pagesize = sysconf(_SC_PAGESIZE)) < 0)) {
		log_warn("WARNING: _SC_PAGESIZE returns negative value.");
		return NULL;
	}

	if (!nr_cache_blocks) {
		log_warn("blkio must have at least one cache block.");
		return NULL;
	}

	if (!block_sectors) {
		log_warn("blkio must have a non zero block size.");
		return NULL;
	}

	if (block_sectors & ((_pagesize >> SECTOR_SHIFT) - 1)) {
		log_warn("blkio block size must be a multiple of page size.");
		return NULL;
	}

	bio = malloc(sizeof(*bio));
	if (!bio)
		return NULL;

	bio->block_sectors = block_sectors;
	bio->nr_cache_blocks = nr_cache_blocks;
	bio->max_io = nr_cache_blocks < max_io ? nr_cache_blocks : max_io;
	bio->engine = engine;
	bio->nr_locked = 0;
	bio->nr_dirty = 0;
	bio->nr_io_pending = 0;

	dm_list_init(&bio->free);
	dm_list_init(&bio->errored);
	dm_list_init(&bio->dirty);
	dm_list_init(&bio->clean);
	dm_list_init(&bio->io_pending);

	bio->rtree = radix_tree_create(NULL, NULL);
	if (!bio->rtree) {
		bio->engine->destroy(bio->engine);
		free(bio);
		return NULL;
	}

	bio->read_hits = 0;
	bio->read_misses = 0;
	bio->write_zeroes = 0;
	bio->write_hits = 0;
	bio->write_misses = 0;
	bio->prefetches = 0;

	if (!_init_free_list(bio, nr_cache_blocks, _pagesize)) {
		bio->engine->destroy(bio->engine);
		radix_tree_destroy(bio->rtree);
		free(bio);
		return NULL;
	}

	_fd_table_size = FD_TABLE_INC;

	if (!(_fd_table = malloc(sizeof(int) * _fd_table_size))) {
		_exit_free_list(bio);
		bio->engine->destroy(bio->engine);
		radix_tree_destroy(bio->rtree);
		free(bio);
		return NULL;
	}

	for (i = 0; i < _fd_table_size; i++)
		_fd_table[i] = -1;

	return bio;
}

/*
 * Pick the engine named by hint.  Auto tries the asynchronous engine first and
 * falls back to the synchronous one, which is also the final fallback for an
 * engine that is unavailable or not known at all.
 */
struct io_engine *blkio_create_io_engine(const char *hint)
{
	bool automatic = !hint || !*hint || !strcmp(hint, "auto");
	bool asynchronous = automatic || !strcmp(hint, "libaio") ||
			    !strcmp(hint, "async") || !strcmp(hint, "aio");
	struct io_engine *engine;

	if (asynchronous) {
		if ((engine = create_async_io_engine()))
			return engine;

		if (automatic)
			log_warn("WARNING: Failed to set up async io, using sync io.");
		else
			log_warn("WARNING: Requested libaio I/O engine is not available.");
	} else if (strcmp(hint, "sync"))
		log_warn("WARNING: Unknown I/O engine \"%s\".", hint);

	if (!(engine = create_sync_io_engine()))
		return_NULL;

	return engine;
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
	if (bio->nr_locked)
		log_warn("some blocks are still locked.");

	if (!blkio_flush(bio))
		stack;
	_exit_free_list(bio);
	radix_tree_destroy(bio->rtree);
	bio->engine->destroy(bio->engine);
	free(bio);
	free(_fd_table);
	_fd_table = NULL;
	_fd_table_size = 0;
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

	if (!_di_valid(di))
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

	if (!_di_valid(di))
		goto bad;

	b = _lookup_or_read_block(bio, di, index, flags);
	if (b) {
		if (b->error) {
			if (b->io_dir == BLKIO_DIR_READ) {
				// Now we know the read failed we can just forget
				// about this block, since there's no dirty data to
				// be written back.
				_recycle_block(b);
			}
			return false;
		}

		if (!b->ref_count)
			bio->nr_locked++;
		b->ref_count++;

		*result = b;
		return true;
	}
bad:

	log_error("blkio failed to get block %llu di %d.", (unsigned long long) index, di);
	return false;
}

/*----------------------------------------------------------------*/

static void _put_ref(struct blkio_block *b)
{
	if (!b->ref_count) {
		log_warn("ref count on blkio block already zero.");
		return;
	}

	b->ref_count--;
	if (!b->ref_count)
		b->bio->nr_locked--;
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

	if (_test_flags(b, BF_IO_PENDING))
		_wait_specific(b);

	if (b->ref_count) {
		log_warn("blkio invalidate: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		return false;
	}

	if (_test_flags(b, BF_DIRTY)) {
		_issue_write(b);
		_wait_specific(b);

		if (b->error)
        		return false;
	}

	_recycle_block(b);

	return true;
}

bool blkio_invalidate(struct blkio *bio, int di, block_address_t index)
{
	if (!_di_valid(di))
		return false;

	return _invalidate_block(_block_lookup(bio, di, index));
}

/*----------------------------------------------------------------*/

struct invalidate_iterator {
	bool success;
	struct radix_tree_iterator it;
};

static bool _writeback_v(struct radix_tree_iterator *it,
                         const void *kb, size_t keylen, union radix_value v)
{
	struct blkio_block *b = v.ptr;

	if (_test_flags(b, BF_DIRTY) && !b->ref_count)
		_issue_write(b);

	return true;
}

static bool _invalidate_v(struct radix_tree_iterator *it,
                          const void *kb, size_t keylen, union radix_value v)
{
	struct blkio_block *b = v.ptr;
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

	/*
	 * We can't remove the block from the radix tree yet because
	 * we're in the middle of an iteration.
	 */
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

/*----------------------------------------------------------------*/

static bool _abort_v(struct radix_tree_iterator *it,
                     const void *kb, size_t keylen, union radix_value v)
{
	struct blkio_block *b = v.ptr;

	if (b->ref_count) {
		log_fatal("blkio destroy: block (%d, %llu) still held.",
			 b->di, (unsigned long long) b->index);
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	/*
	 * We can't remove the block from the radix tree yet because
	 * we're in the middle of an iteration.
	 */
	return true;
}

bool blkio_abort_di(struct blkio *bio, int di)
{
	union key k;
	struct radix_tree_iterator it;

	k.parts.di = di;

	it.visit = _abort_v;
	radix_tree_iterate(bio->rtree, k.bytes, sizeof(k.parts.di), &it);
	(void) radix_tree_remove_prefix(bio->rtree, k.bytes, sizeof(k.parts.di));

	return true;
}

/*----------------------------------------------------------------*/

void blkio_set_last_byte(struct blkio *bio, int di, uint64_t offset, size_t sector_size)
{
	_last_byte_di = di;
	_last_byte_offset = offset;
	if (!sector_size)
		_last_byte_sector_size = 512;
	else
		_last_byte_sector_size = sector_size;
}

void blkio_unset_last_byte(struct blkio *bio, int di)
{
	if (_last_byte_di == di) {
		_last_byte_di = 0;
		_last_byte_offset = 0;
		_last_byte_sector_size = 0;
	}
}

int blkio_set_fd(int fd)
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
		log_error("Cannot extend blkio fd table.");
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

void blkio_clear_fd(int di)
{
	if (!_di_valid(di))
		return;
	_fd_table[di] = -1;
}

int blkio_change_fd(int di, int fd)
{
	if (!_di_valid(di))
		return 0;
	_fd_table[di] = fd;
	return 1;
}
