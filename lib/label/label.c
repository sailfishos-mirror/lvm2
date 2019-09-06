/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/label/label.h"
#include "lib/misc/crc.h"
#include "lib/mm/xlate.h"
#include "lib/cache/lvmcache.h"
#include "lib/device/io-manager.h"
#include "lib/commands/toolcontext.h"
#include "lib/activate/activate.h"
#include "lib/label/hints.h"
#include "lib/metadata/metadata.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/resource.h>

int io_data_ready;

static uint64_t _current_io_size_bytes;

static int _get_dev(struct device *dev, unsigned flags)
{
	if (dev->iodev) {
		if (flags == dev->iom_flags)
			return 1;

		/* currently writable, want to read */
		if ((flags & EF_READ_ONLY) && !dev->iom_flags)
			return 1;

		/* currently excl, want to read */
		if ((flags & EF_READ_ONLY) && (dev->iom_flags & EF_EXCL))
			return 1;

		/* currently excl, want to write */
		if (!flags && (dev->iom_flags & EF_EXCL))
			return 1;

		/* currently readonly, want to write */
		if (!flags && (dev->iom_flags & EF_READ_ONLY)) {
			log_print("dev reopen for writing %s", dev_name(dev));
			io_put_dev(dev->iodev);
			dev->iom_flags = 0;
			goto get;
		}

		/* currently non-excl, want excl */
		if ((flags & EF_EXCL) && !(dev->iom_flags & EF_EXCL)) {
			log_print("dev reopen excl %s", dev_name(dev));
			io_put_dev(dev->iodev);
			dev->iom_flags = 0;
			goto get;
		}

		/* Can this happen? */
		log_print("dev reopen flags %x iom_flags %x %s", flags, dev->iom_flags, dev_name(dev));
		io_put_dev(dev->iodev);
		dev->iom_flags = 0;
	}
get:
	dev->iodev = io_get_dev(lvm_iom, dev_name(dev), flags);

	if (!dev->iodev) {
		log_error("No io device available %s", dev_name(dev));
		return 0;
	}

	dev->iom_flags = flags;

	return 1;
}

static void _put_dev(struct device *dev)
{
	if (!dev->iodev) {
		log_error("put_dev no iodev %s", dev_name(dev));
		return;
	}

	io_put_dev(dev->iodev);
	dev->iodev = NULL;
	dev->iom_flags = 0;
}

/*
 * Internal labeller struct.
 */
struct labeller_i {
	struct dm_list list;

	struct labeller *l;
	char name[0];
};

static struct dm_list _labellers;

static struct labeller_i *_alloc_li(const char *name, struct labeller *l)
{
	struct labeller_i *li;
	size_t len;

	len = sizeof(*li) + strlen(name) + 1;

	if (!(li = malloc(len))) {
		log_error("Couldn't allocate memory for labeller list object.");
		return NULL;
	}

	li->l = l;
	strcpy(li->name, name);

	return li;
}

int label_init(void)
{
	dm_list_init(&_labellers);
	return 1;
}

void label_exit(void)
{
	struct labeller_i *li, *tli;

	dm_list_iterate_items_safe(li, tli, &_labellers) {
		dm_list_del(&li->list);
		li->l->ops->destroy(li->l);
		free(li);
	}

	dm_list_init(&_labellers);
}

int label_register_handler(struct labeller *handler)
{
	struct labeller_i *li;

	if (!(li = _alloc_li(handler->fmt->name, handler)))
		return_0;

	dm_list_add(&_labellers, &li->list);
	return 1;
}

struct labeller *label_get_handler(const char *name)
{
	struct labeller_i *li;

	dm_list_iterate_items(li, &_labellers)
		if (!strcmp(li->name, name))
			return li->l;

	return NULL;
}

/* FIXME Also wipe associated metadata area headers? */
int label_remove(struct device *dev)
{
	char readbuf[LABEL_SIZE] __attribute__((aligned(8)));
	int r = 1;
	uint64_t sector;
	int wipe;
	struct labeller_i *li;
	struct label_header *lh;
	struct lvmcache_info *info;

	log_very_verbose("Scanning for labels to wipe from %s", dev_name(dev));

	if (!_get_dev(dev, EF_EXCL)) {
		log_error("Failed to open device %s", dev_name(dev));
		return 0;
	}

	/* Scan first few sectors for anything looking like a label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {

		memset(readbuf, 0, sizeof(readbuf));

		if (!dev_read_bytes(dev, sector << SECTOR_SHIFT, LABEL_SIZE, readbuf)) {
			log_error("Failed to read label from %s sector %llu",
				  dev_name(dev), (unsigned long long)sector);
			continue;
		}

		lh = (struct label_header *)readbuf;

		wipe = 0;

		if (!memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
			if (xlate64(lh->sector_xl) == sector)
				wipe = 1;
		} else {
			dm_list_iterate_items(li, &_labellers) {
				if (li->l->ops->can_handle(li->l, (char *)lh, sector)) {
					wipe = 1;
					break;
				}
			}
		}

		if (wipe) {
			log_very_verbose("%s: Wiping label at sector %llu",
					 dev_name(dev), (unsigned long long)sector);

			if (!dev_write_zeros(dev, sector << SECTOR_SHIFT, LABEL_SIZE)) {
				log_error("Failed to remove label from %s at sector %llu",
					  dev_name(dev), (unsigned long long)sector);
				r = 0;
			} else {
				/* Also remove the PV record from cache. */
				info = lvmcache_info_from_pvid(dev->pvid, dev, 0);
				if (info)
					lvmcache_del(info);
			}
		}
	}

	_put_dev(dev);
	return r;
}

