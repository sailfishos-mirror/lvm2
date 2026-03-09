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
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/* Thread-pool backend                                                  */
/* ------------------------------------------------------------------ */

struct async_threads {
	struct dm_async_ctx  base;         /* vtable + fd; must be first */
	pthread_mutex_t      lock;
	pthread_cond_t       work_cond;    /* workers wait for items */
	pthread_cond_t       done_cond;    /* main waits for completions / slots */
	struct dm_list       pending;
	struct dm_list       completed;
	unsigned             n_threads;
	unsigned             n_inflight;   /* pending + executing */
	int                  shutdown;
	pthread_t            threads[];    /* flex array; must be last */
};

static void *_worker_fn(void *arg)
{
	struct async_threads *ctx = arg;
	struct dm_work_item *item;

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->shutdown) {
		if (dm_list_empty(&ctx->pending)) {
			pthread_cond_wait(&ctx->work_cond, &ctx->lock);
			continue;
		}

		item = dm_list_item(dm_list_first(&ctx->pending),
				    struct dm_work_item);
		dm_list_del(&item->list);
		pthread_mutex_unlock(&ctx->lock);

		/* Execute the ioctl outside the lock (with EBUSY retry). */
		dm_ioctl_exec_retry(ctx->base.fd, item->dmt);

		pthread_mutex_lock(&ctx->lock);
		dm_list_add(&ctx->completed, &item->list);
		pthread_cond_signal(&ctx->done_cond);
	}
	pthread_mutex_unlock(&ctx->lock);
	return NULL;
}

static int _threads_submit(struct dm_async_ctx *base,
			   struct dm_task *dmt, void *userdata)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_work_item *item;

	if (!(item = malloc(sizeof(*item)))) {
		log_error("Failed to allocate async work item.");
		return 0;
	}
	dm_list_init(&item->list);
	item->dmt      = dmt;
	item->userdata = userdata;

	pthread_mutex_lock(&ctx->lock);

	if (ctx->shutdown) {
		pthread_mutex_unlock(&ctx->lock);
		free(item);
		return 0;
	}

	ctx->n_inflight++;
	dm_list_add(&ctx->pending, &item->list);
	pthread_cond_signal(&ctx->work_cond);
	pthread_mutex_unlock(&ctx->lock);
	return 1;
}

static int _threads_wait(struct dm_async_ctx *base,
			 struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_work_item *item;

	pthread_mutex_lock(&ctx->lock);
	while (dm_list_empty(&ctx->completed) && ctx->n_inflight > 0)
		pthread_cond_wait(&ctx->done_cond, &ctx->lock);

	if (dm_list_empty(&ctx->completed)) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	item = dm_list_item(dm_list_first(&ctx->completed),
			    struct dm_work_item);
	dm_list_del(&item->list);
	ctx->n_inflight--;
	pthread_cond_signal(&ctx->done_cond);   /* wake blocked submitters */
	pthread_mutex_unlock(&ctx->lock);

	*dmt_out      = item->dmt;
	*userdata_out = item->userdata;
	free(item);
	return 1;
}

static int _threads_try_wait(struct dm_async_ctx *base,
			     struct dm_task **dmt_out, void **userdata_out)
{
	struct async_threads *ctx = (struct async_threads *)base;
	struct dm_work_item *item;

	pthread_mutex_lock(&ctx->lock);

	if (dm_list_empty(&ctx->completed)) {
		pthread_mutex_unlock(&ctx->lock);
		return 0;
	}

	item = dm_list_item(dm_list_first(&ctx->completed),
			    struct dm_work_item);
	dm_list_del(&item->list);
	ctx->n_inflight--;
	pthread_cond_signal(&ctx->done_cond);
	pthread_mutex_unlock(&ctx->lock);

	*dmt_out      = item->dmt;
	*userdata_out = item->userdata;
	free(item);
	return 1;
}

static void _threads_destroy(struct dm_async_ctx *base)
{
	struct async_threads *ctx = (struct async_threads *)base;
	unsigned i;

	pthread_mutex_lock(&ctx->lock);
	ctx->shutdown = 1;
	pthread_cond_broadcast(&ctx->work_cond);
	pthread_cond_broadcast(&ctx->done_cond);
	pthread_mutex_unlock(&ctx->lock);

	for (i = 0; i < ctx->n_threads; i++)
		pthread_join(ctx->threads[i], NULL);

	pthread_mutex_destroy(&ctx->lock);
	pthread_cond_destroy(&ctx->work_cond);
	pthread_cond_destroy(&ctx->done_cond);
	free(ctx);
}

/*
 * Drain all deferred async completions from a context.
 * Called after the batch deactivation loop, before dm_udev_wait().
 * Does NOT destroy the ctx -- caller owns that.
 */
int dm_async_drain(struct dm_async_ctx *ctx)
{
	struct dm_task *dmt;
	void *userdata;
	int r = 1;
	unsigned count = 0;

	if (!ctx)
		return 1;

	while (dm_async_wait_completion(ctx, &dmt, &userdata)) {
		if (!dm_task_handle_completion(dmt, userdata))
			r = 0;
		dm_task_destroy(dmt);
		count++;
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

	ctx = calloc(1, sizeof(*ctx) + max_parallel * sizeof(pthread_t));
	if (!ctx) {
		log_error("Failed to allocate async thread pool context.");
		return NULL;
	}

	ctx->base.fd           = fd;
	ctx->base.fn_submit    = _threads_submit;
	ctx->base.fn_wait      = _threads_wait;
	ctx->base.fn_try_wait  = _threads_try_wait;
	ctx->base.fn_destroy   = _threads_destroy;
	ctx->n_threads       = max_parallel;
	dm_list_init(&ctx->pending);
	dm_list_init(&ctx->completed);
	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->work_cond, NULL);
	pthread_cond_init(&ctx->done_cond, NULL);

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

