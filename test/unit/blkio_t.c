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
#include <unistd.h>

#define SHOW_MOCK_CALLS 0

/*----------------------------------------------------------------
 * Mock engine
 *--------------------------------------------------------------*/
struct mock_engine {
	struct io_engine e;
	struct dm_list expected_calls;
	struct dm_list issued_io;
	unsigned max_io;
	sector_t block_sectors;
};

enum method {
	E_DESTROY,
	E_ISSUE,
	E_WAIT,
	E_MAX_IO
};

/* How an expected issue() call should behave. */
enum mock_outcome {
	MOCK_OK,
	MOCK_BAD_ISSUE,
	MOCK_BAD_WAIT
};

struct mock_call {
	struct dm_list list;
	enum method m;

	bool match_args;
	enum blkio_dir d;
	int di;
	block_address_t b;
	bool issue_r;
	bool wait_r;
};

struct mock_io {
	struct dm_list list;
	int di;
	struct blkio_io io;
	void *context;
	bool r;
};

static const char *_show_method(enum method m)
{
	switch (m) {
	case E_DESTROY:
		return "destroy()";
	case E_ISSUE:
		return "issue()";
	case E_WAIT:
		return "wait()";
	case E_MAX_IO:
		return "max_io()";
	}

	return "<unknown>";
}

