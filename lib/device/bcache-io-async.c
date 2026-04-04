/*
 * Copyright (C) 2018-2026 Red Hat, Inc. All rights reserved.
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
#include "lib/misc/lvm-signal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libaio.h>

//----------------------------------------------------------------

#define SECTOR_SHIFT 9L
#define MAX_EVENT 64

static const char _async[] = "async";

static const char *_async_dir(int opcode)
{
	return (opcode == IO_CMD_PREAD) ? "read" : "write";
}

// Assumes the list is not empty.
static inline struct dm_list *_list_pop(struct dm_list *head)
{
	struct dm_list *l;

	l = head->n;
	dm_list_del(l);
	return l;
}

//----------------------------------------------------------------

struct control_block {
	struct dm_list list;
	void *context;
	struct iocb cb;
};

struct cb_set {
	struct dm_list free;
	struct dm_list allocated;
	struct control_block vec[];
};

static struct cb_set *_cb_set_create(unsigned nr)
{
	unsigned i;
	struct cb_set *cbs = malloc(sizeof(*cbs) + nr * sizeof(*cbs->vec));

	if (!cbs)
		return NULL;

	dm_list_init(&cbs->free);
	dm_list_init(&cbs->allocated);

	for (i = 0; i < nr; i++)
		dm_list_add(&cbs->free, &cbs->vec[i].list);

	return cbs;
}

static void _cb_set_destroy(struct cb_set *cbs)
{
	// We know this is always called after a wait_all.  So there should
	// never be in flight IO.
	if (!dm_list_empty(&cbs->allocated)) {
		// bail out
		log_warn("WARNING: bcache async still in flight.");
		return;
	}

	free(cbs);
}

static struct control_block *_cb_alloc(struct cb_set *cbs, void *context)
{
	struct control_block *cb;

	if (dm_list_empty(&cbs->free))
		return NULL;

	cb = dm_list_item(_list_pop(&cbs->free), struct control_block);
	cb->context = context;
	dm_list_add(&cbs->allocated, &cb->list);

	return cb;
}

static void _cb_free(struct cb_set *cbs, struct control_block *cb)
{
	dm_list_del(&cb->list);
	dm_list_add_h(&cbs->free, &cb->list);
}

static struct control_block *_iocb_to_cb(struct iocb *icb)
{
	return dm_list_struct_base(icb, struct control_block, cb);
}

//----------------------------------------------------------------

struct async_engine {
	struct io_engine e;
	io_context_t aio_context;
	struct cb_set *cbs;
	unsigned max_io;
	unsigned page_mask;
	pid_t aio_context_pid; /* PID that created this AIO context */
	struct io_event event[MAX_EVENT];
};

static struct async_engine *_to_async(struct io_engine *e)
{
	return container_of(e, struct async_engine, e);
}

static void _async_destroy(struct io_engine *ioe)
{
	int r;
	struct async_engine *e = _to_async(ioe);

	_cb_set_destroy(e->cbs);

	/*
	 * Only call io_destroy() if we're in the same process that created
	 * the AIO context. After fork(), the child inherits the parent's
	 * aio_context value but must not call io_destroy() on it.
	 */
	if (e->aio_context) {
		if (e->aio_context_pid == getpid()) {
			log_debug_devs("Destroy AIO context.");
			r = io_destroy(e->aio_context); // really slow (~30ms)
			if (r < 0)
				log_bcache_sys_warn(_async, "io_destroy", -r);
		} else
			log_debug_devs("Skipping AIO context destroy for different pid.");
	}

	free(e);
}

static bool _async_issue(struct io_engine *ioe, enum dir d, int fd,
			 sector_t sb, sector_t se, void *data, void *context)
{
	int r;
	struct iocb *cb_array[1];
	struct control_block *cb;
	struct async_engine *e = _to_async(ioe);
	sector_t offset;
	sector_t nbytes;

	if (((uintptr_t) data) & e->page_mask) {
		log_warn("WARNING: bcache async misaligned data buffer.");
		return false;
	}

	offset = sb << SECTOR_SHIFT;
	nbytes = (se - sb) << SECTOR_SHIFT;

	cb = _cb_alloc(e->cbs, context);
	if (!cb) {
		log_warn("WARNING: bcache async failed to allocate control block.");
		return false;
	}

	memset(&cb->cb, 0, sizeof(cb->cb));

	cb->cb.aio_fildes = (int) fd;
	cb->cb.u.c.buf = data;
	cb->cb.u.c.offset = offset;
	cb->cb.u.c.nbytes = nbytes;
	cb->cb.aio_lio_opcode = (d == DIR_READ) ? IO_CMD_PREAD : IO_CMD_PWRITE;

	cb_array[0] = &cb->cb;
	do {
		r = io_submit(e->aio_context, 1, cb_array);
	} while (r == -EAGAIN);

	if (r < 0) {
		_cb_free(e->cbs, cb);
		return false;
	}

	return true;
}

