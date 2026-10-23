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

/*----------------------------------------------------------------*/

/*
 * The number of bytes one cache block holds.  Every byte range walk needs it
 * to turn a byte offset into a block index and an offset within that block.
 */
static size_t _block_bytes(struct blkio *bio)
{
	return (size_t) blkio_block_sectors(bio) << SECTOR_SHIFT;
}

/*
 * Map a byte range to the block range it touches.  be is the exclusive end
 * and may be NULL when the caller only needs the first block.
 */
static bool _byte_range_to_block_range(struct blkio *bio, uint64_t start, size_t len,
				       block_address_t *bb, block_address_t *be)
{
	const size_t block_bytes = _block_bytes(bio);
	uint64_t end;

	if ((uint64_t) len > UINT64_MAX - start) {
		log_error("blkio byte range at %llu len %llu wraps around.",
			  (unsigned long long) start, (unsigned long long) len);
		return false;
	}

	end = start + (uint64_t) len;
	*bb = start / block_bytes;
	/*
	 * Round the exclusive end up using the remainder, avoiding overflow
	 * from adding block_bytes - 1.  Only round nonempty requests so that
	 * zero-length ranges have bb == be even when start is unaligned.
	 */
	if (be)
		*be = end / block_bytes + (len && (end % block_bytes));
	return true;
}

/*----------------------------------------------------------------*/

void blkio_prefetch_bytes(struct blkio *bio, int di, uint64_t start, size_t len)
{
	block_address_t bb, be;

	if (!_byte_range_to_block_range(bio, start, len, &bb, &be))
		return;
	for (; bb < be; bb++)
		blkio_prefetch(bio, di, bb);
}

/*----------------------------------------------------------------*/

bool blkio_invalidate_bytes(struct blkio *bio, int di, uint64_t start, size_t len)
{
	block_address_t bb, be;
	bool result = true;

	if (!_byte_range_to_block_range(bio, start, len, &bb, &be))
		return_false;

	for (; bb < be; bb++) {
		if (!blkio_invalidate(bio, di, bb))
			result = false;
	}

	return result;
}

/*----------------------------------------------------------------*/

/*
 * Reads, writes and sets walk a byte range the same way, so the common
 * walk takes the per-block work as callbacks.
 */

struct updater;

typedef bool (*partial_update_fn)(struct updater *u, int di, block_address_t bb, uint64_t offset, size_t len);
typedef bool (*whole_update_fn)(struct updater *u, int di, block_address_t bb, block_address_t be);

struct updater {
	struct blkio *bio;
	partial_update_fn partial_fn;
	whole_update_fn whole_fn;
	void *data;
	bool prefetch_whole;	/* Zero for writes and sets; see _update_bytes() */
};

static bool _update_bytes(struct updater *u, int di, uint64_t start, size_t len)
{
	struct blkio *bio = u->bio;
	block_address_t bb;
	size_t block_bytes = _block_bytes(bio);
	size_t block_offset = start % block_bytes;
	uint64_t nr_whole;

	if (!len)
		return true;

	if (!_byte_range_to_block_range(bio, start, len, &bb, NULL))
		return_false;

	/*
	 * Reading needs the whole range now, so issue it all before waiting
	 * on the first block.  Otherwise only a partial last block needs a
	 * read, so prefetch just that block.
	 */
	if (u->prefetch_whole)
		blkio_prefetch_bytes(bio, di, start, len);
	else if ((start + len) % block_bytes)
		blkio_prefetch(bio, di, (start + len) / block_bytes);

	/* First block may be partial */
	if (block_offset) {
		size_t blen = min(block_bytes - block_offset, len);
		if (!u->partial_fn(u, di, bb, block_offset, blen))
			return_false;

		len -= blen;
		if (!len)
			return true;

		bb++;
	}

	/* Now process a set of whole blocks */
	nr_whole = len / block_bytes;
	if (!u->whole_fn(u, di, bb, bb + nr_whole))
		return_false;

	bb += nr_whole;
	len -= nr_whole * block_bytes;

	if (!len)
		return true;

	/* Finally process a partial end block */
	if (!u->partial_fn(u, di, bb, 0, len))
		return_false;

	return true;
}