/* Caller may need to use label_get_handler to create label struct! */
int label_write(struct device *dev, struct label *label)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label_header *lh = (struct label_header *) buf;
	uint64_t offset;
	int r = 1;

	if (!label->labeller->ops->write) {
		log_error("Label handler does not support label writes");
		return 0;
	}

	if ((LABEL_SIZE + (label->sector << SECTOR_SHIFT)) > LABEL_SCAN_SIZE) {
		log_error("Label sector %" PRIu64 " beyond range (%ld)",
			  label->sector, LABEL_SCAN_SECTORS);
		return 0;
	}

	memset(buf, 0, LABEL_SIZE);

	memcpy(lh->id, LABEL_ID, sizeof(lh->id));
	lh->sector_xl = xlate64(label->sector);
	lh->offset_xl = xlate32(sizeof(*lh));

	if (!(label->labeller->ops->write)(label, buf))
		return_0;

	lh->crc_xl = xlate32(calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				      ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)));

	log_very_verbose("%s: Writing label to sector %" PRIu64 " with stored offset %"
			 PRIu32 ".", dev_name(dev), label->sector,
			 xlate32(lh->offset_xl));

	if (!_get_dev(dev, 0)) {
		log_error("Failed to open device %s", dev_name(dev));
		return 0;
	}

	offset = label->sector << SECTOR_SHIFT;

	if (!dev_write_bytes(dev, offset, LABEL_SIZE, buf)) {
		log_debug_devs("Failed to write label to %s", dev_name(dev));
		r = 0;
	}

	_put_dev(dev);
	return r;
}

void label_destroy(struct label *label)
{
	label->labeller->ops->destroy_label(label->labeller, label);
	free(label);
}

struct label *label_create(struct labeller *labeller)
{
	struct label *label;

	if (!(label = zalloc(sizeof(*label)))) {
		log_error("label allocaction failed");
		return NULL;
	}

	label->labeller = labeller;

	labeller->ops->initialise_label(labeller, label);

	return label;
}


/* global variable for accessing the io-manager populated by label scan */
struct io_manager *lvm_iom;

#define IOM_BLOCK_SIZE_IN_SECTORS 64 /* 64*512 = 32K */

static struct labeller *_find_lvm_header(struct device *dev,
				   char *scan_buf,
				   uint32_t scan_buf_sectors,
				   char *label_buf,
				   uint64_t *label_sector,
				   uint64_t block_sector,
				   uint64_t start_sector)
{
	struct labeller_i *li;
	struct labeller *labeller_ret = NULL;
	struct label_header *lh;
	uint64_t sector;
	int found = 0;

	/*
	 * Find which sector in scan_buf starts with a valid label,
	 * and copy it into label_buf.
	 */

	for (sector = start_sector; sector < start_sector + LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {

		/*
		 * The scan_buf passed in is a iom block, which is
		 * IOM_BLOCK_SIZE_IN_SECTORS large.  So if start_sector is
		 * one of the last couple sectors in that buffer, we need to
		 * break early.
		 */
		if (sector >= scan_buf_sectors)
			break;

		lh = (struct label_header *) (scan_buf + (sector << SECTOR_SHIFT));

		if (!memcmp(lh->id, LABEL_ID, sizeof(lh->id))) {
			if (found) {
				log_error("Ignoring additional label on %s at sector %llu",
					  dev_name(dev), (unsigned long long)(block_sector + sector));
			}
			if (xlate64(lh->sector_xl) != sector) {
				log_warn("%s: Label for sector %llu found at sector %llu - ignoring.",
					 dev_name(dev),
					 (unsigned long long)xlate64(lh->sector_xl),
					 (unsigned long long)(block_sector + sector));
				continue;
			}
			if (calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl,
				     LABEL_SIZE - ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)) != xlate32(lh->crc_xl)) {
				log_very_verbose("Label checksum incorrect on %s - ignoring", dev_name(dev));
				continue;
			}
			if (found)
				continue;
		}

		dm_list_iterate_items(li, &_labellers) {
			if (li->l->ops->can_handle(li->l, (char *) lh, block_sector + sector)) {
				log_very_verbose("%s: %s label detected at sector %llu", 
						 dev_name(dev), li->name,
						 (unsigned long long)(block_sector + sector));
				if (found) {
					log_error("Ignoring additional label on %s at sector %llu",
						  dev_name(dev),
						  (unsigned long long)(block_sector + sector));
					continue;
				}

				labeller_ret = li->l;
				found = 1;

				memcpy(label_buf, lh, LABEL_SIZE);
				if (label_sector)
					*label_sector = block_sector + sector;
				break;
			}
		}
	}

	return labeller_ret;
}

