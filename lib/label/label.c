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
#include <libaio.h>

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
	if ((r = (l->ops->read)(l, dev, label_buf, labelp)) && *labelp) {
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

struct label_read_data {
	char *buf; /* 2K aligned memory buffer */
	struct iocb iocb;
	struct device *dev;
	struct dm_list list;
	int buf_len; /* LABEL_SCAN_SIZE */
	int try_sync;
	int read_done;
	int read_result;
	int process_done;
};

/*
 * Start label aio read on a device.
 */
static int _label_read_async_start(struct cmd_context *cmd, io_context_t aio_ctx, struct label_read_data *ld)
{
	struct iocb *iocb = &ld->iocb;
	int ret;

	iocb->data = ld;
	iocb->aio_fildes = dev_fd(ld->dev);
	iocb->aio_lio_opcode = IO_CMD_PREAD;
	iocb->u.c.buf = ld->buf;
	iocb->u.c.nbytes = ld->buf_len;
	iocb->u.c.offset = 0;

	ret = io_submit(aio_ctx, 1, &iocb);
	if (ret < 0)
		return 0;

	return 1;
}

#define MAX_GET_EVENTS 8

/*
 * Reap aio reads from devices.
 */
static int _label_read_async_wait(struct cmd_context *cmd, io_context_t aio_ctx, int wait_count)
{
	struct io_event events[MAX_GET_EVENTS];
	int wait_nr;
	int ret;
	int i;

 retry:
	memset(&events, 0, sizeof(events));

	if (wait_count >= MAX_GET_EVENTS)
		wait_nr = MAX_GET_EVENTS;
	else
		wait_nr = wait_count;

	ret = io_getevents(aio_ctx, 1, wait_nr, (struct io_event *)&events, NULL);
	if (ret == -EINTR)
		goto retry;
	if (ret < 0)
		return 0;
	if (!ret)
		return 1;

	for (i = 0; i < ret; i++) {
		struct iocb *iocb = events[i].obj;
		struct label_read_data *ld = iocb->data;
		ld->read_result = events[i].res;
		ld->read_done = 1;
	}

	return 1;
}

/*
 * Process / parse headers from buffer holding label header.
 * Populates lvmcache with device / mda locations / vgname
 * so that vg_read(vgname) will know which devices/locations
 * to read metadata from.
 */
static int _label_read_async_process(struct cmd_context *cmd, struct label_read_data *ld)
{
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label *label = NULL;
	struct labeller *l;
	uint64_t sector;
	int r = 0;

	if ((ld->read_result < 0) || (ld->read_result != ld->buf_len)) {
		/* FIXME: handle errors */
		log_error("Reading label sectors aio error %d from %s", ld->read_result, dev_name(ld->dev));
		goto out;
	}

	/*
	 * Finds the sector from scanbuf containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 */
	if (!(l = _find_label_header(ld->dev, ld->buf, label_buf, &sector, 0))) {
		/* Non-PVs exit here */
		/* FIXME: check for PVs with errors that also exit here. */
		goto_out;
	}

	/*
	 * ops->read() is usually _text_read() which reads
	 * the pv_header, mda locations, mda contents.
	 * It saves the info it finds into lvmcache info/vginfo structs.
	 */
	if ((r = (l->ops->read)(l, ld->dev, label_buf, &label)) && label) {
		label->dev = ld->dev;
		label->sector = sector;
	} else {
		/* FIXME: handle errors */
	}
 out:
	return r;
}

/*
 * label_scan iterates over all visible devices, looking
 * for any that belong to lvm, and fills lvmcache with
 * basic info about them.  It's main job is to prepare
 * for subsequent vg_reads.  vg_read(vgname) needs to
 * know which devices/locations to read metadata from
 * for the given vg name.  The label_scan has scanned
 * all devices and saved info in lvmcache about:
 * the device, the mda locations on that device,
 * and the vgname referenced in those mdas.  So,
 * vg_read(vgname) can map the vgname to a set of
 * mda locations it needs to read to get the metadata.
 */

int label_scan_async(struct cmd_context *cmd)
{
	struct dm_list label_read_list;
	struct label_read_data *ld, *ld2;
	struct dev_iter *iter;
	struct device *dev;
	io_context_t aio_ctx;
	struct lvmcache_info *info;
	char *buf;
	char **p_buf;
	int buf_len;
	int need_wait_count;
	int need_process_count;
	int dev_count = 0;
	int error;

	dm_list_init(&label_read_list);

	/*
	 * "buf" is the buffer into which the first four sectors
	 * of each device is read.
	 * (LABEL_SCAN_SIZE is four 512-byte sectors, i.e. 2K).
	 *
	 * The label is expected to be one of the first four sectors.
	 * buf needs to be aligned.
	 */
	buf_len = LABEL_SCAN_SIZE;

	/*
	 * if aio setup fails, caller will revert to sync scan
	 */
	memset(&aio_ctx, 0, sizeof(io_context_t));

	error = io_setup(128, &aio_ctx);
	if (error < 0) {
		log_debug_devs("async io setup error %d, reverting to sync io.", error);
		return_0;
	}

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_debug_devs("Scanning labels async");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning labels failed to get devices.");
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
		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
			log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
			continue;
        	}

		if (!dev_open_readonly(dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(dev));
			continue;
		}

		/*
		 * FIXME: mem pool code doesn't work for this, probably because
		 * of the posix_memalign below.  Try using mem pool to allocate
		 * all the ld structs first, then allocate all the aligned aio
		 * buffers.
		 */
		if (!(ld = malloc(sizeof(*ld))))
			goto_bad;

		memset(ld, 0, sizeof(*ld));

		buf = NULL;
		p_buf = &buf;

		if (posix_memalign((void *)p_buf, getpagesize(), buf_len))
			goto_bad;

		memset(buf, 0, buf_len);

		ld->dev = dev;
		ld->buf = buf;
		ld->buf_len = buf_len;

		dm_list_add(&label_read_list, &ld->list);
		dev_count++;
	};
	dev_iter_destroy(iter);

	/*
	 * Start the aio reads on each dev.  Flag any that
	 * fail and the next loop will try a sync read for it.
	 */
	dm_list_iterate_items(ld, &label_read_list) {
		if (!_label_read_async_start(cmd, aio_ctx, ld))
			ld->try_sync = 1;
		else
			log_debug_devs("Reading label sectors from device %s async", dev_name(ld->dev));
	}

	/*
	 * Try a synchronous read for any dev where aio couldn't be submitted.
	 */
	dm_list_iterate_items(ld, &label_read_list) {
		if (ld->try_sync) {
			log_debug_devs("Reading label sectors from device %s async", dev_name(ld->dev));

			if (!dev_read(ld->dev, 0, ld->buf_len, ld->buf)) {
				log_debug_devs("%s: Failed to read label area", dev_name(ld->dev));
				ld->read_result = -1;
			} else {
				ld->read_result = ld->buf_len;
			}
			ld->read_done = 1;
		}
	}

	/*
	 * Reap the aio and process the results.
	 */

 check_aio:
	need_wait_count = 0;
	need_process_count = 0;

	dm_list_iterate_items(ld, &label_read_list) {
		if (!ld->read_done)
			need_wait_count++;
		else if (!ld->process_done)
			need_process_count++;
	}

	/*
	 * Process devices that have finished reading label sectors.
	 * Processing includes sync i/o to read mda locations and vg metadata.
	 */
	if (need_process_count) {
		dm_list_iterate_items(ld, &label_read_list) {
			if (!ld->read_done || ld->process_done)
				continue;
			log_debug_devs("Parsing label and data from device %s", dev_name(ld->dev));
			_label_read_async_process(cmd, ld);
			ld->process_done = 1;
		}
	}

	/*
	 * Wait for more devices to finish reading label sectors.
	 */
	if (need_wait_count) {
		if (_label_read_async_wait(cmd, aio_ctx, need_wait_count))
			goto check_aio;

		/* TODO: handle this error */
		/* an error getting aio events, should we fall back
		   to doing sync dev_read() on any that aren't done? */
		log_error(INTERNAL_ERROR "aio getevents error");
	}

	io_destroy(aio_ctx);

	dm_list_iterate_items_safe(ld, ld2, &label_read_list) {
		dm_list_del(&ld->list);
		dev_close(ld->dev);
		if (ld->buf)
			free(ld->buf);
		free(ld);
	}

	log_debug_devs("Scanned %d labels async", dev_count);
	return 1;

