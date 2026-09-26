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

#include "units.h"
#include "lib/device/bcache.h"
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void bcache_threads_test_hold(int hold);
void bcache_threads_test_fail_create_after(int count);

//----------------------------------------------------------------

#define SECTOR_SHIFT 9
#define SECTOR_SIZE 512
#define BLOCK_SIZE_SECTORS 8
#define PAGE_SIZE_SECTORS ((TEST_PAGE_SIZE) >> SECTOR_SHIFT)
#define NR_BLOCKS 64

struct fixture {
	struct io_engine *e;
	uint8_t *data;

	char fname[64];
	int fd;
	int di;
};

static void _fill_buffer(uint8_t *buffer, uint8_t seed, size_t count)
{
	unsigned i;
	uint8_t b = seed;

	for (i = 0; i < count; i++) {
		buffer[i] = b;
		b = ((b << 5) + b) + i;
	}
}

static void _check_buffer(uint8_t *buffer, uint8_t seed, size_t count)
{
	unsigned i;
	uint8_t b = seed;

	for (i = 0; i < count; i++) {
		T_ASSERT_EQUAL(buffer[i], b);
		b = ((b << 5) + b) + i;
	}
}

static void _print_buffer(const char *name, uint8_t *buffer, size_t count)
{
	unsigned col;

	fprintf(stderr, "%s:\n", name);
	while (count) {
		for (col = 0; count && col < 20; col++) {
			fprintf(stderr, "%x, ", (unsigned) *buffer);
			buffer++;
			count--;
		}
		fprintf(stderr, "\n");
	}
}

static void *_fix_init(void)
{
	struct fixture *f = malloc(sizeof(*f));

	T_ASSERT(f);
	f->e = NULL;
#ifdef HAVE_LIBAIO_H
	f->e = create_async_io_engine(0);
	T_ASSERT(f->e);
#endif
	if (posix_memalign((void **) &f->data, TEST_PAGE_SIZE, SECTOR_SIZE * BLOCK_SIZE_SECTORS))
		test_fail("posix_memalign failed");

	snprintf(f->fname, sizeof(f->fname), "unit-test-XXXXXX");
	/* coverity[secure_temp] don't care */
	f->fd = mkstemp(f->fname);
	T_ASSERT(f->fd >= 0);

	_fill_buffer(f->data, 123, SECTOR_SIZE * BLOCK_SIZE_SECTORS);

	T_ASSERT(write(f->fd, f->data, SECTOR_SIZE * BLOCK_SIZE_SECTORS) == SECTOR_SIZE * BLOCK_SIZE_SECTORS);
	T_ASSERT(lseek(f->fd, 0, SEEK_SET) != -1);

	return f;
}

static void _fix_exit(void *fixture)
{
	struct fixture *f = fixture;

	bcache_threads_test_hold(0);
	bcache_threads_test_fail_create_after(-1);

	if (f) {
		(void) close(f->fd);
		(void) unlink(f->fname);
		free(f->data);
		if (f->e)
			f->e->destroy(f->e);
		free(f);
	}
}

static void _test_create(void *fixture)
{
	// empty
}

struct io {
	bool completed;
	int error;
};

static void _io_init(struct io *io)
{
	io->completed = false;
	io->error = 0;
}

static void _complete_io(void *context, int io_error)
{
	struct io *io = context;
	io->completed = true;
	io->error = io_error;
}

