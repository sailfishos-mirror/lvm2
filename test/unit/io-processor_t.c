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

#include "lib/device/io-processor.h"

#include "framework.h"
#include "units.h"

#include "base/memory/zalloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SHOW_MOCK_CALLS 0

//----------------------------------------------------------------

static unsigned _rand(unsigned max)
{
	return rand() % max;
}

//----------------------------------------------------------------

enum method {
	M_DESTROY,
	M_BATCH_SIZE,
	M_GET_DEV,
	M_PUT_DEV,
	M_PREFETCH,
	M_READ,
	M_TASK,
	M_ERROR
};

struct expectation {
	struct dm_list list;

	enum method m;
	bool succeed;
};

struct dev {
	struct dm_list list;
	unsigned id;
};

struct mock_ops {
	struct processor_ops ops;

	struct dm_list expectations;
	unsigned batch_size;
};

static const char *_show_method(enum method m)
{
	switch (m) {
	case M_DESTROY: return "destroy";
	case M_BATCH_SIZE: return "batch_size";
	case M_GET_DEV: return "get_dev";
	case M_PUT_DEV: return "put_dev";
	case M_PREFETCH: return "prefetch";
	case M_READ: return "read";
	case M_TASK: return "task";
	case M_ERROR: return "error";
	}

	return "<unknown>";
}

static struct expectation *_match_pop(struct mock_ops *mops, enum method m)
{
	struct expectation *e;

	if (dm_list_empty(&mops->expectations))
		test_fail("unexpected call to method %s\n", _show_method(m));

	e = dm_list_item(mops->expectations.n, struct expectation);
	dm_list_del(&e->list);

	if (e->m != m)
		test_fail("expected %s, but got %s\n", _show_method(e->m), _show_method(m));
#if SHOW_MOCK_CALLS
	else
		fprintf(stderr, "%s called (expected)\n", _show_method(m));
#endif

	return e;
}

static bool _match(struct mock_ops *mops, enum method m)
{
	struct expectation *e = _match_pop(mops, m);

	bool r = e->succeed;
	free(e);

	return r;
}

static void _mock_destroy(struct processor_ops *ops)
{
	struct mock_ops *mops = container_of(ops, struct mock_ops, ops);
	struct expectation *e, *tmp;

	_match(mops, M_DESTROY);
	if (!dm_list_empty(&mops->expectations)) {
		dm_list_iterate_items_safe(e, tmp, &mops->expectations)
			fprintf(stderr, "   %s", _show_method(e->m));

		test_fail("unsatisfied expectations");
	}

	free(mops);
}

static unsigned _mock_batch_size(struct processor_ops *ops)
{
	struct mock_ops *mops = container_of(ops, struct mock_ops, ops);
	_match(mops, M_BATCH_SIZE);
	return mops->batch_size;
}

static void *_mock_get_dev(struct processor_ops *ops, const char *path, unsigned flags)
{
	struct dev *d;
	struct mock_ops *mops = container_of(ops, struct mock_ops, ops);

	struct expectation *e = _match_pop(mops, M_GET_DEV);

	if (!e->succeed) {
		free(e);
		return NULL;
	}

	free(e);

	d = zalloc(sizeof(*d));
	dm_list_init(&d->list);
	d->id = 0;

	return d;
}

static void _mock_put_dev(struct processor_ops *ops, void *dev)
{
	struct dev *d = dev;
	struct mock_ops *mops = container_of(ops, struct mock_ops, ops);
	_match(mops, M_PUT_DEV);
	free(d);
}

static unsigned _mock_prefetch_bytes(struct processor_ops *ops, void *dev, uint64_t start, size_t len)
{
	struct mock_ops *mops = container_of(ops, struct mock_ops, ops);
	_match(mops, M_PREFETCH);
	return 1;
}

static bool _mock_read_bytes(struct processor_ops *ops, void *dev, uint64_t start, size_t len, void *data)
{
	struct mock_ops *mops = container_of(ops, struct mock_ops, ops);
	return _match(mops, M_READ);
}

static void _mock_task(void *context, void *data, uint64_t len)
{
	struct mock_ops *mops = context;
	_match(mops, M_TASK);
}

