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

#include "lib.h"
#include "toolcontext.h"
#include "label.h"
#include "crc.h"
#include "xlate.h"
#include "lvmcache.h"
#include "lvmetad.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

static DM_LIST_INIT(_label_read_list);
static struct dev_async_context *_ac;

/*
 * Label reading/scanning
 *
 * label_scan
 *   label_scan_async
 *   label_scan_sync
 *   label_scan_limit
 *
 * label_scan_devs
 *   label_scan_devs_async
 *   label_scan_devs_sync
 *   label_scan_devs_limit
 *
 * label_scan_force
 *
 * label_scan() run at the start of a command iterates over all visible devices,
 * reading them, looking for any that belong to lvm, and fills lvmcache with
 * basic info about them.  It's main job is to prepare for subsequent vg_reads.
 * vg_read(vgname) needs to know which devices/locations to read metadata from
 * for the given vg name.  The label_scan has scanned all devices and saved info
 * in lvmcache about: the device, the mda locations on that device, and the vgname
 * referenced in those mdas.  So, using the info gathered during label_scan,
 * vg_read(vgname) can map the vgname to a the subset of devices it needs to read
 * for that VG.
 *
 * label_scan_devs() is run at the beginning of vg_read(vgname) to repeat the
 * reading done by label_scan() on only the devices related to the given vgname.
 * The label_scan() was done without a lock on the vg name, so the vg may have
 * changed between the label_scan() and the vg_read(vgname) in which the vg
 * lock is now held.  Repeating the reading ensures that the correct data from
 * the devices is being used by the vg_read().  vg_read() itself then uses
 * the data that has been reread by label_scan_devs() to process the full metadata.
 *
 * To illustrate the device reads being done, consider the 'vgs' command
 * with three devices: PV1 and PV2 in VGA, and PV3 in VGB.
 *
 * 1. label_scan() reads data from PV1, PV2, PV3,
 * and fills lvmcache with info/vginfo structs that show
 * PV1 and PV2 are used by VGA, and PV3 is used by VGB.
 *
 * 2. vg_read(VGA) rereads the same data from from PV1 and PV2
 * and processes the metadata for VGA.
 *
 * 3. vg_read(VGB) rereads the same data from PV3 and processes
 * the metadata for VGB.
 *
 * In total there are six device reads: three in step 1, two in step 2,
 * and one in step 3.
 *
 * label_scan_limit() and label_scan_devs_limit() are used if memory
 * limitations prevent label_scan_async() or label_scan_sync() from
 * completing.  label_scan_{async,sync} allocate a memory buffer of
 * scan_size bytes for each lvm device.  If there are many devices,
 * all of these buffers may excede memory limitations.  The _limit
 * variations are synchronous and do not use a separate buffer for
 * each lvm device, which means there are many more disk reads.
 */

/* FIXME Allow for larger labels?  Restricted to single sector currently */

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

	if (!(li = dm_malloc(len))) {
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
		dm_free(li);
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

static int _sync_io_setup = 0;
static int _sync_io_max = 0;
static int _sync_io_count = 0;

static char *_sync_io_get(int buf_len)
{
	char *buf;

	if (_sync_io_max && (_sync_io_count >= _sync_io_max))
		return NULL;

	if ((buf = dm_zalloc(buf_len))) {
		_sync_io_count++;
		return buf;
	}
	return NULL;
}

static void _sync_io_put(char *buf)
{
	dm_free(buf);
	if (!_sync_io_count)
		log_error("Bad sync io accounting");
	else
		_sync_io_count--;
}

static void _ld_io_put(struct label_read_data *ld)
{
	if (ld->aio) {
		dev_async_io_put(_ac, ld->aio);
		ld->aio = NULL;
		ld->buf = NULL;
	} else if (ld->buf) {
		_sync_io_put(ld->buf);
		ld->buf = NULL;
	}
}

/*
 * FIXME: handle errors, if there is lvm data on the device but
 * it's bad, then we need to move this device into a special
 * set of defective devices that can be reported or repaired.
 */

static struct labeller *_find_label_header(struct device *dev,
				   char *scan_buf,
				   char *label_buf,
				   uint64_t *label_sector,
				   uint64_t scan_sector)
{
	struct labeller_i *li;
	struct labeller *r = NULL;
	struct label_header *lh;
	struct lvmcache_info *info;
	uint64_t sector;
	int found = 0;

	/*
	 * Find which sector in scan_buf starts with a valid label,
	 * and copy it into label_buf.
	 */

	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (scan_buf + (sector << SECTOR_SHIFT));

		if (!strncmp((char *)lh->id, LABEL_ID, sizeof(lh->id))) {
			if (found) {
				log_error("Ignoring additional label on %s at "
					  "sector %" PRIu64, dev_name(dev),
					  sector + scan_sector);
			}
			if (xlate64(lh->sector_xl) != sector + scan_sector) {
				log_very_verbose("%s: Label for sector %" PRIu64
						 " found at sector %" PRIu64
						 " - ignoring", dev_name(dev),
						 (uint64_t)xlate64(lh->sector_xl),
						 sector + scan_sector);
				continue;
			}
			if (calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				     ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)) !=
			    xlate32(lh->crc_xl)) {
				log_very_verbose("Label checksum incorrect on %s - "
						 "ignoring", dev_name(dev));
				continue;
			}
			if (found)
				continue;
		}

		dm_list_iterate_items(li, &_labellers) {
			if (li->l->ops->can_handle(li->l, (char *) lh,
						   sector + scan_sector)) {
				log_very_verbose("%s: %s label detected at "
					         "sector %" PRIu64, 
						 dev_name(dev), li->name,
						 sector + scan_sector);
				if (found) {
					log_error("Ignoring additional label "
						  "on %s at sector %" PRIu64,
						  dev_name(dev),
						  sector + scan_sector);
					continue;
				}
				r = li->l;
				memcpy(label_buf, lh, LABEL_SIZE);
				if (label_sector)
					*label_sector = sector + scan_sector;
				found = 1;
				break;
			}
		}
	}

	if (!found) {
		log_very_verbose("%s: No label detected", dev_name(dev));

		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
			/* Lacking a label does *not* make the device an orphan! */
			/* _update_lvmcache_orphan(info); */

			log_warn("Device %s has no label, removing PV info from lvmcache.", dev_name(dev));
			lvmcache_del(info);
		}
	}

	return r;
}

