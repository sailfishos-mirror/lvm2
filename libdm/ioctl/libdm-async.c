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

/*
 * Async DM ioctl submission backends:
 *   - thread pool (always compiled)
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

/* ------------------------------------------------------------------ */
/* Thread-pool backend                                                  */
/* ------------------------------------------------------------------ */

struct async_threads {
	struct dm_async_ctx  base;         /* vtable + fd; must be first */
	pthread_mutex_t      lock;
	pthread_cond_t       work_cond;    /* workers wait for items */
	pthread_cond_t       done_cond;    /* main waits for completions / slots */
	pthread_cond_t       retry_cond;   /* retry thread waits for items */
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

/* Return first pending task, or NULL if empty. Must hold ctx->lock. */
static struct dm_task *_first_pending(struct async_threads *ctx)
{
	struct dm_task *dmt;

	/* coverity[unreachable] intentional: return first item */
	dm_list_iterate_items(dmt, &ctx->pending)
		return dmt;

	return NULL;
}

/* Return first completed task, or NULL if empty. Must hold ctx->lock. */
static struct dm_task *_first_completed(struct async_threads *ctx)
{
	struct dm_task *dmt;

	/* coverity[unreachable] intentional: return first item */
	dm_list_iterate_items(dmt, &ctx->completed)
		return dmt;

	return NULL;
}

static void *_worker_fn(void *arg)
{
	struct async_threads *ctx = arg;
	struct dm_task *dmt;

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->shutdown) {
		if (!(dmt = _first_pending(ctx))) {
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
		{
			uint64_t one = 1;
			(void) write(ctx->event_fd, &one, sizeof(one));
		}
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

			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_nsec += (long)DM_RETRY_USLEEP_DELAY * 1000;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
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

/* Block until a completed task is available. Returns 0 when none remain. */
static int _threads_wait(struct dm_async_ctx *base,
			 struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_task *dmt;

	pthread_mutex_lock(&ctx->lock);
	while (!(dmt = _first_completed(ctx)) && ctx->n_inflight > 0)
		pthread_cond_wait(&ctx->done_cond, &ctx->lock);

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

/* Non-blocking poll for a completed task. Returns 0 if none ready. */
static int _threads_try_wait(struct dm_async_ctx *base,
			     struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_task *dmt;

	pthread_mutex_lock(&ctx->lock);

	if (!(dmt = _first_completed(ctx))) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	dm_list_del(&dmt->list);
	ctx->n_inflight--;
	pthread_cond_signal(&ctx->done_cond);
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

static void _leak_list(struct dm_async_ctx *base,
		       struct dm_list *head, unsigned *leaked)
{
	struct dm_task *dmt, *tmp;

	dm_list_iterate_items_safe(dmt, tmp, head) {
		dm_list_del(&dmt->list);
		if (dmt->async_complete_fn)
			(void) dmt->async_complete_fn(base, dmt, 0,
						      dmt->async_userdata);
		dm_task_destroy(dmt);
		(*leaked)++;
	}
}

static void _threads_destroy(struct dm_async_ctx *base)
{
	struct async_threads *ctx = (struct async_threads *)base;
	unsigned i, leaked = 0;

	pthread_mutex_lock(&ctx->lock);
	ctx->shutdown = 1;
	pthread_cond_broadcast(&ctx->work_cond);
	pthread_cond_broadcast(&ctx->done_cond);
	pthread_cond_broadcast(&ctx->retry_cond);
	pthread_mutex_unlock(&ctx->lock);

	(void) pthread_join(ctx->retry_thread, NULL);
	for (i = 0; i < ctx->n_threads; i++)
		(void) pthread_join(ctx->threads[i], NULL);

	/* Destroy any undrained tasks (caller should have called dm_async_drain).
	 * Invoke complete_fn so callbacks can clean up userdata. */
	_leak_list(base, &ctx->pending, &leaked);
	_leak_list(base, &ctx->completed, &leaked);
	_leak_list(base, &ctx->retry, &leaked);

	if (leaked)
		log_warn("WARNING: Destroyed async context with %u undrained task(s).",
			 leaked);

	if (ctx->event_fd >= 0 && close(ctx->event_fd))
		log_sys_debug("close", "eventfd");

	pthread_mutex_destroy(&ctx->lock);
	pthread_cond_destroy(&ctx->work_cond);
	pthread_cond_destroy(&ctx->done_cond);
	pthread_cond_destroy(&ctx->retry_cond);
	dm_free(ctx);
}

/*
 * Drain async completions from a context.
 * Called after the batch deactivation loop, before dm_udev_wait().
 * Does NOT destroy the ctx -- caller owns that.
 *
 * n_inflight == NULL:  blocking -- drain ALL pending completions.
 * n_inflight != NULL:  non-blocking -- drain only ready completions,
 *                      set *n_inflight to tasks still pending (0 = done).
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

		while (ctx->fn_try_wait(ctx, &dmt, &userdata)) {
			if (!dm_task_handle_completion(ctx, dmt, userdata)) {
				stack;
				r = 0;
			}
			count++;
		}

		*n_inflight = ctx->fn_inflight(ctx);
	} else {
		/* Blocking: drain everything */
		while (dm_async_wait_completion(ctx, &dmt, &userdata)) {
			if (!dm_task_handle_completion(ctx, dmt, userdata)) {
				stack;
				r = 0;
			}
			count++;
		}
	}

	if (count)
		log_debug_activation("Drained %u async completion(s).",
				     count);

	return r;
}

struct dm_async_ctx *dm_async_ctx_alloc_threads(int fd, unsigned max_parallel)
{
	struct async_threads *ctx;
	unsigned i;

	ctx = dm_zalloc(sizeof(*ctx) + max_parallel * sizeof(pthread_t));
	if (!ctx) {
		log_error("Failed to allocate async thread pool context.");
		return NULL;
	}

	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _threads_submit;
	ctx->base.fn_wait      = _threads_wait;
	ctx->base.fn_try_wait  = _threads_try_wait;
	ctx->base.fn_inflight  = _threads_inflight;
	ctx->base.fn_destroy   = _threads_destroy;
	ctx->base.fn_get_fd    = _threads_get_fd;
	ctx->n_threads       = max_parallel;
	ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (ctx->event_fd < 0) {
		log_sys_error("eventfd", "async context");
		dm_free(ctx);
		return NULL;
	}
	dm_list_init(&ctx->pending);
	dm_list_init(&ctx->completed);
	dm_list_init(&ctx->retry);
	if (pthread_mutex_init(&ctx->lock, NULL) ||
	    pthread_cond_init(&ctx->work_cond, NULL) ||
	    pthread_cond_init(&ctx->done_cond, NULL) ||
	    pthread_cond_init(&ctx->retry_cond, NULL)) {
		log_error("Failed to initialise async thread pool primitives.");
		if (close(ctx->event_fd))
			log_sys_debug("close", "eventfd");
		dm_free(ctx);
		return NULL;
	}

	if (pthread_create(&ctx->retry_thread, NULL, _retry_fn, ctx)) {
		log_error("Failed to create retry thread.");
		_threads_destroy(&ctx->base);
		return NULL;
	}

	for (i = 0; i < max_parallel; i++) {
		if (pthread_create(&ctx->threads[i], NULL, _worker_fn, ctx)) {
			log_error("Failed to create worker thread %u.", i);
			ctx->n_threads = i;   /* only join threads we started */
			_threads_destroy(&ctx->base);
			return NULL;
		}
	}

	return &ctx->base;
}

