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
#include "lib/device/bcache.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

//----------------------------------------------------------------

#define SECTOR_SHIFT 9L
#define DM_WORKER_STACK_SIZE    (32u * 1024u)
#define BCACHE_MIN_IO_THREADS   16
#define BCACHE_MAX_IO_THREADS   128
#define BCACHE_THREADS_PER_CPU  2

struct io_work {
	enum dir direction;
	int fd;
	sector_t sb;
	sector_t se;
	void *data;
	void *context;
	int errnum;		/* errno on failure, 0 on success */
	size_t done;		/* bytes actually transferred */
};

struct thread_slot {
	struct threads_engine *engine;
	pthread_t thread;
	pthread_mutex_t mutex;		/* protects has_work, shutdown */
	pthread_cond_t cond;		/* per-worker wakeup */
	struct dm_list list;
	int has_work;
	int shutdown;
	struct io_work work;
};

struct threads_engine {
	struct io_engine base;
	pthread_mutex_t completed_lock;	/* protects completed list, n_busy */
	pthread_cond_t completed_cond;	/* signals main: work completed */
	unsigned n_threads;
	unsigned n_busy;		/* in-flight count; under completed_lock */
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

/*
 * Pure I/O - no logging, no signal checks.
 * Worker threads have signals blocked, so EINTR is retried unconditionally.
 * All diagnostics are deferred to the main thread via work->errnum and work->done.
 */
static void _do_io(struct io_work *work)
{
	off_t offset = work->sb << SECTOR_SHIFT;
	size_t len = (work->se - work->sb) << SECTOR_SHIFT;
	size_t pos = 0;
	ssize_t rv;

	while (pos < len) {
		if (work->direction == DIR_READ)
			rv = pread(work->fd, (char *)work->data + pos,
				   len - pos, offset + pos);
		else
			rv = pwrite(work->fd, (char *)work->data + pos,
				    len - pos, offset + pos);

		if (rv < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;

			work->errnum = errno;
			break;
		}

		if (rv == 0)
			break;

		pos += rv;
	}

	work->done = pos;
}

//----------------------------------------------------------------

static void *_worker_fn(void *arg)
{
	struct thread_slot *slot = arg;
	struct threads_engine *e = slot->engine;
	sigset_t set;

	/*
	 * Block all signals in worker threads.  Workers only perform I/O
	 * and must never call log_*() or sigint_caught() (not thread-safe).
	 * EINTR is retried unconditionally in _do_io() so signals are not
	 * needed here.  During label scanning (reads) the main thread
	 * remains interruptible and checks sigint_caught() between
	 * issue()/wait() calls.  During metadata writes the main thread
	 * blocks signals itself, so workers are consistent with that.
	 */
	sigfillset(&set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);

	pthread_mutex_lock(&slot->mutex);
	while (!slot->shutdown) {
		if (!slot->has_work) {
			pthread_cond_wait(&slot->cond, &slot->mutex);
			continue;
		}

		slot->has_work = 0;
		pthread_mutex_unlock(&slot->mutex);

		_do_io(&slot->work);

		pthread_mutex_lock(&e->completed_lock);
		dm_list_add(&e->completed, &slot->list);
		pthread_cond_signal(&e->completed_cond);
		pthread_mutex_unlock(&e->completed_lock);

		pthread_mutex_lock(&slot->mutex);
	}

	return NULL;
}

//----------------------------------------------------------------

static void _threads_destroy(struct io_engine *ioe)
{
	unsigned i;
	struct threads_engine *e = _to_threads(ioe);

	/*
	 * After fork() only the calling thread survives in the child.
	 * Worker threads are gone, and the mutex/condvar state is
	 * indeterminate per POSIX.  Detect this and just free memory.
	 */
	if (getpid() == e->creator_pid) {
		for (i = 0; i < e->n_threads; i++) {
			pthread_mutex_lock(&e->slots[i].mutex);
			e->slots[i].shutdown = 1;
			pthread_cond_signal(&e->slots[i].cond);
			pthread_mutex_unlock(&e->slots[i].mutex);
		}

		for (i = 0; i < e->n_threads; i++) {
			if (pthread_join(e->slots[i].thread, NULL))
				log_sys_debug("pthread_join", "");
			pthread_mutex_destroy(&e->slots[i].mutex);
			pthread_cond_destroy(&e->slots[i].cond);
		}

		pthread_mutex_destroy(&e->completed_lock);
		pthread_cond_destroy(&e->completed_cond);
	}

	free(e);
}

static bool _threads_issue(struct io_engine *ioe, enum dir d, int fd,
			    sector_t sb, sector_t se, void *data, void *context)
{
	struct threads_engine *e = _to_threads(ioe);
	struct thread_slot *slot;
	struct dm_list *first;

	if (!(first = dm_list_first(&e->free))) {
		log_warn("WARNING: bcache threads no free work slots.");
		return false;
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

	/*
	 * Lock ordering: slot->mutex before completed_lock.
	 * Neither is held when taking the other (no nesting),
	 * but consistent ordering avoids confusion.
	 */
	pthread_mutex_lock(&slot->mutex);
	slot->has_work = 1;
	pthread_cond_signal(&slot->cond);
	pthread_mutex_unlock(&slot->mutex);

	/* n_busy is main-thread only but use completed_lock for analyzers */
	pthread_mutex_lock(&e->completed_lock);
	e->n_busy++;
	pthread_mutex_unlock(&e->completed_lock);

	return true;
}

static bool _threads_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct threads_engine *e = _to_threads(ioe);
	struct thread_slot *slot;
	struct dm_list *first;
	struct io_work *work;

	pthread_mutex_lock(&e->completed_lock);

	/* Wait until at least one slot completes or nothing in flight */
	while (!(first = dm_list_first(&e->completed))) {
		if (!e->n_busy)
			break;
		pthread_cond_wait(&e->completed_cond, &e->completed_lock);
	}

	/* Process all completed slots */
	while (first) {
		slot = dm_list_item(first, struct thread_slot);
		dm_list_del(&slot->list);
		e->n_busy--;
		pthread_mutex_unlock(&e->completed_lock);

		work = &slot->work;

		/* Log I/O diagnostics and call completion */
		if (work->errnum) {
			size_t len = (work->se - work->sb) << SECTOR_SHIFT;
			off_t offset = work->sb << SECTOR_SHIFT;

			log_warn("WARNING: bcache threads %s device offset %llu len %llu: %s.",
				 (work->direction == DIR_READ) ? "read" : "write",
				 (unsigned long long)(offset + work->done),
				 (unsigned long long)(len - work->done),
				 strerror(work->errnum));
			fn(work->context, -work->errnum);
		} else if (work->done >= (size_t)((work->se - work->sb) << SECTOR_SHIFT)) {
			fn(work->context, 0);
		} else if (work->done >= (1 << SECTOR_SHIFT)) {
			log_debug_devs("bcache threads %s offset %llu requested %llu got %llu.",
				       (work->direction == DIR_READ) ? "read" : "write",
				       (unsigned long long)(work->sb << SECTOR_SHIFT),
				       (unsigned long long)((work->se - work->sb) << SECTOR_SHIFT),
				       (unsigned long long)work->done);
			fn(work->context, 0);
		} else {
			log_warn("WARNING: bcache threads %s offset %llu requested %llu got %llu.",
				 (work->direction == DIR_READ) ? "read" : "write",
				 (unsigned long long)(work->sb << SECTOR_SHIFT),
				 (unsigned long long)((work->se - work->sb) << SECTOR_SHIFT),
				 (unsigned long long)work->done);
			fn(work->context, -ENODATA);
		}

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

struct io_engine *create_threads_io_engine(unsigned n_threads)
{
	unsigned i;
	long cpu_count;
	struct threads_engine *e;
	pthread_attr_t attr;

	if (n_threads == 0) {
		cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
		if (cpu_count < BCACHE_MIN_IO_THREADS / BCACHE_THREADS_PER_CPU) {
			if (cpu_count < 0)
				log_debug("bcache threads failed to get CPU count.");
			n_threads = BCACHE_MIN_IO_THREADS;
		} else if (cpu_count > BCACHE_MAX_IO_THREADS / BCACHE_THREADS_PER_CPU)
			n_threads = BCACHE_MAX_IO_THREADS;
		else
			n_threads = (unsigned)cpu_count * BCACHE_THREADS_PER_CPU;
	}

	e = malloc(sizeof(*e) + n_threads * sizeof(struct thread_slot));
	if (!e) {
		log_warn("WARNING: bcache threads failed to allocate engine.");
		return NULL;
	}

	e->base.destroy = _threads_destroy;
	e->base.issue = _threads_issue;
	e->base.wait = _threads_wait;
	e->base.max_io = _threads_max_io;

	e->n_threads = 0;
	e->n_busy = 0;
	e->creator_pid = getpid();

	dm_list_init(&e->free);
	dm_list_init(&e->completed);

	for (i = 0; i < n_threads; i++) {
		e->slots[i].engine = e;
		e->slots[i].has_work = 0;
		e->slots[i].shutdown = 0;
		dm_list_add(&e->free, &e->slots[i].list);
	}

	if (pthread_mutex_init(&e->completed_lock, NULL) ||
	    pthread_cond_init(&e->completed_cond, NULL)) {
		log_warn("WARNING: bcache threads failed to init synchronization.");
		free(e);
		return NULL;
	}

	/* From here on, _threads_destroy() handles cleanup */

	if (pthread_attr_init(&attr)) {
		log_warn("WARNING: bcache threads failed to init pthread attributes.");
		_threads_destroy(&e->base);
		return NULL;
	}

	if (pthread_attr_setstacksize(&attr, DM_WORKER_STACK_SIZE))
		log_warn("WARNING: bcache threads failed to set thread stack size.");

	for (i = 0; i < n_threads; i++) {
		char name[16];

		if (pthread_mutex_init(&e->slots[i].mutex, NULL) ||
		    pthread_cond_init(&e->slots[i].cond, NULL)) {
			log_warn("WARNING: bcache threads failed to init slot sync %u.", i);
			pthread_mutex_destroy(&e->slots[i].mutex);
			pthread_cond_destroy(&e->slots[i].cond);
			pthread_attr_destroy(&attr);
			_threads_destroy(&e->base);
			return NULL;
		}

		if (pthread_create(&e->slots[i].thread, &attr, _worker_fn, &e->slots[i])) {
			log_warn("WARNING: bcache threads failed to create worker thread %u.", i);
			pthread_mutex_destroy(&e->slots[i].mutex);
			pthread_cond_destroy(&e->slots[i].cond);
			pthread_attr_destroy(&attr);
			_threads_destroy(&e->base);
			return NULL;
		}

		/* pthread name limit: 16 bytes including NUL */
		(void) dm_snprintf(name, sizeof(name), "bcache_io_%u", i);
		pthread_setname_np(e->slots[i].thread, name);

		e->n_threads++;
	}

	pthread_attr_destroy(&attr);

	log_debug("Created bcache threads io engine with %u threads.", n_threads);

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->base;
}