#ifdef HAVE_LIBAIO_H
static void _test_read(void *fixture)
{
	struct fixture *f = fixture;
	struct io io;
	struct bcache *cache = bcache_create(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, f->e);
	T_ASSERT(cache);

	f->di = bcache_set_fd(f->fd);

	T_ASSERT(f->di >= 0);

	_io_init(&io);
	T_ASSERT(f->e->issue(f->e, DIR_READ, f->fd, 0, BLOCK_SIZE_SECTORS, f->data, &io));
	T_ASSERT(f->e->wait(f->e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT(!io.error);

	_check_buffer(f->data, 123, SECTOR_SIZE * BLOCK_SIZE_SECTORS);

	bcache_destroy(cache);
	f->e = NULL;   // already destroyed
}

static void _test_write(void *fixture)
{
	struct fixture *f = fixture;
	struct io io;
	struct bcache *cache = bcache_create(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, f->e);
	T_ASSERT(cache);

	f->di = bcache_set_fd(f->fd);

	T_ASSERT(f->di >= 0);

	_io_init(&io);
	T_ASSERT(f->e->issue(f->e, DIR_WRITE, f->fd, 0, BLOCK_SIZE_SECTORS, f->data, &io));
	T_ASSERT(f->e->wait(f->e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT(!io.error);

	bcache_destroy(cache);
	f->e = NULL;   // already destroyed
}

static void _test_write_bytes(void *fixture)
{
	struct fixture *f = fixture;

	unsigned offset = 345;
	char buf_out[32];
	char buf_in[32];
	struct bcache *cache = bcache_create(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, f->e);
	T_ASSERT(cache);

	f->di = bcache_set_fd(f->fd);

	// T_ASSERT(bcache_read_bytes(cache, f->di, offset, sizeof(buf_in), buf_in));
	_fill_buffer((uint8_t *) buf_out, 234, sizeof(buf_out));
	T_ASSERT(bcache_write_bytes(cache, f->di, offset, sizeof(buf_out), buf_out));
	T_ASSERT(bcache_read_bytes(cache, f->di, offset, sizeof(buf_in), buf_in));

	if (memcmp(buf_out, buf_in, sizeof(buf_out))) {
		_print_buffer("buf_out", (uint8_t *) buf_out, sizeof(buf_out));
		_print_buffer("buf_in", (uint8_t *) buf_in, sizeof(buf_in));
	}
	T_ASSERT(!memcmp(buf_out, buf_in, sizeof(buf_out)));

	bcache_destroy(cache);
	f->e = NULL;   // already destroyed
}

/*
 * Test that _async_destroy() skips io_destroy() after fork().
 *
 * The aio_context is created in the parent process. After fork() the
 * child inherits the context value but must not call io_destroy() on
 * it - only the original process should do that.  _async_destroy()
 * compares aio_context_pid against getpid() to guard this.
 *
 * Also exercises the normal io_destroy() path in the parent, verifying
 * that the negative-return error reporting (commit 512a39448) works
 * without crashing (io_destroy returns -errno, not -1+errno).
 */
static void _test_destroy_after_fork(void *fixture)
{
	struct io_engine *e;
	pid_t pid;
	int status;

	e = create_async_io_engine(0);
	T_ASSERT(e);

	pid = fork();
	T_ASSERT(pid >= 0);

	if (!pid) {
		/*
		 * Child: destroy must skip io_destroy() because pid
		 * differs from aio_context_pid.  If it incorrectly
		 * calls io_destroy() the parent's context gets
		 * invalidated and the parent's destroy will fail.
		 */
		e->destroy(e);
		_exit(0);
	}

	/* Parent: wait for child to finish its destroy first */
	T_ASSERT(waitpid(pid, &status, 0) == pid);
	T_ASSERT(WIFEXITED(status) && !WEXITSTATUS(status));

	/*
	 * Parent: destroy calls io_destroy() for real.
	 * This would fail if the child incorrectly destroyed
	 * the shared aio_context.
	 */
	e->destroy(e);
	/* coverity[leaked_storage] e freed by e->destroy */
}

/*
 * Test that _async_wait() is interruptible by SIGINT/SIGTERM (via
 * sigint_allow()), but retries on other signals such as SIGALRM.
 *
 * The retry loop in _async_wait() is:
 *   do { r = io_getevents(...); } while (r == -EINTR && !sigint_caught());
 *
 * So EINTR from a stray signal retries; EINTR after SIGINT/SIGTERM
 * (which set sigint_caught()) stops and returns false.
 *
 * Strategy: call wait() with no I/O submitted so io_getevents(min_nr=1)
 * must block.  A child process sends SIGINT to the parent after a short
 * delay, interrupting io_getevents().  Since sigint_allow() installed
 * _catch_sigint (which sets sigint_caught()), the retry loop exits and
 * wait() returns false.
 *
 * Why not raise(SIGINT) before calling wait()?
 * raise() delivers the signal immediately, before io_getevents() is
 * even called.  _catch_sigint sets sigint_caught() but io_getevents()
 * then blocks forever because no I/O is pending and the signal is
 * already consumed.  The child-process approach ensures the signal
 * arrives while io_getevents() is actually blocked.
 *
 * Why not issue I/O and race a signal?
 * Linux AIO on regular files (and character devices like /dev/zero,
 * /dev/urandom) completes synchronously inside io_submit() - the
 * kernel posts the completion before io_submit() returns, so
 * io_getevents() never blocks.  Only O_DIRECT on a real block device
 * goes through the true async path.  Calling wait() with no I/O
 * pending guarantees io_getevents() blocks, making the test
 * deterministic without needing a block device.
 */
static void _test_wait_eintr(void *fixture)
{
	struct io_engine *e;
	pid_t child;
	int status;

	e = create_async_io_engine(0);
	T_ASSERT(e);

	/*
	 * Arm the LVM SIGINT/SIGTERM handler (clears SA_RESTART,
	 * installs _catch_sigint which sets sigint_caught()).
	 */
	sigint_allow();

	/*
	 * Fork a child that waits 10ms then sends SIGINT to the parent.
	 * By then the parent is guaranteed to be inside io_getevents().
	 */
	child = fork();
	T_ASSERT(child >= 0);
	if (!child) {
		usleep(10000);
		kill(getppid(), SIGINT);
		_exit(0);
	}

	/*
	 * No I/O submitted: io_getevents(min_nr=1) blocks until SIGINT
	 * arrives from the child.  sigint_caught() is then set so the
	 * retry loop exits and wait() returns false.
	 */
	T_ASSERT(!e->wait(e, _complete_io));
	T_ASSERT(sigint_caught());

	T_ASSERT(waitpid(child, &status, 0) == child);
	T_ASSERT(WIFEXITED(status) && !WEXITSTATUS(status));

	sigint_restore();
	sigint_clear();

	e->destroy(e);
	/* coverity[leaked_storage] freed by destroy */
}

#endif /* HAVE_LIBAIO_H */

/*
 * Read/write sanity for an arbitrary engine.  The engine is destroyed on
 * success, so callers must not destroy it again.
 */
static void _run_engine_rw(struct fixture *f, struct io_engine *e, enum dir d)
{
	struct io io;
	struct bcache *cache;

	T_ASSERT(e);

	cache = bcache_create(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, e);
	T_ASSERT(cache);

	f->di = bcache_set_fd(f->fd);
	T_ASSERT(f->di >= 0);

	_io_init(&io);
	T_ASSERT(e->issue(e, d, f->fd, 0, BLOCK_SIZE_SECTORS, f->data, &io));
	T_ASSERT(e->wait(e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT(!io.error);

	if (d == DIR_READ)
		_check_buffer(f->data, 123, SECTOR_SIZE * BLOCK_SIZE_SECTORS);

	bcache_destroy(cache);
}

/*
 * Issue a read that goes past EOF.  The file holds BLOCK_SIZE_SECTORS
 * sectors, so reading 2x that must produce a short (but valid) transfer
 * rather than an I/O error.  This deterministically exercises the
 * short-read reporting path of an engine.
 */
static void _run_engine_short_read(struct io_engine *e, struct fixture *f)
{
	struct io io;
	void *buf;

	T_ASSERT(e);

	if (posix_memalign(&buf, TEST_PAGE_SIZE, 2 * SECTOR_SIZE * BLOCK_SIZE_SECTORS))
		test_fail("posix_memalign failed");

	_io_init(&io);
	T_ASSERT(e->issue(e, DIR_READ, f->fd, 0,
			  2 * BLOCK_SIZE_SECTORS, buf, &io));
	T_ASSERT(e->wait(e, _complete_io));
	T_ASSERT(io.completed);
	/* At least one sector was read, so this is not an error. */
	T_ASSERT(!io.error);

	free(buf);
	e->destroy(e);
}

static void _test_read_threads(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_threads_io_engine(4), DIR_READ);
}

static void _test_write_threads(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_threads_io_engine(4), DIR_WRITE);
}

static void _test_short_read_sync(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_short_read(create_sync_io_engine(), f);
}

#ifdef HAVE_LIBAIO_H
static void _test_short_read_async(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_short_read(create_async_io_engine(0), f);
}
#endif

static void _test_short_read_threads(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_short_read(create_threads_io_engine(4), f);
}

#ifdef HAVE_LIBURING_H
static void _test_read_uring(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_uring_io_engine(4), DIR_READ);
}

static void _test_short_read_uring(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_short_read(create_uring_io_engine(4), f);
}
#endif

/*
 * Issue one read and destroy without wait(). Async must reap the
 * completion or io_destroy fails with the buffer still referenced.
 * Threads must join the worker. Sync must free the queued completion.
 * destroy() reports true only when it reaped everything, so the caller
 * knows the buffers are safe to free.
 */
static void _destroy_after_issue(struct io_engine *e, int fd, void *buf)
{
	struct io io;

	T_ASSERT(e);
	_io_init(&io);
	T_ASSERT(e->issue(e, DIR_READ, fd, 0, BLOCK_SIZE_SECTORS, buf, &io));
	T_ASSERT(e->destroy(e));
}

static void _test_destroy_inflight_sync(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_sync_io_engine(), f->fd, f->data);
}

static void _test_destroy_inflight_threads(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_threads_io_engine(2), f->fd, f->data);
}

static void _test_threads_partial_create_failure(void *fixture)
{
	struct io_engine *e;

	(void) fixture;

	bcache_threads_test_fail_create_after(1);
	alarm(5);
	e = create_threads_io_engine(2);
	alarm(0);
	bcache_threads_test_fail_create_after(-1);
	T_ASSERT(!e);

	/* A later creation must still work after the partially built pool exits. */
	e = create_threads_io_engine(2);
	T_ASSERT(e);
	T_ASSERT(e->destroy(e));
}

#ifdef HAVE_LIBAIO_H
static void _test_destroy_inflight_async(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_async_io_engine(0), f->fd, f->data);
}
#endif