static void _expect(struct mock_engine *e, enum method m)
{
	struct mock_call *mc = malloc(sizeof(*mc));

	T_ASSERT(mc);
	mc->m = m;
	mc->match_args = false;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_issue(struct mock_engine *e, enum blkio_dir d, int di,
			  block_address_t b, bool match_args,
			  enum mock_outcome outcome)
{
	struct mock_call *mc = malloc(sizeof(*mc));

	T_ASSERT(mc);
	mc->m = E_ISSUE;
	mc->match_args = match_args;
	mc->d = d;
	mc->di = di;
	mc->b = b;
	mc->issue_r = (outcome != MOCK_BAD_ISSUE);
	mc->wait_r = (outcome != MOCK_BAD_WAIT);
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read(struct mock_engine *e, int di, block_address_t b)
{
	_expect_issue(e, BLKIO_DIR_READ, di, b, true, MOCK_OK);
}

static void _expect_read_any(struct mock_engine *e)
{
	_expect_issue(e, BLKIO_DIR_READ, 0, 0, false, MOCK_OK);
}

static void _expect_write(struct mock_engine *e, int di, block_address_t b)
{
	_expect_issue(e, BLKIO_DIR_WRITE, di, b, true, MOCK_OK);
}

static void _expect_read_bad_issue(struct mock_engine *e, int di, block_address_t b)
{
	_expect_issue(e, BLKIO_DIR_READ, di, b, true, MOCK_BAD_ISSUE);
}

static void _expect_write_bad_issue(struct mock_engine *e, int di, block_address_t b)
{
	_expect_issue(e, BLKIO_DIR_WRITE, di, b, true, MOCK_BAD_ISSUE);
}

static void _expect_read_bad_wait(struct mock_engine *e, int di, block_address_t b)
{
	_expect_issue(e, BLKIO_DIR_READ, di, b, true, MOCK_BAD_WAIT);
}

static void _expect_write_bad_wait(struct mock_engine *e, int di, block_address_t b)
{
	_expect_issue(e, BLKIO_DIR_WRITE, di, b, true, MOCK_BAD_WAIT);
}

static struct mock_call *_match_pop(struct mock_engine *e, enum method m)
{

	struct mock_call *mc;

	if (dm_list_empty(&e->expected_calls))
		test_fail("unexpected call to method %s\n", _show_method(m));

	mc = dm_list_item(e->expected_calls.n, struct mock_call);
	dm_list_del(&mc->list);

	if (mc->m != m)
		test_fail("expected %s, but got %s\n", _show_method(mc->m), _show_method(m));
#if SHOW_MOCK_CALLS
	else
		fprintf(stderr, "%s called (expected)\n", _show_method(m));
#endif

	return mc;
}

static void _match(struct mock_engine *e, enum method m)
{
	free(_match_pop(e, m));
}

static void _no_outstanding_expectations(struct mock_engine *e)
{
	struct mock_call *mc;

	if (!dm_list_empty(&e->expected_calls)) {
		fprintf(stderr, "unsatisfied expectations:\n");
		dm_list_iterate_items (mc, &e->expected_calls)
			fprintf(stderr, "  %s\n", _show_method(mc->m));
	}
	T_ASSERT(dm_list_empty(&e->expected_calls));
}

static struct mock_engine *_to_mock(struct io_engine *e)
{
	return container_of(e, struct mock_engine, e);
}

static bool _mock_destroy(struct io_engine *e)
{
	struct mock_engine *me = _to_mock(e);

	_match(me, E_DESTROY);
	T_ASSERT(dm_list_empty(&me->issued_io));
	T_ASSERT(dm_list_empty(&me->expected_calls));
	/* struct io_engine is the first member; free the caller's pointer. */
	free(e);

	return true;
}

static int _mock_issue(struct io_engine *e, const struct blkio_io *bio_io,
		       void *context)
{
	sector_t sb = bio_io->range.offset >> SECTOR_SHIFT;
	sector_t se = (bio_io->range.offset + bio_io->range.len) >> SECTOR_SHIFT;
	bool r, wait_r;
	struct mock_io *io;
	struct mock_call *mc;
	struct mock_engine *me = _to_mock(e);

	mc = _match_pop(me, E_ISSUE);
	if (mc->match_args) {
		T_ASSERT(bio_io->d == mc->d);
		T_ASSERT(bio_io->fd == mc->di);
		T_ASSERT(sb == mc->b * me->block_sectors);
		T_ASSERT(se == (mc->b + 1) * me->block_sectors);
	}
	r = mc->issue_r;
	wait_r = mc->wait_r;
	free(mc);

	if (r) {
		io = malloc(sizeof(*io));
		if (!io)
			abort();

		io->di = bio_io->fd;
		io->io = *bio_io;
		io->context = context;
		io->r = wait_r;

		dm_list_add(&me->issued_io, &io->list);
	}

	return r ? 0 : -EIO;
}

static bool _mock_wait(struct io_engine *e, io_complete_fn fn)
{
	struct mock_io *io;
	struct mock_engine *me = _to_mock(e);
	_match(me, E_WAIT);

	// FIXME: provide a way to control how many are completed and whether
	// they error.
	T_ASSERT(!dm_list_empty(&me->issued_io));
	io = dm_list_item(me->issued_io.n, struct mock_io);
	dm_list_del(&io->list);
	fn(io->context, io->r ? 0 : -EIO);
	free(io);

	return true;
}

static unsigned _mock_max_io(struct io_engine *e)
{
	struct mock_engine *me = _to_mock(e);
	_match(me, E_MAX_IO);
	return me->max_io;
}

static struct mock_engine *_mock_create(unsigned max_io, sector_t block_sectors)
{
	struct mock_engine *m = malloc(sizeof(*m));

	T_ASSERT(m);

	m->e.name = "mock";
	m->e.destroy = _mock_destroy;
	m->e.issue = _mock_issue;
	m->e.wait = _mock_wait;
	m->e.max_io = _mock_max_io;

	m->max_io = max_io;
	m->block_sectors = block_sectors;
	dm_list_init(&m->expected_calls);
	dm_list_init(&m->issued_io);

	return m;
}

/*----------------------------------------------------------------
 * Fixtures
 *--------------------------------------------------------------*/
struct fixture {
	struct mock_engine *me;
	struct blkio *bio;
};

static struct fixture *_fixture_init(sector_t block_sectors, unsigned nr_cache_blocks)
{
	struct fixture *f = malloc(sizeof(*f));
	unsigned i;

	T_ASSERT(f);

	f->me = _mock_create(16, block_sectors);
	T_ASSERT(f->me);

	_expect(f->me, E_MAX_IO);
	f->bio = blkio_create_engine(block_sectors, nr_cache_blocks, &f->me->e);
	T_ASSERT(f->bio);

	/*
	 * Register mock fds with identity mapping (_fd_table[i] = i)
	 * so tests can use di values directly.  The mock engine does
	 * not perform real I/O, but _issue_low_level() needs a valid
	 * fd table entry before calling engine->issue().
	 */
	for (i = 0; i < BLKIO_FD_TABLE_INC; i++)
		T_ASSERT(blkio_set_fd(f->bio, i) == (int) i);

	return f;
}

static void _fixture_exit(struct fixture *f)
{
	if (f) {
		_expect(f->me, E_DESTROY);
		blkio_destroy(f->bio);

		free(f);
	}
}

static void *_small_fixture_init(void)
{
	return _fixture_init(128, 16);
}

static void _small_fixture_exit(void *context)
{
	_fixture_exit(context);
}

static void *_large_fixture_init(void)
{
	return _fixture_init(128, 1024);
}

static void _large_fixture_exit(void *context)
{
	_fixture_exit(context);
}

/*----------------------------------------------------------------
 * Tests
 *--------------------------------------------------------------*/
#define MEG 2048
#define PAGE_SIZE_SECTORS ((TEST_PAGE_SIZE) >> SECTOR_SHIFT)

static void good_create(sector_t block_sectors, unsigned nr_cache_blocks)
{
	struct blkio *bio;
	struct mock_engine *me = _mock_create(16, 128);

	_expect(me, E_MAX_IO);
	bio = blkio_create_engine(block_sectors, nr_cache_blocks, &me->e);
	T_ASSERT(bio);

	_expect(me, E_DESTROY);
	blkio_destroy(bio);
}

static void bad_create(sector_t block_sectors, unsigned nr_cache_blocks)
{
	struct blkio *bio;
	struct mock_engine *me = _mock_create(16, 128);

	_expect(me, E_MAX_IO);
	_expect(me, E_DESTROY);
	bio = blkio_create_engine(block_sectors, nr_cache_blocks, &me->e);
	T_ASSERT(!bio);
}

static void test_create(void *fixture)
{
	good_create(PAGE_SIZE_SECTORS, 16);
}

static void test_nr_cache_blocks_must_be_positive(void *fixture)
{
	bad_create(PAGE_SIZE_SECTORS, 0);
}

static void test_block_sectors_must_be_positive(void *fixture)
{
	bad_create(0, 16);
}

static void test_block_sectors_must_be_multiple_of_page_size(void *fixture)
{
	static const unsigned _bad_examples[] = {3, 9, 13, 1025};
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_bad_examples); i++)
		bad_create(_bad_examples[i], 16);

	for (i = 1; i < 100; i++)
		good_create(i * PAGE_SIZE_SECTORS, 16);
}

