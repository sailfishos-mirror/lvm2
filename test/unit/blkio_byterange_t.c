/*
 * Copyright (C) 2018-2026 Red Hat, Inc. All rights reserved.
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

#include "units.h"
#include "lib/blkio/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>

//----------------------------------------------------------------

#define T_BLOCK_BYTES ((uint64_t) TEST_PAGE_SIZE)
#define NR_BLOCKS 64
#define INIT_PATTERN 123

static const struct io_engine _test_engine = {
	.name = "test",
};

struct fixture {
	int fd;
	int di;
	char fname[32];
	struct blkio *bio;
};

static inline uint8_t _pattern_at(uint8_t pat, uint8_t byte)
{
	return pat + byte;
}

static uint64_t byte(block_address_t b, uint64_t offset)
{
	return b * T_BLOCK_BYTES + offset;
}

/*
 * Find out once per process whether this file system accepts O_DIRECT.
 * Returns true when it does, false (probably a tmpfs) when it does not.
 */
static bool _detect_direct_io(void)
{
	static int _direct_io = -1;
	char fname[32];
	int fd;

	if (_direct_io != -1)
		return _direct_io;

	snprintf(fname, sizeof(fname), "unit-test-XXXXXX");
	/* coverity[secure_temp] don't care */
	fd = mkstemp(fname);
	T_ASSERT(fd >= 0);
	(void) close(fd);

	// test if we can reopen with O_DIRECT
	if ((fd = open(fname, O_RDWR | O_DIRECT)) >= 0) {
		_direct_io = 1;
		(void) close(fd);
	} else {
		_direct_io = 0; // likely running on tmpfs
		printf("  Running test in tmpfs, *NOT* using O_DIRECT\n");
	}
	(void) unlink(fname);

	return _direct_io;
}

static void *_fix_init(struct io_engine *engine)
{
	const size_t bsize = T_BLOCK_BYTES;
	uint8_t *buffer;
	struct fixture *f = malloc(sizeof(*f));
	unsigned b, i;

	buffer = alloca(bsize);

	memset(buffer, 0, bsize);
	T_ASSERT(f);

	snprintf(f->fname, sizeof(f->fname), "unit-test-XXXXXX");
	/* coverity[secure_temp] don't care */
	f->fd = mkstemp(f->fname);
	T_ASSERT(f->fd >= 0);

	for (b = 0; b < NR_BLOCKS; b++) {
		for (i = 0; i < bsize; i++)
			buffer[i] = _pattern_at(INIT_PATTERN, byte(b, i));
		T_ASSERT(write(f->fd, buffer, bsize) > 0);
	}

	if (_detect_direct_io()) {
		(void) close(f->fd);
		// reopen with O_DIRECT
		f->fd = open(f->fname, O_RDWR | O_DIRECT);
		T_ASSERT(f->fd >= 0);
	}

	f->bio = blkio_create_engine(bsize / 512, NR_BLOCKS, engine);
	T_ASSERT(f->bio);

	f->di = blkio_set_fd(f->bio, f->fd);

	return f;
}

#ifdef HAVE_LIBAIO_H
static void *_async_init(void)
{
	struct io_engine *e = create_async_io_engine(BLKIO_ENGINE_DEFAULT);
	T_ASSERT(e);
	/* coverity[leaked_storage] stored in fixture by _fix_init */
	return _fix_init(e);
}
#endif

static void *_sync_init(void)
{
	struct io_engine *e = create_sync_io_engine(BLKIO_ENGINE_DEFAULT);
	T_ASSERT(e);
	/* coverity[leaked_storage] stored in fixture by _fix_init */
	return _fix_init(e);
}

static void _fix_exit(void *fixture)
{
	struct fixture *f = fixture;

	if (f) {
		blkio_clear_fd(f->bio, f->di);
		blkio_destroy(f->bio);
		(void) close(f->fd);
		(void) unlink(f->fname);
		free(f);
	}
}

//----------------------------------------------------------------

