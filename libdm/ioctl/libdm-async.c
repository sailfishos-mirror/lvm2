/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libdm-async.h"
#include "libdm-targets.h"
#include "libdm/misc/dmlib.h"

#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/*
 * Worker threads only execute ioctl() + list ops.
 * 32 KiB is generous; the default 8-12 MB wastes address space.
 */
#define DM_WORKER_STACK_SIZE  (32u * 1024u)

/*
 * Compute retry deadline: current time + DM_RETRY_USLEEP_DELAY.
 *
 * With HAVE_REALTIME, uses CLOCK_MONOTONIC for sub-second precision
 * immune to wall-clock jumps (NTP, manual date changes).  The retry
 * condvar is initialized with pthread_condattr_setclock(CLOCK_MONOTONIC)
 * to match.
 *
 * Without HAVE_REALTIME, falls back to time() + 1 second -- no
 * sub-second granularity available, so round up to ensure the
 * delay is at least DM_RETRY_USLEEP_DELAY.
 */
static void _get_retry_deadline(struct timespec *ts)
{
#ifdef HAVE_REALTIME
	if (clock_gettime(CLOCK_MONOTONIC, ts) == 0) {
		ts->tv_nsec += (long)DM_RETRY_USLEEP_DELAY * 1000;
		if (ts->tv_nsec >= 1000000000L) {
			ts->tv_sec++;
			ts->tv_nsec -= 1000000000L;
		}
		return;
	}

	log_sys_debug("clock_gettime", "");
#endif
	ts->tv_sec = time(NULL) + 1;
	ts->tv_nsec = 0;
}

/*
 * Initialize condvar -- with CLOCK_MONOTONIC when available,
 * so pthread_cond_timedwait() matches _get_retry_deadline().
 */
static int _pthread_cond_init(pthread_cond_t *cond)
{
#ifdef HAVE_REALTIME
	pthread_condattr_t cattr;
	int r;

	r = (pthread_condattr_init(&cattr) ||
	     pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) ||
	     pthread_cond_init(cond, &cattr));
	pthread_condattr_destroy(&cattr);

	return r;
#else
	return pthread_cond_init(cond, NULL);
#endif
}

/* ------------------------------------------------------------------ */
/* Thread-pool backend                                                  */
/* ------------------------------------------------------------------ */

struct async_threads {
	struct dm_async_ctx  base;         /* vtable + fd; must be first */
	pthread_mutex_t      lock;
	pthread_cond_t       work_cond;    /* workers wait for items */
	pthread_cond_t       done_cond;    /* main waits for completions / slots */
	pthread_cond_t       retry_cond;   /* retry thread waits for items */
	pid_t                creator_pid;  /* for fork() detection */
	struct dm_list       pending;
	struct dm_list       completed;
	struct dm_list       retry;        /* EBUSY tasks awaiting delay */
	unsigned             n_threads;
	unsigned             n_inflight;   /* pending + executing + retry */
	unsigned             n_retry;      /* tasks in retry queue */
	int                  shutdown;
	int                  event_fd;     /* eventfd, readable on completion */
	pthread_t            retry_thread; /* dedicated retry delay thread */
	pthread_t            threads[];    /* flex array; must be last */
};

/* Return first task from list, or NULL if empty. Must hold ctx->lock. */
static struct dm_task *_first_task(struct dm_list *head)
{
	struct dm_list *first = dm_list_first(head);

	return first ? dm_list_item(first, struct dm_task) : NULL;
}

static void *_worker_fn(void *arg)
{
	struct async_threads *ctx = arg;
	struct dm_task *dmt;
	uint64_t one = 1;

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->shutdown) {
		if (!(dmt = _first_task(&ctx->pending))) {
			pthread_cond_wait(&ctx->work_cond, &ctx->lock);
			continue;
		}

		dm_list_del(&dmt->list);
		pthread_mutex_unlock(&ctx->lock);

		/* Execute a single ioctl outside the lock. */
		dm_ioctl_exec(ctx->base.fd, dmt, dmt->dmi.v4);

		pthread_mutex_lock(&ctx->lock);
		dm_list_add(&ctx->completed, &dmt->list);
		pthread_cond_signal(&ctx->done_cond);

		(void) write(ctx->event_fd, &one, sizeof(one));
	}
	pthread_mutex_unlock(&ctx->lock);
	return NULL;
}

