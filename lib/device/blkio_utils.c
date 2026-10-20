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

#include "lib/device/blkio.h"

//----------------------------------------------------------------

static bool _byte_range_to_block_range(struct blkio *bio, uint64_t start, size_t len,
				       block_address *bb, block_address *be)
{
	block_address block_size = blkio_block_sectors(bio) << SECTOR_SHIFT;

	if ((uint64_t)len > UINT64_MAX - start)
		return false;

	*bb = start / block_size;
	*be = (start + len + block_size - 1) / block_size;
	return true;
}

static uint64_t _min(uint64_t lhs, uint64_t rhs)
{
	if (rhs < lhs)
		return rhs;

	return lhs;
}

//----------------------------------------------------------------

void blkio_prefetch_bytes(struct blkio *bio, int di, uint64_t start, size_t len)
{
	block_address bb, be;

	if (!_byte_range_to_block_range(bio, start, len, &bb, &be))
		return;
	while (bb < be) {
		blkio_prefetch(bio, di, bb);
		bb++;
	}
}

//----------------------------------------------------------------

bool blkio_read_bytes(struct blkio *bio, int di, uint64_t start, size_t len, void *data)
{
	struct block *b;
	block_address bb, be;
	uint64_t block_size = blkio_block_sectors(bio) << SECTOR_SHIFT;
	uint64_t block_offset = start % block_size;
	size_t blen;

	blkio_prefetch_bytes(bio, di, start, len);

	if (!_byte_range_to_block_range(bio, start, len, &bb, &be))
		return false;

	for (; bb != be; bb++) {
        	if (!blkio_get(bio, di, bb, 0, &b))
			return false;

		blen = _min(block_size - block_offset, len);
		memcpy(data, ((unsigned char *) b->data) + block_offset, blen);
		blkio_put(b);

		block_offset = 0;
		len -= blen;
		data = ((unsigned char *) data) + blen;
	}

	return true;
}

bool blkio_invalidate_bytes(struct blkio *bio, int di, uint64_t start, size_t len)
{
	block_address bb, be;
	bool result = true;

	if (!_byte_range_to_block_range(bio, start, len, &bb, &be))
		return false;

	for (; bb != be; bb++) {
		if (!blkio_invalidate(bio, di, bb))
			result = false;
	}

	return result;
}

//----------------------------------------------------------------

// Writing bytes and zeroing bytes are very similar, so we factor out
// this common code.

struct updater;

typedef bool (*partial_update_fn)(struct updater *u, int di, block_address bb, uint64_t offset, size_t len);
typedef bool (*whole_update_fn)(struct updater *u, int di, block_address bb, block_address be);

struct updater {
	struct blkio *bio;
	partial_update_fn partial_fn;
	whole_update_fn whole_fn;
	void *data;
};

static bool _update_bytes(struct updater *u, int di, uint64_t start, size_t len)
{
        struct blkio *bio = u->bio;
	block_address bb, be;
	uint64_t block_size = blkio_block_sectors(bio) << SECTOR_SHIFT;
	uint64_t block_offset = start % block_size;
	uint64_t nr_whole;

	if (!_byte_range_to_block_range(bio, start, len, &bb, &be))
		return false;

	// If the last block is partial, we will require a read, so let's
	// prefetch it.
	if ((start + len) % block_size)
        	blkio_prefetch(bio, di, (start + len) / block_size);

	// First block may be partial
	if (block_offset) {
        	size_t blen = _min(block_size - block_offset, len);
		if (!u->partial_fn(u, di, bb, block_offset, blen))
        		return false;

		len -= blen;
        	if (!len)
                	return true;

                bb++;
	}

        // Now we write out a set of whole blocks
        nr_whole = len / block_size;
        if (!u->whole_fn(u, di, bb, bb + nr_whole))
                return false;

	bb += nr_whole;
	len -= nr_whole * block_size;

	if (!len)
        	return true;

        // Finally we write a partial end block
        return u->partial_fn(u, di, bb, 0, len);
}

//----------------------------------------------------------------

static bool _write_partial(struct updater *u, int di, block_address bb,
                           uint64_t offset, size_t len)
{
	struct block *b;

	if (!blkio_get(u->bio, di, bb, GF_DIRTY, &b))
		return false;

	if (u->data) {
		memcpy(((unsigned char *) b->data) + offset, u->data, len);
		u->data = ((unsigned char *) u->data) + len;
	}

	blkio_put(b);
	return true;
}

static bool _write_whole(struct updater *u, int di, block_address bb, block_address be)
{
	struct block *b;
	uint64_t block_size = blkio_block_sectors(u->bio) << SECTOR_SHIFT;

	for (; bb != be; bb++) {
        	// We don't need to read the block since we are overwriting
        	// it completely.
		if (!blkio_get(u->bio, di, bb, GF_ZERO, &b))
        		return false;
		memcpy(b->data, u->data, block_size);
		u->data = ((unsigned char *) u->data) + block_size;
        	blkio_put(b);
	}

	return true;
}

bool blkio_write_bytes(struct blkio *bio, int di, uint64_t start, size_t len, const void *data)
{
        struct updater u;

        u.bio = bio;
        u.partial_fn = _write_partial;
        u.whole_fn = _write_whole;
        u.data = (void *) data;

	return _update_bytes(&u, di, start, len);
}

//----------------------------------------------------------------

static bool _zero_partial(struct updater *u, int di, block_address bb, uint64_t offset, size_t len)
{
	struct block *b;

	if (!blkio_get(u->bio, di, bb, GF_DIRTY, &b))
		return false;

	memset(((unsigned char *) b->data) + offset, 0, len);
	blkio_put(b);

	return true;
}

static bool _zero_whole(struct updater *u, int di, block_address bb, block_address be)
{
	struct block *b;

	for (; bb != be; bb++) {
		if (!blkio_get(u->bio, di, bb, GF_ZERO, &b))
        		return false;
        	blkio_put(b);
	}

	return true;
}

bool blkio_zero_bytes(struct blkio *bio, int di, uint64_t start, size_t len)
{
        struct updater u;

        u.bio = bio;
        u.partial_fn = _zero_partial;
        u.whole_fn = _zero_whole;
        u.data = NULL;

	return _update_bytes(&u, di, start, len);
}

//----------------------------------------------------------------

static bool _set_partial(struct updater *u, int di, block_address bb, uint64_t offset, size_t len)
{
	struct block *b;
	uint8_t val = (u->data) ? *((uint8_t *) u->data) : 0;

	if (!blkio_get(u->bio, di, bb, GF_DIRTY, &b))
		return false;

	memset(((unsigned char *) b->data) + offset, val, len);
	blkio_put(b);

	return true;
}

static bool _set_whole(struct updater *u, int di, block_address bb, block_address be)
{
	struct block *b;
	uint8_t val = (u->data) ? *((uint8_t *) u->data) : 0;
	uint64_t len = blkio_block_sectors(u->bio) << SECTOR_SHIFT;

	for (; bb != be; bb++) {
		if (!blkio_get(u->bio, di, bb, GF_ZERO, &b))
        		return false;
        	memset((unsigned char *) b->data, val, len);
        	blkio_put(b);
	}

	return true;
}

bool blkio_set_bytes(struct blkio *bio, int di, uint64_t start, size_t len, uint8_t val)
{
        struct updater u;

        u.bio = bio;
        u.partial_fn = _set_partial;
        u.whole_fn = _set_whole;
        u.data = &val;

	return _update_bytes(&u, di, start, len);
}

