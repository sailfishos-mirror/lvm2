/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
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

#include "lib/misc/lib.h"
#include "lib/device/blkio.h"
#include "lib/device/blkio_threads_test.h"
#include "lib/misc/lvm-signal.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

//----------------------------------------------------------------

#define DM_WORKER_STACK_SIZE    (32u * 1024u)

static const char _threads[] = "threads";
#define IO_THREADS_MIN   16
#define IO_THREADS_MAX   128
#define IO_THREADS_PER_CPU  2
/* Wake the main thread often enough to notice SIGINT during a hung read. */
#define IO_THREADS_WAIT_NS  200000000L

struct io_work {
	enum dir direction;
	int fd;
	sector_t sb;
	sector_t se;
	void *data;
	void *context;
	int errnum;			/* errno on failure, 0 on success */
	size_t done;			/* bytes actually transferred */
};

struct thread_slot {
	struct threads_engine *engine;
	pthread_t thread;
	pthread_mutex_t mutex;		/* protects has_work */
	pthread_cond_t cond;		/* per-worker wakeup */
	struct dm_list list;
	int has_work;
	int shutdown;			/* protected by mutex */
	struct io_work work;
};

struct threads_engine {
	struct io_engine base;
	pthread_mutex_t completed_lock;	/* protects completed list */
	pthread_cond_t completed_cond;	/* signals main: work completed */
	unsigned n_threads;
	unsigned n_busy;		/* in-flight count; main thread only */
	struct threads_test_hooks test;
	pid_t creator_pid;
	struct dm_list free;		/* main thread only, no lock needed */
	struct dm_list completed;	/* under completed_lock */
	struct thread_slot slots[];
};

static struct threads_engine *_to_threads(struct io_engine *e)
{
	return container_of(e, struct threads_engine, base);
}

//----------------------------------------------------------------

static int _shutdown_requested(struct thread_slot *slot)
{
	int shutdown;

	pthread_mutex_lock(&slot->mutex);
	shutdown = slot->shutdown;
	pthread_mutex_unlock(&slot->mutex);

	return shutdown;
}

static bool _threads_stop(void *arg)
{
	return _shutdown_requested(arg);
}

/*
 * Pure I/O - no logging, no signal checks.
 * All signals are blocked in workers, so none can arrive here.
 * EINTR is retried unless destroy has set shutdown, which is read
 * under slot->mutex; no mutex is held while blocked in I/O.
 * pthread_cancel is enabled by the caller around this function so a
 * blocked pread/pwrite can be interrupted without holding a mutex.
 * Diagnostics are deferred to the main thread via work->errnum and work->done.
 */
static void _do_io(struct thread_slot *slot)
{
	struct io_work *work = &slot->work;
	struct blkio_byte_range range =
		blkio_sector_range_bytes(work->sb, work->se);
	off_t offset = range.offset;
	size_t len = range.len;
	struct blkio_rw_result rw;

	/* Test hook: cancellation point, so destroy can interrupt a stuck read. */
	if (slot->engine->test.before_io)
		slot->engine->test.before_io();

	rw = blkio_rw_loop(work->fd, work->direction, work->data, len, offset,
			   _threads_stop, slot);
	work->errnum = rw.errnum;
	work->done = rw.done;
}

//----------------------------------------------------------------

static void *_worker_fn(void *arg)
{
	struct thread_slot *slot = arg;
	struct threads_engine *e = slot->engine;

	/*
	 * All signals are blocked by the creator before pthread_create(),
	 * so every worker inherits the full mask.  Workers only perform I/O
	 * and must never call log_*() or sigint_caught() (not thread-safe).
	 * EINTR is retried in _do_io() unless shutdown was requested, so
	 * signals are not needed here.  The main thread checks
	 * sigint_caught() in wait().
	 * During metadata writes the main thread blocks signals itself,
	 * so workers are consistent with that.
	 *
	 * Cancellation is disabled here so it cannot run while a mutex is
	 * held, and enabled by the caller only around _do_io().  Blocking
	 * every signal does not defeat that: glibc's read() wrapper
	 * unblocks the cancellation signal around the syscall itself, so
	 * pthread_cancel() still interrupts a blocked pread().
	 */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	pthread_mutex_lock(&slot->mutex);
	/*
	 * Other threads update both flags under slot->mutex.  Coverity
	 * cannot see those writes, so suppress the false finding that
	 * the loop cannot exit.
	 */
	/* coverity[infeasible_escape] */
	/* coverity[constant_loop_condition] */
	while (!slot->shutdown) {
		/*
		 * has_work is set by the submitting thread under slot->mutex.
		 */
		/* coverity[constant_loop_condition] */
		if (!slot->has_work) {
			pthread_cond_wait(&slot->cond, &slot->mutex);
			continue;
		}

		slot->has_work = 0;
		pthread_mutex_unlock(&slot->mutex);

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		_do_io(slot);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		pthread_mutex_lock(&e->completed_lock);
		dm_list_add(&e->completed, &slot->list);
		pthread_cond_signal(&e->completed_cond);
		pthread_mutex_unlock(&e->completed_lock);

		pthread_mutex_lock(&slot->mutex);
	}

	pthread_mutex_unlock(&slot->mutex);
	return NULL;
}

