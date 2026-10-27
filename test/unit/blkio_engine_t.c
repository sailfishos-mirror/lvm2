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
#include "lib/blkio/threads_test.h"
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__SANITIZE_THREAD__)
#define TEST_UNDER_TSAN
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TEST_UNDER_TSAN
#endif
#endif

//----------------------------------------------------------------

#define BLOCK_SIZE_SECTORS 8
#define PAGE_SIZE_SECTORS ((TEST_PAGE_SIZE) >> SECTOR_SHIFT)
#define NR_BLOCKS 64
#define TEST_ALARM_SECS 5

struct fixture {
	struct io_engine *e;
	uint8_t *data;

	char fname[64];
	int fd;
	int di;

	pid_t interrupt_child;
	int interrupt_stop_fd;
};

static bool _stop_interrupt_child(pid_t child, int stop_fd);

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
	f->interrupt_child = 0;
	f->interrupt_stop_fd = -1;
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
	bool drained = true;

	if (f) {
		alarm(0);

		if (f->interrupt_child) {
			(void) _stop_interrupt_child(f->interrupt_child,
						     f->interrupt_stop_fd);
			f->interrupt_child = 0;
			f->interrupt_stop_fd = -1;
		}

		sigint_restore();
		sigint_clear();

		if (f->e) {
			drained = f->e->destroy(f->e);
			f->e = NULL;
		}

		(void) close(f->fd);
		(void) unlink(f->fname);
		if (drained)
			free(f->data);
		free(f);
	}
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

static struct blkio_io _sector_io(enum blkio_dir d, int fd, void *data,
				 sector_t sb, sector_t se)
{
	struct blkio_io io = {
		.d = d,
		.fd = fd,
		.data = data,
		.range = blkio_sector_range_bytes(sb, se),
	};

	return io;
}

static void _run_engine_rw(struct fixture *f, struct io_engine *e, enum blkio_dir d);

#ifdef HAVE_LIBAIO_H
static void _test_async_rw(void *fixture, enum blkio_dir d)
{
	struct fixture *f = fixture;

	f->e = create_async_io_engine(BLKIO_ENGINE_DEFAULT);
	T_ASSERT(f->e);
	_run_engine_rw(f, f->e, d);
	f->e = NULL;   // already destroyed
}

static void _test_read(void *fixture)
{
	_test_async_rw(fixture, BLKIO_DIR_READ);
}

static void _test_write(void *fixture)
{
	_test_async_rw(fixture, BLKIO_DIR_WRITE);
}

static void _test_write_bytes(void *fixture)
{
	struct fixture *f = fixture;

	unsigned offset = 345;
	char buf_out[32];
	char buf_in[32];
	struct blkio *bio;

	f->e = create_async_io_engine(BLKIO_ENGINE_DEFAULT);
	T_ASSERT(f->e);

	bio = blkio_create_engine(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, f->e);
	T_ASSERT(bio);

	f->di = blkio_set_fd(bio, f->fd);

	_fill_buffer((uint8_t *) buf_out, 234, sizeof(buf_out));
	T_ASSERT(blkio_write_bytes(bio, f->di, offset, sizeof(buf_out), buf_out));
	T_ASSERT(blkio_read_bytes(bio, f->di, offset, sizeof(buf_in), buf_in));

	if (memcmp(buf_out, buf_in, sizeof(buf_out))) {
		_print_buffer("buf_out", (uint8_t *) buf_out, sizeof(buf_out));
		_print_buffer("buf_in", (uint8_t *) buf_in, sizeof(buf_in));
	}
	T_ASSERT(!memcmp(buf_out, buf_in, sizeof(buf_out)));

	blkio_destroy(bio);
	f->e = NULL;   // already destroyed
}

/*
 * Test that _async_destroy() skips io_destroy() after fork().
 *
 * The aio_context is created in the parent process. After fork() the
 * child inherits the context value but must not call io_destroy() on
 * it - only the original process should do that.  _async_destroy()
 * uses blkio_engine_is_forked(e->pool.creator_pid) to guard this.
 *
 * Also exercises the normal io_destroy() path in the parent, verifying
 * that the negative-return error reporting works
 * without crashing (io_destroy returns -errno, not -1+errno).
 */
