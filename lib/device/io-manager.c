/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
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

#include "lib/device/io-manager.h"

#include "base/data-struct/radix-tree.h"
#include "lib/log/lvm-logging.h"
#include "lib/log/log.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libaio.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

#define SECTOR_SHIFT 9L

//----------------------------------------------------------------

static void log_sys_warn(const char *call)
{
	log_warn("%s failed: %s", call, strerror(errno));
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
	struct control_block *vec;
} control_block_set;

static struct cb_set *_cb_set_create(unsigned nr)
{
	int i;
	struct cb_set *cbs = malloc(sizeof(*cbs));

	if (!cbs)
		return NULL;

	cbs->vec = malloc(nr * sizeof(*cbs->vec));
	if (!cbs->vec) {
		free(cbs);
		return NULL;
	}

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
		log_error("async io still in flight");
		return;
	}

	free(cbs->vec);
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
	unsigned page_mask;
	unsigned page_sector_mask;
	struct dm_list completed_fallbacks;
};

// Not all io can be issued asynchronously because of bad alignment
// so we need a fallback synchronous method.
struct completed_fallback {
	struct dm_list list;
	void *context;
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

	// io_destroy is really slow
	r = io_destroy(e->aio_context);
	if (r)
		log_sys_warn("io_destroy");

	free(e);
}

// Used by both the async and sync engines
static int _open_common(const char *path, int os_flags)
{
	int fd;

	os_flags |= O_NOATIME;

	fd = open(path, os_flags);
	if (fd < 0) {
		if ((errno == EBUSY) && (os_flags & O_EXCL))
			log_error("Can't open %s exclusively.  Mounted filesystem?", path);
		else
			log_error("Couldn't open %s, errno = %d", path, errno);
	}

	return fd;
}

static int _async_open(struct io_engine *ioe, const char *path, unsigned flags, bool o_direct)
{
	int os_flags = 0;

	if (o_direct)
		os_flags |= O_DIRECT;

	if (flags & EF_READ_ONLY)
		os_flags |= O_RDONLY;
	else
		os_flags |= O_RDWR;

	if (flags & EF_EXCL)
		os_flags |= O_EXCL;

	return _open_common(path, os_flags);
}

static void _async_close(struct io_engine *e, int fd)
{
	close(fd);
}

static bool _aio_aligned(struct async_engine *ae, sector_t b, sector_t e, void *data)
{
	// Buffer must be page aligned
	if (((uintptr_t) data) & ae->page_mask)
		return false;

	// Start sector must be page aligned
	if (b & ae->page_sector_mask)
		return false;

	// End sector must be page aligned
	if (e & ae->page_sector_mask)
		return false;

	return true;
}

static bool _fallback_issue(struct async_engine *ae, enum dir d, int fd,
                            sector_t b, sector_t e, void *data_, void *context)
{
	int r;
	uint8_t *data = data_;
	uint64_t len = (e - b) * 512, where;
	struct completed_fallback *io = malloc(sizeof(*io));
	if (!io) {
		log_warn("unable to allocate completed_fallback");
		return false;
	}

	where = b * 512;
	r = lseek(fd, where, SEEK_SET);
	if (r < 0) {
		log_warn("unable to seek to position %llu", (unsigned long long) where);
		return false;
	}

	while (len) {
		do {
			if (d == DIR_READ)
				r = read(fd, data, len);
			else
				r = write(fd, data, len);

		} while ((r < 0) && ((r == EINTR) || (r == EAGAIN)));

		if (r < 0) {
			log_warn("io failed %d", r);
			return false;
		}

		len -= r;
		data += r;
	}

	if (len) {
		log_warn("short io %u bytes remaining", (unsigned) len);
		return false;
	}

	dm_list_add(&ae->completed_fallbacks, &io->list);
	io->context = context;

	return true;
}

static bool _async_issue_(struct async_engine *e, enum dir d, int fd,
			  sector_t sb, sector_t se, void *data, void *context)
{
	int r;
	struct iocb *cb_array[1];
	struct control_block *cb;

	cb = _cb_alloc(e->cbs, context);
	if (!cb) {
		log_warn("couldn't allocate control block");
		return false;
	}

	memset(&cb->cb, 0, sizeof(cb->cb));

	cb->cb.aio_fildes = (int) fd;
	cb->cb.u.c.buf = data;
	cb->cb.u.c.offset = sb << SECTOR_SHIFT;
	cb->cb.u.c.nbytes = (se - sb) << SECTOR_SHIFT;
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

static bool _async_issue(struct io_engine *ioe, enum dir d, int fd,
			 sector_t sb, sector_t se, void *data, void *context)
{
	struct async_engine *e = _to_async(ioe);

	if (!_aio_aligned(e, sb, se, data))
		return _fallback_issue(e, d, fd, sb, se, data, context);
	else
		return _async_issue_(e, d, fd, sb, se, data, context);
}

static bool _async_issue_ignore_writes(struct io_engine *ioe, enum dir d, int fd,
                                       sector_t sb, sector_t se, void *data, void *context)
{
	if (d == DIR_WRITE) {
		// complete the io without touching the disk
		struct async_engine *e = _to_async(ioe);
		struct completed_fallback *cw = malloc(sizeof(*cw));
		if (!cw) {
			log_error("couldn't allocate completed_fallback struct");
			return false;
		}

		cw->context = context;
		dm_list_add(&e->completed_fallbacks, &cw->list);

		return true;
	} else
		return _async_issue(ioe, d, fd, sb, se, data, context);
}

/*
 * MAX_IO is returned to the layer above via io_max_prefetches() which
 * tells the caller how many devices to submit io for concurrently.  There will
 * be an open file descriptor for each of these, so keep it low enough to avoid
 * reaching the default max open file limit (1024) when there are over 1024
 * devices being scanned.
 */
#define MAX_IO 256
#define MAX_EVENT 64

static bool _async_wait_(struct async_engine *e, io_complete_fn fn)
{
	int i, r;
	struct io_event event[MAX_EVENT];
	struct control_block *cb;

	memset(&event, 0, sizeof(event));
	do {
		r = io_getevents(e->aio_context, 1, MAX_EVENT, event, NULL);
	} while (r == -EINTR);

	if (r < 0) {
		log_sys_warn("io_getevents");
		return false;
	}

	for (i = 0; i < r; i++) {
		struct io_event *ev = event + i;

		cb = _iocb_to_cb((struct iocb *) ev->obj);

		if (ev->res == cb->cb.u.c.nbytes)
			fn((void *) cb->context, 0);

		else if ((int) ev->res < 0)
			fn(cb->context, (int) ev->res);

		else
			fn(cb->context, -ENODATA);

		_cb_free(e->cbs, cb);
	}

	return true;
}

static bool _async_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct async_engine *e = _to_async(ioe);
	struct completed_fallback *cw, *tmp;
	bool r = false;

	dm_list_iterate_items_safe(cw, tmp, &e->completed_fallbacks) {
		dm_list_del(&cw->list);
		fn(cw->context, 0);
		free(cw);
		r = true;
	}

	return r ? r : _async_wait_(e, fn);
}