static void _mock_error(void *context)
{
	struct mock_ops *mops = context;
	_match(mops, M_ERROR);
}

static void _expect(struct mock_ops *mops, enum method m)
{
	struct expectation *e = zalloc(sizeof(*e));

	e->m = m;
	e->succeed = true;
	dm_list_add(&mops->expectations, &e->list);
}

static void _expect_fail(struct mock_ops *mops, enum method m)
{
	struct expectation *e = zalloc(sizeof(*e));

	e->m = m;
	e->succeed = false;
	dm_list_add(&mops->expectations, &e->list);
}

static struct mock_ops *_mock_ops_create(void)
{
	struct mock_ops *mops = zalloc(sizeof(*mops));

	mops->ops.destroy = _mock_destroy;
	mops->ops.batch_size = _mock_batch_size;
	mops->ops.get_dev = _mock_get_dev;
	mops->ops.put_dev = _mock_put_dev;
	mops->ops.prefetch_bytes = _mock_prefetch_bytes;
	mops->ops.read_bytes = _mock_read_bytes;

	dm_list_init(&mops->expectations);
	mops->batch_size = 1;

	return mops;
}

//----------------------------------------------------------------

struct fixture {
	struct mock_ops *mops;
	struct io_processor *iop;
};

static void *_fix_init(void)
{
	struct fixture *f;
	struct mock_ops *mops;
	struct io_processor *iop;

	f = zalloc(sizeof(*f));
	T_ASSERT(f);

	mops = _mock_ops_create();
	T_ASSERT(mops);

	iop = io_processor_create_internal(&mops->ops, _mock_task, _mock_error);
	T_ASSERT(iop);

	f->mops = mops;
	f->iop = iop;

	return f;
}

static void _fix_exit(void *context)
{
	struct fixture *f = context;

	_expect(f->mops, M_DESTROY);
	io_processor_destroy(f->iop);
	free(f);
}

//----------------------------------------------------------------
// Tests
//----------------------------------------------------------------

static void _test_create_destroy(void *context)
{
	// empty
}

static void _test_add_but_no_run(void *context)
{
	struct fixture *f = context;

	unsigned i;
	char buffer[128];

	for (i = 0; i < 100; i++) {
		snprintf(buffer, sizeof(buffer), "/dev/imaginary-%u", i);
		io_processor_add(f->iop, buffer, _rand(10000), _rand(100), NULL);
	}
}

static unsigned min(unsigned lhs, unsigned rhs)
{
	if (lhs < rhs)
		return lhs;

	if (rhs < lhs)
		return rhs;

	return lhs;
}

static void check_batches(struct fixture *f, unsigned nr_areas, unsigned batch_size)
{
	unsigned i, b, nr_batches;
	const char *path = "/dev/foo-1";

	f->mops->batch_size = batch_size;
	_expect(f->mops, M_BATCH_SIZE);

	for (i = 0; i < nr_areas; i++)
		io_processor_add(f->iop, path, 0, 128, f->mops);

	nr_batches = (nr_areas + (batch_size - 1)) / batch_size;
	for (b = 0; b < nr_batches; b++) {
		unsigned count = min(nr_areas - (b * batch_size), batch_size);

		for (i = 0; i < count; i++) {
			_expect(f->mops, M_GET_DEV);
			_expect(f->mops, M_PREFETCH);
			_expect(f->mops, M_PUT_DEV);
		}

		for (i = 0; i < count; i++) {
			_expect(f->mops, M_GET_DEV);
			_expect(f->mops, M_READ);
			_expect(f->mops, M_PUT_DEV);
			_expect(f->mops, M_TASK);
		}
	}

	io_processor_exec(f->iop);
}

static void _test_area_vs_batch_size(void *context)
{
	struct fixture *f = context;
	check_batches(f, 2, 1);
	check_batches(f, 2, 2);
	check_batches(f, 128, 4);
	check_batches(f, 512, 1024);
}

static void _test_get_fails(void *context)
{
	struct fixture *f = context;
	const char *path = "/dev/foo-1";

	io_processor_add(f->iop, path, 0, 128, f->mops);

	_expect(f->mops, M_BATCH_SIZE);
	_expect_fail(f->mops, M_GET_DEV);
	_expect(f->mops, M_ERROR);

	io_processor_exec(f->iop);
}