/*
 * Dedicated retry thread: holds EBUSY tasks for one delay interval,
 * then moves them all back to the pending queue for workers to re-execute.
 * This way workers never sleep -- only the retry thread does.
 */
static void *_retry_fn(void *arg)
{
	struct async_threads *ctx = arg;
	struct dm_task *dmt, *tmp;

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->shutdown) {
		if (dm_list_empty(&ctx->retry)) {
			pthread_cond_wait(&ctx->retry_cond, &ctx->lock);
			continue;
		}

		/* Wait for full delay -- ignore spurious wakes from submit */
		{
			struct timespec ts;
			int rc = 0;

			_get_retry_deadline(&ts);
			while (!ctx->shutdown && rc != ETIMEDOUT)
				rc = pthread_cond_timedwait(&ctx->retry_cond,
							   &ctx->lock, &ts);
		}

		if (ctx->shutdown)
			break;

		/* Move all retry tasks to pending in one batch */
		dm_list_iterate_items_safe(dmt, tmp, &ctx->retry) {
			dm_list_del(&dmt->list);
			ctx->n_retry--;
			dm_list_add(&ctx->pending, &dmt->list);
		}

		pthread_cond_broadcast(&ctx->work_cond);
	}
	pthread_mutex_unlock(&ctx->lock);
	return NULL;
}

/* Submit a task to the thread pool. Returns 1 on success, 0 if shut down. */
static int _threads_submit(struct dm_async_ctx *base,
			   struct dm_task *dmt, void *userdata)
{
	struct async_threads *ctx = (struct async_threads *)base;

	dmt->async_userdata = userdata;

	pthread_mutex_lock(&ctx->lock);

	if (ctx->shutdown) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	ctx->n_inflight++;

	/* EBUSY retry: route to dedicated retry thread for delay */
	if (dmt->retry_remove > 1) {
		dm_list_add(&ctx->retry, &dmt->list);
		ctx->n_retry++;
		pthread_cond_signal(&ctx->retry_cond);
		pthread_mutex_unlock(&ctx->lock);
		return 1;
	}

	dm_list_add(&ctx->pending, &dmt->list);
	pthread_cond_signal(&ctx->work_cond);
	pthread_mutex_unlock(&ctx->lock);
	return 1;
}

static int _threads_wait(struct dm_async_ctx *base, int blocking,
			 struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_task *dmt;

	pthread_mutex_lock(&ctx->lock);

	while (!(dmt = _first_task(&ctx->completed)) &&
	       blocking && ctx->n_inflight > 0) {
		/*
		 * Note: pthread_cond_wait() is not interruptible by signals.
		 * Currently this is fine because async deactivation runs
		 * under VG lock with signals blocked (sigprocmask).  If
		 * async ioctls are ever used outside the VG lock, this
		 * wait would need to be replaced with pthread_cond_timedwait()
		 * or poll() on event_fd so the caller can check for Ctrl+C.
		 */
		pthread_cond_wait(&ctx->done_cond, &ctx->lock);
	}

	if (!dmt) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	dm_list_del(&dmt->list);
	ctx->n_inflight--;
	pthread_cond_signal(&ctx->done_cond);   /* wake blocked submitters */
	pthread_mutex_unlock(&ctx->lock);

	*dmt_out      = dmt;
	*userdata_out = dmt->async_userdata;

	return 1;
}

static unsigned _threads_inflight(struct dm_async_ctx *base)
{
	struct async_threads *ctx = (struct async_threads *)base;
	unsigned n;

	pthread_mutex_lock(&ctx->lock);
	n = ctx->n_inflight;
	pthread_mutex_unlock(&ctx->lock);

	return n;
}