static unsigned _async_max_io(struct io_engine *e)
{
	return MAX_IO;
}

static bool _common_get_size(struct io_engine *e, const char *path, int fd, uint64_t *size)
{
	struct stat info;

	if (fstat(fd, &info) < 0) {
		log_sys_error("stat", path);
		return false;
	}

	switch (info.st_mode & S_IFMT) {
	case S_IFBLK:
		if (ioctl(fd, BLKGETSIZE64, size) < 0) {
			log_sys_error("ioctl BLKGETSIZE64", path);
			return false;
		}
		break;

	case S_IFREG:
		*size = info.st_size;
		break;

	default:
		log_error("%s must be a block device or regular file", path);
		return false;
	}

	*size /= 512;
	return true;
}

static bool _common_get_block_sizes(struct io_engine *e, const char *path, int fd,
                                    unsigned *physical_block_size,
				    unsigned *logical_block_size)
{
	unsigned int pbs = 0;
	unsigned int lbs = 0;

	// There are 3 ioctls for getting a block size:
	// BLKBSZGET  - fs allocation unit size
	// BLKPBSZGET - physical block size (kernel 2.6.32 onwards)
	// BLKSSZGET  - logical block size


#ifdef BLKPBSZGET /* not defined before kernel version 2.6.32 (e.g. rhel5) */
	/*
	 * BLKPBSZGET from kernel comment for blk_queue_physical_block_size:
	 * "the lowest possible sector size that the hardware can operate on
	 * without reverting to read-modify-write operations"
	 */
	if (ioctl(fd, BLKPBSZGET, &pbs)) {
		log_debug_devs("No physical block size for %s", path);
		pbs = 0;
	}
#endif

	/*
	 * BLKSSZGET from kernel comment for blk_queue_logical_block_size:
	 * "the lowest possible block size that the storage device can address."
	 */
	if (ioctl(fd, BLKSSZGET, &lbs)) {
		log_debug_devs("No logical block size for %s", path);
		lbs = 0;
	}

	*physical_block_size = pbs;
	*logical_block_size = lbs;

	if (!lbs)
		return false;
	return true;
}

struct io_engine *create_async_io_engine(void)
{
	int r;
	struct async_engine *e = malloc(sizeof(*e));

	if (!e)
		return NULL;

	e->e.destroy = _async_destroy;
	e->e.open = _async_open;
	e->e.close = _async_close;
	e->e.issue = _async_issue;
	e->e.wait = _async_wait;
	e->e.max_io = _async_max_io;
	e->e.get_size = _common_get_size;
	e->e.get_block_sizes = _common_get_block_sizes;

	e->aio_context = 0;
	r = io_setup(MAX_IO, &e->aio_context);
	if (r < 0) {
		log_warn("io_setup failed");
		free(e);
		return NULL;
	}

	e->cbs = _cb_set_create(MAX_IO);
	if (!e->cbs) {
		log_warn("couldn't create control block set");
		free(e);
		return NULL;
	}

	e->page_mask = sysconf(_SC_PAGESIZE) - 1;
	e->page_sector_mask = (sysconf(_SC_PAGESIZE) / 512) - 1;
	dm_list_init(&e->completed_fallbacks);

	return &e->e;
}

struct io_engine *create_test_io_engine(void)
{
	struct io_engine *ioe = create_async_io_engine();

	if (ioe) {
		struct async_engine *e = _to_async(ioe);
		e->e.issue = _async_issue_ignore_writes;
	}

	return ioe;
}

//----------------------------------------------------------------

struct sync_io {
	struct dm_list list;
	void *context;
};

struct sync_engine {
	struct io_engine e;
	struct dm_list complete;
};

static struct sync_engine *_to_sync(struct io_engine *e)
{
	return container_of(e, struct sync_engine, e);
}

static void _sync_destroy(struct io_engine *ioe)
{
	struct sync_engine *e = _to_sync(ioe);
	free(e);
}

static int _sync_open(struct io_engine *ioe, const char *path, unsigned flags, bool o_direct)
{
	int os_flags = 0;

	if (o_direct)
		os_flags |= O_DIRECT;

	if (flags & EF_READ_ONLY)
		os_flags |= O_RDONLY;
	else
		os_flags |= O_RDWR;

	if (flags & EF_EXCL)
		os_flags |= O_EXCL;

	return _open_common(path, os_flags);
}

static bool _sync_issue(struct io_engine *ioe, enum dir d, int fd,
			sector_t sb, sector_t se, void *data_, void *context)
{
	int r;
	uint8_t *data = data_;
	uint64_t len = (se - sb) * 512, where;
	struct sync_engine *e = _to_sync(ioe);
	struct sync_io *io = malloc(sizeof(*io));
	if (!io) {
		log_warn("unable to allocate sync_io");
		return false;
	}

	where = sb * 512;
	r = lseek(fd, where, SEEK_SET);
	if (r < 0) {
		log_warn("unable to seek to position %llu", (unsigned long long) where);
		return false;
	}

	while (len) {
		do {
			if (d == DIR_READ)
				r = read(fd, data, len);
			else
				r = write(fd, data, len);

		} while ((r < 0) && ((r == EINTR) || (r == EAGAIN)));

		if (r < 0) {
			log_warn("io failed %d", r);
			return false;
		}

		len -= r;
		data += r;
	}

	if (len) {
		log_warn("short io %u bytes remaining", (unsigned) len);
		return false;
	}

	dm_list_add(&e->complete, &io->list);
	io->context = context;

	return true;
}

