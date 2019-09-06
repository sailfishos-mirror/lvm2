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

#include "lib/device/io-manager.h"

// FIXME: need to define this in a common place (that doesn't pull in deps)
#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT 9
#endif

//----------------------------------------------------------------

static void byte_range_to_block_range(struct io_manager *iom, uint64_t start, size_t len,
				      block_address *bb, block_address *be)
{
	block_address block_size = io_block_sectors(iom) << SECTOR_SHIFT;
	*bb = start / block_size;
	*be = (start + len + block_size - 1) / block_size;
}

static uint64_t _min(uint64_t lhs, uint64_t rhs)
{
	if (rhs < lhs)
		return rhs;

	return lhs;
}

//----------------------------------------------------------------

void io_prefetch_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len)
{
	block_address bb, be;

	byte_range_to_block_range(iom, start, len, &bb, &be);
	while (bb < be) {
		io_prefetch_block(iom, dev, bb);
		bb++;
	}
}

//----------------------------------------------------------------

bool io_read_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len, void *data)
{
	struct block *b;
	block_address bb, be;
	uint64_t block_size = io_block_sectors(iom) << SECTOR_SHIFT;
	uint64_t block_offset = start % block_size;

	io_prefetch_bytes(iom, dev, start, len);

	byte_range_to_block_range(iom, start, len, &bb, &be);

	for (; bb != be; bb++) {
        	if (!io_get_block(iom, dev, bb, 0, &b))
			return false;

		size_t blen = _min(block_size - block_offset, len);
		memcpy(data, ((unsigned char *) b->data) + block_offset, blen);
		io_put_block(b);

		block_offset = 0;
		len -= blen;
		data = ((unsigned char *) data) + blen;
	}

	return true;
}

//----------------------------------------------------------------

// Writing bytes and zeroing bytes are very similar, so we factor out
// this common code.
 
struct updater;

typedef bool (*partial_update_fn)(struct updater *u, struct io_dev *dev, block_address bb, uint64_t offset, size_t len);
typedef bool (*whole_update_fn)(struct updater *u, struct io_dev *dev, block_address bb, block_address be);

struct updater {
	struct io_manager *iom;
	partial_update_fn partial_fn;
	whole_update_fn whole_fn;
	void *data;
};

static bool _update_bytes(struct updater *u, struct io_dev *dev, uint64_t start, size_t len)
{
        struct io_manager *iom = u->iom;
	block_address bb, be;
	uint64_t block_size = io_block_sectors(iom) << SECTOR_SHIFT;
	uint64_t block_offset = start % block_size;
	uint64_t nr_whole;

	byte_range_to_block_range(iom, start, len, &bb, &be);

	// If the last block is partial, we will require a read, so let's 
	// prefetch it.
	if ((start + len) % block_size)
        	io_prefetch_block(iom, dev, (start + len) / block_size);

	// First block may be partial
	if (block_offset) {
        	size_t blen = _min(block_size - block_offset, len);
		if (!u->partial_fn(u, dev, bb, block_offset, blen))
        		return false;

		len -= blen;
        	if (!len)
                	return true;

                bb++;
	}

        // Now we write out a set of whole blocks
        nr_whole = len / block_size;
        if (!u->whole_fn(u, dev, bb, bb + nr_whole))
                return false;

	bb += nr_whole;
	len -= nr_whole * block_size;

	if (!len)
        	return true;

        // Finally we write a partial end block
        return u->partial_fn(u, dev, bb, 0, len);
}

// Return a mask with a bit set for each sector touched by the region.
// To be used with io_get_block_mask().
static uint64_t _region_to_mask(uint64_t offset, size_t len)
{
	unsigned i;
	uint64_t r = 0;
	unsigned sb = offset >> SECTOR_SHIFT;
	unsigned se = (offset + len + ((1 << SECTOR_SHIFT) - 1)) >> SECTOR_SHIFT;

	for (i = sb; i < se; i++)
		r |=  1ull << i;

	return r;
}

//----------------------------------------------------------------

static bool _write_partial(struct updater *u, struct io_dev *dev, block_address bb,
                           uint64_t offset, size_t len)
{
	struct block *b;

	if (!io_get_block_mask(u->iom, dev, bb, GF_DIRTY,
                               _region_to_mask(offset, len), &b))
		return false;

	memcpy(((unsigned char *) b->data) + offset, u->data, len);
	u->data = ((unsigned char *) u->data) + len;

	io_put_block(b);
	return true;
}

static bool _write_whole(struct updater *u, struct io_dev *dev, block_address bb, block_address be)
{
	struct block *b;
	uint64_t block_size = io_block_sectors(u->iom) << SECTOR_SHIFT;

	for (; bb != be; bb++) {
        	// We don't need to read the block since we are overwriting
        	// it completely.
		if (!io_get_block(u->iom, dev, bb, GF_ZERO, &b))
        		return false;
		memcpy(b->data, u->data, block_size);
		u->data = ((unsigned char *) u->data) + block_size;
        	io_put_block(b);
	}

	return true;
}

bool io_write_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len, void *data)
{
        struct updater u;

        u.iom = iom;
        u.partial_fn = _write_partial;
        u.whole_fn = _write_whole;
        u.data = data;

	return _update_bytes(&u, dev, start, len);
}

//----------------------------------------------------------------

static bool _zero_partial(struct updater *u, struct io_dev *dev, block_address bb, uint64_t offset, size_t len)
{
	struct block *b;

	if (!io_get_block_mask(u->iom, dev, bb, GF_DIRTY,
                               _region_to_mask(offset, len), &b))
		return false;

	memset(((unsigned char *) b->data) + offset, 0, len);
	io_put_block(b);

	return true;
}

static bool _zero_whole(struct updater *u, struct io_dev *dev, block_address bb, block_address be)
{
	struct block *b;

	for (; bb != be; bb++) {
		if (!io_get_block(u->iom, dev, bb, GF_ZERO, &b))
        		return false;
        	io_put_block(b);
	}

	return true;
}

bool io_zero_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len)
{
        struct updater u;

        u.iom = iom;
        u.partial_fn = _zero_partial;
        u.whole_fn = _zero_whole;
        u.data = NULL;

	return _update_bytes(&u, dev, start, len);
}

//----------------------------------------------------------------

static bool _set_partial(struct updater *u, struct io_dev *dev, block_address bb, uint64_t offset, size_t len)
{
	struct block *b;
	uint8_t val = *((uint8_t *) u->data);

	if (!io_get_block_mask(u->iom, dev, bb, GF_DIRTY,
                               _region_to_mask(offset, len), &b))
		return false;

	memset(((unsigned char *) b->data) + offset, val, len);
	io_put_block(b);

	return true;
}

static bool _set_whole(struct updater *u, struct io_dev *dev, block_address bb, block_address be)
{
	struct block *b;
	uint8_t val = *((uint8_t *) u->data);
        uint64_t len = io_block_sectors(u->iom) * 512;

	for (; bb != be; bb++) {
		if (!io_get_block(u->iom, dev, bb, GF_ZERO, &b))
        		return false;
        	memset((unsigned char *) b->data, val, len);
        	io_put_block(b);
	}

	return true;
}

bool io_set_bytes(struct io_manager *iom, struct io_dev *dev, uint64_t start, size_t len, uint8_t val)
{
        struct updater u;

        u.iom = iom;
        u.partial_fn = _set_partial;
        u.whole_fn = _set_whole;
        u.data = &val;

	return _update_bytes(&u, dev, start, len);
}

