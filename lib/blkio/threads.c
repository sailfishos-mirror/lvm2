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

#include "lib/blkio/internal.h"
#include "lib/blkio/threads_test.h"
#include "lib/misc/lvm-signal.h"
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*----------------------------------------------------------------*/

#define IO_THREADS_STACK_SIZE    (32u * 1024u)

#define IO_THREADS_MIN   16
#define IO_THREADS_MAX   128
#define IO_THREADS_PER_CPU  2
#define NSEC_PER_SEC        1000000000L
/* Wake the main thread often enough to notice SIGINT during a hung read. */
#define IO_THREADS_WAIT_NS  200000000L

struct io_work {
	struct blkio_io io;
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
	unsigned in_flight;		/* in-flight count; main thread only */
	unsigned n_threads;
	/*
	 * Slots are handed out from a dm_list, so only creator_pid is used
	 * here; the pool is embedded so every engine checks the fork the
	 * same way.
	 */
	struct io_engine_pool pool;
	struct threads_test_hooks test;	/* zeroed unless a test asked for it */
	struct dm_list free_list;	/* main thread only, no lock needed */
	struct dm_list completed;	/* under completed_lock */
	struct thread_slot slots[];
};

static struct threads_engine *_to_threads(struct io_engine *ioe)
{
	return container_of(ioe, struct threads_engine, base);
}

/*----------------------------------------------------------------*/

/*
 * The wait loop polls for an interrupt with a timeout, so the condvar must
 * use the same clock as the deadline.  With the default CLOCK_REALTIME a
 * backward wall-clock step would move the deadline into the future and
 * delay both the poll and the SIGINT check with it.
 */
static bool _init_completed_cond(pthread_cond_t *cond)
{
	pthread_condattr_t attr;
	int r;

	if (pthread_condattr_init(&attr))
		return false;

	r = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	if (!r)
		r = pthread_cond_init(cond, &attr);

	pthread_condattr_destroy(&attr);

	return !r;
}

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
	struct blkio_rw_result rw;

	/*
	 * Test hook: a cancellation point, so a test can park a worker here
	 * and then check that destroy interrupts a stuck read.
	 */
	if (slot->engine->test.before_io)
		slot->engine->test.before_io();

	rw = blkio_rw_loop(&work->io, _threads_stop, slot);
	work->errnum = rw.errnum;
	work->done = rw.done;
}

/*----------------------------------------------------------------*/

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
	if (pthread_mutex_init(&slot->mutex, NULL))
		return false;

	if (pthread_cond_init(&slot->cond, NULL)) {
		pthread_mutex_destroy(&slot->mutex);
		return false;
	}

	/* Test hook: fail worker creation part way through, as an OOM would. */
	if ((slot->engine->test.fail_create &&
	     idx >= slot->engine->test.fail_after) ||
	    pthread_create(&slot->thread, attr, _worker_fn, slot)) {
		pthread_cond_destroy(&slot->cond);
		pthread_mutex_destroy(&slot->mutex);
		return false;
	}

#ifdef HAVE_PTHREAD_SETNAME_NP
	{
		char name[16];

		/* pthread name limit: 16 bytes including NUL */
		(void) dm_snprintf(name, sizeof(name), "io_engine_%u", idx);
		pthread_setname_np(slot->thread, name);
	}
#endif

	return true;
}

/*----------------------------------------------------------------*/

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
	if (!blkio_engine_is_forked(e->pool.creator_pid)) {
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

		pthread_mutex_destroy(&e->completed_lock);
		pthread_cond_destroy(&e->completed_cond);
	}

	/* struct io_engine is the first member; free the caller's pointer. */
	free(ioe);

	/*
	 * Reaching here means no worker can still touch a caller's buffer:
	 * every thread was joined, or the process has forked and only this
	 * thread survived.  So tell blkio its buffers may be freed.
	 */
	return true;
}

static int _threads_issue(struct io_engine *ioe, const struct blkio_io *io,
			  void *context)
{
	struct threads_engine *e = _to_threads(ioe);
	struct thread_slot *slot;
	struct dm_list *first;

	if (!(first = dm_list_first(&e->free_list))) {
		log_warn("WARNING: threads io engine no free requests (queue full).");
		return -EAGAIN;
	}

	/* Pop from free list (main thread only, no lock needed) */
	slot = dm_list_item(first, struct thread_slot);
	dm_list_del(&slot->list);

	slot->work.io = *io;
	slot->work.context = context;
	slot->work.errnum = 0;

	/* slot->mutex and completed_lock are never held together. */
	pthread_mutex_lock(&slot->mutex);
	slot->has_work = 1;
	pthread_cond_signal(&slot->cond);
	pthread_mutex_unlock(&slot->mutex);

	/* in_flight is main-thread only. */
	e->in_flight++;

	return 0;
}

/*
 * Report every completed slot and return it to the free list.  The caller
 * holds completed_lock on entry and on return; the lock is dropped around
 * the completion callback so the engine is not held while the caller runs.
 */
static void _threads_reap_completed(struct threads_engine *e,
				    struct io_engine *ioe, io_complete_fn fn)
{
	struct thread_slot *slot;
	struct io_work *work;
	struct blkio_rw_result rw;
	struct dm_list *first;
	int io_error;

