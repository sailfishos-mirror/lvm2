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

#include "lib/device/io-manager.h"
#include "framework.h"
#include "units.h"

#define MEG 2048
#define SECTOR_SHIFT 9
#define SHOW_MOCK_CALLS 0
#define T_BLOCK_SIZE (64ull << SECTOR_SHIFT)
#define SMALL_MAX_CACHE_DEVS 4

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
	E_MAX_IO,
	E_GET_SIZE,
};

struct mock_call {
	struct dm_list list;
	enum method m;

	bool match_args;
	enum dir d;
	// We can't store the dev here because we want to track writebacks
	// and the dev may have been put by then.
	void *fd_context;
	sector_t sb;
	sector_t se;
	bool issue_r;
	bool wait_r;
	unsigned engine_flags;
	uint64_t size;
	bool fail;
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
	case E_GET_SIZE:
		return "get_size()";
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

static void _set_block(struct mock_engine *e, struct mock_call *c, block_address b)
{
	c->sb = b * e->block_size;
	c->se = c->sb + e->block_size;
}

static void _expect_read(struct mock_engine *e, struct io_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_READ;
	mc->fd_context = io_get_dev_context(dev);
	_set_block(e, mc, b);
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

static void _expect_write(struct mock_engine *e, struct io_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->fd_context = io_get_dev_context(dev);
	mc->sb = b * e->block_size;
	mc->se = mc->sb + e->block_size;
	mc->issue_r = true;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_partial_write(struct mock_engine *e,
                                  struct io_dev *dev,
                                  sector_t sb, sector_t se)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;

 	// FIXME: this can be reopened to remove a partial write, so we
 	// shouldn't call the io_get_fd(dev) until the validation step, but
 	// the dev object will not be held/exist at that point ...
 	mc->fd_context = io_get_dev_context(dev);
	mc->sb = sb;
	mc->se = se;
	mc->issue_r = true;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_partial_write_bad_issue(struct mock_engine *e,
                                  	    struct io_dev *dev,
                                  	    sector_t sb, sector_t se)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->fd_context = io_get_dev_context(dev);
	mc->sb = sb;
	mc->se = se;
	mc->issue_r = false;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_partial_write_bad_wait(struct mock_engine *e,
                                  	   struct io_dev *dev,
                                  	   sector_t sb, sector_t se)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->fd_context = io_get_dev_context(dev);
	mc->sb = sb;
	mc->se = se;
	mc->issue_r = true;
	mc->wait_r = false;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read_bad_issue(struct mock_engine *e, struct io_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_READ;
	mc->fd_context = io_get_dev_context(dev);
	_set_block(e, mc, b);
	mc->issue_r = false;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_write_bad_issue(struct mock_engine *e, struct io_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->fd_context = io_get_dev_context(dev);
	_set_block(e, mc, b);
	mc->issue_r = false;
	mc->wait_r = true;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_read_bad_wait(struct mock_engine *e, struct io_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_READ;
	mc->fd_context = io_get_dev_context(dev);
	_set_block(e, mc, b);
	mc->issue_r = true;
	mc->wait_r = false;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_write_bad_wait(struct mock_engine *e, struct io_dev *dev, block_address b)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_ISSUE;
	mc->match_args = true;
	mc->d = DIR_WRITE;
	mc->fd_context = io_get_dev_context(dev);
	_set_block(e, mc, b);
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

static void _expect_get_size(struct mock_engine *e, struct io_dev *dev, uint64_t s)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_GET_SIZE;
	mc->match_args = true;
	mc->fail = true;
	mc->fd_context = io_get_dev_context(dev);
	mc->size = s;
	dm_list_add(&e->expected_calls, &mc->list);
}

static void _expect_get_size_fail(struct mock_engine *e, struct io_dev *dev)
{
	struct mock_call *mc = malloc(sizeof(*mc));
	mc->m = E_GET_SIZE;
	mc->match_args = true;
	mc->fail = false;
	mc->fd_context = io_get_dev_context(dev);
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

static int _mock_open(struct io_engine *e, const char *path, unsigned flags, bool o_direct)
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
		T_ASSERT_EQUAL(fd, io_get_fd(mc->fd_context));
		T_ASSERT(sb == mc->sb);
		T_ASSERT(se == mc->se);
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

static bool _mock_get_size(struct io_engine *e, const char *path, int fd, uint64_t *s)
{
	bool r;
	struct mock_engine *me = _to_mock(e);
	struct mock_call *mc = _match_pop(me, E_GET_SIZE);
	if (mc->match_args && !mc->fail)
		T_ASSERT_EQUAL(fd, io_get_fd(mc->fd_context));

	*s = mc->size;
	r = mc->fail;
	free(mc);
	return r;
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
	m->e.get_size = _mock_get_size;

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
	struct io_manager *iom;
};

static struct fixture *_fixture_init(sector_t block_size, unsigned nr_cache_blocks,
                                     unsigned max_cache_devs,
                                     bool use_o_direct)
{
	struct fixture *f = malloc(sizeof(*f));

	f->me = _mock_create(16, block_size);
	T_ASSERT(f->me);

	_expect(f->me, E_MAX_IO);
	f->iom = io_manager_create(block_size, nr_cache_blocks, max_cache_devs,
                                   &f->me->e, use_o_direct);
	T_ASSERT(f->iom);

	return f;
}

static void _fixture_exit(struct fixture *f)
{
	_expect(f->me, E_DESTROY);
	io_manager_destroy(f->iom);

	free(f);
}

static void *_small_fixture_init(void)
{
	return _fixture_init(T_BLOCK_SIZE >> SECTOR_SHIFT, 16, SMALL_MAX_CACHE_DEVS, true);
}

static void _small_fixture_exit(void *context)
{
	_fixture_exit(context);
}

static void *_no_o_direct_fixture_init(void)
{
	return _fixture_init(T_BLOCK_SIZE >> SECTOR_SHIFT, 16, SMALL_MAX_CACHE_DEVS, false);
}

static void _no_o_direct_fixture_exit(void *context)
{
	_fixture_exit(context);
}

static void *_large_fixture_init(void)
{
	return _fixture_init(T_BLOCK_SIZE >> SECTOR_SHIFT, 1024, 256, true);
}

static void _large_fixture_exit(void *context)
{
	_fixture_exit(context);
}

/*----------------------------------------------------------------
 * Tests
 *--------------------------------------------------------------*/
static void good_create(sector_t block_size, unsigned nr_cache_blocks)
{
	struct io_manager *iom;
	struct mock_engine *me = _mock_create(16, 128);

	_expect(me, E_MAX_IO);
	iom = io_manager_create(block_size, nr_cache_blocks, 256, &me->e, true);
	T_ASSERT(iom);

	_expect(me, E_DESTROY);
	io_manager_destroy(iom);
}

static void bad_create(sector_t block_size, unsigned nr_cache_blocks)
{
	struct io_manager *iom;
	struct mock_engine *me = _mock_create(16, 128);

	_expect(me, E_MAX_IO);
	iom = io_manager_create(block_size, nr_cache_blocks, 256, &me->e, true);
	T_ASSERT(!iom);

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
	static unsigned _bad_examples[] = {3, 9, 13, 63};

	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_bad_examples); i++)
		bad_create(_bad_examples[i], 16);

	for (i = 1; i < 8; i++)
		good_create(i * 8, 16);
}

static void test_get_triggers_read(void *context)
{
	struct fixture *f = context;

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct block *b;

	_expect(f->me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(dev);
	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(io_get_block(f->iom, dev, 0, 0, &b));
	io_put_block(b);

	_expect_read(f->me, dev, 1);
	_expect(f->me, E_WAIT);
	T_ASSERT(io_get_block(f->iom, dev, 1, GF_DIRTY, &b));
	_expect_write(f->me, dev, 1);
	_expect(f->me, E_WAIT);
	io_put_block(b);

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_repeated_reads_are_cached(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;

	unsigned i;
	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);
	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	for (i = 0; i < 100; i++) {
		T_ASSERT(io_get_block(f->iom, dev, 0, 0, &b));
		io_put_block(b);
	}
	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_block_gets_evicted_with_many_reads(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;

	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	const unsigned nr_cache_blocks = 16;

	unsigned i;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect_read(me, dev, i);
		_expect(me, E_WAIT);
		T_ASSERT(io_get_block(iom, dev, i, 0, &b));
		io_put_block(b);
	}

	// Not enough cache blocks to hold this one
	_expect_read(me, dev, nr_cache_blocks);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, nr_cache_blocks, 0, &b));
	io_put_block(b);

	// Now if we run through we should find one block has been
	// evicted.  We go backwards because the oldest is normally
	// evicted first.
	_expect_read_any(me);
	_expect(me, E_WAIT);
	for (i = nr_cache_blocks; i; i--) {
		T_ASSERT(io_get_block(iom, dev, i - 1, 0, &b));
		io_put_block(b);
		T_ASSERT(io_is_well_formed(iom));
	}

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_prefetch_issues_a_read(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	const unsigned nr_cache_blocks = 16;

	unsigned i;
	struct block *b;

	_expect(me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	for (i = 0; i < nr_cache_blocks; i++) {
		// prefetch should not wait
		_expect_read(me, dev, i);
		io_prefetch_block(iom, dev, i);
	}
	_no_outstanding_expectations(me);

	for (i = 0; i < nr_cache_blocks; i++) {
		_expect(me, E_WAIT);
		T_ASSERT(io_get_block(iom, dev, i, 0, &b));
		io_put_block(b);
	}

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_too_many_prefetches_does_not_trigger_a_wait(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const unsigned nr_cache_blocks = 16;
	unsigned i;

	_expect(me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);
	for (i = 0; i < 10 * nr_cache_blocks; i++) {
		// prefetch should not wait
		if (i < nr_cache_blocks)
			_expect_read(me, dev, i);
		io_prefetch_block(iom, dev, i);
	}

	// Destroy will wait for any in flight IO triggered by prefetches.
	for (i = 0; i < nr_cache_blocks; i++)
		_expect(me, E_WAIT);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_dirty_data_gets_written_back(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	// Expect the read
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, GF_DIRTY, &b));
	io_put_block(b);

	// Expect the write
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);

	io_put_dev(dev);
	_expect(f->me, E_CLOSE);
}

static void test_zeroed_data_counts_as_dirty(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	// No read
	T_ASSERT(io_get_block(iom, dev, 0, GF_ZERO, &b));
	io_put_block(b);

	// Expect the write
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_flush_waits_for_all_dirty(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const unsigned count = 16;
	unsigned i;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	for (i = 0; i < count; i++) {
		if (i % 2) {
			T_ASSERT(io_get_block(iom, dev, i, GF_ZERO, &b));
		} else {
			_expect_read(me, dev, i);
			_expect(me, E_WAIT);
			T_ASSERT(io_get_block(iom, dev, i, 0, &b));
		}
		io_put_block(b);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect_write(me, dev, i);
	}

	for (i = 0; i < count; i++) {
		if (i % 2)
			_expect(me, E_WAIT);
	}

	io_flush(iom);
	_no_outstanding_expectations(me);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_multiple_files(void *context)
{
	static const char *_paths[] = {"/dev/dm-1", "/dev/dm-2", "/dev/dm-3", "/dev/dm-4"};

	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct io_dev *dev;
	struct block *b;
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(_paths); i++) {
		_expect(me, E_OPEN);
		dev = io_get_dev(iom, _paths[i], 0);
		_expect_read(me, dev, 0);
		_expect(me, E_WAIT);

		T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
		io_put_block(b);
		io_put_dev(dev);
	}

	for (i = 0; i < DM_ARRAY_SIZE(_paths); i++)
		_expect(me, E_CLOSE);
}

static void test_read_bad_issue(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	_expect_read_bad_issue(me, dev, 0);
	T_ASSERT(!io_get_block(iom, dev, 0, 0, &b));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_read_bad_issue_intermittent(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	_expect_read_bad_issue(me, dev, 0);
	T_ASSERT(!io_get_block(iom, dev, 0, 0, &b));

	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_read_bad_wait(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	_expect_read_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!io_get_block(iom, dev, 0, 0, &b));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_read_bad_wait_intermittent(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	_expect_read_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!io_get_block(iom, dev, 0, 0, &b));

	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_write_bad_issue_stops_flush(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block(iom, dev, 0, GF_ZERO, &b));
	_expect_write_bad_issue(me, dev, 0);
	io_put_block(b);
	T_ASSERT(!io_flush(iom));

	// we'll let it succeed the second time
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_write_bad_io_stops_flush(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block(iom, dev, 0, GF_ZERO, &b));
	_expect_write_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	io_put_block(b);
	T_ASSERT(!io_flush(iom));

	// we'll let it succeed the second time
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_invalidate_not_present(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct io_manager *iom = f->iom;

	_expect(f->me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);
	T_ASSERT(io_invalidate_block(iom, dev, 0));
	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_invalidate_present(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(f->me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);

	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	T_ASSERT(io_invalidate_block(iom, dev, 0));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_invalidate_after_read_error(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);
	_expect_read_bad_issue(me, dev, 0);
	T_ASSERT(!io_get_block(iom, dev, 0, 0, &b));
	T_ASSERT(io_invalidate_block(iom, dev, 0));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_invalidate_after_write_error(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);
	T_ASSERT(io_get_block(iom, dev, 0, GF_ZERO, &b));
	io_put_block(b);

	// invalidate should fail if the write fails
	_expect_write_bad_wait(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(!io_invalidate_block(iom, dev, 0));

	// and should succeed if the write does
	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_invalidate_block(iom, dev, 0));

	// a read is not required to get the block
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_invalidate_held_block(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	struct block *b;

	_expect(me, E_OPEN);
 	dev = io_get_dev(f->iom, path, 0);
	T_ASSERT(io_get_block(iom, dev, 0, GF_ZERO, &b));

	T_ASSERT(!io_invalidate_block(iom, dev, 0));

	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);
	io_put_block(b);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

//----------------------------------------------------------------

static void test_concurrent_devs(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev1, *dev2;

	_expect(me, E_OPEN);
	dev1 = io_get_dev(iom, path, 0);
	dev2 = io_get_dev(iom, path, 0);

	_expect(me, E_CLOSE);  // only one close

	io_put_dev(dev1);
	io_put_dev(dev2);
}

static void test_concurrent_devs_exclusive(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev1, *dev2;

	_expect(me, E_OPEN);
	dev1 = io_get_dev(iom, path, EF_EXCL);
	dev2 = io_get_dev(iom, path, EF_EXCL);

	_expect(me, E_CLOSE);  // only one close

	io_put_dev(dev1);
	io_put_dev(dev2);
}

static void test_exclusive_flags_gets_passed_to_engine(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;

	_expect_open(me, EF_EXCL);
	dev = io_get_dev(iom, path, EF_EXCL);
	_expect(me, E_CLOSE);
	io_put_dev(dev);

	_expect_open(me, EF_READ_ONLY);
	dev = io_get_dev(iom, path, EF_READ_ONLY);
	_expect(me, E_CLOSE);
	io_put_dev(dev);

	_expect_open(me, EF_EXCL | EF_READ_ONLY);
	dev = io_get_dev(iom, path, EF_EXCL | EF_READ_ONLY);
	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_reopen_exclusive_triggers_invalidate(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;
	struct block *b;

	_expect_open(me, 0);
	dev = io_get_dev(iom, path, 0);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);
	io_put_dev(dev);

	_no_outstanding_expectations(me);

	_expect(me, E_CLOSE);
	_expect_open(me, EF_EXCL);

	dev = io_get_dev(iom, path, EF_EXCL);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_concurrent_reopen_excl_fails(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;
	struct block *b;

	_expect_open(me, 0);
	dev = io_get_dev(iom, path, 0);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	_no_outstanding_expectations(me);

	T_ASSERT(!io_get_dev(iom, path, EF_EXCL));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_read_only_observed(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;
	struct block *b;

	// We can get a read lock
	_expect_open(me, EF_READ_ONLY);
	dev = io_get_dev(iom, path, EF_READ_ONLY);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);

	_no_outstanding_expectations(me);

	// but not a write lock
	T_ASSERT(!io_get_block(iom, dev, 0, GF_DIRTY, &b));
	T_ASSERT(!io_get_block(iom, dev, 0, GF_ZERO, &b));

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_upgrade_to_writeable(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;
	struct block *b;

	// We can get a read lock
	_expect_open(me, EF_READ_ONLY);
	dev = io_get_dev(iom, path, EF_READ_ONLY);
	T_ASSERT(dev);
	_expect_read(me, dev, 0);
	_expect(me, E_WAIT);
	T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
	io_put_block(b);
	io_put_dev(dev);

	_no_outstanding_expectations(me);

	// Upgrade to read/write, the open comes first in case
	// it fails.
	_expect_open(me, 0);
	_expect(me, E_CLOSE);
	dev = io_get_dev(iom, path, 0);

	_no_outstanding_expectations(me);

	T_ASSERT(io_get_block(iom, dev, 0, GF_DIRTY, &b));
	_no_outstanding_expectations(me);

	io_put_block(b);

	_expect_write(me, dev, 0);
	_expect(me, E_WAIT);

	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_get_size(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;
	sector_t size;

	// We can get a read lock
	_expect_open(me, EF_READ_ONLY);
	dev = io_get_dev(iom, path, EF_READ_ONLY);
	T_ASSERT(dev);

	_expect_get_size(me, dev, 12345);
	T_ASSERT(io_dev_size(dev, &size));
	T_ASSERT_EQUAL(size, 12345);
	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

static void test_get_size_fail(void *context)
{
	struct fixture *f = context;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	const char *path = "/dev/foo/bar";
	struct io_dev *dev;
	sector_t size;

	// We can get a read lock
	_expect_open(me, EF_READ_ONLY);
	dev = io_get_dev(iom, path, EF_READ_ONLY);
	T_ASSERT(dev);

	_expect_get_size_fail(me, dev);
	T_ASSERT(!io_dev_size(dev, &size));
	_expect(me, E_CLOSE);
	io_put_dev(dev);
}

//----------------------------------------------------------------
// Chasing a bug reported by dct

static void _cycle(struct fixture *f, unsigned nr_cache_blocks)
{
	char buffer[64];
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	unsigned i;
	struct block *b;

	for (i = 0; i < nr_cache_blocks; i++) {
		snprintf(buffer, sizeof(buffer) - 1, "/dev/dm-%u", i);
		_expect(me, E_OPEN);
		dev = io_get_dev(f->iom, buffer, 0);
		// prefetch should not wait
		_expect_read(me, dev, 0);
		io_prefetch_block(iom, dev, 0);
		io_put_dev(dev);
	}

	// This double checks the reads occur in response to the prefetch
	_no_outstanding_expectations(me);

	for (i = 0; i < nr_cache_blocks; i++) {
		snprintf(buffer, sizeof(buffer) - 1, "/dev/dm-%u", i);
		dev = io_get_dev(f->iom, buffer, 0);
		_expect(me, E_WAIT);
		T_ASSERT(io_get_block(iom, dev, 0, 0, &b));
		io_put_block(b);
		io_put_dev(dev);
	}

	_no_outstanding_expectations(me);
}

static void test_concurrent_reads_after_invalidate(void *context)
{
	struct fixture *f = context;
	char buffer[64];
	unsigned i, nr_cache_blocks = 16;
	struct io_dev *dev;

	_cycle(f, nr_cache_blocks);
	for (i = 0; i < nr_cache_blocks; i++) {
		snprintf(buffer, sizeof(buffer) - 1, "/dev/dm-%u", i);
		dev = io_get_dev(f->iom, buffer, 0);
        	io_invalidate_dev(f->iom, dev);
        	_expect(f->me, E_CLOSE);
        	io_put_dev(dev);
        	_no_outstanding_expectations(f->me);
	}

        _cycle(f, nr_cache_blocks);

        for (i = 0; i < nr_cache_blocks; i++)
	        _expect(f->me, E_CLOSE);
}

/*----------------------------------------------------------------
 * Partial block tests
 *--------------------------------------------------------------*/

static void test_reopen_without_direct(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0x1, &b));
	io_put_block(b);

	_expect(f->me, E_OPEN);   // FIXME: check use_o_direct isn't set
	_expect(f->me, E_CLOSE);

	// Expect the write
	_expect_partial_write(me, dev, 0, 1);
	_expect(me, E_WAIT);

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void _single_partial_write(struct fixture *f, uint64_t mask, sector_t sb, sector_t se)
{
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, mask, &b));
	io_put_block(b);

	// Expect the write
	_expect_partial_write(me, dev, sb, se);
	_expect(me, E_WAIT);

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_partial_write_at_start(void *context)
{
	struct fixture *f = context;
	_single_partial_write(f, 0x1ull, 0, 1);
}

static void test_partial_write_at_end(void *context)
{
	struct fixture *f = context;
	_single_partial_write(f, 0x1ull << 63, 63, 64);
}

static void test_partial_write_multiple_sectors_start(void *context)
{
	struct fixture *f = context;
	_single_partial_write(f, 0x7ull, 0, 3);
}

static void test_partial_write_multiple_sectors_end(void *context)
{
	struct fixture *f = context;
	_single_partial_write(f, 0x7ull << 61, 61, 64);
}

static void test_partial_write_multiple_sectors_middle(void *context)
{
	struct fixture *f = context;
	_single_partial_write(f, ((-1ull >> 2) << 1), 1, 63);
}

static void test_partial_write_separate_writes(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0x1ull, &b));
	io_put_block(b);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0x1ull << 63, &b));
	io_put_block(b);

	// Expect two writes
	_expect_partial_write(me, dev, 0, 1);
	_expect_partial_write(me, dev, 63, 64);
	_expect(me, E_WAIT);
	_expect(me, E_WAIT);

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_partial_write_overlapping_writes(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0xffffull << 8, &b));
	io_put_block(b);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0xffffull << 16, &b));
	io_put_block(b);

	// Expect one write
	_expect_partial_write(me, dev, 8, 32);
	_expect(me, E_WAIT);

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_partial_write_fail_bad_issue(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0xffffull << 8, &b));
	io_put_block(b);

	// Expect one write
	_expect_partial_write_bad_issue(me, dev, 8, 24);

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_partial_write_fail_bad_wait(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0xffffull << 8, &b));
	io_put_block(b);

	// Expect one write
	_expect_partial_write_bad_wait(me, dev, 8, 24);
	_expect(me, E_WAIT);

	T_ASSERT(!io_flush(iom));

	// Succeed the second time
	_expect_partial_write(me, dev, 8, 24);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_partial_write_one_bad_stops_all(void *context)
{
	struct fixture *f = context;
	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	struct block *b;

	_expect(f->me, E_OPEN);
	dev = io_get_dev(f->iom, path, 0);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0x1ull, &b));
	io_put_block(b);

	T_ASSERT(io_get_block_mask(iom, dev, 0, GF_ZERO, 0x1ull << 63, &b));
	io_put_block(b);

	// First write succeeds
	_expect_partial_write(me, dev, 0, 1);

	// second fails
	_expect_partial_write_bad_wait(me, dev, 63, 64);

	_expect(me, E_WAIT);
	_expect(me, E_WAIT);

	T_ASSERT(!io_flush(iom));

	// Succeed the second time
	_expect_partial_write(me, dev, 0, 1);
	_expect_partial_write(me, dev, 63, 64);

	_expect(me, E_WAIT);
	_expect(me, E_WAIT);

	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

/*----------------------------------------------------------------
 * Check utils use the blocks masks properly
 *--------------------------------------------------------------*/
static void test_zero_bytes_within_single_sector(void *context)
{
	struct fixture *f = context;

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	_expect(f->me, E_OPEN);

	dev = io_get_dev(f->iom, path, 0);

	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(io_zero_bytes(iom, dev, 34, 433)); 

	// Expect the write
	_expect_partial_write(me, dev, 0, 1);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_zero_bytes_spanning_sectors(void *context)
{
	struct fixture *f = context;

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	_expect(f->me, E_OPEN);

	dev = io_get_dev(f->iom, path, 0);

	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(io_zero_bytes(iom, dev, 700, 2345)); 

	// Expect the write
	_expect_partial_write(me, dev, 1, 6);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static uint64_t round_down(uint64_t n, uint64_t d)
{
	return (n / d) * d;
}

static uint64_t round_up(uint64_t n, uint64_t d)
{
	return ((n + d - 1) / d) * d;
}

static void test_zero_bytes_spanning_blocks(void *context)
{
	struct fixture *f = context;

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	uint64_t byte_start = T_BLOCK_SIZE + 700, byte_len = 2 * T_BLOCK_SIZE + 2345;

	_expect(f->me, E_OPEN);

	dev = io_get_dev(f->iom, path, 0);

	// the last block is prefetched first if it's partial ...
	_expect_read(f->me, dev, (byte_start + byte_len) / T_BLOCK_SIZE);
	_expect_read(f->me, dev, byte_start / T_BLOCK_SIZE);

	_expect(f->me, E_WAIT);
	_expect(f->me, E_WAIT);

	T_ASSERT(io_zero_bytes(iom, dev, byte_start, byte_len));
	_no_outstanding_expectations(f->me);

	// Expect the write
	// FIXME: how can we predict the order of these?
	_expect_partial_write(me, dev,
                              byte_start >> SECTOR_SHIFT,
                              round_up(byte_start, T_BLOCK_SIZE) >> SECTOR_SHIFT);

	_expect_write(me, dev, 2);
	_expect_partial_write(me, dev,
                              round_down(byte_start + byte_len, T_BLOCK_SIZE) >> SECTOR_SHIFT,
                              round_up(byte_start + byte_len, 512) >> SECTOR_SHIFT);

	_expect(me, E_WAIT);
	_expect(me, E_WAIT);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_write_bytes_within_single_sector(void *context)
{
	struct fixture *f = context;
	uint8_t buffer[1024 * 1024];

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	_expect(f->me, E_OPEN);

	dev = io_get_dev(f->iom, path, 0);

	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(io_write_bytes(iom, dev, 34, 433, buffer)); 

	// Expect the write
	_expect_partial_write(me, dev, 0, 1);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

static void test_write_bytes_spanning_sectors(void *context)
{
	struct fixture *f = context;
	uint8_t buffer[1024 * 1024];

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;

	_expect(f->me, E_OPEN);

	dev = io_get_dev(f->iom, path, 0);

	_expect_read(f->me, dev, 0);
	_expect(f->me, E_WAIT);
	T_ASSERT(io_write_bytes(iom, dev, 700, 2345, buffer)); 

	// Expect the write
	_expect_partial_write(me, dev, 1, 6);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}


static void test_write_bytes_spanning_blocks(void *context)
{
	struct fixture *f = context;

	const char *path = "/foo/bar/dev";
	struct io_dev *dev;
	struct mock_engine *me = f->me;
	struct io_manager *iom = f->iom;
	uint64_t byte_start = T_BLOCK_SIZE + 700, byte_len = 2 * T_BLOCK_SIZE + 2345;
	uint8_t buffer[1024 * 1024];

	_expect(f->me, E_OPEN);

	dev = io_get_dev(f->iom, path, 0);

	// the last block is prefetched first if it's partial ...
	_expect_read(f->me, dev, (byte_start + byte_len) / T_BLOCK_SIZE);
	_expect_read(f->me, dev, byte_start / T_BLOCK_SIZE);

	_expect(f->me, E_WAIT);
	_expect(f->me, E_WAIT);

	T_ASSERT(io_write_bytes(iom, dev, byte_start, byte_len, buffer));
	_no_outstanding_expectations(f->me);

	// Expect the write
	// FIXME: how can we predict the order of these?
	_expect_partial_write(me, dev,
                              byte_start >> SECTOR_SHIFT,
                              round_up(byte_start, T_BLOCK_SIZE) >> SECTOR_SHIFT);

	_expect_write(me, dev, 2);
	_expect_partial_write(me, dev,
                              round_down(byte_start + byte_len, T_BLOCK_SIZE) >> SECTOR_SHIFT,
                              round_up(byte_start + byte_len, 512) >> SECTOR_SHIFT);

	_expect(me, E_WAIT);
	_expect(me, E_WAIT);
	_expect(me, E_WAIT);
	T_ASSERT(io_flush(iom));

	_expect(f->me, E_CLOSE);
	io_put_dev(dev);
}

/*----------------------------------------------------------------
 * Max open files
 *--------------------------------------------------------------*/
static void test_get_max_cache_devs(void *context)
{
	struct fixture *f = context;
	T_ASSERT_EQUAL(io_max_cache_devs(f->iom), SMALL_MAX_CACHE_DEVS);
}

static void test_unable_to_hold_max_files(void *context)
{
	struct fixture *f = context;

	const char *path_fmt = "/foo/bar/dev_%u";
	char buffer[1024];
	struct io_dev *devs[SMALL_MAX_CACHE_DEVS];
	unsigned i;

	// Get one block from SMALL_MAX_CACHE_DEVS
	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++) {
		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);

		_expect(f->me, E_OPEN);
		devs[i] = io_get_dev(f->iom, buffer, 0);

		_expect_read(f->me, devs[i], 0);
		io_prefetch_block(f->iom, devs[i], 0);
	}
	_no_outstanding_expectations(f->me);

	// This should fail
	snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);
	T_ASSERT(!io_get_dev(f->iom, buffer, 0));

	// Wait for all those prefetches
	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++)
		_expect(f->me, E_WAIT);

	// Close all the devs
	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++) {
		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);
		_expect(f->me, E_CLOSE);
		io_put_dev(devs[i]);
	}
}