static void _verify_bytes(struct blkio_block *b, uint64_t base,
			  uint64_t offset, uint64_t len, uint8_t pat)
{
	unsigned i;

	for (i = 0; i < len; i++)
		T_ASSERT_EQUAL(((uint8_t *) b->data)[offset + i], _pattern_at(pat, base + offset + i));
}

static void _verify(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t pat)
{
	struct blkio_block *b;
	block_address_t bb = byte_b / T_BLOCK_BYTES;
	block_address_t be = (byte_e + T_BLOCK_BYTES - 1) / T_BLOCK_BYTES;
	uint64_t offset = byte_b % T_BLOCK_BYTES;
	uint64_t blen, len = byte_e - byte_b;

	// Verify via blkio_read_bytes
	{
		unsigned i;
		size_t len2 = byte_e - byte_b;
		uint8_t *buffer = malloc(len2);

		T_ASSERT(buffer);
		memset(buffer, 0, len2);

		T_ASSERT(blkio_read_bytes(f->bio, f->di, byte_b, len2, buffer));
		for (i = 0; i < len; i++)
			T_ASSERT_EQUAL(buffer[i], _pattern_at(pat, byte_b + i));
		free(buffer);
	}

	// Verify again, driving the blkio directly
	for (; bb != be; bb++) {
		T_ASSERT(blkio_get(f->bio, f->di, bb, 0, &b));

		blen = min(T_BLOCK_BYTES - offset, len);
		_verify_bytes(b, bb * T_BLOCK_BYTES, offset, blen, pat);

		offset = 0;
		len -= blen;

		blkio_put(b);
	}
}