static bool _sync_wait(struct io_engine *ioe, io_complete_fn fn)
{
	struct sync_io *io, *tmp;
	struct sync_engine *e = _to_sync(ioe);

	dm_list_iterate_items_safe(io, tmp, &e->complete) {
		fn(io->context, 0);
		dm_list_del(&io->list);
		free(io);
	}

	return true;
}

static unsigned _sync_max_io(struct io_engine *e)
{
	return 1;
}

struct io_engine *create_sync_io_engine(void)
{
	struct sync_engine *e = malloc(sizeof(*e));

	if (!e)
		return NULL;

	e->e.destroy = _sync_destroy;
	e->e.open = _sync_open;
	e->e.close = _async_close;
	e->e.issue = _sync_issue;
	e->e.wait = _sync_wait;
	e->e.max_io = _sync_max_io;
	e->e.get_size = _common_get_size;
	e->e.get_block_sizes = _common_get_block_sizes;

	dm_list_init(&e->complete);
	return &e->e;
}

//----------------------------------------------------------------

#define MIN_BLOCKS 16
#define WRITEBACK_LOW_THRESHOLD_PERCENT 33
#define WRITEBACK_HIGH_THRESHOLD_PERCENT 66

static void *_alloc_aligned(size_t len, size_t alignment)
{
	void *result = NULL;
	int r = posix_memalign(&result, alignment, len);
	if (r)
		return NULL;

	return result;
}

//----------------------------------------------------------------

static bool _test_flags(struct block *b, unsigned bits)
{
	return (b->flags & bits) != 0;
}

static void _set_flags(struct block *b, unsigned bits)
{
	b->flags |= bits;
}

static void _clear_flags(struct block *b, unsigned bits)
{
	b->flags &= ~bits;
}

//----------------------------------------------------------------

enum block_flags {
	BF_IO_PENDING = (1 << 0),
};

// We have a little wrapper io_dev struct that remembers the
// flags that it was accessed with.  This allows us to deny
// write access, even if the internal dev is actually opened
// read/write.
struct io_dev {
	struct io_dev_internal *idev;
	unsigned flags;
};

struct io_dev_internal {
	int fd;

	// Because the files get reopened when upgrading from read_only
	// to read/write, we can't use the fd as an index into the radix
	// trees.  So we use this index to uniquely identify the dev.
	unsigned index;

	struct io_manager *iom;
	char *path;

	// These are the flags that actually used to open the dev.
	unsigned flags;
	bool opened_o_direct;

	// Reopen uses this to check it's reopened the same device.
	bool is_device;
	dev_t dev;

	// The reference counts tracks users that are holding the dev, plus
	// all the blocks on that device that are currently in the iom.
	unsigned holders;
	unsigned blocks;

	// We cache these to avoid repeatedly issuing the ioctls
	bool got_block_sizes;
	unsigned physical_block_size;
	unsigned logical_block_size;

	struct dm_list lru;
};

struct io_manager {
	sector_t block_sectors;
	uint64_t block_mask;
	uint64_t page_sector_mask;

	// 1 bit set for every sector in a block.
	uint64_t sectors_mask;

	uint64_t nr_data_blocks;
	uint64_t nr_cache_blocks;
	unsigned max_io;
	unsigned max_cache_devs;
	bool use_o_direct;

	struct io_engine *engine;

	void *raw_data;
	struct block *raw_blocks;

	/*
	 * Lists that categorise the blocks.
	 */
	unsigned dev_index;
	unsigned nr_open;
	unsigned nr_locked;
	unsigned nr_dirty;

	// nr of _blocks_ that have io pending, rather than
	// nr of ios issued.
	unsigned nr_io_pending;

	struct dm_list free;
	struct dm_list errored;
	struct dm_list dirty;
	struct dm_list clean;
	struct dm_list io_pending;

	struct radix_tree *rtree;

	/*
	 * Statistics
	 */
	unsigned read_hits;
	unsigned read_misses;
	unsigned write_zeroes;
	unsigned write_hits;
	unsigned write_misses;
	unsigned prefetches;

	struct radix_tree *dev_tree;
	struct dm_list dev_lru;
};

//----------------------------------------------------------------

static void _dec_nr_open(struct io_manager *iom)
{
	assert(iom->nr_open);
	iom->nr_open--;
}

static void _inc_nr_open(struct io_manager *iom)
{
	iom->nr_open++;
}

static void _free_dev(struct io_manager *iom, struct io_dev_internal *dev)
{
	assert(!dev->holders);
	assert(!dev->blocks);
	iom->engine->close(iom->engine, dev->fd);
	_dec_nr_open(iom);
	dm_list_del(&dev->lru);
	free(dev->path);
	free(dev);
}

static void _dev_dtr(void *context, union radix_value v)
{
        _free_dev(context, v.ptr);
}

static void _inc_holders(struct io_dev_internal *dev)
{
	dev->holders++;
}

static void _inc_blocks(struct io_dev_internal *dev)
{
	dev->blocks++;
}

static void _dev_maybe_close(struct io_dev_internal *dev)
{
	if (dev->holders || dev->blocks)
		return;

	if (!radix_tree_remove(dev->iom->dev_tree,
                                  (uint8_t *) dev->path,
                                  (uint8_t *) dev->path + strlen(dev->path)))
		log_error("couldn't remove io dev: %s", dev->path);
}

static void _dec_holders(struct io_dev_internal *dev)
{
	if (!dev->holders)
		log_error("internal error: holders refcount already at zero (%s)", dev->path);
	else {
		dev->holders--;
		_dev_maybe_close(dev);
	}
}

static void _dec_blocks(struct io_dev_internal *dev)
{
	if (!dev->blocks)
		log_error("internal error: blocks refcount already at zero (%s)", dev->path);
	else {
		dev->blocks--;
		_dev_maybe_close(dev);
	}
}

static void _block_dtr(void *context, union radix_value v)
{
	struct block *b = v.ptr;
	_dec_blocks(b->dev);
}

static bool _eflags(unsigned flags, unsigned flag)
{
	return flags & flag;
}

static bool _invalidate_dev(struct io_manager *iom, struct io_dev_internal *dev);