static void test_rolling_max_files(void *context)
{
	struct fixture *f = context;

	const char *path_fmt = "/foo/bar/dev_%u";
	char buffer[1024];
	struct io_dev *devs[SMALL_MAX_CACHE_DEVS];
	unsigned i;

	// Prep SMALL_MAX_CACHE_DEVS
	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++) {
		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);

		_expect(f->me, E_OPEN);
		devs[i] = io_get_dev(f->iom, buffer, 0);

		_expect_read(f->me, devs[i], 0);
		io_prefetch_block(f->iom, devs[i], 0);
	}
	_no_outstanding_expectations(f->me);

	for (i = 0; i < SMALL_MAX_CACHE_DEVS - 1; i++)
		_expect(f->me, E_WAIT);

	for (i = SMALL_MAX_CACHE_DEVS; i < 64; i++) {
		unsigned di = i % SMALL_MAX_CACHE_DEVS;

		_expect(f->me, E_WAIT);
		_expect(f->me, E_CLOSE);
		io_put_dev(devs[di]);

		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);
		_expect(f->me, E_OPEN);
		devs[di] = io_get_dev(f->iom, buffer, 0);
		T_ASSERT(devs[di]);

		_expect_read(f->me, devs[di], 0);
		io_prefetch_block(f->iom, devs[di], 0);
	}

	_expect(f->me, E_WAIT);

	// Close all the devs
	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++) {
		unsigned di = i % SMALL_MAX_CACHE_DEVS;
		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);
		_expect(f->me, E_CLOSE);
		io_put_dev(devs[di]);
	}
}