static void _test_destroy_after_fork(void *fixture)
{
	struct io_engine *e;
	pid_t pid;
	int status;

	e = create_async_io_engine(BLKIO_ENGINE_DEFAULT);
	T_ASSERT(e);

	pid = fork();
	T_ASSERT(pid >= 0);

	if (!pid) {
		/*
		 * Child: destroy must skip io_destroy() because
		 * blkio_engine_is_forked() sees a different pid.  If it
		 * incorrectly calls io_destroy() the parent's context
		 * gets invalidated and the parent's destroy will fail.
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
}

/*
 * Test that _async_wait() is interruptible by SIGINT/SIGTERM (via
 * sigint_allow()), but retries on other signals such as SIGALRM.
 *
 * _async_wait() checks sigint_caught() before each blocking
 * io_getevents(), and also stops when the syscall returns EINTR after
 * an LVM signal.  A child process sends SIGINT to the parent after a
 * short delay, so the parent is blocked in io_getevents() and the EINTR
 * path is exercised as well.
 *
 * Strategy: call wait() with no I/O submitted so io_getevents(min_nr=1)
 * must block.  The signal sets sigint_caught(), so wait() returns false.
 *
 * Why issue no I/O?
 * Linux AIO on regular files (and character devices like /dev/zero,
 * /dev/urandom) completes synchronously inside io_submit() - the
 * kernel posts the completion before io_submit() returns, so
 * io_getevents() never blocks.  Only O_DIRECT on a real block device
 * goes through the true async path.  Calling wait() with no I/O
 * pending guarantees io_getevents() blocks, making the test
 * deterministic without needing a block device.
 */
#endif /* HAVE_LIBAIO_H */

static bool _wait_child(pid_t child)
{
	pid_t r;
	int status;

	do {
		r = waitpid(child, &status, 0);
	} while (r < 0 && errno == EINTR);

	return r == child && WIFEXITED(status) && !WEXITSTATUS(status);
}

/*
 * Fork a child that keeps sending SIGINT until the parent closes its end
 * of the pipe.  A single signal can arrive before wait() blocks under
 * instrumentation; a later one then interrupts the blocked syscall.  The
 * pipe also tells us the child is running, so the test never depends on
 * how long a fork takes.
 */
static pid_t _fork_interrupt_child(int *stop_fd)
{
	int done[2];
	pid_t child;

	T_ASSERT(pipe(done) == 0);

	child = fork();
	T_ASSERT(child >= 0);

	if (!child) {
		struct pollfd pfd = { .fd = done[0], .events = POLLIN };

		(void) close(done[1]);
		for (;;) {
			if (poll(&pfd, 1, 10) > 0)
				break;
			kill(getppid(), SIGINT);
		}
		(void) close(done[0]);
		_exit(0);
	}

	(void) close(done[0]);
	*stop_fd = done[1];
	return child;
}

static bool _stop_interrupt_child(pid_t child, int stop_fd)
{
	bool stopped = write(stop_fd, "x", 1) == 1;

	(void) close(stop_fd);
	return _wait_child(child) && stopped;
}

/*
 * A wait() with nothing to reap blocks.  SIGINT must make it return false
 * instead of retrying forever, and destroy() must then succeed.
 *
 * Always built, but only called by the engine tests that can be compiled
 * in; the attribute keeps the build quiet when neither of them is.
 */
static __attribute__((unused))
void _test_wait_interrupted(struct fixture *f, struct io_engine *e)
{
	bool caught, destroyed, interrupted, stopped;

	f->e = e;
	T_ASSERT(f->e);

	/*
	 * Arm the LVM SIGINT/SIGTERM handler (clears SA_RESTART,
	 * installs _catch_sigint which sets sigint_caught()).
	 */
	sigint_allow();

	f->interrupt_child = _fork_interrupt_child(&f->interrupt_stop_fd);
	alarm(TEST_ALARM_SECS);
	interrupted = !f->e->wait(f->e, _complete_io);
	alarm(0);
	caught = sigint_caught();

	stopped = _stop_interrupt_child(f->interrupt_child,
					f->interrupt_stop_fd);
	f->interrupt_child = 0;
	f->interrupt_stop_fd = -1;

	sigint_restore();
	sigint_clear();

	destroyed = f->e->destroy(f->e);
	f->e = NULL;

	T_ASSERT(interrupted);
	T_ASSERT(caught);
	T_ASSERT(stopped);
	T_ASSERT(destroyed);
}

#ifdef HAVE_LIBAIO_H
/*
 * TSan defers user signal handlers until an instrumented atomic access.
 * sigint_caught() only reads a volatile flag here, so the handler never
 * runs while libaio is blocked in io_getevents(); skip this test.
 */
#ifndef TEST_UNDER_TSAN
/*
 * No I/O submitted: io_getevents(min_nr=1) blocks until SIGINT arrives
 * from the child.  sigint_caught() is then set so the retry loop exits
 * and wait() returns false.
 */
static void _test_wait_eintr(void *fixture)
{
	_test_wait_interrupted(fixture, create_async_io_engine(BLKIO_ENGINE_DEFAULT));
}
#endif /* !TEST_UNDER_TSAN */

#endif /* HAVE_LIBAIO_H */

/*
 * Read/write sanity for an arbitrary engine.  The engine is destroyed on
 * success, so callers must not destroy it again.
 */
static void _run_engine_rw(struct fixture *f, struct io_engine *e, enum blkio_dir d)
{
	struct io io;
	struct blkio_io bio_io;
	struct blkio *bio;
	uint8_t readback[SECTOR_SIZE * BLOCK_SIZE_SECTORS];

	T_ASSERT(e);

	bio = blkio_create_engine(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, e);
	T_ASSERT(bio);

	f->di = blkio_set_fd(bio, f->fd);
	T_ASSERT(f->di >= 0);

	if (d == BLKIO_DIR_WRITE)
		_fill_buffer(f->data, 234, sizeof(readback));

	_io_init(&io);
	bio_io = _sector_io(d, f->fd, f->data, 0, BLOCK_SIZE_SECTORS);
	T_ASSERT(!e->issue(e, &bio_io, &io));
	T_ASSERT(e->wait(e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT(!io.error);

	if (d == BLKIO_DIR_READ)
		_check_buffer(f->data, 123, sizeof(readback));
	else {
		T_ASSERT(pread(f->fd, readback, sizeof(readback), 0) == (ssize_t) sizeof(readback));
		_check_buffer(readback, 234, sizeof(readback));
	}

	blkio_destroy(bio);
}

/*
 * Issue a read that goes past EOF.  The file holds BLOCK_SIZE_SECTORS
 * sectors, so reading 2x that produces a short transfer.  A short read
 * is a normal end of device condition: the read succeeds and the unread
 * tail of the buffer is zero filled.  This deterministically exercises
 * the short-read reporting path of an engine.  The caller owns e and
 * must destroy it.
 */
static void _run_engine_short_read(struct io_engine *e, struct fixture *f)
{
	size_t len = 2 * SECTOR_SIZE * BLOCK_SIZE_SECTORS;
	struct io io;
	struct blkio_io bio_io;
	void *buf;
	size_t i;

	T_ASSERT(e);

	if (posix_memalign(&buf, TEST_PAGE_SIZE, len))
		test_fail("posix_memalign failed");

	memset(buf, 0xAA, len);

	_io_init(&io);
	bio_io = _sector_io(BLKIO_DIR_READ, f->fd, buf, 0,
			    2 * BLOCK_SIZE_SECTORS);
	T_ASSERT(!e->issue(e, &bio_io, &io));
	T_ASSERT(e->wait(e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT_EQUAL(io.error, 0);

	/* The data was read in full and the tail beyond EOF is zero filled. */
	_check_buffer(buf, 123, SECTOR_SIZE * BLOCK_SIZE_SECTORS);
	for (i = SECTOR_SIZE * BLOCK_SIZE_SECTORS; i < len; i++)
		T_ASSERT_EQUAL(((uint8_t *) buf)[i], 0);

	free(buf);
}

static void _run_engine_subsector_read(struct io_engine *e, struct fixture *f)
{
	struct io io;
	struct blkio_io bio_io;

	T_ASSERT(e);
	T_ASSERT(ftruncate(f->fd, SECTOR_SIZE - 1) == 0);

	_io_init(&io);
	bio_io = _sector_io(BLKIO_DIR_READ, f->fd, f->data, 0, 1);
	T_ASSERT(!e->issue(e, &bio_io, &io));
	T_ASSERT(e->wait(e, _complete_io));
	T_ASSERT(io.completed);
	T_ASSERT_EQUAL(io.error, -ENODATA);
}

static void _test_read_threads(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_threads_io_engine(4), BLKIO_DIR_READ);
}

static void _test_write_threads(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_threads_io_engine(4), BLKIO_DIR_WRITE);
}

static void _test_short_read_sync(void *fixture)
{
	struct fixture *f = fixture;
	struct io_engine *e = create_sync_io_engine(BLKIO_ENGINE_DEFAULT);

	_run_engine_short_read(e, f);
	_run_engine_subsector_read(e, f);
	T_ASSERT(e->destroy(e));
}

#ifdef HAVE_LIBAIO_H
static void _test_short_read_async(void *fixture)
{
	struct fixture *f = fixture;
	struct io_engine *e = create_async_io_engine(BLKIO_ENGINE_DEFAULT);

	_run_engine_short_read(e, f);
	_run_engine_subsector_read(e, f);
	T_ASSERT(e->destroy(e));
}
#endif

static void _test_short_read_threads(void *fixture)
{
	struct fixture *f = fixture;
	struct io_engine *e = create_threads_io_engine(4);

	_run_engine_short_read(e, f);
	_run_engine_subsector_read(e, f);
	T_ASSERT(e->destroy(e));
}

#ifdef HAVE_LIBURING_H
static void _test_read_uring(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_uring_io_engine(4), BLKIO_DIR_READ);
}

static void _test_write_uring(void *fixture)
{
	struct fixture *f = fixture;

	_run_engine_rw(f, create_uring_io_engine(4), BLKIO_DIR_WRITE);
}

static void _test_short_read_uring(void *fixture)
{
	struct fixture *f = fixture;
	struct io_engine *e = create_uring_io_engine(4);

	_run_engine_short_read(e, f);
	_run_engine_subsector_read(e, f);
	T_ASSERT(e->destroy(e));
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
	struct blkio_io bio_io;

	T_ASSERT(e);
	_io_init(&io);
	bio_io = _sector_io(BLKIO_DIR_READ, fd, buf, 0, BLOCK_SIZE_SECTORS);
	T_ASSERT(!e->issue(e, &bio_io, &io));
	T_ASSERT(e->destroy(e));
}

static void _test_destroy_inflight_sync(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_sync_io_engine(BLKIO_ENGINE_DEFAULT), f->fd, f->data);
}

static void _test_destroy_inflight_threads(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_threads_io_engine(2), f->fd, f->data);
}

static void _test_threads_partial_create_failure(void *fixture)
{
	const struct threads_test_hooks hooks = {
		.fail_create = true,
		.fail_after = 1,
	};
	struct io_engine *e;

	(void) fixture;

	alarm(TEST_ALARM_SECS);
	e = create_threads_io_engine_with_hooks(2, &hooks);
	alarm(0);
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

	_destroy_after_issue(create_async_io_engine(BLKIO_ENGINE_DEFAULT), f->fd, f->data);
}
#endif

#ifdef HAVE_LIBURING_H
static void _test_destroy_inflight_uring(void *fixture)
{
	struct fixture *f = fixture;

	_destroy_after_issue(create_uring_io_engine(4), f->fd, f->data);
}

/*
 * io_uring_wait_cqe() with nothing submitted blocks.
 */
static void _test_uring_wait_eintr(void *fixture)
{
	_test_wait_interrupted(fixture, create_uring_io_engine(4));
}
#endif

static void _hold_io(void)
{
	/* coverity[no_escape] worker is cancelled at the pause() cancellation point */
	for (;;)
		pause();
}

/*
 * The before_io test hook makes the worker block in pause(), which is
 * a cancellation point, before pread. wait() must return on SIGINT, and
 * destroy() must cancel that wait instead of joining forever.
 */
static void _test_threads_inflight_int(void *fixture)
{
	const struct threads_test_hooks hooks = {
		.before_io = _hold_io,
	};
	struct fixture *f = fixture;
	struct io io;
	struct blkio_io bio_io;
	bool caught, child_ok, destroyed, interrupted;

	f->e = create_threads_io_engine_with_hooks(1, &hooks);
	T_ASSERT(f->e);

	sigint_allow();
	_io_init(&io);
	bio_io = _sector_io(BLKIO_DIR_READ, f->fd, f->data, 0, 1);
	T_ASSERT(!f->e->issue(f->e, &bio_io, &io));

	f->interrupt_child = _fork_interrupt_child(&f->interrupt_stop_fd);

	alarm(TEST_ALARM_SECS);
	interrupted = !f->e->wait(f->e, _complete_io);
	alarm(0);
	caught = sigint_caught();

	alarm(TEST_ALARM_SECS);
	destroyed = f->e->destroy(f->e);
	alarm(0);
	f->e = NULL;

	child_ok = _stop_interrupt_child(f->interrupt_child,
					 f->interrupt_stop_fd);
	f->interrupt_child = 0;
	f->interrupt_stop_fd = -1;

	sigint_restore();
	sigint_clear();

	T_ASSERT(interrupted);
	T_ASSERT(caught);
	T_ASSERT(destroyed);
	T_ASSERT(child_ok);
}

//----------------------------------------------------------------

static void _check_engine_name(struct io_engine *e, const char *name)
{
	T_ASSERT(e);
	T_ASSERT(e->name);
	T_ASSERT(!strcmp(e->name, name));
}

#if defined(HAVE_LIBAIO_H) || defined(HAVE_LIBURING_H)
/*
 * A built-in engine either creates or reports that this system cannot set
 * it up, and nothing else: any other failure has been logged already.
 */
static void _check_optional_engine(struct io_engine *(*create)(unsigned),
				   const char *name)
{
	struct io_engine *e;

	errno = 0;
	if ((e = create(BLKIO_ENGINE_DEFAULT))) {
		_check_engine_name(e, name);
		T_ASSERT(blkio_engine_usable(e));
		T_ASSERT(e->destroy(e));
		return;
	}

	T_ASSERT(errno == ENODEV);
}
#endif /* HAVE_LIBAIO_H || HAVE_LIBURING_H */

#if !defined(HAVE_LIBAIO_H) || !defined(HAVE_LIBURING_H)
/*
 * An engine that was not built in is absent from the table, so naming it
 * is not an unknown hint and the walk falls back to sync.
 */
static void _check_engine_fallback(const char *hint)
{
	struct io_engine *e;

	if (!(e = blkio_create_io_engine(hint)))
		test_fail("engine selection failed");
	_check_engine_name(e, "sync");
	T_ASSERT(e->destroy(e));
}
#endif /* !HAVE_LIBAIO_H || !HAVE_LIBURING_H */

static void _test_engine_availability(void *fixture)
{
	struct io_engine *e;

	(void) fixture;

	if (!(e = create_sync_io_engine(BLKIO_ENGINE_DEFAULT)))
		test_fail("sync io engine could not be created");
	_check_engine_name(e, "sync");
	T_ASSERT(blkio_engine_usable(e));
	T_ASSERT(e->destroy(e));

	if (!(e = create_threads_io_engine(2)))
		test_fail("threads io engine could not be created");
	_check_engine_name(e, "threads");
	T_ASSERT(blkio_engine_usable(e));
	T_ASSERT(e->destroy(e));

#ifdef HAVE_LIBAIO_H
	_check_optional_engine(create_async_io_engine, "libaio");
#else
	_check_engine_fallback("libaio");
#endif

#ifdef HAVE_LIBURING_H
	_check_optional_engine(create_uring_io_engine, "iouring");
#else
	_check_engine_fallback("iouring");
#endif
}

/*
 * Every hint, including an unknown one, must produce a usable engine:
 * the selector only returns NULL when creation really failed.
 */
static void _test_engine_select(void *fixture)
{
	static const char *hints[] = {
		NULL, "", "auto", "sync", "threads", "libaio", "async",
		"aio", "iouring", "uring", "no-such-engine",
	};
	struct io_engine *e;
	unsigned i;

	(void) fixture;

	for (i = 0; i < DM_ARRAY_SIZE(hints); i++) {
		if (!(e = blkio_create_io_engine(hints[i])))
			test_fail("engine selection failed");

		T_ASSERT(blkio_engine_usable(e));
		T_ASSERT(e->destroy(e));
	}
}

/*
 * An alias must select the same engine as the canonical name.  Comparing
 * the two results keeps the test independent of whether the kernel
 * accepts the engine at run time, and a truncated inline name would make
 * the alias fail to match and fall back to sync.
 */
static void _test_engine_alias(void *fixture)
{
	static const char *const aliases[][2] = {
		{ "libaio", "async" },
		{ "libaio", "aio" },
		{ "iouring", "uring" },
	};
	struct io_engine *canon, *alias;
	unsigned i;

	(void) fixture;

	for (i = 0; i < DM_ARRAY_SIZE(aliases); i++) {
		if (!(canon = blkio_create_io_engine(aliases[i][0])))
			test_fail("engine selection failed");

		if (!(alias = blkio_create_io_engine(aliases[i][1])))
			test_fail("engine selection failed");

		T_ASSERT(!strcmp(canon->name, aliases[i][0]) ||
			 !strcmp(canon->name, "sync"));
		T_ASSERT(!strcmp(canon->name, alias->name));

		T_ASSERT(canon->destroy(canon));
		T_ASSERT(alias->destroy(alias));
	}
}

static unsigned _zero_max_io(struct io_engine *ioe)
{
	return 0;
}

/*
 * blkio_create_engine() must reject an engine before calling any method,
 * so an unavailable engine cannot dereference a NULL vtable entry.  A
 * zero queue depth is rejected as well: it would stall the submission
 * loop.
 */
static void _test_create_rejects_unavailable(void *fixture)
{
	static struct io_engine unavailable = {
		.name = "unavailable",
	};
	struct io_engine *e;
	struct blkio *bio;

	(void) fixture;

	/* Rejected before any method is called, so nothing to destroy. */
	bio = blkio_create_engine(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS,
				  &unavailable);
	T_ASSERT(!bio);

	/* blkio_create_engine() destroys the engine it was given on failure. */
	if (!(e = create_sync_io_engine(BLKIO_ENGINE_DEFAULT)))
		test_fail("sync io engine could not be created");

	e->max_io = _zero_max_io;
	bio = blkio_create_engine(PAGE_SIZE_SECTORS, BLOCK_SIZE_SECTORS, e);
	T_ASSERT(!bio);
}

#define T(path, desc, fn) register_test(ts, "/base/blkio/engine/" path, desc, fn)

static struct test_suite *_tests(void)
{
	struct test_suite *ts = test_suite_create(_fix_init, _fix_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

#ifdef HAVE_LIBAIO_H
	T("read", "read sanity check", _test_read);
	T("write", "write sanity check", _test_write);
	T("blkio-write-bytes", "test the utility fns", _test_write_bytes);
	T("destroy-after-fork", "io_destroy skipped in child after fork", _test_destroy_after_fork);
#ifndef TEST_UNDER_TSAN
	T("wait-eintr", "io_getevents interrupted by signal", _test_wait_eintr);
#endif
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
	T("write-uring", "io_uring write sanity check", _test_write_uring);
	T("short-read-uring", "short read handled by io_uring engine", _test_short_read_uring);
	T("destroy-inflight-uring", "uring destroy drains issued io", _test_destroy_inflight_uring);
	T("uring-wait-eintr", "io_uring wait interrupted by signal", _test_uring_wait_eintr);
#endif
	T("engine-availability", "unavailable engines are reported, not NULL", _test_engine_availability);
	T("engine-select", "engine selection always yields a usable engine", _test_engine_select);
	T("engine-alias", "an alias selects the same engine as its name", _test_engine_alias);
	T("create-unavailable", "blkio_create_engine rejects an unusable engine", _test_create_rejects_unavailable);

	return ts;
}

void blkio_engine_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_tests()->list);
}