/*
 * Process/parse the headers from the data read from a device.
 * Populates lvmcache with device / mda locations / vgname
 * so that vg_read(vgname) will know which devices/locations
 * to read metadata from.
 *
 * If during processing, headers/metadata are found to be needed
 * beyond the range of the scanned block, then additional reads
 * are performed in the processing functions to get that data.
 */
static int _process_block(struct cmd_context *cmd, struct dev_filter *f,
			  struct device *dev, struct block *bb,
			  uint64_t block_sector, uint64_t start_sector,
			  int *is_lvm_device)
{
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct labeller *labeller;
	uint64_t sector = 0;
	int is_duplicate = 0;
	int ret = 0;
	int pass;

	dev->flags &= ~DEV_SCAN_FOUND_LABEL;

	/*
	 * The device may have signatures that exclude it from being processed.
	 * If filters were applied before iom data was available, some
	 * filters may have deferred their check until the point where iom
	 * data had been read (here).  They set this flag to indicate that the
	 * filters should be retested now that data from the device is ready.
	 */
	if (f && (dev->flags & DEV_FILTER_AFTER_SCAN)) {
		dev->flags &= ~DEV_FILTER_AFTER_SCAN;

		log_debug_devs("Scan filtering %s", dev_name(dev));
		
		pass = f->passes_filter(cmd, f, dev, NULL);

		if ((pass == -EAGAIN) || (dev->flags & DEV_FILTER_AFTER_SCAN)) {
			/* Shouldn't happen */
			dev->flags &= ~DEV_FILTER_OUT_SCAN;
			log_debug_devs("Scan filter should not be deferred %s", dev_name(dev));
			pass = 1;
		}

		if (!pass) {
			log_very_verbose("%s: Not processing filtered", dev_name(dev));
			dev->flags |= DEV_FILTER_OUT_SCAN;
			*is_lvm_device = 0;
			goto_out;
		}
	}

	/*
	 * Finds the data sector containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 *
	 * FIXME: we don't need to copy one sector from bb->data into label_buf,
	 * we can just point label_buf at one sector in ld->buf.
	 */
	if (!(labeller = _find_lvm_header(dev, bb->data, IOM_BLOCK_SIZE_IN_SECTORS, label_buf, &sector, block_sector, start_sector))) {

		/*
		 * Non-PVs exit here
		 *
		 * FIXME: check for PVs with errors that also exit here!
		 * i.e. this code cannot distinguish between a non-lvm
		 * device an an lvm device with errors.
		 */

		log_very_verbose("%s: No lvm label detected", dev_name(dev));

		lvmcache_del_dev(dev); /* FIXME: if this is needed, fix it. */

		*is_lvm_device = 0;
		goto_out;
	}

	dev->flags |= DEV_SCAN_FOUND_LABEL;
	*is_lvm_device = 1;

	/*
	 * This is the point where the scanning code dives into the rest of
	 * lvm.  ops->read() is _text_read() which reads the pv_header, mda
	 * locations, and metadata text.  All of the info it finds about the PV
	 * and VG is stashed in lvmcache which saves it in the form of
	 * info/vginfo structs.  That lvmcache info is used later when the
	 * command wants to read the VG to do something to it.
	 */
	ret = labeller->ops->read(labeller, dev, label_buf, sector, &is_duplicate);

	if (!ret) {
		if (is_duplicate) {
			/*
			 * _text_read() called lvmcache_add() which found an
			 * existing info struct for this PVID but for a
			 * different dev.  lvmcache_add() did not add an info
			 * struct for this dev, but added this dev to the list
			 * of duplicate devs.
			 */
			log_debug("label scan found duplicate PVID %s on %s", dev->pvid, dev_name(dev));
		} else {
			/*
			 * Leave the info in lvmcache because the device is
			 * present and can still be used even if it has
			 * metadata that we can't process (we can get metadata
			 * from another PV/mda.) _text_read only saves mdas
			 * with good metadata in lvmcache (this includes old
			 * metadata), and if a PV has no mdas with good
			 * metadata, then the info for the PV will be in
			 * lvmcache with empty info->mdas, and it will behave
			 * like a PV with no mdas (a common configuration.)
			 */
			log_warn("WARNING: scan failed to get metadata summary from %s PVID %s", dev_name(dev), dev->pvid);
		}
	}
 out:
	return ret;
}