static void test_held_devs_are_not_evicted(void *context)
{
	struct fixture *f = context;

	const char *path_fmt = "/foo/bar/dev_%u";
	char buffer[1024];
	struct io_dev *devs[SMALL_MAX_CACHE_DEVS];
	unsigned i;

	// Get one block from SMALL_MAX_CACHE_DEVS
	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++) {
		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);

		_expect(f->me, E_OPEN);
		devs[i] = io_get_dev(f->iom, buffer, 0);

		_expect_read(f->me, devs[i], 0);
		io_prefetch_block(f->iom, devs[i], 0);
	}
	_no_outstanding_expectations(f->me);

	// drop all but the first dev
	for (i = 1; i < SMALL_MAX_CACHE_DEVS; i++) {
		snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);
		_expect(f->me, E_WAIT);
		io_put_dev(devs[i]);
	}

	// getting a new dev should evict the second dev
        _expect(f->me, E_WAIT);
	_expect(f->me, E_CLOSE);
	_expect(f->me, E_OPEN);
	snprintf(buffer, sizeof(buffer) - 1, path_fmt, i);
	devs[1] = io_get_dev(f->iom, buffer, 0);
	T_ASSERT(devs[1]);

	for (i = 0; i < SMALL_MAX_CACHE_DEVS; i++)
		_expect(f->me, E_CLOSE);

	io_put_dev(devs[0]);
	io_put_dev(devs[1]);
}