#ifdef HAVE_LIBURING_H
static void _test_destroy_inflight_uring(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_uring_io_engine(4), f->fd, f->data);
}

/*
 * io_uring_wait_cqe() with nothing submitted blocks. SIGINT must make
 * wait() return false instead of retrying forever.
 */
static void _test_uring_wait_eintr(void *fixture)
{
	struct io_engine *e;
	pid_t child;
	int status;

	(void) fixture;

	e = create_uring_io_engine(4);
	T_ASSERT(e);
	sigint_allow();

	child = fork();
	T_ASSERT(child >= 0);
	if (!child) {
		usleep(20000);
		kill(getppid(), SIGINT);
		_exit(0);
	}

	alarm(5);
	T_ASSERT(!e->wait(e, _complete_io));
	alarm(0);
	T_ASSERT(sigint_caught());

	T_ASSERT(waitpid(child, &status, 0) == child);
	T_ASSERT(WIFEXITED(status) && !WEXITSTATUS(status));

	sigint_restore();
	sigint_clear();
	e->destroy(e);
}
#endif

/*
 * bcache_threads_test_hold() makes the worker block in pause(), which is
 * a cancellation point, before pread. wait() must return on SIGINT, and
 * destroy() must cancel that wait instead of joining forever.
 */
static void _test_threads_inflight_int(void *fixture)
{
	struct fixture *f = fixture;
	struct io_engine *e;
	struct io io;
	pid_t child;
	int status;

	bcache_threads_test_hold(1);

	e = create_threads_io_engine(1);
	T_ASSERT(e);

	sigint_allow();
	_io_init(&io);
	T_ASSERT(e->issue(e, DIR_READ, f->fd, 0, 1, f->data, &io));

	child = fork();
	T_ASSERT(child >= 0);
	if (!child) {
		usleep(20000);
		kill(getppid(), SIGINT);
		_exit(0);
	}

	alarm(5);
	T_ASSERT(!e->wait(e, _complete_io));
	alarm(0);
	T_ASSERT(sigint_caught());

	alarm(5);
	e->destroy(e);
	alarm(0);

	T_ASSERT(waitpid(child, &status, 0) == child);
	T_ASSERT(WIFEXITED(status) && !WEXITSTATUS(status));

	bcache_threads_test_hold(0);
	sigint_restore();
	sigint_clear();
}

