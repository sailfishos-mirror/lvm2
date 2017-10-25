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
			if (ld->aio) {
				dev_async_io_put(_ac, ld->aio);
				ld->aio = NULL;
				ld->buf = NULL;
			} else if (ld->buf)
				dm_free(ld->buf);
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

	dm_list_iterate_items(ld, &_label_read_list) {
		if (ld->aio) {
			dev_async_io_put(_ac, ld->aio);
			ld->aio = NULL;
			ld->buf = NULL;
		} else if (ld->buf) {
			/* when aio exists, ld->buf just points to aio->buf,
			   but when aio is not used, ld->buf is allocated. */
			dm_free(ld->buf);
		}
	}

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
static int _label_read_data_process(struct cmd_context *cmd, struct label_read_data *ld,
				    int mem_limit)
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

		/* In mem_limit mode, there is one static buffer used for all scanning. */
		if (mem_limit)
			goto_out;

		/*
		 * This device is not an lvm device, so we don't need to
		 * process it any further.  So we don't need to keep the
		 * buffers around for this dev.  Put the aio struct back
		 * so it can be reused for scanning another device.
		 */
		if (ld->aio) {
			dev_async_io_put(_ac, ld->aio);
			ld->aio = NULL;
			ld->buf = NULL;
		} else if (ld->buf) {
			dm_free(ld->buf);
			ld->buf = NULL;
		}
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
static int _label_read_async_start(struct dev_async_context *ac, struct label_read_data *ld)
{
	int nospace = 0;

	if (!dev_async_read_submit(ac, ld->aio, ld->dev, ld->buf_len, 0, &nospace)) {
		if (nospace)
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
static int _label_read_async_wait(struct dev_async_context *ac, int wait_count)
{
	return dev_async_getevents(ac, wait_count, NULL);
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
	int pre_alloc_count;
	int started_count;
	int try_sync_count;
	int need_wait_count;
	int need_process_count;
	int dev_count = 0;
	int available = 0;
	int async_event_count;
	int no_aio_bufs;

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

	log_debug_devs("Scanning data from all devs async");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning data failed to get devices.");
		dev_async_context_destroy(_ac);
		_ac = NULL;
		return 0;
	}

	/*
	 * allocate all the structs/mem for the aio
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

		ld->dev = dev;
		dm_list_add(&_label_read_list, &ld->list);
		dev_count++;
	};
	dev_iter_destroy(iter);

	/*
	 * We need at least async_event_count aio structs/buffers, but
	 * we may not need dev_count of them.  dev_count may include
	 * any number of non-lvm devices.  We need one aio struct for
	 * every dev while it is being scanned, and we need keep one
	 * for every lvm dev we find.
	 */
	if (dev_count < async_event_count)
		pre_alloc_count = dev_count;
	else
		pre_alloc_count = async_event_count;

	if (!dev_async_alloc_ios(_ac, pre_alloc_count, buf_len, &available)) {
		log_error("No memory for io buffer pre allocation in async label scan.");
		goto bad;
	}

	/*
	 * Start the aio reads on each dev.  Flag any that
	 * fail and the next loop will try a sync read for it.
	 *
	 * We only start up to the maximum number of aio events
	 * that the aio context is configured to handle.  After
	 * this is reached, we process all of the ld's that have
	 * been started, and then return here to start more.
	 *
	 * FIXME: handle case when "available" above is less
	 * than pre_alloc_count.  If "available" is a hard
	 * limit, and it is not enough for ever lvm device,
	 * then we would have to abort async scanning and
	 * revert to sync scan.
	 */
 start_more:
	started_count = 0;
	try_sync_count = 0;
	no_aio_bufs = 0;

	dm_list_iterate_items(ld, &_label_read_list) {
		if (ld->io_started)
			continue;

		/* the limit on the number of concurrent reads the aio context supports */
		if (started_count == async_event_count)
			break;

		if (!(ld->aio = dev_async_io_get(_ac, buf_len))) {
			no_aio_bufs = 1;
			break;
		}

		ld->buf = ld->aio->buf;
		ld->buf_len = buf_len;

		ld->io_started = 1;

		if (!dev_open_readonly(ld->dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(ld->dev));
			dev_async_io_put(_ac, ld->aio);
			ld->aio = NULL;
			ld->buf = NULL;
			ld->io_done = 1;
			ld->process_done = 1;
			ld->result = -1;
			continue;
		}

		if (!_label_read_async_start(_ac, ld)) {
			ld->try_sync = 1;
			try_sync_count++;
		} else {
			log_debug_devs("Reading sectors from device %s async", dev_name(ld->dev));
			started_count++;
		}
	}

	/*
	 * Only fail if there are no aio buffers to start more reads and no
	 * reads have already been started.  More aio bufs may become available
	 * after processing in progress reads.
	 */
	if (no_aio_bufs && !try_sync_count && !started_count) {
		log_error("No memory for io buffer in async label scan.");
		goto bad;
	}

	if (!try_sync_count)
		goto check_aio;

	/*
	 * Try a synchronous read for any dev where aio couldn't be submitted.
	 * Reuse the aio buffer, result and done fields.
	 */
	dm_list_iterate_items(ld, &_label_read_list) {
		/* Skip any ld that has not started or is finished. */
		if (!ld->io_started || ld->io_done)
			continue;

		if (!ld->try_sync)
			continue;

		log_debug_devs("Reading sectors from device %s trying sync", dev_name(ld->dev));

		if (!ld->aio) {
			log_error(INTERNAL_ERROR "label read data for %s has no aio.", dev_name(ld->dev));
			continue;
		}

		if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf))
			ld->aio->result = -1;
		else
			ld->aio->result = ld->buf_len;
		ld->aio->done = 1;

		/*
		 * Not setting ld->done / ld->result here to emulate the
		 * async transitions, so the loop below is responsible for
		 * transfering aio->done / aio->result to ld->done / ld->result.
		 */
	}

	/*
	 * Reap the aio completions and process the results.
	 */

 check_aio:
	need_wait_count = 0;
	need_process_count = 0;

	dm_list_iterate_items(ld, &_label_read_list) {
		if (!ld->io_started || ld->io_done)
			continue;

		if (!ld->aio) {
			log_error(INTERNAL_ERROR "label read data for %s has no aio.", dev_name(ld->dev));
			continue;
		}

		/*
		 * If there are ios started that are not done, we need to wait for more.
		 * If there are ios that are done but not processed, we need to process more.
		 */
		if (!ld->aio->done)
			need_wait_count++;
		else if (!ld->process_done)
			need_process_count++;
	}

	/*
	 * Process devices that have finished reading.
	 * Processing can include sync i/o to read metadata areas
	 * beyond the scan_size.
	 */
	if (need_process_count) {
		dm_list_iterate_items(ld, &_label_read_list) {
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
				log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
				dev_async_io_put(_ac, ld->aio);
				ld->aio = NULL;
				ld->buf = NULL;
			} else {
				log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
				/* process() will put ld->aio if the dev is not lvm's */
				_label_read_data_process(cmd, ld, 0);
			}

			ld->process_done = 1;
			dev_close(ld->dev);
		}
	}

	/*
	 * Wait for more devices to finish reading label sectors.
	 * (As a further optimization, we could submit more reads
	 * before all the current reads are finished.)
	 */
	if (need_wait_count) {
		if (_label_read_async_wait(_ac, need_wait_count))
			goto check_aio;

		/* TODO: handle this error */
		/* an error getting aio events, should we fall back
		   to doing sync dev_read() on any that aren't done? */
		log_error(INTERNAL_ERROR "aio getevents error");
	}

	dm_list_iterate_items(ld, &_label_read_list) {
		if (!ld->io_started) {
			log_debug_devs("Starting more async io.");
			goto start_more;
		}
	}

	/* remove ld structs for non-lvm devs (ld->aio already put) */
	_prune_label_read_list(cmd);

	log_debug_devs("Scanned all %d devs async", dev_count);
	return 1;

 bad:
	/*
	 * This is the error path for memory limitation.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("async full scan failed, trying sync scan.");
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
	int scan_failed_count = 0;
	int try_sync_count = 0;
	int need_wait_count;
	int need_process_count;
	int dev_count = 0;

	buf_len = _get_scan_size(cmd);

	dm_list_init(&tmp_label_read_list);

	log_debug_devs("Scanning data from devs async");

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		if (!(ld = get_label_read_data(cmd, dev))) {
			/* New device hasn't been scanned before. */

			if (!(ld = dm_zalloc(sizeof(struct label_read_data))))
				goto_bad;

			if (!(ld->aio = dev_async_io_get(_ac, buf_len))) {
				dm_free(ld);
				goto_bad;
			}

			ld->buf = ld->aio->buf;
			ld->buf_len = buf_len;
			ld->dev = dev;
		} else {
			if (!ld->aio) {
				log_error(INTERNAL_ERROR "Rescan device %s with no aio.", dev_name(ld->dev));
				goto_bad;
			}

			/* Temporarily move structs being reread onto local list. */
			dm_list_del(&ld->list);
		}

		dm_list_add(&tmp_label_read_list, &ld->list);
	}

	dm_list_iterate_items(ld, &tmp_label_read_list) {
		ld->try_sync = 0;
		ld->process_done = 0;
		ld->aio->done = 0;
		ld->io_done = 0;

		if (!dev_open_readonly(ld->dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(ld->dev));
			dev_async_io_put(_ac, ld->aio);
			ld->aio = NULL;
			ld->buf = NULL;
			ld->io_done = 1;
			ld->process_done = 1;
			ld->result = -1;
			scan_failed_count++;
			continue;
		}

		if (!_label_read_async_start(_ac, ld)) {
			ld->try_sync = 1;
			try_sync_count++;
		} else
			log_debug_devs("Reading sectors from device %s async", dev_name(ld->dev));

		dev_count++;
	}

	if (!try_sync_count)
		goto check_aio;

	/*
	 * Try a synchronous read for any dev where aio couldn't be submitted.
	 */
	dm_list_iterate_items(ld, &tmp_label_read_list) {
		if (ld->try_sync) {
			log_debug_devs("Reading sectors from device %s trying sync", dev_name(ld->dev));

			if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf))
				ld->aio->result = -1;
			else
				ld->aio->result = ld->buf_len;
			ld->aio->done = 1;

			/* Loop below sets ld->done / ld->result from aio fields. */
		}
	}

	/*
	 * Reap the aio and process the results.
	 */

 check_aio:
	need_wait_count = 0;
	need_process_count = 0;

	dm_list_iterate_items(ld, &tmp_label_read_list) {
		if (ld->io_done)
			continue;

		if (!ld->aio->done)
			need_wait_count++;
		else if (!ld->process_done)
			need_process_count++;
	}

	/*
	 * Process devices that have finished reading.
	 * Processing can include sync i/o to read metadata areas
	 * beyond the scan_size.
	 *
	 * FIXME: we shouldn't need to fully reprocess everything when rescanning.
	 * lvmcache is already populated from the previous scan, and if nothing
	 * has changed we don't need to repopulate it with the same data.
	 * Do something like check the metadata checksum from previous label scan
	 * and don't reprocess here if it's the same.
	 */
	if (need_process_count) {
		dm_list_iterate_items(ld, &tmp_label_read_list) {
			if (ld->process_done)
				continue;

			if (!ld->aio->done)
				continue;
			ld->io_done = 1;
			ld->result = ld->aio->result;

			if (!ld->result || (ld->result < 0)) {
				log_debug_devs("%s: Failed to read label data", dev_name(ld->dev));
				dev_async_io_put(_ac, ld->aio);
				ld->aio = NULL;
				ld->buf = NULL;
				scan_failed_count++;
			} else {
				log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
				/* process() will put ld->aio if the dev is not lvm's */
				_label_read_data_process(cmd, ld, 0);
			}

			ld->process_done = 1;
			dev_close(ld->dev);
		}
	}

	/*
	 * Wait for more devices to finish reading label sectors.
	 */
	if (need_wait_count) {
		if (_label_read_async_wait(_ac, need_wait_count))
			goto check_aio;

		/* TODO: handle this error */
		/* an error getting aio events, should we fall back
		   to doing sync dev_read() on any that aren't done? */
		log_error(INTERNAL_ERROR "aio getevents error");
	}

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	if (scan_failed_count) {
		log_debug_devs("Failed to scan data from %d devs async", scan_failed_count);
		return 0;
	}

	/* remove ld structs for non-lvm devs (ld->aio already put) */
	_prune_label_read_list(cmd);

	log_debug_devs("Scanned %d devs async", dev_count);
	return 1;

 bad:
	/*
	 * This is the error path for memory limitation.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("async devs scan failed, trying sync scan.");
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
	int buf_len;
	int dev_count = 0;

	_free_label_read_list(cmd);

	buf_len = _get_scan_size(cmd);

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_very_verbose("Scanning data from all devs sync");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning data failed to get devices.");
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

		ld->io_done = 0;
		ld->process_done = 0;

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
			continue;
		}

		if (!(ld->buf = dm_zalloc(buf_len))) {
			log_error("No memory for label data buffer in sync label scan.");
			dev_close(ld->dev);
			goto_bad;
		}

		if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf)) {
			log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
			ld->result = -1;
			dm_free(ld->buf);
			ld->buf = NULL;
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
			/* process() will free ld->buf if the dev is not lvm's */
			ld->result = ld->buf_len;
			_label_read_data_process(cmd, ld, 0);
		}
		ld->io_done = 1;
		ld->process_done = 1;
		dev_close(ld->dev);
	}

	/* remove ld structs for non-lvm devs (ld->buf already freed) */
	_prune_label_read_list(cmd);

	log_very_verbose("Scanned all %d devs sync", dev_count);
	return 1;

 bad:
	/*
	 * This is the error path for memory limitation.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("sync full scan failed, trying limited scan.");
	*nomem = 1;

	_free_label_read_list(cmd);
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
	int buf_len;
	int dev_count = 0;
	int scan_failed_count = 0;

	buf_len = _get_scan_size(cmd);

	dm_list_init(&tmp_label_read_list);

	log_debug_devs("Scanning data from devs sync");

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		if (!(ld = get_label_read_data(cmd, dev))) {
                        /* New device hasn't been scanned before. */

			if (!(ld = dm_zalloc(sizeof(struct label_read_data))))
				goto_bad;

			if (!(ld->buf = dm_zalloc(buf_len))) {
				dm_free(ld);
				goto_bad;
			}

			ld->buf_len = buf_len;
			ld->dev = dev;
		} else {
			if (!ld->buf) {
				log_error(INTERNAL_ERROR "Rescan device %s with no buf.", dev_name(dev));
				goto bad;
			}

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
			dm_free(ld->buf);
			ld->buf = NULL;
			ld->io_done = 1;
			ld->process_done = 1;
			ld->result = -1;
			scan_failed_count++;
			continue;
		}

		if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf)) {
			log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
			ld->result = -1;
			dm_free(ld->buf);
			ld->buf = NULL;
			scan_failed_count++;
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
			/* process() will free ld->buf if the dev is not lvm's */
			ld->result = ld->buf_len;
			_label_read_data_process(cmd, ld, 0);
		}
		ld->io_done = 1;
		ld->process_done = 1;
		dev_close(ld->dev);
	}

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	/* remove ld structs for non-lvm devs (ld->buf already freed) */
	_prune_label_read_list(cmd);

	if (scan_failed_count) {
		log_debug_devs("Failed to scan data from %d devs sync", scan_failed_count);
		return 0;
	}

	log_debug_devs("Scanned %d devs sync", dev_count);
	return 1;

 bad:
	/*
	 * This is the error path for memory limitation.
	 * Tell caller to try _limit variation which uses less mem.
	 */
	log_debug_devs("sync devs scan failed, trying limited scan.");
	*nomem = 1;

	/* Move structs being reread back to normal list */
	dm_list_iterate_items_safe(ld, ld2, &tmp_label_read_list) {
		dm_list_del(&ld->list);
		dm_list_add(&_label_read_list, &ld->list);
	}

	_free_label_read_list(cmd);
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
	int dev_count = 0;

	_free_label_read_list(cmd);

	buf_len = _get_scan_size(cmd);

	if (!(buf = dm_malloc(buf_len))) {
		log_error("No memory to scan devices.");
		return 0;
	}

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_very_verbose("Scanning data from all devs sync_limit");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning data failed to get devices.");
		return 0;
	}

	while ((dev = dev_iter_get(iter))) {
		if (skip_cached && (info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
			log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
			continue;
        	}

		log_debug_devs("Reading sectors from device %s sync_limit", dev_name(dev));

		if (!dev_open_readonly(dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(dev));
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
		} else {
			log_debug_devs("Parsing label and data from device %s", dev_name(dev));
			_label_read_data_process(cmd, &ld, 1);
		}
		dev_close(dev);
		dev_count++;
	}
	dev_iter_destroy(iter);

	log_very_verbose("Scanned all %d devs sync_limit", dev_count);
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

	log_debug_devs("Scanning data from devs sync_limit");

	dm_list_iterate_items(devl, devs) {
		dev = devl->dev;

		log_debug_devs("Reading sectors from device %s sync_limit", dev_name(dev));

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
			_label_read_data_process(cmd, &ld, 1);
		}
		dev_close(dev);
		dev_count++;
	}

	if (scan_failed_count) {
		log_debug_devs("Failed to scan data from %d devs sync_limit", scan_failed_count);
		return 0;
	}

	log_debug_devs("Scanned %d devs sync_limit", dev_count);
	return 1;
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