	while ((first = dm_list_first(&e->completed))) {
		slot = dm_list_item(first, struct thread_slot);
		dm_list_del(&slot->list);
		pthread_mutex_unlock(&e->completed_lock);
		/* in_flight is main-thread only. */
		e->in_flight--;

		work = &slot->work;
		rw.done = work->done;
		rw.errnum = work->errnum;
		io_error = blkio_completion_result(ioe, &work->io, &rw);

		fn(work->context, io_error);

		/* Return to free list (main thread only, no lock needed) */
		dm_list_add(&e->free_list, &slot->list);

		pthread_mutex_lock(&e->completed_lock);
	}
}

static bool _threads_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct threads_engine *e = _to_threads(ioe);
	bool busy = (e->in_flight != 0);

	/*
	 * in_flight is main-thread only and cannot change before
	 * _threads_reap_completed() runs, so it needs no lock and stays
	 * valid for the whole wait.
	 */
	pthread_mutex_lock(&e->completed_lock);

	/*
	 * Wait until at least one slot completes, nothing is in
	 * flight, or an LVM interrupt is pending. pthread_cond_wait
	 * does not return EINTR, so poll with a timeout.
	 */
	while (!dm_list_first(&e->completed)) {
		struct timespec ts;

		if (!busy)
			break;

		if (sigint_caught()) {
			pthread_mutex_unlock(&e->completed_lock);
			return false;
		}

		if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
			pthread_cond_wait(&e->completed_cond, &e->completed_lock);
			continue;
		}

		ts.tv_nsec += IO_THREADS_WAIT_NS;
		if (ts.tv_nsec >= NSEC_PER_SEC) {
			ts.tv_sec++;
			ts.tv_nsec -= NSEC_PER_SEC;
		}

		pthread_cond_timedwait(&e->completed_cond, &e->completed_lock, &ts);
	}

	_threads_reap_completed(e, ioe, fn);

	pthread_mutex_unlock(&e->completed_lock);

	return true;
}

static unsigned _threads_max_io(struct io_engine *ioe)
{
	return _to_threads(ioe)->n_threads;
}

/*
 * The thread count to use when the caller did not ask for one: a couple of
 * workers per online CPU, clamped to the range the engine supports.
 */
static unsigned _threads_default_count(void)
{
	long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);

	if (cpu_count < IO_THREADS_MIN / IO_THREADS_PER_CPU) {
		if (cpu_count < 0)
			log_debug("threads io engine failed to get CPU count.");
		return IO_THREADS_MIN;
	}

	if (cpu_count > IO_THREADS_MAX / IO_THREADS_PER_CPU)
		return IO_THREADS_MAX;

	return (unsigned) cpu_count * IO_THREADS_PER_CPU;
}

struct io_engine *create_threads_io_engine_with_hooks(unsigned n_threads,
						      const struct threads_test_hooks *test)
{
	unsigned i;
	struct threads_engine *e;
	pthread_attr_t attr;
	sigset_t set, oldset;

	/* The default thread count is an engine property, not a client choice. */
	if (n_threads == BLKIO_ENGINE_DEFAULT)
		n_threads = _threads_default_count();

	if (!(e = zalloc(sizeof(*e) + n_threads * sizeof(struct thread_slot)))) {
		log_error("Failed to allocate threads io engine.");
		return NULL;
	}

	e->base.name = "threads";

	if (test)
		e->test = *test;

	e->base.destroy = _threads_destroy;
	e->base.issue = _threads_issue;
	e->base.wait = _threads_wait;
	e->base.max_io = _threads_max_io;

	e->pool.creator_pid = getpid();

	dm_list_init(&e->free_list);
	dm_list_init(&e->completed);

	for (i = 0; i < n_threads; i++) {
		e->slots[i].engine = e;
		dm_list_add(&e->free_list, &e->slots[i].list);
	}

	if (pthread_mutex_init(&e->completed_lock, NULL)) {
		log_error("Failed to initialize the threads io engine lock.");
		goto bad_free;
	}

	if (!_init_completed_cond(&e->completed_cond)) {
		log_error("Failed to initialize the threads io engine condition.");
		goto bad_lock;
	}

	/* From here on, _threads_destroy() handles cleanup */

	if (pthread_attr_init(&attr)) {
		log_error("Failed to initialize the threads io engine attributes.");
		goto bad_engine;
	}

	{
		size_t stack_size = IO_THREADS_STACK_SIZE;

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
		log_error("Failed to block signals for the threads io engine.");
		pthread_attr_destroy(&attr);
		goto bad_engine;
	}

	for (i = 0; i < n_threads; i++) {
		if (!_create_worker(&e->slots[i], &attr, i)) {
			log_error("Failed to create a threads io engine worker.");
			goto bad_signals;
		}
		e->n_threads++;
	}

	if (pthread_sigmask(SIG_SETMASK, &oldset, NULL)) {
		log_error("Failed to restore the threads io engine signal mask.");
		goto bad;
	}

	pthread_attr_destroy(&attr);

	log_debug("Created threads io engine with %u threads.", n_threads);

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->base;

bad_signals:
	if (pthread_sigmask(SIG_SETMASK, &oldset, NULL))
		log_warn("WARNING: threads io engine failed to restore signal mask.");
bad:
	pthread_attr_destroy(&attr);
bad_engine:
	_threads_destroy(&e->base);
	return NULL;

bad_lock:
	pthread_mutex_destroy(&e->completed_lock);
bad_free:
	free(e);
	return NULL;
}

struct io_engine *create_threads_io_engine(unsigned n_threads)
{
	return create_threads_io_engine_with_hooks(n_threads, NULL);
}