static void _remove_label_read_data(struct device *dev)
{
	struct label_read_data *ld;

	dm_list_iterate_items(ld, &_label_read_list) {
		if (ld->dev == dev) {
			dm_list_del(&ld->list);
			_ld_io_put(ld);
			dm_free(ld);
			return;
		}

	}
}

/* FIXME Also wipe associated metadata area headers? */
int label_remove(struct device *dev)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	char readbuf[LABEL_SCAN_SIZE] __attribute__((aligned(8)));
	int r = 1;
	uint64_t sector;
	int wipe;
	struct labeller_i *li;
	struct label_header *lh;
	struct lvmcache_info *info;

	_remove_label_read_data(dev);

	memset(buf, 0, LABEL_SIZE);

	if (!dev_open(dev)) {
		log_debug_devs("Removing label skipped can't open %s", dev_name(dev));
		return 0;
	}

	/*
	 * We flush the device just in case someone is stupid
	 * enough to be trying to import an open pv into lvm.
	 */
	dev_flush(dev);

	log_debug_devs("Reading label sectors to remove from device %s", dev_name(dev));

	if (!dev_read(dev, UINT64_C(0), LABEL_SCAN_SIZE, readbuf)) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	/* Scan first few sectors for anything looking like a label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (readbuf + (sector << SECTOR_SHIFT));

		wipe = 0;

		if (!strncmp((char *)lh->id, LABEL_ID, sizeof(lh->id))) {
			if (xlate64(lh->sector_xl) == sector)
				wipe = 1;
		} else {
			dm_list_iterate_items(li, &_labellers) {
				if (li->l->ops->can_handle(li->l, (char *) lh, sector)) {
					wipe = 1;
					break;
				}
			}
		}

		if (wipe) {
			log_very_verbose("%s: Wiping label at sector %" PRIu64,
					 dev_name(dev), sector);
			if (dev_write(dev, sector << SECTOR_SHIFT, LABEL_SIZE,
				       buf)) {
				/* Also remove the PV record from cache. */
				info = lvmcache_info_from_pvid(dev->pvid, dev, 0);
				if (info)
					lvmcache_del(info);
			} else {
				log_error("Failed to remove label from %s at "
					  "sector %" PRIu64, dev_name(dev),
					  sector);
				r = 0;
			}
		}
	}

      out:
	if (!dev_close(dev))
		stack;

	return r;
}

/*
 * synchronously read label from one device (not for label_scan)
 *
 * Note that this will *not* reread the label from disk if an
 * info struct for this dev exists in lvmcache.
 */

int label_read(struct device *dev, struct label **labelp, uint64_t scan_sector)
{
	char scanbuf[LABEL_SCAN_SIZE] __attribute__((aligned(8)));
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label *label;
	struct labeller *l;
	uint64_t sector;
	struct lvmcache_info *info;
	int r = 0;

	if (!labelp)
		labelp = &label;

	if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
		log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
		if (labelp)
			*labelp = lvmcache_get_label(info);
		return 1;
	}


	if (!dev_open_readonly(dev)) {
		log_debug_devs("Reading label skipped can't open %s", dev_name(dev));

		/* Can't open the device, does *not* mean it's an orphan! */
		/*
		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0)))
			_update_lvmcache_orphan(info);
		*/

		return r;
	}

	memset(scanbuf, 0, sizeof(scanbuf));

	log_debug_devs("Reading label sectors from device %s", dev_name(dev));

	/*
	 * Read first four sectors into scanbuf.
	 */
	if (!dev_read(dev, scan_sector << SECTOR_SHIFT, LABEL_SCAN_SIZE, scanbuf)) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	log_debug_devs("Parsing label and data from device %s", dev_name(dev));

	/*
	 * Finds the sector from scanbuf containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 */
	if (!(l = _find_label_header(dev, scanbuf, label_buf, &sector, scan_sector))) {
		/* FIXME: handle bad label */
		goto_out;
	}

	/*
	 * ops->read() is usually _text_read() which reads
	 * the pv_header, mda locations, mda contents.
	 * It saves the info it finds into lvmcache info/vginfo structs.
	 */
	if ((r = (l->ops->read)(l, dev, label_buf, NULL, labelp)) && *labelp) {
		(*labelp)->dev = dev;
		(*labelp)->sector = sector;
	} else {
		/* FIXME: handle errors */
	}
 out:
	if (!dev_close(dev))
		stack;

	return r;
}

static unsigned int _time_diff_ms(struct timeval *begin, struct timeval *end)
{
	struct timeval result;
	timersub(end, begin, &result);
	return (result.tv_sec * 1000) + (result.tv_usec / 1000);
}

/* Caller may need to use label_get_handler to create label struct! */
int label_write(struct device *dev, struct label *label)
{
	char buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label_header *lh = (struct label_header *) buf;
	int r = 1;

	/*
	 * This shouldn't necessary because there's nothing that would
	 * use the existing ld, but there's no sense in keeping around
	 * data we know is stale.
	 */
	_remove_label_read_data(dev);

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

	strncpy((char *)lh->id, LABEL_ID, sizeof(lh->id));
	lh->sector_xl = xlate64(label->sector);
	lh->offset_xl = xlate32(sizeof(*lh));

	if (!(label->labeller->ops->write)(label, buf))
		return_0;

	lh->crc_xl = xlate32(calc_crc(INITIAL_CRC, (uint8_t *)&lh->offset_xl, LABEL_SIZE -
				      ((uint8_t *) &lh->offset_xl - (uint8_t *) lh)));

	if (!dev_open(dev))
		return_0;

	log_very_verbose("%s: Writing label to sector %" PRIu64 " with stored offset %"
			 PRIu32 ".", dev_name(dev), label->sector,
			 xlate32(lh->offset_xl));
	if (!dev_write(dev, label->sector << SECTOR_SHIFT, LABEL_SIZE, buf)) {
		log_debug_devs("Failed to write label to %s", dev_name(dev));
		r = 0;
	}

	if (!dev_close(dev))
		stack;

	return r;
}

