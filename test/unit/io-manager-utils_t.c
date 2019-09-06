/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/device/io-manager.h"
#include "framework.h"
#include "units.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>

//----------------------------------------------------------------

#define T_BLOCK_SIZE 4096
#define NR_BLOCKS 64
#define INIT_PATTERN 123

struct fixture {
	char fname[32];
	struct io_manager *iom;
	struct io_dev *dev;
};

static inline uint8_t _pattern_at(uint8_t pat, uint8_t byte)
{
	return pat + byte;
}

static uint64_t byte(block_address b, uint64_t offset)
{
	return b * T_BLOCK_SIZE + offset;
}

// With testing in tmpfs directory O_DIRECT cannot be used
// tmpfs has  f_fsid == 0  (unsure if this is best guess)
static bool _use_o_direct_internal(void)
{
	struct statvfs fsdata;

	if (statvfs(".", &fsdata))
		// assume we can
		return true;

	return fsdata.f_fsid;
}

static bool _use_o_direct(void)
{
	static bool latch = false;
	static bool result;

	if (!latch) {
		latch = true;
		result = _use_o_direct_internal();
		if (!result)
			printf("  Running test in tmpfs, *NOT* using O_DIRECT\n");
	}

	return result;
}

static void *_fix_init(struct io_engine *engine)
{
	int fd;
        uint8_t buffer[T_BLOCK_SIZE];
        struct fixture *f = malloc(sizeof(*f));
        unsigned b, i;

        T_ASSERT(f);

        snprintf(f->fname, sizeof(f->fname), "unit-test-XXXXXX");
	fd = mkstemp(f->fname);
	T_ASSERT(fd >= 0);

	for (b = 0; b < NR_BLOCKS; b++) {
        	for (i = 0; i < sizeof(buffer); i++)
                	buffer[i] = _pattern_at(INIT_PATTERN, byte(b, i));
		T_ASSERT(write(fd, buffer, T_BLOCK_SIZE) > 0);
	}
	close(fd);

	f->iom = io_manager_create(T_BLOCK_SIZE / 512, NR_BLOCKS, 256, engine);
	T_ASSERT(f->iom);

	f->dev = io_get_dev(f->iom, f->fname, 0);
	T_ASSERT(f->dev);

        return f;
}

static void *_async_init(void)
{
	struct io_engine *e = create_async_io_engine(_use_o_direct());
	T_ASSERT(e);
	return _fix_init(e);
}

static void *_sync_init(void)
{
	struct io_engine *e = create_sync_io_engine(_use_o_direct());
	T_ASSERT(e);
	return _fix_init(e);
}

static void _fix_exit(void *fixture)
{
        struct fixture *f = fixture;

	io_put_dev(f->dev);
	io_manager_destroy(f->iom);
	unlink(f->fname);
        free(f);
}

//----------------------------------------------------------------

static void _verify_bytes(struct block *b, uint64_t base,
                          uint64_t offset, uint64_t len, uint8_t pat)
{
	unsigned i;

	for (i = 0; i < len; i++)
		T_ASSERT_EQUAL(((uint8_t *) b->data)[offset + i], _pattern_at(pat, base + offset + i));
}

static uint64_t _min(uint64_t lhs, uint64_t rhs)
{
	return rhs < lhs ? rhs : lhs;
}

static void _verify(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t pat)
{
	struct block *b;
	block_address bb = byte_b / T_BLOCK_SIZE;
	block_address be = (byte_e + T_BLOCK_SIZE - 1) / T_BLOCK_SIZE;
	uint64_t offset = byte_b % T_BLOCK_SIZE;
	uint64_t blen, len = byte_e - byte_b;

	// Verify via io_read_bytes
	{
        	unsigned i;
        	size_t len2 = byte_e - byte_b;
		uint8_t *buffer = malloc(len2);
		T_ASSERT(io_read_bytes(f->iom, f->dev, byte_b, len2, buffer));
		for (i = 0; i < len; i++)
        		T_ASSERT_EQUAL(buffer[i], _pattern_at(pat, byte_b + i));
        	free(buffer);
	}

	// Verify again, driving io directly
	for (; bb != be; bb++) {
        	T_ASSERT(io_get_block(f->iom, f->dev, bb, 0, &b));

		blen = _min(T_BLOCK_SIZE - offset, len);
        	_verify_bytes(b, bb * T_BLOCK_SIZE, offset, blen, pat);

        	offset = 0;
        	len -= blen;

        	io_put_block(b);
	}
}

static void _verify_set(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t val)
{
        unsigned i;
	struct block *b;
	block_address bb = byte_b / T_BLOCK_SIZE;
	block_address be = (byte_e + T_BLOCK_SIZE - 1) / T_BLOCK_SIZE;
	uint64_t offset = byte_b % T_BLOCK_SIZE;
	uint64_t blen, len = byte_e - byte_b;

	for (; bb != be; bb++) {
        	T_ASSERT(io_get_block(f->iom, f->dev, bb, 0, &b));

		blen = _min(T_BLOCK_SIZE - offset, len);
		for (i = 0; i < blen; i++)
        		T_ASSERT(((uint8_t *) b->data)[offset + i] == val);

        	offset = 0;
        	len -= blen;

        	io_put_block(b);
	}
}