static bool _is_block_device(int fd, dev_t *mm)
{
	int r;
	struct stat info;

	r = fstat(fd, &info);

	if (!r && ((info.st_mode & S_IFMT) == S_IFBLK)) {
		*mm = info.st_rdev;
		return true;
	}

	return false;
}

static void _evict_lru_dev(struct io_manager *iom)
{
	struct io_dev_internal *dev;

	// We need to find the least recently used device, that isn't held.
	dm_list_iterate_items_gen(dev, &iom->dev_lru, lru) {
		if (!dev->holders) {
			// we have a winner
			_invalidate_dev(iom, dev);
			return;
		}
	}
}

static struct io_dev_internal *_new_dev(struct io_manager *iom,
                                        const char *path, unsigned flags)
{
	union radix_value v;
	struct io_dev_internal *dev = malloc(sizeof(*dev));

	if (iom->nr_open >= iom->max_cache_devs) {
		_evict_lru_dev(iom);

		if (iom->nr_open >= iom->max_cache_devs) {
			log_error("Couldn't open io_dev(%s): Too many devices/files open.", path);
			free(dev);
			return NULL;
		}
	}

	dev->fd = iom->engine->open(iom->engine, path, flags, iom->use_o_direct);
	if (dev->fd < 0) {
		log_error("couldn't open io_dev(%s)", path);
		free(dev);
		return NULL;
	}
	dev->index = iom->dev_index++;

	dev->path = strdup(path);
	if (!dev->path) {
		log_error("couldn't copy path when getting new device (%s)", path);
		iom->engine->close(iom->engine, dev->fd);
		free(dev);
		return NULL;
	}
	dev->flags = flags;
	dev->is_device = _is_block_device(dev->fd, &dev->dev);

	dev->iom = iom;
	dev->holders = 1;
	dev->blocks = 0;
	dev->opened_o_direct = iom->use_o_direct;

	v.ptr = dev;
	if (!radix_tree_insert(iom->dev_tree, (uint8_t *) path, (uint8_t *) (path + strlen(path)), v)) {
		log_error("couldn't insert device into radix tree: %s", path);
		iom->engine->close(iom->engine, dev->fd);
		free(dev->path);
		free(dev);
		dev = NULL;
	} else {
		dm_list_add(&iom->dev_lru, &dev->lru);
		_inc_nr_open(iom);
	}

	return dev;
}


static bool _need_upgrade_dev(struct io_dev_internal *dev, unsigned flags)
{
	return (_eflags(flags, EF_EXCL) && !_eflags(dev->flags, EF_EXCL)) ||
	       (_eflags(dev->flags, EF_READ_ONLY) && !_eflags(flags, EF_READ_ONLY));
}

static bool _check_same_device(struct io_dev_internal *dev, int fd, const char *path)
{
	dev_t major_minor;
	bool is_dev = _is_block_device(fd, &major_minor);
	if (dev->is_device) {
		if (!is_dev) {
			log_error("error reopening io_dev(%s), path is no longer a device", path);
			_dec_holders(dev);
			return false;
		}

		if (dev->dev != major_minor) {
			log_error("error reopening io_dev(%s), device node changed:"
                                  "(major %u, minor %u) -> (major %u, minor %u)",
                                  path, major(dev->dev), minor(dev->dev),
                                  major(major_minor), minor(major_minor));
			_dec_holders(dev);
			return false;
		}
	} else if (is_dev) {
		log_error("error reopening io_dev(%s), originally pointed to a regular file,"
                          "now points to a device",
                          path);
		_dec_holders(dev);
		return false;
	}

	return true;
}

static struct io_dev_internal *_upgrade_dev(struct io_manager *iom, const char *path,
		                            struct io_dev_internal *dev, unsigned flags)
{
	if (_eflags(flags, EF_EXCL) || _eflags(dev->flags, EF_EXCL)) {
		// Slow path; invalidate everything,
		// close the old fd and start again.
		if (dev->holders != 1) {
			log_error("you can't update an io dev to exclusive with a concurrent holder (%s)",
                                  path);
			_dec_holders(dev);
			return NULL;
		}

		_invalidate_dev(iom, dev);
		_dec_holders(dev);

		return _new_dev(iom, path, flags);

	} else {
		// Fast path
		int fd;

		fd = iom->engine->open(iom->engine, path, flags, iom->use_o_direct);
		if ((fd < 0) || !_check_same_device(dev, fd, path)) {
			log_error("couldn't reopen io_dev(%s)", path);
			_dec_holders(dev);
			return NULL;
		}

		iom->engine->close(iom->engine, dev->fd);

		dev->fd = fd;
		dev->flags = flags;
		dev->opened_o_direct = iom->use_o_direct;
	}

	return dev;
}

static struct io_dev_internal *_get_dev(struct io_manager *iom,
                                        const char *path, unsigned flags)
{
	union radix_value v;
	struct io_dev_internal *dev = NULL;

	if (radix_tree_lookup(iom->dev_tree, (uint8_t *) path, (uint8_t *) (path + strlen(path)), &v)) {
		dev = v.ptr;
		_inc_holders(dev);
		dm_list_del(&dev->lru);
		dm_list_add(&iom->dev_lru, &dev->lru);

		if (_need_upgrade_dev(dev, flags))
			dev = _upgrade_dev(iom, path, dev, flags);

	} else
		dev = _new_dev(iom, path, flags);

	return dev;
}

struct io_dev *io_get_dev(struct io_manager *iom, const char *path, unsigned flags)
{
	struct io_dev *dev;

	dev = malloc(sizeof(*dev));
	if (!dev)
		return NULL;

	dev->idev = _get_dev(iom, path, flags);
	if (!dev->idev) {
		free(dev);
		return NULL;
	}
	dev->flags = flags;

	return dev;
}

void io_put_dev(struct io_dev *dev)
{
	_dec_holders(dev->idev);
	free(dev);
}

//----------------------------------------------------------------

struct key_parts {
	uint32_t dev_index;
	uint64_t b;
} __attribute__ ((packed));

union key {
	struct key_parts parts;
	uint8_t bytes[12];
} __attribute__ ((packed));

