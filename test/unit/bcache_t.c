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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "lib/device/bcache.h"
#include "framework.h"
#include "units.h"

#define SHOW_MOCK_CALLS 0

//----------------------------------------------------------------
// We're assuming the file descriptor is the first element of the
// bcache_dev.
struct bcache_dev {
	int fd;
};

/*----------------------------------------------------------------
 * Mock engine
 *--------------------------------------------------------------*/
struct mock_engine {
	struct io_engine e;
	struct dm_list expected_calls;
	struct dm_list issued_io;
	unsigned max_io;
	sector_t block_size;
	int last_fd;
};

enum method {
	E_DESTROY,
	E_OPEN,
	E_CLOSE,
	E_ISSUE,
	E_WAIT,
	E_MAX_IO
};

struct mock_call {
	struct dm_list list;
	enum method m;

	bool match_args;
	enum dir d;
	struct bcache_dev *dev;
	block_address b;
	bool issue_r;
	bool wait_r;
	unsigned engine_flags;
};

struct mock_io {
	struct dm_list list;
	int fd;
	sector_t sb;
	sector_t se;
	void *data;
	void *context;
	bool r;
};

static const char *_show_method(enum method m)
{
	switch (m) {
	case E_DESTROY:
		return "destroy()";
	case E_OPEN:
		return "open()";
	case E_CLOSE:
		return "close()";
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
	mc->m = m;
	mc->match_args = false;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read(struct mock_engine *e, struct bcache_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_READ;
	mc->dev = dev;
	mc->b = b;
	mc->issue_r = true;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read_any(struct mock_engine *e)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = false;
	mc->issue_r = true;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_write(struct mock_engine *e, struct bcache_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->dev = dev;
	mc->b = b;
	mc->issue_r = true;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read_bad_issue(struct mock_engine *e, struct bcache_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_READ;
	mc->dev = dev;
	mc->b = b;
	mc->issue_r = false;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_write_bad_issue(struct mock_engine *e, struct bcache_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->dev = dev;
	mc->b = b;
	mc->issue_r = false;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read_bad_wait(struct mock_engine *e, struct bcache_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_READ;
	mc->dev = dev;
	mc->b = b;
	mc->issue_r = true;
	mc->wait_r = false;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_write_bad_wait(struct mock_engine *e, struct bcache_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->dev = dev;
	mc->b = b;
	mc->issue_r = true;
	mc->wait_r = false;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_open(struct mock_engine *e, unsigned eflags)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_OPEN;
	mc->match_args = true;
	mc->engine_flags = eflags;
	dm_list_add(&e->expected_calls, &mc->list);
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

static void _mock_destroy(struct io_engine *e)
{
	struct mock_engine *me = _to_mock(e);

	_match(me, E_DESTROY);
	T_ASSERT(dm_list_empty(&me->issued_io));
	T_ASSERT(dm_list_empty(&me->expected_calls));
	free(_to_mock(e));
}

static int _mock_open(struct io_engine *e, const char *path, unsigned flags)
{
	struct mock_engine *me = _to_mock(e);
	struct mock_call *mc;

	mc = _match_pop(me, E_OPEN);
	if (mc->match_args)
		T_ASSERT_EQUAL(mc->engine_flags, flags);
	free(mc);
	return me->last_fd++;
}

static void _mock_close(struct io_engine *e, int fd)
{
	struct mock_engine *me = _to_mock(e);
	struct mock_call *mc;

	mc = _match_pop(me, E_CLOSE);
	free(mc);
}

static bool _mock_issue(struct io_engine *e, enum dir d, int fd,
	      		sector_t sb, sector_t se, void *data, void *context)
{
	bool r, wait_r;
	struct mock_io *io;
	struct mock_call *mc;
	struct mock_engine *me = _to_mock(e);

	mc = _match_pop(me, E_ISSUE);
	if (mc->match_args) {
		T_ASSERT(d == mc->d);
		T_ASSERT(fd == mc->dev->fd);
		T_ASSERT(sb == mc->b * me->block_size);
		T_ASSERT(se == (mc->b + 1) * me->block_size);
	}
	r = mc->issue_r;
	wait_r = mc->wait_r;
	free(mc);

	if (r) {
		io = malloc(sizeof(*io));
		if (!io)
			abort();

		io->fd = fd;
		io->sb = sb;
		io->se = se;
		io->data = data;
		io->context = context;
		io->r = wait_r;

		dm_list_add(&me->issued_io, &io->list);
	}

	return r;
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

static struct mock_engine *_mock_create(unsigned max_io, sector_t block_size)
{
	struct mock_engine *m = malloc(sizeof(*m));

	m->e.destroy = _mock_destroy;
	m->e.open = _mock_open;
	m->e.close = _mock_close;
	m->e.issue = _mock_issue;
	m->e.wait = _mock_wait;
	m->e.max_io = _mock_max_io;

	m->max_io = max_io;
	m->block_size = block_size;
	dm_list_init(&m->expected_calls);
	dm_list_init(&m->issued_io);
	m->last_fd = 2;

	return m;
}

/*----------------------------------------------------------------
 * Fixtures
 *--------------------------------------------------------------*/
struct fixture {
	struct mock_engine *me;
	struct bcache *cache;
};

static struct fixture *_fixture_init(sector_t block_size, unsigned nr_cache_blocks)
{
	struct fixture *f = malloc(sizeof(*f));

	f->me = _mock_create(16, block_size);
	T_ASSERT(f->me);

	_expect(f->me, E_MAX_IO);
	f->cache = bcache_create(block_size, nr_cache_blocks, &f->me->e);
	T_ASSERT(f->cache);

	return f;
}

static void _fixture_exit(struct fixture *f)
{
	_expect(f->me, E_DESTROY);
	bcache_destroy(f->cache);

	free(f);
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
#define SECTOR_SHIFT 9

static void good_create(sector_t block_size, unsigned nr_cache_blocks)
{
	struct bcache *cache;
	struct mock_engine *me = _mock_create(16, 128);

	_expect(me, E_MAX_IO);
	cache = bcache_create(block_size, nr_cache_blocks, &me->e);
	T_ASSERT(cache);

	_expect(me, E_DESTROY);
	bcache_destroy(cache);
}

static void bad_create(sector_t block_size, unsigned nr_cache_blocks)
{
	struct bcache *cache;
	struct mock_engine *me = _mock_create(16, 128);

	_expect(me, E_MAX_IO);
	cache = bcache_create(block_size, nr_cache_blocks, &me->e);
	T_ASSERT(!cache);

	_expect(me, E_DESTROY);
	me->e.destroy(&me->e);
}

static void test_create(void *fixture)
{
	good_create(8, 16);
}

static void test_nr_cache_blocks_must_be_positive(void *fixture)
{
	bad_create(8, 0);
}

static void test_block_size_must_be_positive(void *fixture)
{
	bad_create(0, 16);
}

static void test_block_size_must_be_multiple_of_page_size(void *fixture)
{
	static unsigned _bad_examples[] = {3, 9, 13, 1025};

	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_bad_examples); i++)
		bad_create(_bad_examples[i], 16);

	for (i = 1; i < 100; i++)
		good_create(i * 8, 16);
}

static void test_get_triggers_read(void *context)
{
	struct fixture *f = context;

	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct block *b;

	_expect(f->me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	T_ASSERT(dev);
	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(bcache_get(f->cache, dev, 0, 0, &b));
	bcache_put(b);

	_expect_read(f->me, dev, 1);
	_expect(f->me, E_WAIT);
	T_ASSERT(bcache_get(f->cache, dev, 1, GF_DIRTY, &b));
	_expect_write(f->me, dev, 1);
	_expect(f->me, E_WAIT);
	bcache_put(b);

	_expect(f->me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_repeated_reads_are_cached(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;

	unsigned i;
	struct block *b;

	_expect(f->me, E_OPEN);
	dev = bcache_get_dev(f->cache, path, 0);
	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	for (i = 0; i < 100; i++) {
		T_ASSERT(bcache_get(f->cache, dev, 0, 0, &b));
		bcache_put(b);
	}
	_expect(f->me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_block_gets_evicted_with_many_reads(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;

	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	const unsigned nr_cache_blocks = 16;

	unsigned i;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	fprintf(stderr, "1\n");
	for (i = 0; i < nr_cache_blocks; i++) {
		_expect_read(me, dev, i);
		_expect(me, E_WAIT);
		T_ASSERT(bcache_get(cache, dev, i, 0, &b));
		bcache_put(b);
	}

	fprintf(stderr, "2\n");
	// Not enough cache blocks to hold this one
	_expect_read(me, dev, nr_cache_blocks);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, nr_cache_blocks, 0, &b));
	bcache_put(b);

	fprintf(stderr, "3\n");
	// Now if we run through we should find one block has been
	// evicted.  We go backwards because the oldest is normally
	// evicted first.
	_expect_read_any(me);
	_expect(me, E_WAIT);
	for (i = nr_cache_blocks; i; i--) {
		T_ASSERT(bcache_get(cache, dev, i - 1, 0, &b));
		bcache_put(b);
		T_ASSERT(bcache_is_well_formed(cache));
	}

	fprintf(stderr, "4\n");
	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_prefetch_issues_a_read(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	const unsigned nr_cache_blocks = 16;

	unsigned i;
	struct block *b;

	_expect(me, E_OPEN);
	dev = bcache_get_dev(f->cache, path, 0);

	for (i = 0; i < nr_cache_blocks; i++) {
		// prefetch should not wait
		_expect_read(me, dev, i);
		bcache_prefetch(cache, dev, i);
	}
	_no_outstanding_expectations(me);

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect(me, E_WAIT);
		T_ASSERT(bcache_get(cache, dev, i, 0, &b));
		bcache_put(b);
	}

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_too_many_prefetches_does_not_trigger_a_wait(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const unsigned nr_cache_blocks = 16;
	unsigned i;

	_expect(me, E_OPEN);
	dev = bcache_get_dev(f->cache, path, 0);
	for (i = 0; i < 10 * nr_cache_blocks; i++) {
		// prefetch should not wait
		if (i < nr_cache_blocks)
			_expect_read(me, dev, i);
		bcache_prefetch(cache, dev, i);
	}

	// Destroy will wait for any in flight IO triggered by prefetches.
	for (i = 0; i < nr_cache_blocks; i++)
		_expect(me, E_WAIT);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_dirty_data_gets_written_back(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = bcache_get_dev(f->cache, path, 0);

	// Expect the read
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, GF_DIRTY, &b));
	bcache_put(b);

	// Expect the write
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);

	bcache_put_dev(dev);
	_expect(f->me, E_CLOSE);
}

static void test_zeroed_data_counts_as_dirty(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	// No read
	T_ASSERT(bcache_get(cache, dev, 0, GF_ZERO, &b));
	bcache_put(b);

	// Expect the write
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_flush_waits_for_all_dirty(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const unsigned count = 16;
	unsigned i;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	for (i = 0; i < count; i++) {
		if (i % 2) {
			T_ASSERT(bcache_get(cache, dev, i, GF_ZERO, &b));
		} else {
			_expect_read(me, dev, i);
			_expect(me, E_WAIT);
			T_ASSERT(bcache_get(cache, dev, i, 0, &b));
		}
		bcache_put(b);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect_write(me, dev, i);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect(me, E_WAIT);
	}

	bcache_flush(cache);
	_no_outstanding_expectations(me);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_multiple_files(void *context)
{
	static const char *_paths[] = {"/dev/dm-1", "/dev/dm-2", "/dev/dm-3", "/dev/dm-4"};

	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct bcache_dev *dev;
	struct block *b;
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_paths); i++) {
		_expect(me, E_OPEN);
		dev = bcache_get_dev(cache, _paths[i], 0);
		_expect_read(me, dev, 0);
		_expect(me, E_WAIT);

		T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
		bcache_put(b);
		bcache_put_dev(dev);
	}

	for (i = 0; i < DM_ARRAY_SIZE(_paths); i++)
		_expect(me, E_CLOSE);
}

static void test_read_bad_issue(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	_expect_read_bad_issue(me, dev, 0);
	T_ASSERT(!bcache_get(cache, dev, 0, 0, &b));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_read_bad_issue_intermittent(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
	dev = bcache_get_dev(f->cache, path, 0);

	_expect_read_bad_issue(me, dev, 0);
	T_ASSERT(!bcache_get(cache, dev, 0, 0, &b));

	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_read_bad_wait(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	_expect_read_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!bcache_get(cache, dev, 0, 0, &b));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_read_bad_wait_intermittent(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	_expect_read_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!bcache_get(cache, dev, 0, 0, &b));

	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_write_bad_issue_stops_flush(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	T_ASSERT(bcache_get(cache, dev, 0, GF_ZERO, &b));
	_expect_write_bad_issue(me, dev, 0);
	bcache_put(b);
	T_ASSERT(!bcache_flush(cache));

	// we'll let it succeed the second time
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_flush(cache));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_write_bad_io_stops_flush(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	T_ASSERT(bcache_get(cache, dev, 0, GF_ZERO, &b));
	_expect_write_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	bcache_put(b);
	T_ASSERT(!bcache_flush(cache));

	// we'll let it succeed the second time
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_flush(cache));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_invalidate_not_present(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct bcache *cache = f->cache;

	_expect(f->me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);
	T_ASSERT(bcache_invalidate(cache, dev, 0));
	_expect(f->me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_invalidate_present(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(f->me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);

	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);

	T_ASSERT(bcache_invalidate(cache, dev, 0));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_invalidate_after_read_error(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);
	_expect_read_bad_issue(me, dev, 0);
	T_ASSERT(!bcache_get(cache, dev, 0, 0, &b));
	T_ASSERT(bcache_invalidate(cache, dev, 0));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_invalidate_after_write_error(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);
	T_ASSERT(bcache_get(cache, dev, 0, GF_ZERO, &b));
	bcache_put(b);

	// invalidate should fail if the write fails
	_expect_write_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!bcache_invalidate(cache, dev, 0));

	// and should succeed if the write does
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_invalidate(cache, dev, 0));

	// a read is not required to get the block
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_invalidate_held_block(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = bcache_get_dev(f->cache, path, 0);
	T_ASSERT(bcache_get(cache, dev, 0, GF_ZERO, &b));

	T_ASSERT(!bcache_invalidate(cache, dev, 0));

	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	bcache_put(b);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

//----------------------------------------------------------------

static void test_concurrent_devs(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const char *path = "/dev/foo/bar";
	struct bcache_dev *dev1, *dev2;

	_expect(me, E_OPEN);
	dev1 = bcache_get_dev(cache, path, 0);
	dev2 = bcache_get_dev(cache, path, 0);

	_expect(me, E_CLOSE);  // only one close

	bcache_put_dev(dev1);
	bcache_put_dev(dev2);
}

static void test_concurrent_devs_exclusive(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const char *path = "/dev/foo/bar";
	struct bcache_dev *dev1, *dev2;

	_expect(me, E_OPEN);
	dev1 = bcache_get_dev(cache, path, EF_EXCL);
	dev2 = bcache_get_dev(cache, path, EF_EXCL);

	_expect(me, E_CLOSE);  // only one close

	bcache_put_dev(dev1);
	bcache_put_dev(dev2);
}

static void test_exclusive_flags_gets_passed_to_engine(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const char *path = "/dev/foo/bar";
	struct bcache_dev *dev;

	_expect_open(me, EF_EXCL);
	dev = bcache_get_dev(cache, path, EF_EXCL);
	_expect(me, E_CLOSE);
	bcache_put_dev(dev);

	_expect_open(me, EF_READ_ONLY);
	dev = bcache_get_dev(cache, path, EF_READ_ONLY);
	_expect(me, E_CLOSE);
	bcache_put_dev(dev);

	_expect_open(me, EF_EXCL | EF_READ_ONLY);
	dev = bcache_get_dev(cache, path, EF_EXCL | EF_READ_ONLY);
	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_reopen_exclusive_triggers_invalidate(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const char *path = "/dev/foo/bar";
	struct bcache_dev *dev;
	struct block *b;

	_expect_open(me, 0);
	dev = bcache_get_dev(cache, path, 0);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);
	bcache_put_dev(dev);

	_no_outstanding_expectations(me);

	_expect(me, E_CLOSE);
	_expect_open(me, EF_EXCL);

	dev = bcache_get_dev(cache, path, EF_EXCL);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

static void test_concurrent_reopen_excl_fails(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	const char *path = "/dev/foo/bar";
	struct bcache_dev *dev;
	struct block *b;

	_expect_open(me, 0);
	dev = bcache_get_dev(cache, path, 0);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
	bcache_put(b);

	_no_outstanding_expectations(me);

	T_ASSERT(!bcache_get_dev(cache, path, EF_EXCL));

	_expect(me, E_CLOSE);
	bcache_put_dev(dev);
}

//----------------------------------------------------------------
// Chasing a bug reported by dct

static void _cycle(struct fixture *f, unsigned nr_cache_blocks)
{
	char buffer[64];
	struct bcache_dev *dev;
	struct mock_engine *me = f->me;
	struct bcache *cache = f->cache;

	unsigned i;
	struct block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		snprintf(buffer, sizeof(buffer) - 1, "/dev/dm-%u", i);
		_expect(me, E_OPEN);
		dev = bcache_get_dev(f->cache, buffer, 0);
		// prefetch should not wait
		_expect_read(me, dev, 0);
		bcache_prefetch(cache, dev, 0);
		bcache_put_dev(dev);
	}

	// This double checks the reads occur in response to the prefetch
	_no_outstanding_expectations(me);

	for (i = 0; i < nr_cache_blocks; i++) {
		snprintf(buffer, sizeof(buffer) - 1, "/dev/dm-%u", i);
		dev = bcache_get_dev(f->cache, buffer, 0);
		_expect(me, E_WAIT);
		T_ASSERT(bcache_get(cache, dev, 0, 0, &b));
		bcache_put(b);
		bcache_put_dev(dev);
	}

	_no_outstanding_expectations(me);
}

static void test_concurrent_reads_after_invalidate(void *context)
{
	struct fixture *f = context;
	char buffer[64];
	unsigned i, nr_cache_blocks = 16;
	struct bcache_dev *dev;

	_cycle(f, nr_cache_blocks);
	for (i = 0; i < nr_cache_blocks; i++) {
		snprintf(buffer, sizeof(buffer) - 1, "/dev/dm-%u", i);
		dev = bcache_get_dev(f->cache, buffer, 0);
        	bcache_invalidate_dev(f->cache, dev);
        	_expect(f->me, E_CLOSE);
        	bcache_put_dev(dev);
        	_no_outstanding_expectations(f->me);
	}

        _cycle(f, nr_cache_blocks);

        for (i = 0; i < nr_cache_blocks; i++)
	        _expect(f->me, E_CLOSE);
}

/*----------------------------------------------------------------
 * Top level
 *--------------------------------------------------------------*/
#define T(path, desc, fn) register_test(ts, "/base/device/bcache/core/" path, desc, fn)

static struct test_suite *_tiny_tests(void)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("create-destroy", "simple create/destroy", test_create);
	T("cache-blocks-positive", "nr cache blocks must be positive", test_nr_cache_blocks_must_be_positive);
	T("block-size-positive", "block size must be positive", test_block_size_must_be_positive);
	T("block-size-multiple-page", "block size must be a multiple of page size", test_block_size_must_be_multiple_of_page_size);

	return ts;
}

static struct test_suite *_small_tests(void)
{
	struct test_suite *ts = test_suite_create(_small_fixture_init, _small_fixture_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("get-reads", "bcache_get() triggers read", test_get_triggers_read);
	T("reads-cached", "repeated reads are cached", test_repeated_reads_are_cached);
	T("blocks-get-evicted", "block get evicted with many reads", test_block_gets_evicted_with_many_reads);
	T("prefetch-reads", "prefetch issues a read", test_prefetch_issues_a_read);
	T("prefetch-never-waits", "too many prefetches does not trigger a wait", test_too_many_prefetches_does_not_trigger_a_wait);
	T("writeback-occurs", "dirty data gets written back", test_dirty_data_gets_written_back);
	T("zero-flag-dirties", "zeroed data counts as dirty", test_zeroed_data_counts_as_dirty);
	T("read-multiple-files", "read from multiple files", test_multiple_files);
	T("read-bad-issue", "read fails if io engine unable to issue", test_read_bad_issue);
	T("read-bad-issue-intermittent", "failed issue, followed by succes", test_read_bad_issue_intermittent);
	T("read-bad-io", "read issued ok, but io fails", test_read_bad_wait);
	T("read-bad-io-intermittent", "failed io, followed by success", test_read_bad_wait_intermittent);
	T("write-bad-issue-stops-flush", "flush fails temporarily if any block fails to write", test_write_bad_issue_stops_flush);
	T("write-bad-io-stops-flush", "flush fails temporarily if any block fails to write", test_write_bad_io_stops_flush);
	T("invalidate-not-present", "invalidate a block that isn't in the cache", test_invalidate_not_present);
	T("invalidate-present", "invalidate a block that is in the cache", test_invalidate_present);
	T("invalidate-read-error", "invalidate a block that errored", test_invalidate_after_read_error);
	T("invalidate-write-error", "invalidate a block that errored", test_invalidate_after_write_error);
	T("invalidate-fails-in-held", "invalidating a held block fails", test_invalidate_held_block);
	T("concurrent-reads-after-invalidate", "prefetch should still issue concurrent reads after invalidate",
          test_concurrent_reads_after_invalidate);
	T("concurrent-devs", "a device may have more than one holder", test_concurrent_devs);
	T("concurrent-devs-exclusive", "a device, opened exclusively, may have more than one holder", test_concurrent_devs_exclusive);
	T("dev-flags-get-passed-to-engine", "EF_EXCL and EF_READ_ONLY get passed down", test_exclusive_flags_gets_passed_to_engine);
	T("reopen-excl-invalidates", "reopening a dev EF_EXCL indicates you want to invalidate everything", test_reopen_exclusive_triggers_invalidate);
	T("concurrent-reopen-excl-fails", "you can't reopen a dev EF_EXCL if there's already a holder", test_concurrent_reopen_excl_fails);

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

void bcache_tests(struct dm_list *all_tests)
{
        dm_list_add(all_tests, &_tiny_tests()->list);
	dm_list_add(all_tests, &_small_tests()->list);
	dm_list_add(all_tests, &_large_tests()->list);
}