static void _verify_zeroes(struct fixture *f, uint64_t byte_b, uint64_t byte_e)
{
        _verify_set(f, byte_b, byte_e, 0);
}

static void _do_write(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t pat)
{
        unsigned i;
        size_t len = byte_e - byte_b;
        uint8_t *buffer = malloc(len);
        T_ASSERT(buffer);

        for (i = 0; i < len; i++)
		buffer[i] = _pattern_at(pat, byte_b + i);

        T_ASSERT(io_write_bytes(f->iom, f->dev, byte_b, byte_e - byte_b, buffer));
	free(buffer);
}

static void _do_zero(struct fixture *f, uint64_t byte_b, uint64_t byte_e)
{
	T_ASSERT(io_zero_bytes(f->iom, f->dev, byte_b, byte_e - byte_b));
}

static void _do_set(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t val)
{
	T_ASSERT(io_set_bytes(f->iom, f->dev, byte_b, byte_e - byte_b, val));
}

static void _reopen(struct fixture *f)
{
        struct io_engine *engine;

	io_put_dev(f->dev);
	io_manager_destroy(f->iom);

	engine = create_async_io_engine(_use_o_direct());
	T_ASSERT(engine);

	f->iom = io_manager_create(T_BLOCK_SIZE / 512, NR_BLOCKS, 256, engine);
	T_ASSERT(f->iom);

	f->dev = io_get_dev(f->iom, f->fname, 0);
	T_ASSERT(f->iom);
}

//----------------------------------------------------------------

static uint8_t _random_pattern(void)
{
	return random();
}

static uint64_t _max_byte(void)
{
        return T_BLOCK_SIZE * NR_BLOCKS;
}

static void _rwv_cycle(struct fixture *f, uint64_t b, uint64_t e)
{
	uint8_t pat = _random_pattern();

	_verify(f, b, e, INIT_PATTERN);
	_do_write(f, b, e, pat); 
	_reopen(f);
	_verify(f, b < 128 ? 0 : b - 128, b, INIT_PATTERN);
	_verify(f, b, e, pat);
	_verify(f, e, _min(e + 128, _max_byte()), INIT_PATTERN);
}

static void _test_rw_first_block(void *fixture)
{
	_rwv_cycle(fixture, byte(0, 0), byte(0, T_BLOCK_SIZE));
}

static void _test_rw_last_block(void *fixture)
{
        uint64_t last_block = NR_BLOCKS - 1;
	_rwv_cycle(fixture, byte(last_block, 0),
                   byte(last_block, T_BLOCK_SIZE)); 
}

static void _test_rw_several_whole_blocks(void *fixture)
{
        _rwv_cycle(fixture, byte(5, 0), byte(10, 0));
}

static void _test_rw_within_single_block(void *fixture)
{
        _rwv_cycle(fixture, byte(7, 3), byte(7, T_BLOCK_SIZE / 2));
}

static void _test_rw_cross_one_boundary(void *fixture)
{
        _rwv_cycle(fixture, byte(13, 43), byte(14, 43));
}

static void _test_rw_many_boundaries(void *fixture)
{
        _rwv_cycle(fixture, byte(13, 13), byte(23, 13));
}

//----------------------------------------------------------------

static void _zero_cycle(struct fixture *f, uint64_t b, uint64_t e)
{
	_verify(f, b, e, INIT_PATTERN);
	_do_zero(f, b, e); 
	_reopen(f);
	_verify(f, b < 128 ? 0 : b - 128, b, INIT_PATTERN);
	_verify_zeroes(f, b, e);
	_verify(f, e, _min(e + 128, _max_byte()), INIT_PATTERN);
}

static void _test_zero_first_block(void *fixture)
{
	_zero_cycle(fixture, byte(0, 0), byte(0, T_BLOCK_SIZE));
}

static void _test_zero_last_block(void *fixture)
{
        uint64_t last_block = NR_BLOCKS - 1;
	_zero_cycle(fixture, byte(last_block, 0), byte(last_block, T_BLOCK_SIZE)); 
}

static void _test_zero_several_whole_blocks(void *fixture)
{
        _zero_cycle(fixture, byte(5, 0), byte(10, 0));
}

static void _test_zero_within_single_block(void *fixture)
{
        _zero_cycle(fixture, byte(7, 3), byte(7, T_BLOCK_SIZE / 2));
}

static void _test_zero_cross_one_boundary(void *fixture)
{
        _zero_cycle(fixture, byte(13, 43), byte(14, 43));
}

static void _test_zero_many_boundaries(void *fixture)
{
        _zero_cycle(fixture, byte(13, 13), byte(23, 13));
}

//----------------------------------------------------------------