static struct block *_block_lookup(struct io_manager *iom, struct io_dev_internal *dev,
                                   uint64_t i)
{
	union key k;
	union radix_value v;

	k.parts.dev_index = dev->index;
	k.parts.b = i;

	if (radix_tree_lookup(iom->rtree, k.bytes, k.bytes + sizeof(k.bytes), &v))
		return v.ptr;

	return NULL;
}

static bool _block_insert(struct block *b)
{
	union key k;
	union radix_value v;

	k.parts.dev_index = b->dev->index;
	k.parts.b = b->index;
	v.ptr = b;

	return radix_tree_insert(b->iom->rtree, k.bytes, k.bytes + sizeof(k.bytes), v);
}

static void _block_remove(struct block *b)
{
	union key k;

	k.parts.dev_index = b->dev->index;
	k.parts.b = b->index;

	radix_tree_remove(b->iom->rtree, k.bytes, k.bytes + sizeof(k.bytes));
}

//----------------------------------------------------------------

static bool _init_free_list(struct io_manager *iom, unsigned count, unsigned pgsize)
{
	unsigned i;
	size_t block_size = iom->block_sectors << SECTOR_SHIFT;
	unsigned char *data =
		(unsigned char *) _alloc_aligned(count * block_size, pgsize);

	/* Allocate the data for each block.  We page align the data. */
	if (!data)
		return false;

	iom->raw_blocks = malloc(count * sizeof(*iom->raw_blocks));

	if (!iom->raw_blocks)
		free(iom->raw_data);

	iom->raw_data = data;

	for (i = 0; i < count; i++) {
		struct block *b = iom->raw_blocks + i;
		b->iom = iom;
		b->data = data + (block_size * i);
		dm_list_add(&iom->free, &b->list);
	}

	return true;
}

static void _exit_free_list(struct io_manager *iom)
{
	free(iom->raw_data);
	free(iom->raw_blocks);
}

static struct block *_alloc_block(struct io_manager *iom)
{
	if (dm_list_empty(&iom->free))
		return NULL;

	return dm_list_struct_base(_list_pop(&iom->free), struct block, list);
}

static void _free_block(struct block *b)
{
	dm_list_add(&b->iom->free, &b->list);
}

/*----------------------------------------------------------------
 * Clean/dirty list management.
 * Always use these methods to ensure nr_dirty_ is correct.
 *--------------------------------------------------------------*/

static void _unlink_block(struct block *b)
{
	if (b->dirty_bits)
		b->iom->nr_dirty--;

	dm_list_del(&b->list);
}

static void _link_block(struct block *b)
{
	struct io_manager *iom = b->iom;

	if (b->dirty_bits) {
		dm_list_add(&iom->dirty, &b->list);
		iom->nr_dirty++;
	} else
		dm_list_add(&iom->clean, &b->list);
}

static void _relink(struct block *b)
{
	_unlink_block(b);
	_link_block(b);
}

/*----------------------------------------------------------------
 * Low level IO handling
 *
 * We cannot have two concurrent writes on the same block.
 * eg, background writeback, put with dirty, flush?
 *
 * To avoid this we introduce some restrictions:
 *
 * i)  A held block can never be written back.
 * ii) You cannot get a block until writeback has completed.
 *
 *--------------------------------------------------------------*/

static void _complete_io(void *context, int err)
{
	struct block *b = context;
	struct io_manager *iom = b->iom;

	assert(b->io_count);

	// accumulate the error
	if (err && !b->error)
		b->error = err;

	if (!--b->io_count) {
		_clear_flags(b, BF_IO_PENDING);
		iom->nr_io_pending--;

		// b is on the io_pending list, so we don't want to use
		// unlink_block.  Which would incorrectly adjust nr_dirty.
		dm_list_del(&b->list);

		if (b->error) {
			dm_list_add(&iom->errored, &b->list);

		} else {
			b->dirty_bits = 0;
			_link_block(b);
		}
	}
}

static void _wait_all(struct io_manager *iom);
static bool _reopen_without_o_direct(struct io_manager *iom, struct io_dev_internal *dev)
{
	int fd;

	_wait_all(iom);

	fd = iom->engine->open(iom->engine, dev->path, dev->flags, false);
	if (fd < 0)
		return false;

	if (!_check_same_device(dev, fd, dev->path)) {
		iom->engine->close(iom->engine, fd);
		return false;
	}

	iom->engine->close(iom->engine, dev->fd);
	dev->fd = fd;
	dev->opened_o_direct = false;

	return true;
}

static void _issue_sectors(struct block *b, sector_t sb, sector_t se)
{
	struct io_manager *iom = b->iom;
	sector_t base = b->index * iom->block_sectors;

	b->io_count++;
	if (!iom->engine->issue(iom->engine, b->io_dir, b->dev->fd,
                                base + sb, base + se,
                                ((uint8_t *) b->data) + (sb << SECTOR_SHIFT), b))
		_complete_io(b, -EIO);
}

static bool _test_bit(uint64_t bits, unsigned bit)
{
	return !!(bits & (1ull << bit));
}

static void _issue_partial_write(struct block *b)
{
	struct io_manager *iom = b->iom;
	unsigned sb = 0, se;

	while (sb < iom->block_sectors) {
		while ((sb < iom->block_sectors) && !_test_bit(b->dirty_bits, sb))
                       sb++;

		if (sb >= iom->block_sectors)
			break;

                se = sb + 1;

		while ((se < iom->block_sectors) && _test_bit(b->dirty_bits, se))
			se++;

		_issue_sectors(b, sb, se);
		sb = se;
	}
}

static void _issue_whole_block(struct block *b, enum dir d)
{
	_issue_sectors(b, 0, b->iom->block_sectors);
}

static bool _is_partial_write(struct io_manager *iom, struct block *b)
{
	return (b->io_dir == DIR_WRITE) && (b->dirty_bits != iom->block_mask);
}

// |b->list| should be valid (either pointing to itself, on one of the other
// lists.
static void _issue(struct block *b, enum dir d)
{
	bool fail = false;
	struct io_manager *iom = b->iom;

	if (_test_flags(b, BF_IO_PENDING))
		return;

	assert(b);
	assert(!b->io_count);

	b->io_dir = d;
	if (_is_partial_write(iom, b) && b->dev->opened_o_direct) {
		if (!_reopen_without_o_direct(iom, b->dev))
			fail = true;
	}

	_set_flags(b, BF_IO_PENDING);
	iom->nr_io_pending++;
	dm_list_move(&iom->io_pending, &b->list);

	if (fail)
		_complete_io(b, -EIO);

	else if (_is_partial_write(iom, b))
		_issue_partial_write(b);

	else
		_issue_whole_block(b, d);
}