static void _verify_set(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t val)
{
	unsigned i;
	struct blkio_block *b;
	block_address_t bb = byte_b / T_BLOCK_BYTES;
	block_address_t be = (byte_e + T_BLOCK_BYTES - 1) / T_BLOCK_BYTES;
	uint64_t offset = byte_b % T_BLOCK_BYTES;
	uint64_t blen, len = byte_e - byte_b;

	for (; bb != be; bb++) {
		T_ASSERT(blkio_get(f->bio, f->di, bb, 0, &b));

		blen = min(T_BLOCK_BYTES - offset, len);
		for (i = 0; i < blen; i++)
			T_ASSERT(((uint8_t *) b->data)[offset + i] == val);

		offset = 0;
		len -= blen;

		blkio_put(b);
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
	memset(buffer, 0, len);

	for (i = 0; i < len; i++)
		buffer[i] = _pattern_at(pat, byte_b + i);

	T_ASSERT(blkio_write_bytes(f->bio, f->di, byte_b, byte_e - byte_b, buffer));
	free(buffer);
}

static void _do_zero(struct fixture *f, uint64_t byte_b, uint64_t byte_e)
{
	T_ASSERT(blkio_zero_bytes(f->bio, f->di, byte_b, byte_e - byte_b));
}

static void _do_set(struct fixture *f, uint64_t byte_b, uint64_t byte_e, uint8_t val)
{
	T_ASSERT(blkio_set_bytes(f->bio, f->di, byte_b, byte_e - byte_b, val));
}

/*
 * Flush dirty blocks to disk and drop all cached blocks so the
 * next read goes back to the file.  This replaces the old _reopen()
 * which destroyed and recreated the entire blkio + its io engine -
 * io_destroy() alone takes ~40ms, so avoiding it per-test saves
 * significant time.
 */
static void _flush_and_invalidate(struct fixture *f)
{
	T_ASSERT(blkio_flush(f->bio));
	blkio_invalidate_di(f->bio, f->di);
}

//----------------------------------------------------------------

static uint8_t _random_pattern(void)
{
	/* coverity[dont_call] don't care */
	return random();
}

static uint64_t _max_byte(void)
{
	return T_BLOCK_BYTES * NR_BLOCKS;
}

static void _rwv_cycle(struct fixture *f, uint64_t b, uint64_t e)
{
	uint8_t pat = _random_pattern();

	_verify(f, b, e, INIT_PATTERN);
	_do_write(f, b, e, pat);
	_flush_and_invalidate(f);
	_verify(f, b < 128 ? 0 : b - 128, b, INIT_PATTERN);
	_verify(f, b, e, pat);
	_verify(f, e, min(e + 128, _max_byte()), INIT_PATTERN);
}

static void _test_rw_first_block(void *fixture)
{
	_rwv_cycle(fixture, byte(0, 0), byte(0, T_BLOCK_BYTES));
}

static void _test_rw_last_block(void *fixture)
{
	uint64_t last_block = NR_BLOCKS - 1;
	_rwv_cycle(fixture, byte(last_block, 0),
		   byte(last_block, T_BLOCK_BYTES));
}

static void _test_rw_several_whole_blocks(void *fixture)
{
	_rwv_cycle(fixture, byte(5, 0), byte(10, 0));
}

static void _test_rw_within_single_block(void *fixture)
{
	_rwv_cycle(fixture, byte(7, 3), byte(7, T_BLOCK_BYTES / 2));
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
	_flush_and_invalidate(f);
	_verify(f, b < 128 ? 0 : b - 128, b, INIT_PATTERN);
	_verify_zeroes(f, b, e);
	_verify(f, e, min(e + 128, _max_byte()), INIT_PATTERN);
}

static void _test_zero_first_block(void *fixture)
{
	_zero_cycle(fixture, byte(0, 0), byte(0, T_BLOCK_BYTES));
}

static void _test_zero_last_block(void *fixture)
{
	uint64_t last_block = NR_BLOCKS - 1;
	_zero_cycle(fixture, byte(last_block, 0), byte(last_block, T_BLOCK_BYTES));
}

static void _test_zero_several_whole_blocks(void *fixture)
{
	_zero_cycle(fixture, byte(5, 0), byte(10, 0));
}

static void _test_zero_within_single_block(void *fixture)
{
	_zero_cycle(fixture, byte(7, 3), byte(7, T_BLOCK_BYTES / 2));
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
	uint8_t val = _random_pattern();

	_verify(f, b, e, INIT_PATTERN);
	_do_set(f, b, e, val);
	_flush_and_invalidate(f);
	_verify(f, b < 128 ? 0 : b - 128, b, INIT_PATTERN);
	_verify_set(f, b, e, val);
	_verify(f, e, min(e + 128, _max_byte()), INIT_PATTERN);
}

static void _test_set_first_block(void *fixture)
{
	_set_cycle(fixture, byte(0, 0), byte(0, T_BLOCK_BYTES));
}

static void _test_set_last_block(void *fixture)
{
	uint64_t last_block = NR_BLOCKS - 1;
	_set_cycle(fixture, byte(last_block, 0), byte(last_block, T_BLOCK_BYTES));
}

static void _test_set_several_whole_blocks(void *fixture)
{
	_set_cycle(fixture, byte(5, 0), byte(10, 0));
}

static void _test_set_within_single_block(void *fixture)
{
	_set_cycle(fixture, byte(7, 3), byte(7, T_BLOCK_BYTES / 2));
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

static void _test_completion_short_read(void *fixture)
{
	uint8_t buffer[T_BLOCK_BYTES];
	struct blkio_io io = {
		.d = BLKIO_DIR_READ,
		.data = buffer,
		.range = {
			.offset = 0,
			.len = sizeof(buffer),
		},
	};
	unsigned i;
	(void) fixture;

	memset(buffer, 0xAA, sizeof(buffer));

	/*
	 * A short read is a normal end of device condition: the unread
	 * tail of the block is zero filled and the read succeeds.
	 */
	T_ASSERT_EQUAL(blkio_completion_status(&_test_engine, &io, 512), 0);
	for (i = 0; i < 512; i++)
		T_ASSERT_EQUAL(buffer[i], 0xAA);
	for (; i < sizeof(buffer); i++)
		T_ASSERT_EQUAL(buffer[i], 0);

	/* A transfer shorter than a whole sector is an error. */
	T_ASSERT_EQUAL(blkio_completion_status(&_test_engine, &io, 0),
		       -ENODATA);
}

static void _test_completion_short_write(void *fixture)
{
	uint8_t buffer[T_BLOCK_BYTES];
	struct blkio_io io = {
		.d = BLKIO_DIR_WRITE,
		.data = buffer,
		.range = {
			.offset = 0,
			.len = sizeof(buffer),
		},
	};
	(void) fixture;

	T_ASSERT_EQUAL(blkio_completion_status(&_test_engine, &io,
					       sizeof(buffer)), 0);
	T_ASSERT(blkio_completion_status(&_test_engine, &io, 512) < 0);
}

static void _test_range_overflow(void *fixture)
{
	struct fixture *f = fixture;
	uint64_t start = UINT64_MAX - (T_BLOCK_BYTES - 1);
	size_t len = T_BLOCK_BYTES;
	uint8_t *buffer = alloca(T_BLOCK_BYTES);

	T_ASSERT(!blkio_read_bytes(f->bio, f->di, start, len, buffer));
	T_ASSERT(!blkio_write_bytes(f->bio, f->di, start, len, buffer));
	T_ASSERT(!blkio_zero_bytes(f->bio, f->di, start, len));
	T_ASSERT(!blkio_set_bytes(f->bio, f->di, start, len, 0));
	T_ASSERT(!blkio_invalidate_bytes(f->bio, f->di, start, len));

	/* Rounding a representable end must not overflow. */
	T_ASSERT(blkio_invalidate_bytes(f->bio, f->di, start, len - 1));
}

static void _test_zero_length(void *fixture)
{
	struct fixture *f = fixture;
	struct blkio_block *b;
	uint8_t data = 0;
	uint64_t start[2] = { byte(7, 0), byte(7, 3) };
	unsigned i;

	T_ASSERT(blkio_get(f->bio, f->di, 7, GF_DIRTY, &b));

	for (i = 0; i < DM_ARRAY_SIZE(start); i++) {
		T_ASSERT(blkio_read_bytes(f->bio, f->di, start[i], 0, &data));
		T_ASSERT(blkio_write_bytes(f->bio, f->di, start[i], 0, &data));
		T_ASSERT(blkio_zero_bytes(f->bio, f->di, start[i], 0));
		T_ASSERT(blkio_set_bytes(f->bio, f->di, start[i], 0, 0));
		T_ASSERT(blkio_invalidate_bytes(f->bio, f->di, start[i], 0));
	}

	blkio_put(b);
	T_ASSERT(blkio_flush(f->bio));
	_verify(f, byte(7, 0), byte(7, T_BLOCK_BYTES), INIT_PATTERN);
}

static struct test_suite *_completion_tests(void)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/blkio/byterange/completion/" path, desc, fn)
	T("short-read", "short read zero fills the tail of the block", _test_completion_short_read);
	T("short-write", "short write is an error", _test_completion_short_write);
#undef T

	return ts;
}

//----------------------------------------------------------------

#ifdef HAVE_LIBAIO_H
static struct test_suite *_async_tests(void)
{
	struct test_suite *ts = test_suite_create(_async_init, _fix_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/blkio/byterange/async/" path, desc, fn)
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
#endif

static struct test_suite *_sync_tests(void)
{
	struct test_suite *ts = test_suite_create(_sync_init, _fix_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/blkio/byterange/sync/" path, desc, fn)
	T("range-overflow", "overflowing byte range is rejected", _test_range_overflow);
	T("zero-length", "zero-length byte operations do not touch a block", _test_zero_length);
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

void blkio_byterange_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_completion_tests()->list);
#ifdef HAVE_LIBAIO_H
	dm_list_add(all_tests, &_async_tests()->list);
#endif
	dm_list_add(all_tests, &_sync_tests()->list);
}