static void _set_cycle(struct fixture *f, uint64_t b, uint64_t e)
{
	uint8_t val = random();

	_verify(f, b, e, INIT_PATTERN);
	_do_set(f, b, e, val); 
	_reopen(f);
	_verify(f, b < 128 ? 0 : b - 128, b, INIT_PATTERN);
	_verify_set(f, b, e, val);
	_verify(f, e, _min(e + 128, _max_byte()), INIT_PATTERN);
}

static void _test_set_first_block(void *fixture)
{
	_set_cycle(fixture, byte(0, 0), byte(0, T_BLOCK_SIZE));
}

static void _test_set_last_block(void *fixture)
{
        uint64_t last_block = NR_BLOCKS - 1;
	_set_cycle(fixture, byte(last_block, 0), byte(last_block, T_BLOCK_SIZE)); 
}

static void _test_set_several_whole_blocks(void *fixture)
{
        _set_cycle(fixture, byte(5, 0), byte(10, 0));
}

static void _test_set_within_single_block(void *fixture)
{
        _set_cycle(fixture, byte(7, 3), byte(7, T_BLOCK_SIZE / 2));
}

static void _test_set_cross_one_boundary(void *fixture)
{
        _set_cycle(fixture, byte(13, 43), byte(14, 43));
}

static void _test_set_many_boundaries(void *fixture)
{
        _set_cycle(fixture, byte(13, 13), byte(23, 13));
}

//----------------------------------------------------------------

static struct test_suite *_async_tests(void)
{
        struct test_suite *ts = test_suite_create(_async_init, _fix_exit);
        if (!ts) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/utils/async/" path, desc, fn)
        T("rw-first-block", "read/write/verify the first block", _test_rw_first_block);
        T("rw-last-block", "read/write/verify the last block", _test_rw_last_block);
        T("rw-several-blocks", "read/write/verify several whole blocks", _test_rw_several_whole_blocks);
        T("rw-within-single-block", "read/write/verify within single block", _test_rw_within_single_block);
        T("rw-cross-one-boundary", "read/write/verify across one boundary", _test_rw_cross_one_boundary);
        T("rw-many-boundaries", "read/write/verify many boundaries", _test_rw_many_boundaries);

        T("zero-first-block", "zero the first block", _test_zero_first_block);
        T("zero-last-block", "zero the last block", _test_zero_last_block);
        T("zero-several-blocks", "zero several whole blocks", _test_zero_several_whole_blocks);
        T("zero-within-single-block", "zero within single block", _test_zero_within_single_block);
        T("zero-cross-one-boundary", "zero across one boundary", _test_zero_cross_one_boundary);
        T("zero-many-boundaries", "zero many boundaries", _test_zero_many_boundaries);

        T("set-first-block", "set the first block", _test_set_first_block);
        T("set-last-block", "set the last block", _test_set_last_block);
        T("set-several-blocks", "set several whole blocks", _test_set_several_whole_blocks);
        T("set-within-single-block", "set within single block", _test_set_within_single_block);
        T("set-cross-one-boundary", "set across one boundary", _test_set_cross_one_boundary);
        T("set-many-boundaries", "set many boundaries", _test_set_many_boundaries);
#undef T

        return ts;
}


static struct test_suite *_sync_tests(void)
{
        struct test_suite *ts = test_suite_create(_sync_init, _fix_exit);
        if (!ts) {
                fprintf(stderr, "out of memory\n");
                exit(1);
        }

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/utils/sync/" path, desc, fn)
        T("rw-first-block", "read/write/verify the first block", _test_rw_first_block);
        T("rw-last-block", "read/write/verify the last block", _test_rw_last_block);
        T("rw-several-blocks", "read/write/verify several whole blocks", _test_rw_several_whole_blocks);
        T("rw-within-single-block", "read/write/verify within single block", _test_rw_within_single_block);
        T("rw-cross-one-boundary", "read/write/verify across one boundary", _test_rw_cross_one_boundary);
        T("rw-many-boundaries", "read/write/verify many boundaries", _test_rw_many_boundaries);

        T("zero-first-block", "zero the first block", _test_zero_first_block);
        T("zero-last-block", "zero the last block", _test_zero_last_block);
        T("zero-several-blocks", "zero several whole blocks", _test_zero_several_whole_blocks);
        T("zero-within-single-block", "zero within single block", _test_zero_within_single_block);
        T("zero-cross-one-boundary", "zero across one boundary", _test_zero_cross_one_boundary);
        T("zero-many-boundaries", "zero many boundaries", _test_zero_many_boundaries);

        T("set-first-block", "set the first block", _test_set_first_block);
        T("set-last-block", "set the last block", _test_set_last_block);
        T("set-several-blocks", "set several whole blocks", _test_set_several_whole_blocks);
        T("set-within-single-block", "set within single block", _test_set_within_single_block);
        T("set-cross-one-boundary", "set across one boundary", _test_set_cross_one_boundary);
        T("set-many-boundaries", "set many boundaries", _test_set_many_boundaries);
#undef T

        return ts;
}

void io_manager_utils_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_async_tests()->list);
	dm_list_add(all_tests, &_sync_tests()->list);
}