/* Unused */
int label_verify(struct device *dev)
{
	char scanbuf[LABEL_SCAN_SIZE] __attribute__((aligned(8)));
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct labeller *l;
	uint64_t sector;
	int r = 0;

	if (!dev_open_readonly(dev))
		return_0;

	if (!dev_read(dev, 0, LABEL_SCAN_SIZE, scanbuf)) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	if (!(l = _find_label_header(dev, scanbuf, label_buf, &sector, UINT64_C(0)))) {
		/* FIXME: handle bad label */
		goto out;
	}

	r = l->ops->verify ? l->ops->verify(l, label_buf, sector) : 1;

      out:
	if (!dev_close(dev))
		stack;

	return r;
}

void label_destroy(struct label *label)
{
	_remove_label_read_data(label->dev);
	label->labeller->ops->destroy_label(label->labeller, label);
	dm_free(label);
}

struct label *label_create(struct labeller *labeller)
{
	struct label *label;

	if (!(label = dm_zalloc(sizeof(*label)))) {
		log_error("label allocaction failed");
		return NULL;
	}

	label->labeller = labeller;

	labeller->ops->initialise_label(labeller, label);

	return label;
}

static void _free_label_read_list(struct cmd_context *cmd)
{
	struct label_read_data *ld, *ld2;

	dm_list_iterate_items(ld, &_label_read_list)
		_ld_io_put(ld);

	dm_list_iterate_items_safe(ld, ld2, &_label_read_list) {
		dm_list_del(&ld->list);
		dm_free(ld);
	}
}

/*
 * ld structs for non-lvm devices have ld->aio / ld->buf put/freed
 * in _label_read_data_process() which finds they are not for lvm.
 * ld structs are used to track progress through scanning, so they
 * continue to be used until the end of the scanning function at
 * which point this function is called to remove the ld structs
 * for non-lvm devs.
 */
static void _prune_label_read_list(struct cmd_context *cmd)
{
	struct label_read_data *ld, *ld2;

	dm_list_iterate_items_safe(ld, ld2, &_label_read_list) {
		if (!ld->aio && !ld->buf) {
			dm_list_del(&ld->list);
			dm_free(ld);
		}
	}
}

struct label_read_data *get_label_read_data(struct cmd_context *cmd, struct device *dev)
{
	struct label_read_data *ld;

	dm_list_iterate_items(ld, &_label_read_list) {
		if (ld->dev == dev) {
			if (!ld->buf) {
				/* this ld struct should have been pruned in the scan function */
				log_error(INTERNAL_ERROR "label data for %s has no buffer", dev_name(dev));
				return NULL;
			}
			return ld;
		}
	}
	return NULL;
}

/*
 * ld->buf is the buffer into which the first scan_size bytes
 * of each device are read.
 *
 * This data is meant to big large enough to cover all the
 * headers and metadata that need to be read from the device
 * during the label scan for most common cases.
 *
 * 1. one of the first four sectors holds:
 *    label_header, pv_header, pv_header_extention
 *
 * 2. the mda_header whose location is found from 1.
 *
 * 3. the metadata whose location is from found 2.
 *
 * If during processing, metadata needs to be read in a region
 * beyond this buffer, then the code will revert do doing a
 * synchronous read of the data it needs.
 */
static int _get_scan_size(struct cmd_context *cmd)
{
	return cmd->current_settings.scan_size_kb * 1024;
}

/*
 * scan_size bytes of data has been read into ld->buf, using either
 * async or sync io.  Now process/parse the headers from that buffer.
 * Populates lvmcache with device / mda locations / vgname
 * so that vg_read(vgname) will know which devices/locations
 * to read metadata from.
 *
 * If during processing, headers/metadata are found to be needed
 * beyond the range of scan_size, then additional synchronous
 * reads are performed in the processing functions to get that data.
 */
static int _label_read_data_process(struct cmd_context *cmd, struct label_read_data *ld)
{
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label *label = NULL;
	struct labeller *l;
	uint64_t sector;
	int r = 0;

	if ((ld->result < 0) || (ld->result != ld->buf_len)) {
		/* FIXME: handle errors */
		log_error("Reading label sectors error %d from %s", ld->result, dev_name(ld->dev));
		goto out;
	}

	/*
	 * Finds the sector from scanbuf containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 *
	 * FIXME: we don't need to copy one sector from ld->buf into label_buf,
	 * we can just point label_buf at one sector in ld->buf.
	 */
	if (!(l = _find_label_header(ld->dev, ld->buf, label_buf, &sector, 0))) {
		/* Non-PVs exit here */
		/* FIXME: check for PVs with errors that also exit here. */

		/* A common buffer does not belong to the ld. */
		if (ld->common_buf)
			goto_out;

		/*
		 * This device is not an lvm device, so we don't need to
		 * process it any further.  So we don't need to keep the
		 * buffers around for this dev.  Put the aio struct back
		 * so it can be reused for scanning another device.
		 */
		_ld_io_put(ld);
		goto_out;
	}

	/*
	 * ops->read() is usually _text_read() which reads
	 * the pv_header, mda locations, mda contents.
	 * It saves the info it finds into lvmcache info/vginfo structs.
	 */
	if ((r = (l->ops->read)(l, ld->dev, label_buf, ld, &label)) && label) {
		label->dev = ld->dev;
		label->sector = sector;
	} else {
		/* FIXME: handle errors */
	}
 out:
	return r;
}

/*
 * Start label aio read on a device.
 */