//----------------------------------------------------------------

#define T(path, desc, fn) register_test(ts, "/base/device/bcache/io-engine/" path, desc, fn)

static struct test_suite *_tests(void)
{
	struct test_suite *ts = test_suite_create(_fix_init, _fix_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("create-destroy", "simple create/destroy", _test_create);
#ifdef HAVE_LIBAIO_H
	T("read", "read sanity check", _test_read);
	T("write", "write sanity check", _test_write);
	T("bcache-write-bytes", "test the utility fns", _test_write_bytes);
	T("destroy-after-fork", "io_destroy skipped in child after fork", _test_destroy_after_fork);
	T("wait-eintr", "io_getevents interrupted by signal", _test_wait_eintr);
	T("short-read-async", "short read handled by async engine", _test_short_read_async);
	T("destroy-inflight-async", "async destroy reaps issued io", _test_destroy_inflight_async);
#endif
	T("read-threads", "threads read sanity check", _test_read_threads);
	T("write-threads", "threads write sanity check", _test_write_threads);
	T("short-read-sync", "short read handled by sync engine", _test_short_read_sync);
	T("short-read-threads", "short read handled by threads engine", _test_short_read_threads);
	T("destroy-inflight-sync", "sync destroy frees issued io", _test_destroy_inflight_sync);
	T("destroy-inflight-threads", "threads destroy joins issued io", _test_destroy_inflight_threads);
	T("threads-partial-create-failure", "threads clean up after partial worker creation", _test_threads_partial_create_failure);
	T("threads-inflight-int", "threads wait and destroy while pread blocked", _test_threads_inflight_int);
#ifdef HAVE_LIBURING_H
	T("read-uring", "io_uring read sanity check", _test_read_uring);
	T("short-read-uring", "short read handled by io_uring engine", _test_short_read_uring);
	T("destroy-inflight-uring", "uring destroy drains issued io", _test_destroy_inflight_uring);
	T("uring-wait-eintr", "io_uring wait interrupted by signal", _test_uring_wait_eintr);
#endif

	return ts;
}

void io_engine_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_tests()->list);
}