static void _test_second_get_dev_fails(void *context)
{
	struct fixture *f = context;
	const char *path = "/dev/foo-1";

	io_processor_add(f->iop, path, 0, 128, f->mops);

	_expect(f->mops, M_BATCH_SIZE);
	_expect(f->mops, M_GET_DEV);
	_expect(f->mops, M_PREFETCH);
	_expect(f->mops, M_PUT_DEV);
	_expect_fail(f->mops, M_GET_DEV);
	_expect(f->mops, M_ERROR);

	io_processor_exec(f->iop);
}

static void _test_read_fails(void *context)
{
	struct fixture *f = context;
	const char *path = "/dev/foo-1";

	io_processor_add(f->iop, path, 0, 128, f->mops);

	_expect(f->mops, M_BATCH_SIZE);
	_expect(f->mops, M_GET_DEV);
	_expect(f->mops, M_PREFETCH);
	_expect(f->mops, M_PUT_DEV);
	_expect(f->mops, M_GET_DEV);
	_expect_fail(f->mops, M_READ);
	_expect(f->mops, M_PUT_DEV);
	_expect(f->mops, M_ERROR);

	io_processor_exec(f->iop);
}

static void _test_one_bad_one_good(void *context)
{
	struct fixture *f = context;
	const char *path1 = "/dev/foo-1";
	const char *path2 = "/dev/foo-2";

	io_processor_add(f->iop, path1, 0, 128, f->mops);
	io_processor_add(f->iop, path2, 0, 128, f->mops);

	f->mops->batch_size = 2;
	_expect(f->mops, M_BATCH_SIZE);

	_expect_fail(f->mops, M_GET_DEV);
	_expect(f->mops, M_ERROR);

	_expect(f->mops, M_GET_DEV);
	_expect(f->mops, M_PREFETCH);
	_expect(f->mops, M_PUT_DEV);

	_expect(f->mops, M_GET_DEV);
	_expect(f->mops, M_READ);
	_expect(f->mops, M_PUT_DEV);
	_expect(f->mops, M_TASK);

	io_processor_exec(f->iop);
}

static void _test_one_good_one_bad(void *context)
{
	struct fixture *f = context;
	const char *path1 = "/dev/foo-1";
	const char *path2 = "/dev/foo-2";

	io_processor_add(f->iop, path1, 0, 128, f->mops);
	io_processor_add(f->iop, path2, 0, 128, f->mops);

	f->mops->batch_size = 2;
	_expect(f->mops, M_BATCH_SIZE);

	_expect(f->mops, M_GET_DEV);
	_expect(f->mops, M_PREFETCH);
	_expect(f->mops, M_PUT_DEV);

	_expect_fail(f->mops, M_GET_DEV);
	_expect(f->mops, M_ERROR);

	_expect(f->mops, M_GET_DEV);
	_expect(f->mops, M_READ);
	_expect(f->mops, M_PUT_DEV);
	_expect(f->mops, M_TASK);

	io_processor_exec(f->iop);
}

//----------------------------------------------------------------

static struct test_suite *_tests(void)
{
	struct test_suite *ts = test_suite_create(_fix_init, _fix_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#define T(path, desc, fn) register_test(ts, "/base/device/io-processor/" path, desc, fn)
	T("create-destroy", "empty test", _test_create_destroy);
	T("create-add-destroy", "add jobs, but don't run them", _test_add_but_no_run);
	T("areas-vs-batch-size", "process different nrs of areas vs batch size", _test_area_vs_batch_size);
	T("get-fails", "get failure is propogated", _test_get_fails);
	T("get-fails-second", "second get failure is propogated", _test_second_get_dev_fails);
	T("read-fails", "read failure is propogated", _test_read_fails);
	T("one-bad-one-good", "one bad, one good", _test_one_bad_one_good);
	T("one-good-one-bad", "one good, one bad", _test_one_good_one_bad);
#undef T

	return ts;
}

void io_processor_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_tests()->list);
}

//----------------------------------------------------------------