static int _label_read_async_start(struct dev_async_context *ac, struct label_read_data *ld, int *no_event)
{
	*no_event = 0;

	if (!dev_async_read_submit(ac, ld->aio, ld->dev, ld->buf_len, 0, no_event)) {
		if (*no_event)
			log_debug_devs("Reading label no aio event for %s", dev_name(ld->dev));
		else
			log_debug_devs("Reading label aio submit error for %s", dev_name(ld->dev));
		return 0;
	}

	return 1;
}

/*
 * Reap aio reads from devices.
 */
static int _label_read_async_wait(struct dev_async_context *ac, int wait_count, int timeout_sec,
				  int *done_count)
{
	struct timespec ts;

	memset(&ts, 0, sizeof(ts));
	if (timeout_sec)
		ts.tv_sec = timeout_sec;

	return dev_async_getevents(ac, wait_count, timeout_sec ? &ts : NULL, done_count);
}

static int _label_scan_list_async(struct cmd_context *cmd,
				  struct dm_list *ld_list,
				  int async_event_count,
				  int buf_len,
				  int *try_sync_count,
				  int *scan_failed_count)
{
	struct label_read_data *ld;
	int need_start_count;
	int need_wait_count;
	int need_process_count;
	int pending_count = 0;
	int try_sync = 0;
	int mem_limit = 0;
	int event_limit = 0;
	int scan_failed = 0;
	int no_event;

	*try_sync_count = 0;
	*scan_failed_count = 0;

	/*
	 * Start async reads
	 */
 start_more:

	dm_list_iterate_items(ld, ld_list) {
		if (ld->io_started)
			continue;

		/*
		 * Don't start more than we know the aio context can handle.
		 * This should avoid getting a no_event failure below.
		 */
		if (pending_count >= async_event_count)
			break;

		if (!ld->aio) {
			if (!(ld->aio = dev_async_io_get(_ac, buf_len))) {
				log_debug_devs("Reading label no aio buffer for %s", dev_name(ld->dev));
				ld->io_started = 1;
				ld->io_done = 1;
				ld->process_done = 1;
				ld->mem_limit = 1;
				mem_limit++;
				/* Try to complete outstanding reads which may free up aio bufs. */
				goto check_results;
			}
			ld->buf = ld->aio->buf;
			ld->buf_len = buf_len;
		}

		ld->aio->done = 0;
		ld->io_started = 1;

		if (!dev_open_readonly(ld->dev)) {
			/* User should not set async_events above open files limit. */
			log_debug_devs("Defer scan for fd limit on %s.", dev_name(ld->dev));
			ld->io_done = 1;
			ld->process_done = 1;
			ld->try_sync = 1;
			try_sync++;
			goto check_results;
		}

		if (!_label_read_async_start(_ac, ld, &no_event)) {
			dev_close(ld->dev);
			ld->io_done = 1;
			ld->process_done = 1;

			/*
			 * The first time there's no aio event, flag this to
			 * retry at the end when events may be available. If
			 * the same happens on retry, then revert to try_sync.
			 */
			if (no_event && !ld->event_limit) {
				ld->event_limit = 1;
				event_limit++;
				/* Try to complete outstanding reads which may free up events. */
				goto check_results;
			} else {
				log_debug_devs("Defer scan for event limit on %s.", dev_name(ld->dev));
				ld->event_limit = 0;
				ld->try_sync = 1;
				try_sync++;
			}
		} else {
			log_debug_devs("Reading sectors from device %s async", dev_name(ld->dev));
			pending_count++;
		}
	}

	/*
	 * Reap the aio completions and process the results.
	 */

 check_results:
	need_start_count = 0;
	need_wait_count = 0;
	need_process_count = 0;

	/*
	 * If there are ios that are not started, we need to start more.
	 * If there are ios started that are not done, we need to wait for more.
	 * If there are ios that are done but not processed, we need to process more.
	 */
	dm_list_iterate_items(ld, ld_list) {
		if (ld->io_done)
			continue;
		if (!ld->io_started)
			need_start_count++;
		else if (!ld->aio->done)
			need_wait_count++;
		else if (!ld->process_done)
			need_process_count++;
	}

	/*
	 * Wait for some aio to finish and collect results.
	 */
	if (need_wait_count) {
		struct timeval start, now; 
		int timeout_sec;
		int done_count;
		unsigned int ms;

		/*
		 * When more ios need to be started, we don't want to
		 * wait forever for some slow ios to complete.
		 */
		if (need_start_count)
			timeout_sec = 1;
		else
			timeout_sec = 0;

		done_count = 0;

		gettimeofday(&start, NULL);

		if (_label_read_async_wait(_ac, need_wait_count, timeout_sec, &done_count)) {
			gettimeofday(&now, NULL);
			ms = _time_diff_ms(&start, &now);
			log_debug_devs("Waited for %u msec for %d of %d aio completions",
					ms, done_count, need_wait_count);
			pending_count -= done_count;
			need_process_count += done_count;
		} else {
			/*
			 * It's not clear what could cause an error here.
			 * TODO: this should probably be handled by falling
			 * back to try_sync on any remaining ld's
			 */
			log_error(INTERNAL_ERROR "aio getevents error");
		}
	}

	/*
	 * Process all devices that have finished reading.
	 * (Processing can include sync io to read metadata areas beyond the scan_size.)
	 */
	if (need_process_count) {
		dm_list_iterate_items(ld, ld_list) {
			if (!ld->io_started || ld->process_done)
				continue;

			/*
			 * Lower level aio code sets aio->done when the aio
			 * is finished and a result is available.  Transfer
			 * this aio->done and aio->result to ld->done and
			 * ld->result.
			 */
			if (!ld->aio->done)
				continue;
			ld->io_done = 1;
			ld->result = ld->aio->result;

			if (!ld->result || (ld->result < 0)) {
				log_debug_devs("%s: Failed to read label data", dev_name(ld->dev));
				_ld_io_put(ld);
				scan_failed++;
			} else {
				log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
				/* process() will put ld->aio if the dev is not lvm's */
				_label_read_data_process(cmd, ld);
			}

			ld->process_done = 1;
			dev_close(ld->dev);
		}
	}

	/*
	 * Start more io, if events are available, before waiting/processing
	 * more results.
	 */
	if (need_start_count && (pending_count < async_event_count)) {
		log_debug_devs("Starting more of %d remaining async io, %d pending.", need_start_count, pending_count);
		goto start_more;
	}

	if (need_process_count || need_wait_count)
		goto check_results;

	dm_list_iterate_items(ld, ld_list) {
		if (!ld->io_started) {
			log_debug_devs("Starting more of %d remaining async io.", need_start_count);
			goto start_more;
		}
	}

	/*
	 * At this point, all outstanding aio's are completed and processed,
	 * and are not consuming aio events, so it should be possible to submit
	 * any ld's that were deferred above because of event_limit.  This
	 * should not generally happen because we try to limit how many aios we
	 * submit based on our count of how many have been submitted.
	 */
	if (event_limit) {
		dm_list_iterate_items(ld, ld_list) {
			if (!ld->event_limit)
				continue;
			log_debug_devs("Retrying event limited aio for %s.", dev_name(ld->dev));
			ld->io_started = 0;
			ld->io_done = 0;
			ld->process_done = 0;
		}
		event_limit = 0;
		goto start_more;
	}

	if (mem_limit) {
		dm_list_iterate_items(ld, ld_list) {
			if (!ld->mem_limit)
				continue;

			if (!(ld->aio = dev_async_io_get(_ac, buf_len))) {
				log_debug_devs("Defer scan for mem limit on %s.", dev_name(ld->dev));
				ld->mem_limit = 0;
				ld->try_sync = 1;
				try_sync++;
			} else {
				log_debug_devs("Retrying mem limited aio for %s.", dev_name(ld->dev));
				ld->mem_limit = 0;
				ld->io_started = 0;
				ld->io_done = 0;
				ld->process_done = 0;
				ld->buf = ld->aio->buf;
				ld->buf_len = buf_len;
			}
		}
		mem_limit = 0;
		goto start_more;
	}

	*try_sync_count = try_sync;
	*scan_failed_count = scan_failed;
	return 0;
}