static inline void _issue_read(struct block *b)
{
	_issue(b, DIR_READ);
}

static inline void _issue_write(struct block *b)
{
	b->error = 0;
	_issue(b, DIR_WRITE);
}

static bool _wait_io(struct io_manager *iom)
{
	return iom->engine->wait(iom->engine, _complete_io);
}

/*----------------------------------------------------------------
 * High level IO handling
 *--------------------------------------------------------------*/

static void _wait_all(struct io_manager *iom)
{
	while (!dm_list_empty(&iom->io_pending))
		_wait_io(iom);
}

static void _wait_specific(struct block *b)
{
	while (_test_flags(b, BF_IO_PENDING))
		_wait_io(b->iom);
}

static unsigned _writeback(struct io_manager *iom, unsigned count)
{
	unsigned actual = 0;
	struct block *b, *tmp;

	dm_list_iterate_items_gen_safe (b, tmp, &iom->dirty, list) {
		if (actual == count)
			break;

		// We can't writeback anything that's still in use.
		if (!b->ref_count) {
			_issue_write(b);
			actual++;
		}
	}

	return actual;
}

/*----------------------------------------------------------------
 * High level allocation
 *--------------------------------------------------------------*/

static struct block *_find_unused_clean_block(struct io_manager *iom)
{
	struct block *b;

	dm_list_iterate_items (b, &iom->clean) {
		if (!b->ref_count) {
			_unlink_block(b);
			_block_remove(b);
			return b;
		}
	}

	return NULL;
}

static struct block *_new_block(struct io_manager *iom, struct io_dev_internal *dev,
                                block_address i, bool can_wait)
{
	struct block *b;

	b = _alloc_block(iom);

	// FIXME: if there are no clean we should just writeback
	while (!b && !dm_list_empty(&iom->clean)) {
		b = _find_unused_clean_block(iom);
		if (!b) {
			if (can_wait) {
				if (dm_list_empty(&iom->io_pending))
					_writeback(iom, 16);  // FIXME: magic number
				_wait_io(iom);
			} else {
				log_error("io no new blocks for %s, index %u",
					  dev->path, (uint32_t) i);
				return NULL;
			}
		}
	}

	if (b) {
		dm_list_init(&b->list);
		b->flags = 0;
		b->dev = dev;
		b->index = i;
		b->ref_count = 0;
		b->error = 0;
		b->io_count = 0;
		b->dirty_bits = 0;

		if (!_block_insert(b)) {
			log_error("io unable to insert block in radix tree (OOM?)");
			_free_block(b);
			return NULL;
		}

		_inc_blocks(dev);
	}

	return b;
}

/*----------------------------------------------------------------
 * Block reference counting
 *--------------------------------------------------------------*/
static void _zero_block(struct block *b, uint64_t mask)
{
	struct io_manager *iom = b->iom;

	iom->write_zeroes++;

	if (mask == iom->block_mask)
		// Zero whole block
		memset(b->data, 0, b->iom->block_sectors << SECTOR_SHIFT);

	else {
		sector_t sb = 0, se;

		while (sb < iom->block_sectors) {
			while ((sb < iom->block_sectors) && !_test_bit(mask, sb))
	                       sb++;

			if (sb >= iom->block_sectors)
				break;

			se = sb + 1;

			while ((se < iom->block_sectors) && _test_bit(b->dirty_bits, se))
				se++;

			memset(((uint8_t *) b->data) + (sb << SECTOR_SHIFT), 0, (se - sb) << SECTOR_SHIFT);
			sb = se;
		}
	}
}

static void _hit(struct block *b, unsigned flags)
{
	struct io_manager *iom = b->iom;

	if (flags & (GF_ZERO | GF_DIRTY))
		iom->write_hits++;
	else
		iom->read_hits++;

	_relink(b);
}

static void _miss(struct io_manager *iom, unsigned flags)
{
	if (flags & (GF_ZERO | GF_DIRTY))
		iom->write_misses++;
	else
		iom->read_misses++;
}

static struct block *_lookup_or_read_block(struct io_manager *iom,
				  	   struct io_dev_internal *dev, block_address i,
					   unsigned flags, uint64_t mask)
{
	struct block *b = _block_lookup(iom, dev, i);

	if (b) {
		// FIXME: this is insufficient.  We need to also catch a read
		// lock of a write locked block.  Ref count needs to distinguish.
		if (b->ref_count && (flags & (GF_DIRTY | GF_ZERO))) {
			log_warn("concurrent write lock attempted");
			return NULL;
		}

		if (_test_flags(b, BF_IO_PENDING)) {
			_miss(iom, flags);
			_wait_specific(b);

		} else
			_hit(b, flags);

		_unlink_block(b);

		if (flags & GF_ZERO)
			_zero_block(b, mask);

	} else {
		_miss(iom, flags);

		b = _new_block(iom, dev, i, true);
		if (b) {
			if (flags & GF_ZERO)
				_zero_block(b, mask);

			else {
				_issue_read(b);
				_wait_specific(b);

				// we know the block is clean and unerrored.
				_unlink_block(b);
			}
		}
	}

	if (b) {
		if (flags & (GF_DIRTY | GF_ZERO))
			b->dirty_bits |= mask;

		_link_block(b);
		return b;
	}

	return NULL;
}

static void _preemptive_writeback(struct io_manager *iom)
{
	// FIXME: this ignores those blocks that are in the error state.  Track
	// nr_clean instead?
	unsigned nr_available = iom->nr_cache_blocks - (iom->nr_dirty - iom->nr_io_pending);
	if (nr_available < (WRITEBACK_LOW_THRESHOLD_PERCENT * iom->nr_cache_blocks / 100))
		_writeback(iom, (WRITEBACK_HIGH_THRESHOLD_PERCENT * iom->nr_cache_blocks / 100) - nr_available);

}

/*----------------------------------------------------------------
 * Public interface
 *--------------------------------------------------------------*/