static void test_get_triggers_read(void *context)
{
	struct fixture *f = context;

	int di = 17;   // arbitrary key
	struct blkio_block *b;

	_expect_read(f->me, di, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(blkio_get(f->bio, di, 0, 0, &b));
	blkio_put(b);

	_expect_read(f->me, di, 1);
	_expect(f->me, E_WAIT);
	T_ASSERT(blkio_get(f->bio, di, 1, GF_DIRTY, &b));
	_expect_write(f->me, di, 1);
	_expect(f->me, E_WAIT);
	blkio_put(b);
}

static void test_repeated_reads_are_cached(void *context)
{
	struct fixture *f = context;

	int di = 17;   // arbitrary key
	unsigned i;
	struct blkio_block *b;

	_expect_read(f->me, di, 0);
	_expect(f->me, E_WAIT);
	for (i = 0; i < 100; i++) {
		T_ASSERT(blkio_get(f->bio, di, 0, 0, &b));
		blkio_put(b);
	}
}

static void test_block_gets_evicted_with_many_reads(void *context)
{
	struct fixture *f = context;

	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	const unsigned nr_cache_blocks = 16;

	int di = 17;   // arbitrary key
	unsigned i;
	struct blkio_block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect_read(me, di, i);
		_expect(me, E_WAIT);
		T_ASSERT(blkio_get(bio, di, i, 0, &b));
		blkio_put(b);
	}

	// Not enough cache blocks to hold this one
	_expect_read(me, di, nr_cache_blocks);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, nr_cache_blocks, 0, &b));
	blkio_put(b);

	// Now if we run through we should find one block has been
	// evicted.  We go backwards because the oldest is normally
	// evicted first.
	_expect_read_any(me);
	_expect(me, E_WAIT);
	for (i = nr_cache_blocks; i; i--) {
		T_ASSERT(blkio_get(bio, di, i - 1, 0, &b));
		blkio_put(b);
	}
}