/*
 * For any device (ld struct) for which async io failed to be
 * submitted, or ld->aio struct failed to be allocated, we
 * set ld->try_sync, and arrive here to process those devs.
 *
 * If the ld has an aio struct, we can use that for the sync
 * read and benefit from the cache.  If the ld has no aio
 * struct we use a common buffer here that is not saved.
 */

static int _label_scan_list_sync(struct cmd_context *cmd, struct dm_list *ld_list, int buf_len,
				 int *scan_failed_count)
{
	struct label_read_data *ld;
	char *buf;
	int scan_failed = 0;

	*scan_failed_count = 0;

	/*
	 * mem limitations can result in some ld's having no ld->aio, in which
	 * case we use this common buffer for reading those devs.
	 */
	if (!(buf = dm_malloc(buf_len))) {
		log_error("No memory to scan devices.");
		return 0;
	}

	dm_list_iterate_items(ld, ld_list) {
		if (!ld->try_sync)
			continue;

		ld->try_sync = 0;

		if (!dev_open_readonly(ld->dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(ld->dev));
			_ld_io_put(ld);
			ld->io_done = 1;
			ld->process_done = 1;
			ld->result = -1;
			scan_failed++;
			continue;
		}

		if (ld->aio) {
			log_debug_devs("Reading sectors from device %s trying sync", dev_name(ld->dev));

			if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf)) {
				log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
				ld->aio->done = 1;
				ld->aio->result = -1;
				ld->io_done = 1;
				ld->result = -1;
				_ld_io_put(ld);
				scan_failed++;
			} else {
				log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
				ld->aio->done = 1;
				ld->aio->result = ld->buf_len;
				ld->io_done = 1;
				ld->result = ld->aio->result;
				/* process() will put ld->aio if the dev is not lvm's */
				_label_read_data_process(cmd, ld);
			}
			ld->process_done = 1;
			dev_close(ld->dev);
		} else {
			log_debug_devs("Reading sectors from device %s trying sync with common buf", dev_name(ld->dev));

			ld->common_buf = 1;
			ld->buf = buf;
			ld->buf_len = buf_len;
			memset(buf, 0, buf_len);

			if (!dev_read(ld->dev, 0, buf_len, buf)) {
				log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
				ld->io_done = 1;
				ld->result = -1;
			} else {
				log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
				ld->io_done = 1;
				ld->result = buf_len;
				_label_read_data_process(cmd, ld);
			}
			ld->process_done = 1;
			dev_close(ld->dev);

			ld->common_buf = 0;
			ld->buf = NULL;
		}
	}

	*scan_failed_count = scan_failed;
	dm_free(buf);
	return 1;
}

/*
 * Scan labels/metadata for all devices (async)
 *
 * Reads and looks at label_header, pv_header, pv_header_extension,
 * mda_header, raw_locns, vg metadata from each device.
 *
 * Effect is populating lvmcache with latest info/vginfo (PV/VG) data
 * from the devs.  If a scanned device does not have a label_header,
 * its info is removed from lvmcache.
 */