/*
 * Read or reread label/metadata from selected devs.
 *
 * Reads and looks at label_header, pv_header, pv_header_extension,
 * mda_header, raw_locns, vg metadata from each device.
 *
 * Effect is populating lvmcache with latest info/vginfo (PV/VG) data
 * from the devs.  If a scanned device does not have a label_header,
 * its info is removed from lvmcache.
 */

static int _scan_list(struct cmd_context *cmd, struct dev_filter *f,
		      struct dm_list *devs, int *failed)
{
	struct dm_list wait_devs;
	struct dm_list done_devs;
	struct dm_list reopen_devs;
	struct device_list *devl, *devl2;
	struct block *bb;
	int scan_read_errors = 0;
	int scan_process_errors = 0;
	int scan_failed_count = 0;
	int rem_prefetches;
	int submit_count;
	int scan_failed;
	int is_lvm_device;
	int error;
	int ret;

	dm_list_init(&wait_devs);
	dm_list_init(&done_devs);
	dm_list_init(&reopen_devs);

	log_debug_devs("Scanning %d devices for VG info", dm_list_size(devs));

 scan_more:
	rem_prefetches = io_max_prefetches(lvm_iom);
	submit_count = 0;

	dm_list_iterate_items_safe(devl, devl2, devs) {

		/*
		 * If we prefetch more devs than blocks in the cache, then the
		 * cache will wait for earlier reads to complete, toss the
		 * results, and reuse those blocks before we've had a chance to
		 * use them.  So, prefetch as many as are available, wait for
		 * and process them, then repeat.
		 */
		if (!rem_prefetches)
			break;

		if (!_get_dev(devl->dev, EF_READ_ONLY))
			break;

		/*
		 * Prefetch the first block of the disk which holds the label
		 * and pv header, the mda header, and some the metadata text
		 * (if current metadata text is further into the metadata area,
		 * it will not be in this block and will require reading another
		 * block or more later.)
		 */
		io_prefetch_block(lvm_iom, devl->dev->iodev, 0);

		rem_prefetches--;
		submit_count++;

		dm_list_del(&devl->list);
		dm_list_add(&wait_devs, &devl->list);
	}

	log_debug_devs("Scanning submitted %d reads", submit_count);

	dm_list_iterate_items_safe(devl, devl2, &wait_devs) {
		bb = NULL;
		error = 0;
		scan_failed = 0;
		is_lvm_device = 0;

		if (!io_get_block(lvm_iom, devl->dev->iodev, 0, 0, &bb)) {
			log_debug_devs("Scan failed to read %s error %d.", dev_name(devl->dev), error);
			scan_failed = 1;
			scan_read_errors++;
			scan_failed_count++;
			lvmcache_del_dev(devl->dev);
		} else {
			log_debug_devs("Processing data from device %s %d:%d block %p",
				       dev_name(devl->dev),
				       (int)MAJOR(devl->dev->dev),
				       (int)MINOR(devl->dev->dev),
				       bb);

			ret = _process_block(cmd, f, devl->dev, bb, 0, 0, &is_lvm_device);

			if (!ret && is_lvm_device) {
				log_debug_devs("Scan failed to process %s", dev_name(devl->dev));
				scan_failed = 1;
				scan_process_errors++;
				scan_failed_count++;
			}
		}

		if (bb)
			io_put_block(bb);

		/*
		 * If iom failed to read the block, or the device does not
		 * belong to lvm, then drop it from iom.
		 */
		if (scan_failed || !is_lvm_device)
			io_invalidate_dev(lvm_iom, devl->dev->iodev);

		/*
		 * Allow io manager to drop this dev (close the fd and
		 * invalidate the cached block) if needed due to a full cache.
		 * If the common case, the cache should not be full and fds do
		 * not reach the max, so the dev will remain open in iomanager
		 * and the block we've read will remain cached, and when
		 * vg_read() comes to reading the metadata again, no new open
		 * or read will be needed.  In the uncommon case, vg_read()
		 * will trigger a new open() and rereading the data from disk.
		 */
		io_put_dev(devl->dev->iodev);
		devl->dev->iodev = NULL;

		dm_list_del(&devl->list);
		dm_list_add(&done_devs, &devl->list);
	}

	if (!dm_list_empty(devs))
		goto scan_more;

	log_debug_devs("Scanned devices: read errors %d process errors %d failed %d",
			scan_read_errors, scan_process_errors, scan_failed_count);

	if (failed)
		*failed = scan_failed_count;

	dm_list_splice(devs, &done_devs);

	return 1;
}