static int _threads_get_fd(struct dm_async_ctx *base)
{
	return ((struct async_threads *)base)->event_fd;
}

/* Destroy undrained tasks, invoke callbacks so callers can clean up userdata.
 * _safe iterator needed because dm_task_destroy() frees the dmt. */
static unsigned _leak_list(struct dm_async_ctx *base, struct dm_list *head)
{
	struct dm_task *dmt, *tmp;
	unsigned leaked = 0;

	dm_list_iterate_items_safe(dmt, tmp, head) {
		dm_list_del(&dmt->list);
		if (dmt->async_complete_fn)
			(void) dmt->async_complete_fn(base, dmt, 0,
						      dmt->async_userdata);
		dm_task_destroy(dmt);
		leaked++;
	}

	if (leaked)
		log_warn("WARNING: Destroyed async context with %u undrained task(s).",
			 leaked);

	return leaked;
}

static void _threads_destroy(struct dm_async_ctx *base)
{
	struct async_threads *ctx = (struct async_threads *)base;
	unsigned i;

	/*
	 * After fork() only the calling thread survives in the child.
	 * Worker threads are gone, and the mutex/condvar state is
	 * indeterminate per POSIX.  Detect this and just free memory.
	 */
	if (getpid() == ctx->creator_pid) {
		if (ctx->n_threads || ctx->retry_thread) {
			pthread_mutex_lock(&ctx->lock);
			ctx->shutdown = 1;
			pthread_cond_broadcast(&ctx->work_cond);
			pthread_cond_broadcast(&ctx->done_cond);
			pthread_cond_broadcast(&ctx->retry_cond);
			pthread_mutex_unlock(&ctx->lock);

			/*
			 * No pthread_kill() before join: DM remove ioctls
			 * are fast (immediate success or EBUSY), and most
			 * DM ioctls use TASK_UNINTERRUPTIBLE in the kernel
			 * so signals would not interrupt them anyway.
			 */
			for (i = 0; i < ctx->n_threads; i++)
				(void) pthread_join(ctx->threads[i], NULL);
			if (ctx->retry_thread)
				(void) pthread_join(ctx->retry_thread, NULL);

			pthread_cond_destroy(&ctx->retry_cond);
			pthread_cond_destroy(&ctx->done_cond);
			pthread_cond_destroy(&ctx->work_cond);
			pthread_mutex_destroy(&ctx->lock);
		}

		if (ctx->event_fd >= 0 && close(ctx->event_fd))
			log_sys_debug("close", "eventfd");
	}

	/* Splice all lists and destroy undrained tasks in one pass */
	dm_list_splice(&ctx->pending, &ctx->completed);
	dm_list_splice(&ctx->pending, &ctx->retry);
	_leak_list(base, &ctx->pending);

	dm_free(ctx);
}

/*
 * Drain async completions from a context.
 * Called after the batch deactivation loop, before dm_udev_wait().
 * Does NOT destroy the ctx -- caller owns that.
 *
 * n_inflight == NULL:  blocking -- drain ALL pending completions.
 *                      Not interruptible by Ctrl+C (see _threads_wait).
 *                      Currently safe because callers hold VG lock
 *                      which blocks signals via sigprocmask.
 * n_inflight != NULL:  non-blocking -- drain only ready completions,
 *                      set *n_inflight to tasks still pending (0 = done).
 *                      For interruptible drain, use this mode in a loop
 *                      with poll() on dm_async_get_fd() between iterations.
 */