static int _label_scan_async(struct cmd_context *cmd, int skip_cached, int *nomem)
{
	struct label_read_data *ld;
	struct dev_iter *iter;
	struct device *dev;
	struct lvmcache_info *info;
	int buf_len;
	int async_event_count;
	int try_sync_count = 0;
	int scan_failed_count = 0;
	int pre_alloc_count;
	int dev_count = 0;
	int available = 0;

	_free_label_read_list(cmd);

	buf_len = _get_scan_size(cmd);

	async_event_count = find_config_tree_int(cmd, devices_async_events_CFG, NULL);

	/*
	 * if aio setup fails, caller will revert to sync scan
	 * The number of events set up here is the max number of
	 * concurrent async reads that can be submitted.  After
	 * all of those are used, we revert to synchronous reads.
	 */
	if (!_ac) {
		if (!(_ac = dev_async_context_setup(async_event_count, 0, 0, buf_len))) {
			log_debug_devs("async io setup error, trying sync io.");
			return 0;
		}
	}

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_debug_devs("Scanning all devs async.");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning failed to get devices.");
		dev_async_context_destroy(_ac);
		_ac = NULL;
		return 0;
	}

	/*
	 * allocate label_read_data struct for each device
	 *
	 * FIXME: dev_iter_get opens/closes each dev once or twice when applying filters,
	 * once to check the size, and a second time (in some cases) to check the block size.
	 */
	while ((dev = dev_iter_get(iter))) {
		/*
		 * FIXME: fix code so it's not scanning labels when it's not needed,
		 * then stuff like this can be removed.
		 */
		if (skip_cached && (info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
			log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
			continue;
        	}

		/*
		 * FIXME: mem pool code doesn't work for this, probably because
		 * of the posix_memalign below.  Try using mem pool to allocate
		 * all the ld structs first, then allocate all aio and aio->buf.
		 */
		if (!(ld = dm_zalloc(sizeof(struct label_read_data)))) {
			log_error("No memory for label buffer in async label scan.");
			dev_iter_destroy(iter);
			goto bad;
		}

		ld->io_started = 0;
		ld->io_done = 0;
		ld->try_sync = 0;
		ld->process_done = 0;
		ld->dev = dev;
		dm_list_add(&_label_read_list, &ld->list);
		dev_count++;
	};
	dev_iter_destroy(iter);

	if (_ac->max_ios && (_ac->max_ios < async_event_count))
		log_warn("Scanning cache limit (%d devices) is less than async events (%d).",
			 _ac->max_ios, async_event_count);

	if (_ac->max_ios && (_ac->max_ios < dev_count))
		log_warn("Scanning cache limit (%d devices) is less than number of devices (%d).",
			 _ac->max_ios, dev_count);


	/*
	 * We need at least async_event_count aio structs/buffers, but we may
	 * not need dev_count of them.  dev_count may include any number of
	 * non-lvm devices.  We need one aio struct for every dev while it is
	 * being scanned, and we keep one for every lvm dev we find.
	 *
	 * We may want avoid attempting async scanning if the available
	 * aio buffer mem is not enough to support the async_event_count
	 * and the full async_event_count is needed for the dev_count.
	 */
	if (dev_count < async_event_count)
		pre_alloc_count = dev_count;
	else
		pre_alloc_count = async_event_count;

	dev_async_alloc_ios(_ac, pre_alloc_count, buf_len, &available);

	_label_scan_list_async(cmd, &_label_read_list, async_event_count, buf_len,
			       &try_sync_count, &scan_failed_count);

	if (try_sync_count) {
		log_debug_devs("Scanning trying sync on %d devs.", try_sync_count);
		_label_scan_list_sync(cmd, &_label_read_list, buf_len, &scan_failed_count);
	}

	/* There's no point in keeping ld structs that have no ld->aio/buf of cached data. */
	_prune_label_read_list(cmd);

	if (scan_failed_count)
		log_debug_devs("Scanned %d devs %d failed.", dev_count, scan_failed_count);
	else
		log_debug_devs("Scanned %d devs.", dev_count);
	return 1;

 bad:
	/*
	 * This is the error path for memory limitation.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("async full scan failed");
	*nomem = 1;

	_free_label_read_list(cmd);
	dev_async_free_ios(_ac);
	dev_async_context_destroy(_ac);
	_ac = NULL;
	return 0;
}

/*
 * Read or reread label/metadata from selected devs (async).
 *
 * Reads and looks at label_header, pv_header, pv_header_extension,
 * mda_header, raw_locns, vg metadata from each device.
 *
 * Effect is populating lvmcache with latest info/vginfo (PV/VG) data
 * from the devs.  If a scanned device does not have a label_header,
 * its info is removed from lvmcache.
 */

static int _label_scan_devs_async(struct cmd_context *cmd, struct dm_list *devs, int *nomem)
{
	struct dm_list tmp_label_read_list;
	struct label_read_data *ld, *ld2;
	struct device_list *devl;
	struct device *dev;
	int buf_len;
	int async_event_count;
	int try_sync_count = 0;
	int scan_failed_count = 0;
	int dev_count = 0;

	buf_len = _get_scan_size(cmd);

	async_event_count = find_config_tree_int(cmd, devices_async_events_CFG, NULL);

	dm_list_init(&tmp_label_read_list);

	log_debug_devs("Scanning devs async.");

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		if (!(ld = get_label_read_data(cmd, dev))) {
			if (!(ld = dm_zalloc(sizeof(struct label_read_data)))) {
				log_error("No memory for label buffer in async label scan.");
				goto_bad;
			}

			ld->buf_len = buf_len;
			ld->dev = dev;
		} else {
			/* Temporarily move structs being reread onto local list. */
			dm_list_del(&ld->list);
		}

		ld->io_started = 0;
		ld->io_done = 0;
		ld->process_done = 0;
		ld->try_sync = 0;
		dm_list_add(&tmp_label_read_list, &ld->list);
		dev_count++;
	}

	_label_scan_list_async(cmd, &tmp_label_read_list, async_event_count, buf_len,
			       &try_sync_count, &scan_failed_count);

	if (try_sync_count) {
		log_debug_devs("Scanning trying sync on %d devs.", try_sync_count);
		_label_scan_list_sync(cmd, &tmp_label_read_list, buf_len, &scan_failed_count);
	}

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	/* There's no point in keeping ld structs that have no ld->aio/buf of cached data. */
	_prune_label_read_list(cmd);

	if (scan_failed_count) {
		log_debug_devs("Scanned %d devs %d failed.", dev_count, scan_failed_count);
		return 0;
	} else {
		log_debug_devs("Scanned %d devs.", dev_count);
		return 1;
	}

 bad:
	/*
	 * This is the error path when mem for ld structs isn't available.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("async devs scan failed.");
	*nomem = 1;

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	_free_label_read_list(cmd);
	dev_async_free_ios(_ac);
	dev_async_context_destroy(_ac);
	_ac = NULL;
	return 0;
}

/*
 * Scan labels/metadata for all devices (sync)
 *
 * Reads and looks at label_header, pv_header, pv_header_extension,
 * mda_header, raw_locns, vg metadata from each device.
 *
 * Effect is populating lvmcache with latest info/vginfo (PV/VG) data
 * from the devs.  If a scanned device does not have a label_header,
 * its info is removed from lvmcache.
 */

static int _label_scan_sync(struct cmd_context *cmd, int skip_cached, int *nomem)
{
	struct label_read_data *ld;
	struct dev_iter *iter;
	struct device *dev;
	struct lvmcache_info *info;
	char *buf;
	int buf_len;
	int dev_count = 0;
	int scan_failed_count = 0;
	int mem_limit = 0;

	if (!_sync_io_setup) {
		_sync_io_setup = 1;
		_sync_io_max = 0;
	}

	_free_label_read_list(cmd);

	buf_len = _get_scan_size(cmd);

	/* common buf to use for reading a dev when no per-dev buffers are available */
	if (!(buf = dm_malloc(buf_len))) {
		log_error("No memory to scan devices.");
		return 0;
	}

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_very_verbose("Scanning all devs sync.");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning failed to get devices.");
		return 0;
	}

	while ((dev = dev_iter_get(iter))) {
		if (skip_cached && (info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
			log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
			continue;
        	}

		if (!(ld = dm_zalloc(sizeof(struct label_read_data)))) {
			log_error("No memory for label buffer in sync label scan.");
			dev_iter_destroy(iter);
			goto_bad;
		}

		ld->dev = dev;
		ld->buf_len = buf_len;

		dm_list_add(&_label_read_list, &ld->list);
		dev_count++;
	}
	dev_iter_destroy(iter);

	/* Do the sync i/o on each dev. */

	dm_list_iterate_items(ld, &_label_read_list) {
		log_debug_devs("Reading sectors from device %s sync", dev_name(ld->dev));

		if (!dev_open_readonly(ld->dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(ld->dev));
			ld->io_done = 1;
			ld->process_done = 1;
			ld->result = -1;
			scan_failed_count++;
			continue;
		}

		if (!(ld->buf = _sync_io_get(buf_len))) {
			ld->common_buf = 1;
			ld->buf = buf;
			memset(buf, 0, buf_len);
			mem_limit++;
		}

		if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf)) {
			log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
			ld->result = -1;
			if (!ld->common_buf)
				_ld_io_put(ld);
			scan_failed_count++;
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
			/* process() will free ld->buf if the dev is not lvm's */
			ld->result = ld->buf_len;
			_label_read_data_process(cmd, ld);
		}
		ld->io_done = 1;
		ld->process_done = 1;
		dev_close(ld->dev);

		if (ld->common_buf) {
			ld->common_buf = 0;
			ld->buf = NULL;
		}
	}

	if (mem_limit)
		log_debug_devs("Scanning mem limit for %d devs.", mem_limit);

	/* There's no point in keeping ld structs that have no ld->aio/buf of cached data. */
	_prune_label_read_list(cmd);

	dm_free(buf);

	if (scan_failed_count)
		log_debug_devs("Scanned %d devs %d failed.", dev_count, scan_failed_count);
	else
		log_debug_devs("Scanned %d devs.", dev_count);
	return 1;

 bad:
	/*
	 * This is the error path when mem for ld structs isn't available.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("sync full scan failed.");
	*nomem = 1;

	_free_label_read_list(cmd);
	dm_free(buf);
	return_0;
}

/*
 * Read or reread label/metadata from selected devs (sync).
 *
 * Reads and looks at label_header, pv_header, pv_header_extension,
 * mda_header, raw_locns, vg metadata from each device.
 *
 * Effect is populating lvmcache with latest info/vginfo (PV/VG) data
 * from the devs.  If a scanned device does not have a label_header,
 * its info is removed from lvmcache.
 */