// Restricted by the size of the dirty_bits for each block
#define MAX_BLOCK_SIZE 64

static bool _valid_block_size(sector_t block_sectors, unsigned pgsize)
{
	if (!block_sectors) {
		log_warn("io must have a non zero block size");
		return false;
	}

	if (block_sectors & ((pgsize >> SECTOR_SHIFT) - 1)) {
		log_warn("io block size must be a multiple of page size");
		return false;
	}

	if (block_sectors > MAX_BLOCK_SIZE) {
		log_warn("io block size must not be greater than %u",
                         MAX_BLOCK_SIZE);
		return false;
	}

	return true;
}

static uint64_t _calc_block_mask(sector_t nr_sectors)
{
	unsigned i;
	uint64_t r = 0;

	for (i = 0; i < nr_sectors; i++)
		r = (r << 1) | 0x1;

	return r;
}

struct io_manager *io_manager_create(sector_t block_sectors, unsigned nr_cache_blocks,
                                     unsigned max_cache_devs, struct io_engine *engine,
                                     bool use_o_direct)
{
	struct io_manager *iom;
	unsigned max_io = engine->max_io(engine);
	long pgsize = sysconf(_SC_PAGESIZE);

	if (!nr_cache_blocks) {
		log_warn("io must have at least one cache block");
		return NULL;
	}

	if (!_valid_block_size(block_sectors, pgsize))
		return NULL;

	iom = malloc(sizeof(*iom));
	if (!iom)
		return NULL;

	iom->block_sectors = block_sectors;
	iom->block_mask = _calc_block_mask(block_sectors);
	iom->nr_cache_blocks = nr_cache_blocks;
	iom->max_io = nr_cache_blocks < max_io ? nr_cache_blocks : max_io;
	iom->max_cache_devs = max_cache_devs;
	iom->use_o_direct = use_o_direct;
	iom->engine = engine;
	iom->dev_index = 0;
	iom->nr_open = 0;
	iom->nr_locked = 0;
	iom->nr_dirty = 0;
	iom->nr_io_pending = 0;

	dm_list_init(&iom->free);
	dm_list_init(&iom->errored);
	dm_list_init(&iom->dirty);
	dm_list_init(&iom->clean);
	dm_list_init(&iom->io_pending);

	iom->rtree = radix_tree_create(_block_dtr, iom);
	if (!iom->rtree) {
		iom->engine->destroy(iom->engine);
		free(iom);
		return NULL;
	}

	iom->read_hits = 0;
	iom->read_misses = 0;
	iom->write_zeroes = 0;
	iom->write_hits = 0;
	iom->write_misses = 0;
	iom->prefetches = 0;

	if (!_init_free_list(iom, nr_cache_blocks, pgsize)) {
		iom->engine->destroy(iom->engine);
		radix_tree_destroy(iom->rtree);
		free(iom);
		return NULL;
	}

	iom->dev_tree = radix_tree_create(_dev_dtr, iom);
	if (!iom->dev_tree) {
		_exit_free_list(iom);
		iom->engine->destroy(iom->engine);
		radix_tree_destroy(iom->rtree);
		free(iom);
		return NULL;
	}
	dm_list_init(&iom->dev_lru);

	iom->page_sector_mask = (sysconf(_SC_PAGESIZE) / 512) - 1;

	return iom;
}

//----------------------------------------------------------------

struct dev_iterator {
	bool chastised;
	struct radix_tree_iterator it;
};

static bool _check_dev(struct radix_tree_iterator *it,
			 uint8_t *kb, uint8_t *ke, union radix_value v)
{
	struct dev_iterator *dit = container_of(it, struct dev_iterator, it);
	struct io_dev_internal *dev = v.ptr;

	if (dev->holders) {
		if (!dit->chastised) {
			log_warn("Destroying an io_dev whilst still held (holders = %u)",
                                 dev->holders);
			dit->chastised = true;
		}

		log_warn("    %s", dev->path);
	}

	return true;
}

static void _check_for_holders(struct io_manager *iom)
{
	struct dev_iterator dit;

	dit.chastised = false;
	dit.it.visit = _check_dev;
	radix_tree_iterate(iom->dev_tree, NULL, NULL, &dit.it);
}

void io_manager_destroy(struct io_manager *iom)
{
	if (iom->nr_locked)
		log_warn("some blocks are still locked");

	_check_for_holders(iom);

	io_flush(iom);
	_wait_all(iom);

	radix_tree_destroy(iom->rtree);
	radix_tree_destroy(iom->dev_tree);
	_exit_free_list(iom);
	iom->engine->destroy(iom->engine);
	free(iom);
}

//----------------------------------------------------------------

sector_t io_block_sectors(struct io_manager *iom)
{
	return iom->block_sectors;
}

unsigned io_nr_cache_blocks(struct io_manager *iom)
{
	return iom->nr_cache_blocks;
}

unsigned io_max_cache_devs(struct io_manager *iom)
{
	return iom->max_cache_devs;
}

unsigned io_max_prefetches(struct io_manager *iom)
{
	return iom->max_io;
}

void io_prefetch_block(struct io_manager *iom, struct io_dev *dev, block_address i)
{
	struct block *b = _block_lookup(iom, dev->idev, i);

	if (!b) {
		if (iom->nr_io_pending < iom->max_io) {
			b = _new_block(iom, dev->idev, i, false);
			if (b) {
				iom->prefetches++;
				_issue_read(b);
			}
		}
	}
}

//----------------------------------------------------------------

static void _recycle_block(struct io_manager *iom, struct block *b)
{
	_unlink_block(b);
	_block_remove(b);
	_free_block(b);
}

static bool _access_allowed(struct io_dev *dev, unsigned get_flags)
{
	if ((get_flags & (GF_DIRTY | GF_ZERO)) && dev->flags & EF_READ_ONLY)
		return false;

	return true;
}