/*
 * We don't know ahead of time if we will find some VG metadata 
 * that is larger than the total size of the iom, which would
 * prevent us from reading/writing the VG since we do not dynamically
 * increase the iom size when we find it's too small.  In these
 * cases the user would need to set io_memory_size to be larger
 * than the max VG metadata size (lvm does not impose any limit on
 * the metadata size.)
 */

#define MIN_IOM_BLOCKS 32    /* 4MB (128 * 32KB) */
#define MAX_IOM_BLOCKS 16384 /* 512MB (16384 * 32KB) */

#define IOM_MAX_DEVS 4096

static int _setup_io_manager(void)
{
	struct io_engine *ioe = NULL;
	int iomem_kb = io_memory_size();
	int block_size_kb = (IOM_BLOCK_SIZE_IN_SECTORS * 512) / 1024;
	int cache_blocks;

	cache_blocks = iomem_kb / block_size_kb;

	if (cache_blocks < MIN_IOM_BLOCKS)
		cache_blocks = MIN_IOM_BLOCKS;

	if (cache_blocks > MAX_IOM_BLOCKS)
		cache_blocks = MAX_IOM_BLOCKS;

	_current_io_size_bytes = cache_blocks * IOM_BLOCK_SIZE_IN_SECTORS * 512;

	if (use_aio()) {
		if (!(ioe = create_async_io_engine(true))) {
			log_warn("Failed to set up async io, using sync io.");
			init_use_aio(0);
		}
	}

	if (!ioe) {
		if (!(ioe = create_sync_io_engine(true))) {
			log_error("Failed to set up sync io.");
			return 0;
		}
	}

	if (!(lvm_iom = io_manager_create(IOM_BLOCK_SIZE_IN_SECTORS, cache_blocks, IOM_MAX_DEVS, ioe))) {
		log_error("Failed to create io-manager with %d cache blocks.", cache_blocks);
		return 0;
	}

	return 1;
}

static void _free_hints(struct dm_list *hints)
{
	struct hint *hint, *hint2;

	dm_list_iterate_items_safe(hint, hint2, hints) {
		dm_list_del(&hint->list);
		free(hint);
	}
}

/*
 * We don't know how many of num_devs will be PVs that we need to
 * keep open, but if it's greater than the soft limit, then we'll
 * need the soft limit raised, so do that before starting.
 *
 * If opens approach the raised soft/hard limit while scanning, then
 * we could also attempt to raise the soft/hard limits during the scan.
 */

#define BASE_FD_COUNT 32 /* Number of open files we want apart from devs */

static void _prepare_open_file_limit(struct cmd_context *cmd, unsigned int num_devs)
{
#ifdef HAVE_PRLIMIT
	struct rlimit old, new;
	unsigned int want = num_devs + BASE_FD_COUNT;
	int rv;

	rv = prlimit(0, RLIMIT_NOFILE, NULL, &old);
	if (rv < 0) {
		log_debug("Checking fd limit for num_devs %u failed %d", num_devs, errno);
		return;
	}

	log_debug("Checking fd limit for num_devs %u want %u soft %lld hard %lld",
		  num_devs, want, (long long)old.rlim_cur, (long long)old.rlim_max);

	/* Current soft limit is enough */
	if (old.rlim_cur > want)
		return;

	/* Soft limit already raised to max */
	if (old.rlim_cur == old.rlim_max)
		return;

	/* Raise soft limit up to hard/max limit */
	new.rlim_cur = old.rlim_max;
	new.rlim_max = old.rlim_max;

	log_debug("Setting fd limit for num_devs %u soft %lld hard %lld",
		  num_devs, (long long)new.rlim_cur, (long long)new.rlim_max);

	rv = prlimit(0, RLIMIT_NOFILE, &new, &old);
	if (rv < 0) {
		if (errno == EPERM)
			log_warn("WARNING: permission error setting open file limit for scanning %u devices.", num_devs);
		else
			log_warn("WARNING: cannot set open file limit for scanning %u devices.", num_devs);
		return;
	}
#endif
}

/*
 * Scan devices on the system to discover which are LVM devices.
 * Info about the LVM devices (PVs) is saved in lvmcache in a
 * basic/summary form (info/vginfo structs).  The vg_read phase
 * uses this summary info to know which PVs to look at for
 * processing a given VG.
 */