static int _label_scan_devs_sync(struct cmd_context *cmd, struct dm_list *devs, int *nomem)
{
	struct dm_list tmp_label_read_list;
	struct label_read_data *ld, *ld2;
	struct device_list *devl;
	struct device *dev;
	char *buf;
	int buf_len;
	int dev_count = 0;
	int scan_failed_count = 0;

	buf_len = _get_scan_size(cmd);

	/* common buf to use for reading a dev when no per-dev buffers are available */
	if (!(buf = dm_malloc(buf_len))) {
		log_error("No memory to scan devices.");
		return 0;
	}

	dm_list_init(&tmp_label_read_list);

	log_debug_devs("Scanning devs sync.");

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		if (!(ld = get_label_read_data(cmd, dev))) {
			if (!(ld = dm_zalloc(sizeof(struct label_read_data)))) {
				log_error("No memory for label buffer in sync label scan.");
				goto_bad;
			}

			ld->buf_len = buf_len;
			ld->dev = dev;
		} else {
			/* Temporarily move structs being reread onto local list. */
			dm_list_del(&ld->list);
		}

		ld->io_done = 0;
		ld->process_done = 0;
		dm_list_add(&tmp_label_read_list, &ld->list);
		dev_count++;
	}

	dm_list_iterate_items(ld, &tmp_label_read_list) {
		log_debug_devs("Reading sectors from device %s sync", dev_name(ld->dev));

		if (!dev_open_readonly(ld->dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(ld->dev));
			_ld_io_put(ld);
			ld->io_done = 1;
			ld->process_done = 1;
			ld->result = -1;
			scan_failed_count++;
			continue;
		}

		if (!ld->buf) {
			if (!(ld->buf = _sync_io_get(buf_len))) {
				ld->common_buf = 1;
				ld->buf = buf;
				memset(buf, 0, buf_len);
			}
		}

		if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf)) {
			log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
			ld->result = -1;
			if (!ld->common_buf)
				_ld_io_put(ld);
			scan_failed_count++;
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
			/* process() will free ld->buf if the dev is not lvm's */
			ld->result = ld->buf_len;
			_label_read_data_process(cmd, ld);
		}
		ld->io_done = 1;
		ld->process_done = 1;
		dev_close(ld->dev);

		if (ld->common_buf) {
			ld->common_buf = 0;
			ld->buf = NULL;
		}
	}

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	/* There's no point in keeping ld structs that have no ld->aio/buf of cached data. */
	_prune_label_read_list(cmd);

	dm_free(buf);

	if (scan_failed_count) {
		log_debug_devs("Scanned %d devs %d failed.", dev_count, scan_failed_count);
		return 0;
	} else {
		log_debug_devs("Scanned %d devs.", dev_count);
		return 1;
	}

 bad:
	/*
	 * This is the error path when mem for ld structs isn't available.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("sync devs scan failed.");
	*nomem = 1;

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	_free_label_read_list(cmd);
	dm_free(buf);
	return_0;
}

static int _label_scan_limit(struct cmd_context *cmd, int skip_cached)
{
	struct dev_iter *iter;
	struct device *dev;
	struct lvmcache_info *info;
	struct label_read_data ld;
	char *buf;
	int buf_len;
	int scan_failed_count = 0;
	int dev_count = 0;

	_free_label_read_list(cmd);

	buf_len = _get_scan_size(cmd);

	if (!(buf = dm_malloc(buf_len))) {
		log_error("No memory to scan devices.");
		return 0;
	}

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_debug_devs("Scanning all devs sync-limit.");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning data failed to get devices.");
		return 0;
	}

	while ((dev = dev_iter_get(iter))) {
		if (skip_cached && (info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
			log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
			continue;
        	}

		log_debug_devs("Reading sectors from device %s sync-limit", dev_name(dev));

		if (!dev_open_readonly(dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(dev));
			scan_failed_count++;
			continue;
		}

		memset(&ld, 0, sizeof(ld));
		memset(buf, 0, buf_len);

		ld.dev = dev;
		ld.buf = buf;
		ld.buf_len = buf_len;
		ld.result = buf_len;

		if (!dev_read(dev, 0, buf_len, buf)) {
			log_debug_devs("%s: Failed to read label area", dev_name(dev));
			scan_failed_count++;
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(dev));
			_label_read_data_process(cmd, &ld);
		}
		dev_close(dev);
		dev_count++;
	}
	dev_iter_destroy(iter);

	if (scan_failed_count)
		log_debug_devs("Scanned %d devs %d failed.", dev_count, scan_failed_count);
	else
		log_debug_devs("Scanned %d devs.", dev_count);
        return 1;
}

static int _label_scan_devs_limit(struct cmd_context *cmd, struct dm_list *devs)
{
	struct label_read_data ld;
	struct device_list *devl;
	struct device *dev;
	char *buf;
	int buf_len;
	int dev_count = 0;
	int scan_failed_count = 0;

	buf_len = _get_scan_size(cmd);

	if (!(buf = dm_malloc(buf_len))) {
		log_error("No memory to scan devices.");
		return 0;
	}

	log_debug_devs("Scanning devs sync-limit.");

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		log_debug_devs("Reading sectors from device %s sync-limit", dev_name(dev));

		if (!dev_open_readonly(dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(dev));
			scan_failed_count++;
			continue;
		}

		memset(&ld, 0, sizeof(ld));
		memset(buf, 0, buf_len);

		ld.dev = dev;
		ld.buf = buf;
		ld.buf_len = buf_len;
		ld.result = buf_len;
		ld.common_buf = 1;

		if (!dev_read(dev, 0, buf_len, buf)) {
			log_debug_devs("%s: Failed to read label area", dev_name(dev));
			scan_failed_count++;
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(dev));
			_label_read_data_process(cmd, &ld);
		}
		dev_close(dev);
		dev_count++;
	}

	if (scan_failed_count) {
		log_debug_devs("Scanned %d devs %d failed.", dev_count, scan_failed_count);
		return 0;
	} else {
		log_debug_devs("Scanned %d devs.", dev_count);
		return 1;
	}
}

/*
 * FIXME: get rid of the force variations by making label_scan
 * never skip scanning when info is cached.
 * _force versions don't skip scanning label when info exists
 * in lvmcache.
 */