bool io_get_block_mask(struct io_manager *iom, struct io_dev *dev, block_address i,
	               unsigned flags, uint64_t mask, struct block **result)
{
	struct block *b;

	if (!_access_allowed(dev, flags))
		return false;

	b = _lookup_or_read_block(iom, dev->idev, i, flags, mask);
	if (b) {
		if (b->error) {
			if (b->io_dir == DIR_READ) {
				// Now we know the read failed we can just forget
				// about this block, since there's no dirty data to
				// be written back.
				_recycle_block(iom, b);
			}
			return false;
		}

		if (!b->ref_count)
			iom->nr_locked++;
		b->ref_count++;

		*result = b;
		return true;
	}

	*result = NULL;

	log_error("io failed to get block (%s, %llu)", dev->idev->path, (unsigned long long) i);
	return false;
}

bool io_get_block(struct io_manager *iom, struct io_dev *dev, block_address i,
	          unsigned flags, struct block **result)
{
	return io_get_block_mask(iom, dev, i, flags, iom->block_mask, result);
}

//----------------------------------------------------------------

static void _put_ref(struct block *b)
{
	if (!b->ref_count) {
		log_warn("ref count on io block already zero");
		return;
	}

	b->ref_count--;
	if (!b->ref_count)
		b->iom->nr_locked--;
}

void io_put_block(struct block *b)
{
	_put_ref(b);

	if (b->dirty_bits)
		_preemptive_writeback(b->iom);
}

//----------------------------------------------------------------

bool io_flush(struct io_manager *iom)
{
	// Only dirty data is on the errored list, since bad read blocks get
	// recycled straight away.  So we put these back on the dirty list, and
	// try and rewrite everything.
	dm_list_splice(&iom->dirty, &iom->errored);

	while (!dm_list_empty(&iom->dirty)) {
		struct block *b = dm_list_item(_list_pop(&iom->dirty), struct block);
		if (b->ref_count || _test_flags(b, BF_IO_PENDING)) {
			// The superblock may well be still locked.
			continue;
		}

		_issue_write(b);
	}

	_wait_all(iom);

	return dm_list_empty(&iom->errored);
}

//----------------------------------------------------------------
/*
 * You can safely call this with a NULL block.
 */
static bool _invalidate_block(struct io_manager *iom, struct block *b)
{
	if (!b)
		return true;

	if (_test_flags(b, BF_IO_PENDING))
		_wait_specific(b);

	if (b->ref_count) {
		log_warn("io_invalidate: block (%s, %llu) still held",
			 b->dev->path, (unsigned long long) b->index);
		return false;
	}

	if (b->dirty_bits) {
		_issue_write(b);
		_wait_specific(b);

		if (b->error)
			return false;
	}

	_recycle_block(iom, b);

	return true;
}

bool io_invalidate_block(struct io_manager *iom, struct io_dev *dev, block_address i)
{
	return _invalidate_block(iom, _block_lookup(iom, dev->idev, i));
}

//----------------------------------------------------------------

struct invalidate_iterator {
	bool success;
	struct radix_tree_iterator it;
};

static bool _writeback_v(struct radix_tree_iterator *it,
			 uint8_t *kb, uint8_t *ke, union radix_value v)
{
	struct block *b = v.ptr;

	if (b->dirty_bits)
		_issue_write(b);

	return true;
}

static bool _invalidate_v(struct radix_tree_iterator *it,
			  uint8_t *kb, uint8_t *ke, union radix_value v)
{
	struct block *b = v.ptr;
	struct invalidate_iterator *iit = container_of(it, struct invalidate_iterator, it);

	if (b->error || b->dirty_bits) {
		log_warn("io_invalidate: block (%s, %llu) still dirty",
			 b->dev->path, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	if (b->ref_count) {
		log_warn("io_invalidate: block (%s, %llu) still held",
			 b->dev->path, (unsigned long long) b->index);
		iit->success = false;
		return true;
	}

	_unlink_block(b);
	_free_block(b);

	// We can't remove the block from the radix tree yet because
	// we're in the middle of an iteration.
	return true;
}

static bool _invalidate_dev(struct io_manager *iom, struct io_dev_internal *dev)
{
	union key k;
	struct invalidate_iterator it;

	k.parts.dev_index = dev->index;

	it.it.visit = _writeback_v;
	radix_tree_iterate(iom->rtree, k.bytes, k.bytes + sizeof(k.parts.dev_index), &it.it);

	_wait_all(iom);

	it.success = true;
	it.it.visit = _invalidate_v;
	radix_tree_iterate(iom->rtree, k.bytes, k.bytes + sizeof(k.parts.dev_index), &it.it);
	radix_tree_remove_prefix(iom->rtree, k.bytes, k.bytes + sizeof(k.parts.dev_index));

	return it.success;
}

bool io_invalidate_dev(struct io_manager *iom, struct io_dev *dev)
{
	return _invalidate_dev(iom, dev->idev);
}

bool io_dev_size(struct io_dev *dev, uint64_t *size)
{
	struct io_manager *iom = dev->idev->iom;
	return iom->engine->get_size(iom->engine, dev->idev->path, dev->idev->fd, size);
}

bool io_dev_block_sizes(struct io_dev *dev, unsigned *physical_block_size,
					    unsigned *logical_block_size)
{
	struct io_manager *iom = dev->idev->iom;
	struct io_dev_internal *d = dev->idev;

	if (!d->got_block_sizes) {
		if (!iom->engine->get_block_sizes(iom->engine, d->path, d->fd,
                                                  &d->physical_block_size,
                                                  &d->logical_block_size))
			return false;
		else
			d->got_block_sizes = true;
	}

	*physical_block_size = dev->idev->physical_block_size;
	*logical_block_size = dev->idev->logical_block_size;
	return true;
}

bool io_invalidate_all(struct io_manager *iom)
{
	struct invalidate_iterator it;

	it.it.visit = _writeback_v;
	radix_tree_iterate(iom->rtree, NULL, NULL, &it.it);
	radix_tree_remove_prefix(iom->rtree, NULL, NULL);

	return it.success;
}

void *io_get_dev_context(struct io_dev *dev)
{
	return dev->idev;
}

int io_get_fd(void *dev_context)
{
	struct io_dev_internal *idev = dev_context;
	return idev->fd;
}

bool io_is_well_formed(struct io_manager *iom)
{
	if (!radix_tree_is_well_formed(iom->rtree)) {
		log_error("block tree is badly formed");
		return false;
	}

	if (!radix_tree_is_well_formed(iom->dev_tree)) {
		log_error("dev tree is badly formed");
		return false;
	}

	return true;
}

//----------------------------------------------------------------