/*
 * Initialize the slot and start its worker, undoing the partial setup
 * on failure.  The caller only has to tear down the completed slots.
 */
static bool _create_worker(struct thread_slot *slot, pthread_attr_t *attr,
			   unsigned idx)
{
	char name[16];

	if (pthread_mutex_init(&slot->mutex, NULL))
		return false;

	if (pthread_cond_init(&slot->cond, NULL)) {
		pthread_mutex_destroy(&slot->mutex);
		return false;
	}

	if ((slot->engine->test.fail_create &&
	     idx >= slot->engine->test.fail_after) ||
	    pthread_create(&slot->thread, attr, _worker_fn, slot)) {
		pthread_cond_destroy(&slot->cond);
		pthread_mutex_destroy(&slot->mutex);
		return false;
	}

	/* pthread name limit: 16 bytes including NUL */
	(void) dm_snprintf(name, sizeof(name), "io_engine_%u", idx);
	pthread_setname_np(slot->thread, name);

	return true;
}

//----------------------------------------------------------------

static bool _threads_destroy(struct io_engine *ioe)
{
	unsigned i;
	int r;
	bool drained = true;
	struct threads_engine *e = _to_threads(ioe);

	/*
	 * After fork() only the calling thread survives in the child.
	 * Worker threads are gone, and the mutex/condvar state is
	 * indeterminate per POSIX.  Detect this and just free memory.
	 */
	if (!blkio_engine_is_forked(e->creator_pid)) {
		for (i = 0; i < e->n_threads; i++) {
			pthread_mutex_lock(&e->slots[i].mutex);
			e->slots[i].shutdown = 1;
			pthread_cond_signal(&e->slots[i].cond);
			pthread_mutex_unlock(&e->slots[i].mutex);
		}

		/*
		 * Interrupt a worker blocked in pread/pwrite. Cancel is
		 * disabled except around _do_io(), so this does not run
		 * while a worker holds a mutex.
		 */
		for (i = 0; i < e->n_threads; i++)
			pthread_cancel(e->slots[i].thread);

		for (i = 0; i < e->n_threads; i++) {
			if ((r = pthread_join(e->slots[i].thread, NULL))) {
				log_debug("pthread_join failed: %s.", strerror(r));
				drained = false;
			}
		}

		if (!drained) {
			/*
			 * A worker is still alive and may use the engine's
			 * locks or the caller's buffers.  Leak the engine
			 * rather than freeing state the worker can reach.
			 */
			return false;
		}

		for (i = 0; i < e->n_threads; i++) {
			pthread_mutex_destroy(&e->slots[i].mutex);
			pthread_cond_destroy(&e->slots[i].cond);
		}

		pthread_mutex_lock(&e->completed_lock);
		dm_list_init(&e->completed);
		pthread_mutex_unlock(&e->completed_lock);
		e->n_busy = 0;

		pthread_mutex_destroy(&e->completed_lock);
		pthread_cond_destroy(&e->completed_cond);
	}

	free(e);

	/*
	 * All workers have been joined, so no buffer is still in use.
	 */
	return true;
}

static int _threads_issue(struct io_engine *ioe, enum dir d, int fd,
			  sector_t sb, sector_t se, void *data, void *context)
{
	struct threads_engine *e = _to_threads(ioe);
	struct thread_slot *slot;
	struct dm_list *first;

	if (!(first = dm_list_first(&e->free))) {
		log_warn("WARNING: threads io engine no free work slots.");
		return -EAGAIN;
	}

	/* Pop from free list (main thread only, no lock needed) */
	slot = dm_list_item(first, struct thread_slot);
	dm_list_del(&slot->list);

	slot->work.direction = d;
	slot->work.fd = fd;
	slot->work.sb = sb;
	slot->work.se = se;
	slot->work.data = data;
	slot->work.context = context;
	slot->work.errnum = 0;

	/* slot->mutex and completed_lock are never held together. */
	pthread_mutex_lock(&slot->mutex);
	slot->has_work = 1;
	pthread_cond_signal(&slot->cond);
	pthread_mutex_unlock(&slot->mutex);

	/* n_busy is main-thread only. */
	e->n_busy++;

	return 0;
}