static void test_prefetch_issues_a_read(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	const unsigned nr_cache_blocks = 16;

	int di = 17;   // arbitrary key
	unsigned i;
	struct blkio_block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		// prefetch should not wait
		_expect_read(me, di, i);
		blkio_prefetch(bio, di, i);
	}
	_no_outstanding_expectations(me);

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect(me, E_WAIT);
		T_ASSERT(blkio_get(bio, di, i, 0, &b));
		blkio_put(b);
	}
}

static void test_too_many_prefetches_does_not_trigger_a_wait(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;

	const unsigned nr_cache_blocks = 16;
	int di = 17;   // arbitrary key
	unsigned i;

	for (i = 0; i < 10 * nr_cache_blocks; i++) {
		// prefetch should not wait
		if (i < nr_cache_blocks)
			_expect_read(me, di, i);
		blkio_prefetch(bio, di, i);
	}

	// Destroy will wait for any in flight IO triggered by prefetches.
	for (i = 0; i < nr_cache_blocks; i++)
		_expect(me, E_WAIT);
}

static void test_dirty_data_gets_written_back(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;

	int di = 17;   // arbitrary key
	struct blkio_block *b;

	// Expect the read
	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, GF_DIRTY, &b));
	blkio_put(b);

	// Expect the write
	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
}

static void test_zeroed_data_counts_as_dirty(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;

	int di = 17;   // arbitrary key
	struct blkio_block *b;

	// No read
	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	blkio_put(b);

	// Expect the write
	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
}

static void test_flush_waits_for_all_dirty(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;

	const unsigned count = 16;
	int di = 17;   // arbitrary key
	unsigned i;
	struct blkio_block *b;

	for (i = 0; i < count; i++) {
		if (i % 2) {
			T_ASSERT(blkio_get(bio, di, i, GF_ZERO, &b));
		} else {
			_expect_read(me, di, i);
			_expect(me, E_WAIT);
			T_ASSERT(blkio_get(bio, di, i, 0, &b));
		}
		blkio_put(b);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect_write(me, di, i);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect(me, E_WAIT);
	}

	T_ASSERT(blkio_flush(bio));
	_no_outstanding_expectations(me);
}

static void test_multiple_files(void *context)
{
	static int _dis[] = {1, 128, 345, 678, 890};

	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_dis); i++) {
		_expect_read(me, _dis[i], 0);
		_expect(me, E_WAIT);

		T_ASSERT(blkio_get(bio, _dis[i], 0, 0, &b));
		blkio_put(b);
	}
}

static void test_read_bad_issue(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;

	_expect_read_bad_issue(me, 17, 0);
	T_ASSERT(!blkio_get(bio, 17, 0, 0, &b));
}

static void test_read_bad_issue_intermittent(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	_expect_read_bad_issue(me, di, 0);
	T_ASSERT(!blkio_get(bio, di, 0, 0, &b));

	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, 0, &b));
	blkio_put(b);
}

static void test_read_bad_wait(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	_expect_read_bad_wait(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!blkio_get(bio, di, 0, 0, &b));
}

static void test_read_bad_wait_intermittent(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	_expect_read_bad_wait(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!blkio_get(bio, di, 0, 0, &b));

	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, 0, &b));
	blkio_put(b);
}

static void test_write_bad_issue_stops_flush(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	_expect_write_bad_issue(me, di, 0);
	blkio_put(b);
	T_ASSERT(!blkio_flush(bio));

	// we'll let it succeed the second time
	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_flush(bio));
}

static void test_write_bad_io_stops_flush(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	_expect_write_bad_wait(me, di, 0);
	_expect(me, E_WAIT);
	blkio_put(b);
	T_ASSERT(!blkio_flush(bio));

	// we'll let it succeed the second time
	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_flush(bio));
}

static void test_invalidate_not_present(void *context)
{
	struct fixture *f = context;
	struct blkio *bio = f->bio;
	int di = 17;

	T_ASSERT(blkio_invalidate(bio, di, 0));
}

static void test_invalidate_present(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, 0, &b));
	blkio_put(b);

	T_ASSERT(blkio_invalidate(bio, di, 0));
}

static void test_invalidate_after_read_error(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	_expect_read_bad_issue(me, di, 0);
	T_ASSERT(!blkio_get(bio, di, 0, 0, &b));
	T_ASSERT(blkio_invalidate(bio, di, 0));
}