int label_scan(struct cmd_context *cmd)
{
	struct dm_list all_devs;
	struct dm_list scan_devs;
	struct dm_list hints_list;
	struct dev_iter *iter;
	struct device_list *devl, *devl2;
	struct device *dev;
	uint64_t max_metadata_size_bytes;
	int using_hints;
	int create_hints = 0; /* NEWHINTS_NONE */

	log_debug_devs("Finding devices to scan");

	dm_list_init(&all_devs);
	dm_list_init(&scan_devs);
	dm_list_init(&hints_list);

	if (lvm_iom)
		io_invalidate_all(lvm_iom);

	if (!lvm_iom) {
		if (!_setup_io_manager())
			return 0;
	}

	/*
	 * dev_cache_scan() creates a list of devices on the system
	 * (saved in in dev-cache) which we can iterate through to
	 * search for LVM devs.  The dev cache list either comes from
	 * looking at dev nodes under /dev, or from udev.
	 */
	dev_cache_scan();

	/*
	 * If we know that there will be md components with an end
	 * superblock, then enable the full md filter before label
	 * scan begins.  FIXME: we could skip the full md check on
	 * devs that are not identified as PVs, but then we'd need
	 * to do something other than using the standard md filter.
	 */
	if (cmd->md_component_detection && !cmd->use_full_md_check &&
	    !strcmp(cmd->md_component_checks, "auto") &&
	    dev_cache_has_md_with_end_superblock(cmd->dev_types)) {
		log_debug("Enable full md component check.");
		cmd->use_full_md_check = 1;
	}

	/*
	 * Set up the iterator that is needed to step through each device in
	 * dev cache.
	 */
	if (!(iter = dev_iter_create(cmd->filter, 0))) {
		log_error("Scanning failed to get devices.");
		return 0;
	}

	log_debug_devs("Filtering devices to scan");

	/*
	 * Iterate through all devices in dev cache and apply filters
	 * to exclude devs that we do not need to scan.  Those devs
	 * that pass the filters are returned by the iterator and
	 * saved in a list of devs that we will proceed to scan to
	 * check if they are LVM devs.  IOW this loop is the
	 * application of filters (those that do not require reading
	 * the devs) to the list of all devices.  It does that because
	 * the 'cmd->filter' is used above when setting up the iterator.
	 * Unfortunately, it's not obvious that this is what's happening
	 * here.  filters that require reading the device are not applied
	 * here, but in process_block(), see DEV_FILTER_AFTER_SCAN.
	 */
	while ((dev = dev_iter_get(cmd, iter))) {
		if (!(devl = zalloc(sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&all_devs, &devl->list);
	};
	dev_iter_destroy(iter);

	/*
	 * In some common cases we can avoid scanning all devices
	 * by using hints which tell us which devices are PVs, which
	 * are the only devices we actually need to scan.  Without
	 * hints we need to scan all devs to find which are PVs.
	 *
	 * TODO: if the command is using hints and a single vgname
	 * arg, we can also take the vg lock here, prior to scanning.
	 * This means we would not need to rescan the PVs in the VG
	 * in vg_read (skip lvmcache_label_rescan_vg) after the
	 * vg lock is usually taken.  (Some commands are already
	 * able to avoid rescan in vg_read, but locking early would
	 * apply to more cases.)
	 */
	if (!get_hints(cmd, &hints_list, &create_hints, &all_devs, &scan_devs)) {
		dm_list_splice(&scan_devs, &all_devs);
		dm_list_init(&hints_list);
		using_hints = 0;
	} else
		using_hints = 1;

	log_debug("Will scan %d devices skip %d", dm_list_size(&scan_devs), dm_list_size(&all_devs));

	/*
	 * If the total number of devices exceeds the soft open file
	 * limit, then increase the soft limit to the hard/max limit
	 * in case the number of PVs in scan_devs (it's only the PVs
	 * which we want to keep open) is higher than the current
	 * soft limit.
	 */
	_prepare_open_file_limit(cmd, dm_list_size(&scan_devs));

	io_data_ready = 1;

	/*
	 * Do the main scan.
	 */
	_scan_list(cmd, cmd->filter, &scan_devs, NULL);

	/*
	 * Metadata could be larger than total size of iom, and iom 
	 * cannot currently be resized during the command.  If this is the
	 * case (or within reach), warn that io_memory_size needs to be
	 * set larger.
	 *
	 * Even if iom out of space did not cause a failure during scan, it
	 * may cause a failure during the next vg_read phase or during vg_write.
	 *
	 * If there was an error during scan, we could recreate iom here
	 * with a larger size and then restart label_scan.  But, this does not
	 * address the problem of writing new metadata that excedes the iom
	 * size and failing, which would often be hit first, i.e. we'll fail
	 * to write new metadata exceding the max size before we have a chance
	 * to read any metadata with that size, unless we find an existing vg
	 * that has been previously created with the larger size.
	 *
	 * If the largest metadata is within 1MB of the iom size, then start
	 * warning.
	 */
	max_metadata_size_bytes = lvmcache_max_metadata_size();

	if (max_metadata_size_bytes + (1024 * 1024) > _current_io_size_bytes) {
		/* we want io-manager to be 1MB larger than the max metadata seen */
		uint64_t want_size_kb = (max_metadata_size_bytes / 1024) + 1024;
		uint64_t remainder;
		if ((remainder = (want_size_kb % 1024)))
			want_size_kb = want_size_kb + 1024 - remainder;

		log_warn("WARNING: metadata may not be usable with current io_memory_size %d KiB",
			 io_memory_size());
		log_warn("WARNING: increase lvm.conf io_memory_size to at least %llu KiB",
			 (unsigned long long)want_size_kb);
	}

	dm_list_init(&cmd->hints);

	/*
	 * If we're using hints to limit which devs we scanned, verify
	 * that those hints were valid, and if not we need to scan the
	 * rest of the devs.
	 */
	if (using_hints) {
		if (!validate_hints(cmd, &hints_list)) {
			log_debug("Will scan %d remaining devices", dm_list_size(&all_devs));
			_scan_list(cmd, cmd->filter, &all_devs, NULL);
			_free_hints(&hints_list);
			using_hints = 0;
			create_hints = 0;
		} else {
			/* The hints may be used by another device iteration. */
			dm_list_splice(&cmd->hints, &hints_list);
		}
	}

	/*
	 * Stronger exclusion of md components that might have been
	 * misidentified as PVs due to having an end-of-device md superblock.
	 * If we're not using hints, and are not already doing a full md check
	 * on devs being scanned, then if udev info is missing for a PV, scan
	 * the end of the PV to verify it's not an md component.  The full
	 * dev_is_md_component call will do new reads at the end of the dev.
	 */
	if (cmd->md_component_detection && !cmd->use_full_md_check && !using_hints &&
	    !strcmp(cmd->md_component_checks, "auto")) {
		int once = 0;
		dm_list_iterate_items(devl, &scan_devs) {
			if (!(devl->dev->flags & DEV_SCAN_FOUND_LABEL))
				continue;
			if (!(devl->dev->flags & DEV_UDEV_INFO_MISSING))
				continue;
			if (!once++)
				log_debug_devs("Scanning end of PVs with no udev info for MD components");

			if (dev_is_md_component(devl->dev, NULL, 1)) {
				log_debug_devs("Scan dropping PV from MD component %s", dev_name(devl->dev));
				devl->dev->flags &= ~DEV_SCAN_FOUND_LABEL;
				lvmcache_del_dev(devl->dev);
				lvmcache_del_dev_from_duplicates(devl->dev);
			}
		}
	}

	dm_list_iterate_items_safe(devl, devl2, &all_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	dm_list_iterate_items_safe(devl, devl2, &scan_devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	/*
	 * If hints were not available/usable, then we scanned all devs,
	 * and we now know which are PVs.  Save this list of PVs we've
	 * identified as hints for the next command to use.
	 * (create_hints variable has NEWHINTS_X value which indicates
	 * the reason for creating the new hints.)
	 */
	if (create_hints)
		write_hint_file(cmd, create_hints);

	return 1;
}

/*
 * Scan and cache lvm data from the listed devices.  If a device is already
 * scanned and cached, this replaces the previously cached lvm data for the
 * device.  This is called when vg_read() wants to guarantee that it is using
 * the latest data from the devices in the VG (since the scan populated iom
 * without a lock.)
 */

int label_scan_devs(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	struct device_list *devl;

	if (!lvm_iom) {
		if (!_setup_io_manager())
			return 0;
	}

	dm_list_iterate_items(devl, devs) {
		if (!_get_dev(devl->dev, EF_READ_ONLY))
			continue;

		io_invalidate_dev(lvm_iom, devl->dev->iodev);
	}

	_scan_list(cmd, f, devs, NULL);

	return 1;
}

/*
 * This function is used when the caller plans to write to the devs, so opening
 * them RW during rescan avoids needing to close and reopen with WRITE in
 * dev_write_bytes.
 */

int label_scan_devs_rw(struct cmd_context *cmd, struct dev_filter *f, struct dm_list *devs)
{
	struct device_list *devl;

	if (!lvm_iom) {
		if (!_setup_io_manager())
			return 0;
	}

	dm_list_iterate_items(devl, devs) {
		if (!_get_dev(devl->dev, 0))
			continue;

		io_invalidate_dev(lvm_iom, devl->dev->iodev);
	}

	_scan_list(cmd, f, devs, NULL);

	return 1;
}

int label_scan_devs_excl(struct dm_list *devs)
{
	struct device_list *devl;
	int failed = 0;

	dm_list_iterate_items(devl, devs) {
		if (!_get_dev(devl->dev, EF_EXCL))
			continue;

		io_invalidate_dev(lvm_iom, devl->dev->iodev);
	}

	_scan_list(NULL, NULL, devs, &failed);

	if (failed)
		return 0;
	return 1;
}

void label_scan_invalidate(struct device *dev)
{
	if (!_get_dev(dev, EF_READ_ONLY))
		return;
	io_invalidate_dev(lvm_iom, dev->iodev);
	_put_dev(dev);
}

/*
 * If a PV is stacked on an LV, then the LV is kept open
 * in iom, and needs to be closed so the open fd doesn't
 * interfere with processing the LV.
 */

void label_scan_invalidate_lv(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct lvinfo lvinfo;
	struct device *dev;
	dev_t devt;

	if (!lv_info(cmd, lv, 0, &lvinfo, 0, 0))
		return;

	devt = MKDEV(lvinfo.major, lvinfo.minor);
	if ((dev = dev_cache_get_by_devt(cmd, devt, NULL, NULL)))
		label_scan_invalidate(dev);
}

/*
 * Empty the iom of all blocks and close all open fds,
 * but keep the iom set up.
 */

void label_scan_drop(struct cmd_context *cmd)
{
	if (lvm_iom)
		io_invalidate_all(lvm_iom);
}

/*
 * Close devices that are open because iom is holding blocks for them.
 * Destroy the iom.
 */

void label_scan_destroy(struct cmd_context *cmd)
{
	if (!lvm_iom)
		return;

	io_invalidate_all(lvm_iom);
	io_manager_destroy(lvm_iom);
	lvm_iom = NULL;
}

/*
 * Read (or re-read) and process (or re-process) the data for a device.  This
 * will reset (clear and repopulate) the iom and lvmcache info for this
 * device.  There are only a couple odd places that want to reread a specific
 * device, this is not a commonly used function.
 */

int label_read(struct device *dev)
{
	struct dm_list one_dev;
	struct device_list *devl;
	int failed = 0;

	/* scanning is done by list, so make a single item list for this dev */
	if (!(devl = zalloc(sizeof(*devl))))
		return 0;
	devl->dev = dev;
	dm_list_init(&one_dev);
	dm_list_add(&one_dev, &devl->list);

	if (!_get_dev(dev, EF_READ_ONLY)) {
		log_error("No io device available for %s", dev_name(devl->dev));
		return 0;
	}

	io_invalidate_dev(lvm_iom, dev->iodev);

	_scan_list(NULL, NULL, &one_dev, &failed);

	free(devl);

	if (failed)
		return 0;
	return 1;
}

int label_scan_setup_io_manager(void)
{
	if (!lvm_iom) {
		if (!_setup_io_manager())
			return 0;
	}

	return 1;
}

/* FIXME: probably not needed, read will do it */
int label_scan_open(struct device *dev)
{
	return _get_dev(dev, EF_READ_ONLY);
}

int label_scan_open_excl(struct device *dev)
{
	return _get_dev(dev, EF_EXCL);
}

/* FIXME: probably not needed, write will do it */
int label_scan_open_rw(struct device *dev)
{
	return _get_dev(dev, 0);
}

bool dev_read_bytes(struct device *dev, uint64_t start, size_t len, void *data)
{
	int put = !dev->iodev;

	if (!lvm_iom) {
		/* Should not happen */
		log_error("dev_read io manager not set up %s", dev_name(dev));
		return false;
	}

	if (!_get_dev(dev, EF_READ_ONLY))
		return false;

	if (!io_read_bytes(lvm_iom, dev->iodev, start, len, data)) {
		log_error("Error reading device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (put)
		_put_dev(dev);
	return true;

}

bool dev_write_bytes(struct device *dev, uint64_t start, size_t len, void *data)
{
	int put = !dev->iodev;

	if (test_mode())
		return true;

	if (!lvm_iom) {
		/* Should not happen */
		log_error("dev_write io manager not set up %s", dev_name(dev));
		return false;
	}

	if (!_get_dev(dev, 0))
		return false;

	if (!io_write_bytes(lvm_iom, dev->iodev, start, len, data)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (!io_flush(lvm_iom)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (put)
		_put_dev(dev);
	return true;
}

bool dev_write_zeros(struct device *dev, uint64_t start, size_t len)
{
	int put = !dev->iodev;

	if (test_mode())
		return true;

	if (!lvm_iom) {
		log_error("dev_write_zeros io manager not set up %s", dev_name(dev));
		return false;
	}

	if (!_get_dev(dev, 0))
		return false;

	if (!io_zero_bytes(lvm_iom, dev->iodev, start, len)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (!io_flush(lvm_iom)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (put)
		_put_dev(dev);
	return true;
}

bool dev_set_bytes(struct device *dev, uint64_t start, size_t len, uint8_t val)
{
	int put = !dev->iodev;

	if (test_mode())
		return true;

	if (!lvm_iom) {
		log_error("dev_set_bytes io manager not set up %s", dev_name(dev));
		return false;
	}

	if (!_get_dev(dev, 0))
		return false;

	if (!io_set_bytes(lvm_iom, dev->iodev, start, len, val)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (!io_flush(lvm_iom)) {
		log_error("Error writing device %s at %llu length %u.",
			  dev_name(dev), (unsigned long long)start, (uint32_t)len);
		if (put)
			_put_dev(dev);
		return false;
	}

	if (put)
		_put_dev(dev);
	return true;
}