bad:
	/* caller will try sync scan */
	log_error("async label scan failed, reverting to sync scan.");

	dev_iter_destroy(iter);
	io_destroy(aio_ctx);

	dm_list_iterate_items_safe(ld, ld2, &label_read_list) {
		dm_list_del(&ld->list);
		dev_close(ld->dev);
		if (ld->buf)
			free(ld->buf);
		free(ld);
	}
	return 0;
}

static int _label_read_sync(struct cmd_context *cmd, struct device *dev)
{
	char scanbuf[LABEL_SCAN_SIZE] __attribute__((aligned(8)));
	char label_buf[LABEL_SIZE] __attribute__((aligned(8)));
	struct label *label = NULL;
	struct labeller *l;
	uint64_t sector;
	int r = 0;

	memset(scanbuf, 0, sizeof(scanbuf));

	log_debug_devs("Reading label sectors from device %s", dev_name(dev));

	/*
	 * Read first four sectors into scanbuf.
	 */
	if (!dev_read(dev, 0, LABEL_SCAN_SIZE, scanbuf)) {
		log_debug_devs("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	log_debug_devs("Parsing label and data from device %s", dev_name(dev));

	/*
	 * Finds the sector from scanbuf containing the label and copies into label_buf.
	 * label_buf: struct label_header + struct pv_header + struct pv_header_extension
	 */
	if (!(l = _find_label_header(dev, scanbuf, label_buf, &sector, 0))) {
		/* FIXME: handle bad label */
		goto_out;
	}

	/*
	 * ops->read() is usually _text_read() which reads
	 * the pv_header, mda locations, mda contents.
	 * It saves the info it finds into lvmcache info/vginfo structs.
	 */
	if ((r = (l->ops->read)(l, dev, label_buf, &label)) && label) {
		label->dev = dev;
		label->sector = sector;
	} else {
		/* FIXME: handle errors */
	}
 out:
	return r;
}

/*
 * Read and process device labels/data without aio.
 */
int label_scan_sync(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct device *dev;
	int dev_count = 0;
	struct lvmcache_info *info;

	log_debug_devs("Finding devices to scan");

	dev_cache_full_scan(cmd->full_filter);

	log_very_verbose("Scanning labels sync");

	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning labels failed to get devices.");
		return 0;
	}

	while ((dev = dev_iter_get(iter))) {
		if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 1))) {
			log_debug_devs("Reading label skipped in cache %s", dev_name(dev));
			continue;
        	}

		if (!dev_open_readonly(dev)) {
			log_debug_devs("Reading label skipped can't open %s", dev_name(dev));
			continue;
		}

		_label_read_sync(cmd, dev);

		if (!dev_close(dev))
			stack;

		dev_count++;
	}
	dev_iter_destroy(iter);

	log_very_verbose("Scanned %d labels sync", dev_count);
	return 1;
}