static void test_invalidate_after_write_error(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	blkio_put(b);

	// invalidate should fail if the write fails
	_expect_write_bad_wait(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!blkio_invalidate(bio, di, 0));

	// and should succeed if the write does
	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_invalidate(bio, di, 0));

	// a read is not required to get the block
	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, 0, &b));
	blkio_put(b);
}

static void test_invalidate_held_block(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));

	T_ASSERT(!blkio_invalidate(bio, di, 0));

	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
	blkio_put(b);
}

static void test_invalidate_held_leaves_earlier_block(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b, *held;
	int di = 17;
	unsigned i;

	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, 0, &b));
	blkio_put(b);

	_expect_read(me, di, 1);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 1, 0, &held));

	T_ASSERT(!blkio_invalidate_di(bio, di));

	for (i = 2; i < 17; i++) {
		_expect_read(me, di, i);
		_expect(me, E_WAIT);
		T_ASSERT(blkio_get(bio, di, i, 0, &b));
		blkio_put(b);
	}

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	T_ASSERT_EQUAL(b->index, 0);
	blkio_put(b);
	blkio_put(held);
	blkio_abort_di(bio, di);
}

//----------------------------------------------------------------
// abort tests

static void test_abort_no_blocks(void *context)
{
	struct fixture *f = context;
	struct blkio *bio = f->bio;
	int di = 17;

	// We have no expectations
	blkio_abort_di(bio, di);
}

static void test_abort_single_block(void *context)
{
	struct fixture *f = context;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	blkio_put(b);

	blkio_abort_di(bio, di);

	// no write should be issued
	T_ASSERT(blkio_flush(bio));
}

static void test_abort_forces_reread(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, GF_DIRTY, &b));
	blkio_put(b);

	blkio_abort_di(bio, di);
	T_ASSERT(blkio_flush(bio));

	// Check the block is re-read
	_expect_read(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_get(bio, di, 0, 0, &b));
	blkio_put(b);
}

static void test_abort_held_block(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di = 17;

	T_ASSERT(blkio_get(bio, di, 0, GF_ZERO, &b));
	blkio_abort_di(bio, di);

	/* The held block keeps the cache intact, so its dirty data survives. */
	blkio_put(b);
	_expect_write(me, di, 0);
	_expect(me, E_WAIT);
	T_ASSERT(blkio_flush(bio));
	_no_outstanding_expectations(me);
}

static void test_all_blocks_held(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *blocks[16], *b = (void *) 1;
	unsigned i;
	int di = 17;
	bool got;

	for (i = 0; i < DM_ARRAY_SIZE(blocks); i++) {
		_expect_read(me, di, i);
		_expect(me, E_WAIT);
		T_ASSERT(blkio_get(bio, di, i, 0, &blocks[i]));
	}

	/* No block can be reclaimed while all of them are held. */
	alarm(5);
	got = blkio_get(bio, di, DM_ARRAY_SIZE(blocks), 0, &b);
	alarm(0);
	T_ASSERT(!got);
	T_ASSERT(!b);

	for (i = 0; i < DM_ARRAY_SIZE(blocks); i++)
		blkio_put(blocks[i]);

	blkio_abort_di(bio, di);
	_no_outstanding_expectations(me);
}

static void test_abort_only_specific_di(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;
	struct blkio_block *b;
	int di1 = 17, di2 = 18;

	T_ASSERT(blkio_get(bio, di1, 0, GF_ZERO, &b));
	blkio_put(b);

	T_ASSERT(blkio_get(bio, di1, 1, GF_ZERO, &b));
	blkio_put(b);

	T_ASSERT(blkio_get(bio, di2, 0, GF_ZERO, &b));
	blkio_put(b);

	T_ASSERT(blkio_get(bio, di2, 1, GF_ZERO, &b));
	blkio_put(b);

	blkio_abort_di(bio, di2);

	// writes for di1 should still be issued
	_expect_write(me, di1, 0);
	_expect_write(me, di1, 1);

	_expect(me, E_WAIT);
	_expect(me, E_WAIT);

	T_ASSERT(blkio_flush(bio));
}

//----------------------------------------------------------------
// Chasing a bug reported by dct

static void _cycle(struct fixture *f, unsigned nr_cache_blocks)
{
	struct mock_engine *me = f->me;
	struct blkio *bio = f->bio;

	unsigned i;
	struct blkio_block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		// prefetch should not wait
		_expect_read(me, i, 0);
		blkio_prefetch(bio, i, 0);
	}

	// This double checks the reads occur in response to the prefetch
	_no_outstanding_expectations(me);

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect(me, E_WAIT);
		T_ASSERT(blkio_get(bio, i, 0, 0, &b));
		blkio_put(b);
	}

	_no_outstanding_expectations(me);
}