int label_scan_force(struct cmd_context *cmd)
{
	int ret = 0;
	int nomem = 0;

	if (cmd->use_aio)
		ret = _label_scan_async(cmd, 0, &nomem);

	if (!ret && !nomem)
		ret = _label_scan_sync(cmd, 0, &nomem);

	if (!ret)
		ret = _label_scan_limit(cmd, 0);

	return ret;
}

int label_scan(struct cmd_context *cmd)
{
	int ret = 0;
	int nomem = 0;

	if (cmd->use_aio)
		ret = _label_scan_async(cmd, 1, &nomem);

	if (!ret && !nomem)
		ret = _label_scan_sync(cmd, 1, &nomem);

	if (nomem)
		ret = _label_scan_limit(cmd, 1);

	return ret;
}

int label_scan_devs(struct cmd_context *cmd, struct dm_list *devs)
{
	int ret = 0;
	int nomem = 0;

	if (cmd->use_aio && _ac)
		ret = _label_scan_devs_async(cmd, devs, &nomem);

	if (!ret && !nomem)
		ret = _label_scan_devs_sync(cmd, devs, &nomem);

	if (nomem)
		ret = _label_scan_devs_limit(cmd, devs);

	return ret;
}

void label_scan_destroy(struct cmd_context *cmd)
{
	_free_label_read_list(cmd);
	if (_ac) {
		dev_async_free_ios(_ac);
		dev_async_context_destroy(_ac);
		_ac = NULL;
	}
}