/*----------------------------------------------------------------
 * Top level
 *--------------------------------------------------------------*/

static struct test_suite *_tiny_tests(void)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/" path, desc, fn)
	T("create-destroy", "simple create/destroy", test_create);
	T("cache-blocks-positive", "nr cache blocks must be positive", test_nr_cache_blocks_must_be_positive);
	T("block-size-positive", "block size must be positive", test_block_size_must_be_positive);
	T("block-size-multiple-page", "block size must be a multiple of page size", test_block_size_must_be_multiple_of_page_size);
#undef T

	return ts;
}

static struct test_suite *_small_tests(void)
{
	struct test_suite *ts = test_suite_create(_small_fixture_init, _small_fixture_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/" path, desc, fn)
	T("get-reads", "io_get_block() triggers read", test_get_triggers_read);
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
	T("concurrent-devs", "a device may have more than one holder", test_concurrent_devs);
	T("concurrent-devs-exclusive", "a device, opened exclusively, may have more than one holder", test_concurrent_devs_exclusive);
	T("dev-flags-get-passed-to-engine", "EF_EXCL and EF_READ_ONLY get passed down", test_exclusive_flags_gets_passed_to_engine);
	T("reopen-excl-invalidates", "reopening a dev EF_EXCL indicates you want to invalidate everything", test_reopen_exclusive_triggers_invalidate);
	T("concurrent-reopen-excl-fails", "you can't reopen a dev EF_EXCL if there's already a holder", test_concurrent_reopen_excl_fails);
	T("read-only-observed", "You can't use GF_DIRTY or GF_ZERO with a read-only dev", test_read_only_observed);
	T("upgrade-to-write", "Upgrading forces a reopen (but not invalidate)", test_upgrade_to_writeable);
	T("dev-size", "we can get the dev size", test_get_size);
	T("dev-size-fail", "failure gets handed up from the engine", test_get_size_fail);
#undef T

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/partial-write/" path, desc, fn)
	T("reopen-without-o-direct", "Partial writes prevent O_DIRECT being used", test_reopen_without_direct);
#undef T

	return ts;
}


static struct test_suite *_partial_tests(void)
{
	struct test_suite *ts = test_suite_create(_no_o_direct_fixture_init,
                                                  _no_o_direct_fixture_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/partial-write/" path, desc, fn)
	T("single-start", "Writes a single sector at the start of a block", test_partial_write_at_start);
	T("single-end", "Writes a single sector at the end of a block", test_partial_write_at_end);
	T("multi-start", "Writes multiple sectors at the start of a block", test_partial_write_multiple_sectors_start);
	T("multi-end", "Writes multiple sectors at the end of a block", test_partial_write_multiple_sectors_end);
	T("multi-middle", "Writes multiple sectors at the middle of a block", test_partial_write_multiple_sectors_middle);
	T("start-end", "Writes sectors at the start and end of a block", test_partial_write_separate_writes);
	T("overlapping", "Writes sectors that overlap", test_partial_write_overlapping_writes);
	T("bad-issue", "Partial write can fail issue", test_partial_write_fail_bad_issue);
	T("bad-wait", "Partial write can fail wait", test_partial_write_fail_bad_wait);
	T("bad-part", "Bad IO on part of block fails whole block", test_partial_write_one_bad_stops_all);
#undef T

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/partial-write/zero-bytes/" path, desc, fn)
	T("within-single-sector", "Zero bytes only touches a single sector", test_zero_bytes_within_single_sector);
	T("spanning-sectors", "Zero bytes only touches correct multiple sectors", test_zero_bytes_spanning_sectors);
	T("spanning-blocks", "Zero bytes only touches correct multiple blocks", test_zero_bytes_spanning_blocks);
#undef T

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/partial-write/write-bytes/" path, desc, fn)
	T("within-single-sector", "Zero bytes only touches a single sector", test_write_bytes_within_single_sector);
	T("spanning-sectors", "Zero bytes only touches correct multiple sectors", test_write_bytes_spanning_sectors);
	T("spanning-blocks", "Zero bytes only touches correct multiple blocks", test_write_bytes_spanning_blocks);
#undef T

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/partial-write/max-cache-devs/" path, desc, fn)
	T("get-max", "Check accessor function", test_get_max_cache_devs);
	T("open-too-many", "Try and hold too many open devs", test_unable_to_hold_max_files);
	T("rolling-max", "continually opening and closing causes eviction", test_rolling_max_files);
	T("held-devs-are-not-evicted", "when choosing a dev to evict because max reached, ignore held", test_held_devs_are_not_evicted);
#undef T

	return ts;
}

static struct test_suite *_large_tests(void)
{
	struct test_suite *ts = test_suite_create(_large_fixture_init, _large_fixture_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/device/io-manager/core/" path, desc, fn)
	T("concurrent-reads-after-invalidate", "prefetch should still issue concurrent reads after invalidate",
          test_concurrent_reads_after_invalidate);
	T("flush-waits", "flush waits for all dirty", test_flush_waits_for_all_dirty);
#undef T

	return ts;
}

void io_manager_tests(struct dm_list *all_tests)
{
        dm_list_add(all_tests, &_tiny_tests()->list);
	dm_list_add(all_tests, &_small_tests()->list);
	dm_list_add(all_tests, &_partial_tests()->list);
	dm_list_add(all_tests, &_large_tests()->list);
}