int dm_async_drain(struct dm_async_ctx *ctx, unsigned *n_inflight)
{
	struct dm_task *dmt;
	void *userdata;
	int r = 1;
	unsigned count = 0;

	if (!ctx) {
		if (n_inflight)
			*n_inflight = 0;
		return 1;
	}

	if (n_inflight) {
		/* Non-blocking: drain only ready completions.
		 * Clear eventfd first so poll() re-arms after we return. */
		int efd = ctx->fn_get_fd(ctx);

		if (efd >= 0) {
			uint64_t val;
			ssize_t ret = read(efd, &val, sizeof(val));
			if (ret < 0 && errno != EAGAIN)
				log_debug_activation("eventfd read error: %s.",
						     strerror(errno));
		}

		while (ctx->fn_wait(ctx, DM_WAIT_POLL, &dmt, &userdata)) {
			if (!dm_async_task_completion(ctx, dmt, userdata)) {
				stack;
				r = 0;
			}
			count++;
		}

		*n_inflight = ctx->fn_inflight(ctx);
	} else {
		/* Blocking: drain everything */
		while (ctx->fn_wait(ctx, DM_WAIT_BLOCK, &dmt, &userdata)) {
			if (!dm_async_task_completion(ctx, dmt, userdata)) {
				stack;
				r = 0;
			}
			count++;
		}
	}

	/* Pick up any sticky error from back-pressure in dm_async_submit */
	if (ctx->submit_drain_error) {
		stack;
		r = 0;
		ctx->submit_drain_error = 0;
	}

	if (count)
		log_debug_activation("Drained %u async completion(s).",
				     count);

	return r;
}

static int _create_async_threads(struct async_threads *ctx, unsigned max_inflight)
{
	pthread_attr_t attr;
	size_t ssize = DM_WORKER_STACK_SIZE;
	unsigned i = 0;
	int r = 0;

	if (ssize < PTHREAD_STACK_MIN)
		ssize = PTHREAD_STACK_MIN;

	if (pthread_attr_init(&attr)) {
		log_error("Failed to init worker thread attributes.");
		return 0;
	}

	if (pthread_attr_setstacksize(&attr, ssize)) {
		log_error("Failed to set worker thread stack size.");
		goto out;
	}

	if (pthread_create(&ctx->retry_thread, &attr, _retry_fn, ctx)) {
		log_error("Failed to create retry thread.");
		goto out;
	}
	/* pthread name limit: 16 bytes including NUL,
	 * see with: ps -eL -o pid,spid,comm */
	pthread_setname_np(ctx->retry_thread, "dm_ioctl_retry");

	for (; i < max_inflight; i++) {
		char name[16];

		if (pthread_create(&ctx->threads[i], &attr, _worker_fn, ctx)) {
			log_error("Failed to create worker thread %u.", i);
			goto out;
		}
		(void) dm_snprintf(name, sizeof(name), "dm_ioctl_%u", i);
		pthread_setname_np(ctx->threads[i], name);
	}

	r = 1;
out:
	ctx->n_threads = i;   /* only join threads we started */
	pthread_attr_destroy(&attr);

	return r;
}

struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_inflight)
{
	struct async_threads *ctx;

	ctx = dm_zalloc(sizeof(*ctx) + max_inflight * sizeof(pthread_t));
	if (!ctx) {
		log_error("Failed to allocate async thread pool context.");
		return NULL;
	}

	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _threads_submit;
	ctx->base.fn_wait      = _threads_wait;
	ctx->base.fn_inflight  = _threads_inflight;
	ctx->base.fn_destroy   = _threads_destroy;
	ctx->creator_pid       = getpid();
	ctx->base.fn_get_fd    = _threads_get_fd;
	ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ctx->event_fd < 0) {
		log_sys_error("eventfd", "async context");
		dm_free(ctx);
		return NULL;
	}
	dm_list_init(&ctx->pending);
	dm_list_init(&ctx->completed);
	dm_list_init(&ctx->retry);

	/*
	 * On Linux/glibc, pthread_mutex/cond_init with NULL attrs on
	 * zeroed (dm_zalloc) memory performs no heap allocation -- the
	 * zero state matches the initialized state.
	 */
	if (_pthread_cond_init(&ctx->retry_cond) ||
	    pthread_mutex_init(&ctx->lock, NULL) ||
	    pthread_cond_init(&ctx->work_cond, NULL) ||
	    pthread_cond_init(&ctx->done_cond, NULL) ||
	    !_create_async_threads(ctx, max_inflight)) {
		log_error("Failed to initialise async thread pool primitives.");
		_threads_destroy(&ctx->base);
		return NULL;
	}

	return &ctx->base;
}