static void test_concurrent_reads_after_invalidate(void *context)
{
	struct fixture *f = context;
	unsigned i, nr_cache_blocks = 16;

	_cycle(f, nr_cache_blocks);
	for (i = 0; i < nr_cache_blocks; i++)
		blkio_invalidate_di(f->bio, i);
	_cycle(f, nr_cache_blocks);
}

/*----------------------------------------------------------------
 * Top level
 *--------------------------------------------------------------*/
#define T(path, desc, fn) register_test(ts, "/base/blkio/" path, desc, fn)

static struct test_suite *_tiny_tests(void)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("create-destroy", "simple create/destroy", test_create);
	T("cache-blocks-positive", "nr cache blocks must be positive", test_nr_cache_blocks_must_be_positive);
	T("block-size-positive", "block size must be positive", test_block_sectors_must_be_positive);
	T("block-size-multiple-page", "block size must be a multiple of page size", test_block_sectors_must_be_multiple_of_page_size);

	return ts;
}

static void test_rejected_read_preserves_pending(void *context)
{
	struct fixture *f = context;
	struct blkio_block *b = (void *) 1;

	/* Exercise rejection with an unregistered fd. */
	blkio_clear_fd(f->bio, 0);
	T_ASSERT(!blkio_get(f->bio, 0, 0, 0, &b));
	T_ASSERT(!b);
	T_ASSERT(blkio_set_fd(f->bio, 0) == 0);

	/* Rejected requests must not consume the prefetch queue capacity. */
	_expect_read(f->me, 0, 0);
	blkio_prefetch(f->bio, 0, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(blkio_get(f->bio, 0, 0, 0, &b));
	blkio_put(b);
	_no_outstanding_expectations(f->me);
}

static void test_reserved_fd_reuse(void *context)
{
	struct fixture *f = context;
	struct blkio_block *b;

	/* Cache a block on di 0 and keep it held by the caller. */
	_expect_read(f->me, 0, 0);
	blkio_prefetch(f->bio, 0, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(blkio_get(f->bio, 0, 0, 0, &b));

	/*
	 * Closing a di while its blocks cannot be dropped reserves the
	 * slot, and blkio_clear_fd() must not release that reservation.
	 */
	blkio_reserve_fd(f->bio, 0);
	blkio_clear_fd(f->bio, 0);

	/* A held block keeps di 0 out of the free pool. */
	T_ASSERT_EQUAL(blkio_set_fd(f->bio, 0), BLKIO_FD_TABLE_INC);

	/* Dropping the blocks releases the reservation with them. */
	blkio_put(b);
	T_ASSERT(blkio_abort_di(f->bio, 0));
	T_ASSERT_EQUAL(blkio_set_fd(f->bio, 0), 0);
}

static void test_rejected_write_preserves_pending(void *context)
{
	struct fixture *f = context;
	struct blkio_block *b;

	T_ASSERT(blkio_get(f->bio, 0, 1, GF_ZERO, &b));
	blkio_put(b);
	blkio_set_last_byte(f->bio, 0, 512, 512);
	T_ASSERT(!blkio_flush(f->bio));

	/* Removing the limit must let the same dirty block be retried. */
	blkio_unset_last_byte(f->bio, 0);
	_expect_write(f->me, 0, 1);
	_expect(f->me, E_WAIT);
	T_ASSERT(blkio_flush(f->bio));
	_no_outstanding_expectations(f->me);
}

static bool _abort_on_internal_errors(void)
{
	const char *env = getenv("DM_ABORT_ON_INTERNAL_ERRORS");

	return env && strcmp(env, "0");
}

static void test_read_lock_conflict(void *context)
{
	struct fixture *f = context;
	struct blkio_block *b, *b2 = (void *) 1;

	T_ASSERT(blkio_get(f->bio, 0, 0, GF_ZERO, &b));
	T_ASSERT(!blkio_get(f->bio, 0, 0, 0, &b2));
	T_ASSERT(!b2);
	blkio_put(b);
	blkio_abort_di(f->bio, 0);
}

static void test_write_lock_conflict(void *context)
{
	struct fixture *f = context;
	struct blkio_block *b, *b2 = (void *) 1;

	T_ASSERT(blkio_get(f->bio, 0, 0, GF_ZERO, &b));
	T_ASSERT(!blkio_get(f->bio, 0, 0, GF_DIRTY, &b2));
	T_ASSERT(!b2);
	blkio_put(b);
	blkio_abort_di(f->bio, 0);
}

static struct test_suite *_small_tests(void)
{
	struct test_suite *ts = test_suite_create(_small_fixture_init, _small_fixture_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("get-reads", "blkio_get() triggers read", test_get_triggers_read);
	T("rejected-read-pending", "rejected reads preserve queue capacity", test_rejected_read_preserves_pending);
	T("rejected-write-pending", "rejected writes can be retried", test_rejected_write_preserves_pending);
	T("reserved-fd-reuse", "reserved fd is only reused once its blocks are dropped", test_reserved_fd_reuse);
	if (!_abort_on_internal_errors()) {
		T("lock-conflict-read", "read lock fails on a write locked block", test_read_lock_conflict);
		T("lock-conflict-write", "write lock fails on a held block", test_write_lock_conflict);
	}
	T("reads-cached", "repeated reads are cached", test_repeated_reads_are_cached);
	T("blocks-get-evicted", "block get evicted with many reads", test_block_gets_evicted_with_many_reads);
	T("prefetch-reads", "prefetch issues a read", test_prefetch_issues_a_read);
	T("prefetch-never-waits", "too many prefetches does not trigger a wait", test_too_many_prefetches_does_not_trigger_a_wait);
	T("writeback-occurs", "dirty data gets written back", test_dirty_data_gets_written_back);
	T("zero-flag-dirties", "zeroed data counts as dirty", test_zeroed_data_counts_as_dirty);
	T("read-multiple-files", "read from multiple files", test_multiple_files);
	T("read-bad-issue", "read fails if io engine unable to issue", test_read_bad_issue);
	T("read-bad-issue-intermittent", "failed issue, followed by success", test_read_bad_issue_intermittent);
	T("read-bad-io", "read issued ok, but io fails", test_read_bad_wait);
	T("read-bad-io-intermittent", "failed io, followed by success", test_read_bad_wait_intermittent);
	T("write-bad-issue-stops-flush", "flush fails temporarily if any block fails to write", test_write_bad_issue_stops_flush);
	T("write-bad-io-stops-flush", "flush fails temporarily if any block fails to write", test_write_bad_io_stops_flush);
	T("invalidate-not-present", "invalidate a block that isn't in the cache", test_invalidate_not_present);
	T("invalidate-present", "invalidate a block that is in the blkio", test_invalidate_present);
	T("invalidate-read-error", "invalidate a block that errored", test_invalidate_after_read_error);
	T("invalidate-write-error", "invalidate a block that errored", test_invalidate_after_write_error);
	T("invalidate-fails-in-held", "invalidating a held block fails", test_invalidate_held_block);
	T("invalidate-held-keeps-earlier", "failed invalidate does not free earlier blocks", test_invalidate_held_leaves_earlier_block);

	T("abort-with-no-blocks", "you can call abort, even if there are no blocks in the cache", test_abort_no_blocks);
	T("abort-single-block", "single block get silently dropped", test_abort_single_block);
	T("abort-forces-read", "if a block has been dropped then another read is necc.", test_abort_forces_reread);
	T("abort-specific-di", "abort doesn't effect other dis", test_abort_only_specific_di);
	T("abort-held-block", "abort leaves held blocks in the cache", test_abort_held_block);
	T("all-blocks-held", "get fails when all cache blocks are held", test_all_blocks_held);

	T("concurrent-reads-after-invalidate", "prefetch should still issue concurrent reads after invalidate",
	  test_concurrent_reads_after_invalidate);

	return ts;
}

static struct test_suite *_large_tests(void)
{
	struct test_suite *ts = test_suite_create(_large_fixture_init, _large_fixture_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("flush-waits", "flush waits for all dirty", test_flush_waits_for_all_dirty);

	return ts;
}

void blkio_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_tiny_tests()->list);
	dm_list_add(all_tests, &_small_tests()->list);
	dm_list_add(all_tests, &_large_tests()->list);
}