/*----------------------------------------------------------------*/

static bool _read_partial(struct updater *u, int di, block_address_t bb,
                          uint64_t offset, size_t len)
{
	struct blkio_block *b;

	if (!blkio_get(u->bio, di, bb, 0, &b))
		return_false;

	memcpy(u->data, ((unsigned char *) b->data) + offset, len);
	u->data = ((unsigned char *) u->data) + len;

	blkio_put(b);
	return true;
}

static bool _read_whole(struct updater *u, int di, block_address_t bb, block_address_t be)
{
	struct blkio_block *b;
	size_t block_bytes = _block_bytes(u->bio);

	for (; bb < be; bb++) {
		if (!blkio_get(u->bio, di, bb, 0, &b))
			return_false;

		memcpy(u->data, b->data, block_bytes);
		u->data = ((unsigned char *) u->data) + block_bytes;

		blkio_put(b);
	}

	return true;
}

bool blkio_read_bytes(struct blkio *bio, int di, uint64_t start, size_t len, void *data)
{
	struct updater u = {
		.bio = bio,
		.partial_fn = _read_partial,
		.whole_fn = _read_whole,
		.data = data,
		.prefetch_whole = true,
	};

	return _update_bytes(&u, di, start, len);
}

/*----------------------------------------------------------------*/

static bool _write_partial(struct updater *u, int di, block_address_t bb,
                           uint64_t offset, size_t len)
{
	struct blkio_block *b;

	if (!blkio_get(u->bio, di, bb, GF_DIRTY, &b))
		return_false;

	memcpy(((unsigned char *) b->data) + offset, u->data, len);
	u->data = ((unsigned char *) u->data) + len;

	blkio_put(b);
	return true;
}

static bool _write_whole(struct updater *u, int di, block_address_t bb, block_address_t be)
{
	struct blkio_block *b;
	size_t block_bytes = _block_bytes(u->bio);

	for (; bb < be; bb++) {
		/*
		 * We don't need to read the block since we are overwriting
		 * it completely.
		 */
		if (!blkio_get(u->bio, di, bb, GF_ZERO, &b))
			return_false;
		memcpy(b->data, u->data, block_bytes);
		u->data = ((unsigned char *) u->data) + block_bytes;
		blkio_put(b);
	}

	return true;
}

bool blkio_write_bytes(struct blkio *bio, int di, uint64_t start, size_t len, const void *data)
{
	struct updater u = {
		.bio = bio,
		.partial_fn = _write_partial,
		.whole_fn = _write_whole,
		.data = (void *) data,
	};

	return _update_bytes(&u, di, start, len);
}

/*----------------------------------------------------------------*/

bool blkio_zero_bytes(struct blkio *bio, int di, uint64_t start, size_t len)
{
	return blkio_set_bytes(bio, di, start, len, 0);
}

/*----------------------------------------------------------------*/

static bool _set_partial(struct updater *u, int di, block_address_t bb, uint64_t offset, size_t len)
{
	struct blkio_block *b;
	uint8_t val = *((uint8_t *) u->data);

	if (!blkio_get(u->bio, di, bb, GF_DIRTY, &b))
		return_false;

	memset(((unsigned char *) b->data) + offset, val, len);
	blkio_put(b);

	return true;
}

static bool _set_whole(struct updater *u, int di, block_address_t bb, block_address_t be)
{
	struct blkio_block *b;
	uint8_t val = *((uint8_t *) u->data);
	size_t len = _block_bytes(u->bio);

	for (; bb < be; bb++) {
		if (!blkio_get(u->bio, di, bb, GF_ZERO, &b))
			return_false;
		memset((unsigned char *) b->data, val, len);
		blkio_put(b);
	}

	return true;
}

bool blkio_set_bytes(struct blkio *bio, int di, uint64_t start, size_t len, uint8_t val)
{
	struct updater u = {
		.bio = bio,
		.partial_fn = _set_partial,
		.whole_fn = _set_whole,
		.data = &val,
	};

	return _update_bytes(&u, di, start, len);
}