static bool _threads_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct threads_engine *e = _to_threads(ioe);
	struct thread_slot *slot;
	struct dm_list *first;
	struct io_work *work;
	struct blkio_byte_range range;
	bool busy = (e->n_busy != 0);
	int io_error;

	/*
	 * n_busy is main-thread only and cannot change before the
	 * processing loop below, so it needs no lock and stays valid
	 * for the whole wait loop.
	 */
	pthread_mutex_lock(&e->completed_lock);

	/*
	 * Wait until at least one slot completes, nothing is in
	 * flight, or an LVM interrupt is pending. pthread_cond_wait
	 * does not return EINTR, so poll with a timeout.
	 */
	while (!(first = dm_list_first(&e->completed))) {
		struct timespec ts;

		if (!busy)
			break;

		if (sigint_caught()) {
			pthread_mutex_unlock(&e->completed_lock);
			return false;
		}

		if (clock_gettime(CLOCK_REALTIME, &ts)) {
			pthread_cond_wait(&e->completed_cond, &e->completed_lock);
			continue;
		}

		ts.tv_nsec += IO_THREADS_WAIT_NS;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}

		pthread_cond_timedwait(&e->completed_cond, &e->completed_lock, &ts);
	}

	/* Process all completed slots */
	while (first) {
		slot = dm_list_item(first, struct thread_slot);
		dm_list_del(&slot->list);
		pthread_mutex_unlock(&e->completed_lock);
		/* n_busy is main-thread only. */
		e->n_busy--;

		work = &slot->work;
		range = blkio_sector_range_bytes(work->sb, work->se);

		if (work->errnum) {
			io_error = blkio_completion_errno(_threads, work->direction,
							  range.offset + work->done,
							  range.len - work->done,
							  work->errnum);
		} else {
			io_error = blkio_completion_status(_threads, work->direction,
							   range.offset, range.len,
							   (int64_t) work->done);
		}

		fn(work->context, io_error);

		/* Return to free list (main thread only, no lock needed) */
		dm_list_add(&e->free, &slot->list);

		pthread_mutex_lock(&e->completed_lock);

		first = dm_list_first(&e->completed);
	}

	pthread_mutex_unlock(&e->completed_lock);

	return true;
}

static unsigned _threads_max_io(struct io_engine *ioe)
{
	return _to_threads(ioe)->n_threads;
}

struct io_engine *create_threads_io_engine_with_hooks(unsigned n_threads,
						      const struct threads_test_hooks *test)
{
	unsigned i;
	long cpu_count;
	struct threads_engine *e;
	pthread_attr_t attr;
	sigset_t set, oldset;

	if (n_threads == 0) {
		cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
		if (cpu_count < IO_THREADS_MIN / IO_THREADS_PER_CPU) {
			if (cpu_count < 0)
				log_debug("threads io engine failed to get CPU count.");
			n_threads = IO_THREADS_MIN;
		} else if (cpu_count > IO_THREADS_MAX / IO_THREADS_PER_CPU)
			n_threads = IO_THREADS_MAX;
		else
			n_threads = (unsigned)cpu_count * IO_THREADS_PER_CPU;
	}

	e = zalloc(sizeof(*e) + n_threads * sizeof(struct thread_slot));
	if (!e) {
		log_warn("WARNING: threads io engine failed to allocate engine.");
		return NULL;
	}

	if (test)
		e->test = *test;

	e->base.destroy = _threads_destroy;
	e->base.issue = _threads_issue;
	e->base.wait = _threads_wait;
	e->base.max_io = _threads_max_io;

	e->creator_pid = getpid();

	dm_list_init(&e->free);
	dm_list_init(&e->completed);

	for (i = 0; i < n_threads; i++) {
		e->slots[i].engine = e;
		dm_list_add(&e->free, &e->slots[i].list);
	}

	if (pthread_mutex_init(&e->completed_lock, NULL))
		goto bad_free;

	if (pthread_cond_init(&e->completed_cond, NULL))
		goto bad_lock;

	/* From here on, _threads_destroy() handles cleanup */

	if (pthread_attr_init(&attr))
		goto bad_engine;

	{
		size_t stack_size = DM_WORKER_STACK_SIZE;

#ifdef PTHREAD_STACK_MIN
		if (PTHREAD_STACK_MIN > 0 &&
		    stack_size < (size_t) PTHREAD_STACK_MIN)
			stack_size = (size_t) PTHREAD_STACK_MIN;
#endif
		if (pthread_attr_setstacksize(&attr, stack_size))
			log_warn("WARNING: threads io engine failed to set thread stack size.");
	}

	/*
	 * Block all signals before creating the workers so they inherit the
	 * mask; a worker cannot log_*() if blocking the signals fails.
	 */
	if (sigfillset(&set) ||
	    pthread_sigmask(SIG_SETMASK, &set, &oldset)) {
		pthread_attr_destroy(&attr);
		goto bad_engine;
	}

	for (i = 0; i < n_threads; i++) {
		if (!_create_worker(&e->slots[i], &attr, i))
			goto bad;
		e->n_threads++;
	}

	if (pthread_sigmask(SIG_SETMASK, &oldset, NULL))
		goto bad;

	pthread_attr_destroy(&attr);

	log_debug("Created threads io engine with %u threads.", n_threads);

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->base;

bad:
	if (pthread_sigmask(SIG_SETMASK, &oldset, NULL))
		log_warn("WARNING: threads io engine failed to restore signal mask.");
	pthread_attr_destroy(&attr);
bad_engine:
	log_warn("WARNING: threads io engine failed to initialize.");
	_threads_destroy(&e->base);
	return NULL;

bad_lock:
	pthread_mutex_destroy(&e->completed_lock);
bad_free:
	log_warn("WARNING: threads io engine failed to initialize.");
	free(e);
	return NULL;
}

struct io_engine *create_threads_io_engine(unsigned n_threads)
{
	return create_threads_io_engine_with_hooks(n_threads, NULL);
}