static bool _async_wait(struct io_engine *ioe, io_complete_fn fn)
{
	int i, r;
	struct control_block *cb;
	struct io_event *ev;
	struct async_engine *e = _to_async(ioe);

	/* e->event[] zeroed at allocation; io_getevents fills 0..r-1 */

	/*
	 * Retry on EINTR from stray signals, but stop if an LVM interrupt
	 * signal (SIGINT/SIGTERM via sigint_allow()) has been caught.
	 */
	do {
		r = io_getevents(e->aio_context, 1, MAX_EVENT, e->event, NULL);
	} while (r == -EINTR && !sigint_caught());

	if (r < 0) {
		if (r == -EINTR)
			stack;
		else
			log_bcache_sys_warn(_async, "io_getevents", -r);
		return false;
	}

	for (i = 0; i < r; i++) {
		ev = e->event + i;
		cb = _iocb_to_cb((struct iocb *) ev->obj);

		if ((int) ev->res < 0) {
			log_warn("WARNING: bcache async %s device offset %llu len %llu: %s.",
				 _async_dir(cb->cb.aio_lio_opcode),
				 (unsigned long long)cb->cb.u.c.offset,
				 (unsigned long long)cb->cb.u.c.nbytes,
				 strerror(-(int)ev->res));
			fn(cb->context, (int) ev->res);
		} else if (ev->res >= cb->cb.u.c.nbytes) {
			fn(cb->context, 0);
		} else if (ev->res >= (1 << SECTOR_SHIFT)) {
			/* minimum acceptable read is 1 sector */
			log_debug_devs("bcache async %s offset %llu requested %llu got %llu.",
				       _async_dir(cb->cb.aio_lio_opcode),
				       (unsigned long long)cb->cb.u.c.offset,
				       (unsigned long long)cb->cb.u.c.nbytes,
				       (unsigned long long)ev->res);
			fn(cb->context, 0);
		} else {
			log_warn("WARNING: bcache async %s offset %llu requested %llu got %llu.",
				 _async_dir(cb->cb.aio_lio_opcode),
				 (unsigned long long)cb->cb.u.c.offset,
				 (unsigned long long)cb->cb.u.c.nbytes,
				 (unsigned long long)ev->res);
			fn(cb->context, -ENODATA);
		}

		_cb_free(e->cbs, cb);
	}

	return true;
}

static unsigned _async_max_io(struct io_engine *ioe)
{
	return _to_async(ioe)->max_io;
}

struct io_engine *create_async_io_engine(unsigned queue_depth)
{
	static int _pagesize = 0;
	int r;
	struct async_engine *e;

	if (!queue_depth)
		queue_depth = BCACHE_MAX_IO;

	if ((_pagesize <= 0) && (_pagesize = sysconf(_SC_PAGESIZE)) < 0) {
		log_warn("WARNING: bcache async _SC_PAGESIZE returns negative value.");
		return NULL;
	}

	if (!(e = zalloc(sizeof(*e)))) {
		log_warn("WARNING: bcache async failed to allocate engine.");
		return NULL;
	}

	e->e.destroy = _async_destroy;
	e->e.issue = _async_issue;
	e->e.wait = _async_wait;
	e->e.max_io = _async_max_io;
	e->max_io = queue_depth;

	e->aio_context = 0;
	e->aio_context_pid = getpid();
	r = io_setup(queue_depth, &e->aio_context);
	if (r < 0) {
		log_bcache_sys_warn(_async, "io_setup", -r);
		free(e);
		return NULL;
	}

	e->cbs = _cb_set_create(queue_depth);
	if (!e->cbs) {
		log_warn("WARNING: bcache async failed to create control block set.");
		io_destroy(e->aio_context);
		free(e);
		return NULL;
	}

	e->page_mask = (unsigned) _pagesize - 1;

	log_debug("Created bcache async io engine with queue depth %u.", queue_depth);

	/* coverity[leaked_storage] 'e' is not leaking */
	return &e->e;
}
