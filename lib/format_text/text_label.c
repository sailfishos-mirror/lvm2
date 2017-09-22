/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
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
#include "format-text.h"
#include "layout.h"
#include "label.h"
#include "xlate.h"
#include "lvmcache.h"

#include <sys/stat.h>
#include <fcntl.h>

static int _text_can_handle(struct labeller *l __attribute__((unused)),
			    void *buf,
			    uint64_t sector __attribute__((unused)))
{
	struct label_header *lh = (struct label_header *) buf;

	if (!strncmp((char *)lh->type, LVM2_LABEL, sizeof(lh->type)))
		return 1;

	return 0;
}

struct _dl_setup_baton {
	struct disk_locn *pvh_dlocn_xl;
	struct device *dev;
};

static int _da_setup(struct disk_locn *da, void *baton)
{
	struct _dl_setup_baton *p = baton;
	p->pvh_dlocn_xl->offset = xlate64(da->offset);
	p->pvh_dlocn_xl->size = xlate64(da->size);
	p->pvh_dlocn_xl++;
	return 1;
}

static int _ba_setup(struct disk_locn *ba, void *baton)
{
	return _da_setup(ba, baton);
}

static int _mda_setup(struct metadata_area *mda, void *baton)
{
	struct _dl_setup_baton *p = baton;
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;

	if (mdac->area.dev != p->dev)
		return 1;

	p->pvh_dlocn_xl->offset = xlate64(mdac->area.start);
	p->pvh_dlocn_xl->size = xlate64(mdac->area.size);
	p->pvh_dlocn_xl++;

	return 1;
}

static int _dl_null_termination(void *baton)
{
	struct _dl_setup_baton *p = baton;

	p->pvh_dlocn_xl->offset = xlate64(UINT64_C(0));
	p->pvh_dlocn_xl->size = xlate64(UINT64_C(0));
	p->pvh_dlocn_xl++;

	return 1;
}

static int _text_write(struct label *label, void *buf)
{
	struct label_header *lh = (struct label_header *) buf;
	struct pv_header *pvhdr;
	struct pv_header_extension *pvhdr_ext;
	struct lvmcache_info *info;
	struct _dl_setup_baton baton;
	char buffer[64] __attribute__((aligned(8)));
	int ba1, da1, mda1, mda2;

	/*
	 * PV header base
	 */
	/* FIXME Move to where label is created */
	strncpy(label->type, LVM2_LABEL, sizeof(label->type));

	strncpy((char *)lh->type, label->type, sizeof(label->type));

	pvhdr = (struct pv_header *) ((char *) buf + xlate32(lh->offset_xl));
	info = (struct lvmcache_info *) label->info;
	pvhdr->device_size_xl = xlate64(lvmcache_device_size(info));
	memcpy(pvhdr->pv_uuid, &lvmcache_device(info)->pvid, sizeof(struct id));
	if (!id_write_format((const struct id *)pvhdr->pv_uuid, buffer,
			     sizeof(buffer))) {
		stack;
		buffer[0] = '\0';
	}

	baton.dev = lvmcache_device(info);
	baton.pvh_dlocn_xl = &pvhdr->disk_areas_xl[0];

	/* List of data areas (holding PEs) */
	lvmcache_foreach_da(info, _da_setup, &baton);
	_dl_null_termination(&baton);

	/* List of metadata area header locations */
	lvmcache_foreach_mda(info, _mda_setup, &baton);
	_dl_null_termination(&baton);

	/*
	 * PV header extension
	 */
	pvhdr_ext = (struct pv_header_extension *) ((char *) baton.pvh_dlocn_xl);
	pvhdr_ext->version = xlate32(PV_HEADER_EXTENSION_VSN);
	pvhdr_ext->flags = xlate32(lvmcache_ext_flags(info));

	/* List of bootloader area locations */
	baton.pvh_dlocn_xl = &pvhdr_ext->bootloader_areas_xl[0];
	lvmcache_foreach_ba(info, _ba_setup, &baton);
	_dl_null_termination(&baton);

	/* Create debug message with ba, da and mda locations */
	ba1 = (xlate64(pvhdr_ext->bootloader_areas_xl[0].offset) ||
	       xlate64(pvhdr_ext->bootloader_areas_xl[0].size)) ? 0 : -1;

	da1 = (xlate64(pvhdr->disk_areas_xl[0].offset) ||
	       xlate64(pvhdr->disk_areas_xl[0].size)) ? 0 : -1;

	mda1 = da1 + 2;
	mda2 = mda1 + 1;
	
	if (!xlate64(pvhdr->disk_areas_xl[mda1].offset) &&
	    !xlate64(pvhdr->disk_areas_xl[mda1].size))
		mda1 = mda2 = 0;
	else if (!xlate64(pvhdr->disk_areas_xl[mda2].offset) &&
		 !xlate64(pvhdr->disk_areas_xl[mda2].size))
		mda2 = 0;

	log_debug_metadata("%s: Preparing PV label header %s size %" PRIu64 " with"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s"
			   "%s%.*" PRIu64 "%s%.*" PRIu64 "%s",
			   dev_name(lvmcache_device(info)), buffer, lvmcache_device_size(info),
			   (ba1 > -1) ? " ba1 (" : "",
			   (ba1 > -1) ? 1 : 0,
			   (ba1 > -1) ? xlate64(pvhdr_ext->bootloader_areas_xl[ba1].offset) >> SECTOR_SHIFT : 0,
			   (ba1 > -1) ? "s, " : "",
			   (ba1 > -1) ? 1 : 0,
			   (ba1 > -1) ? xlate64(pvhdr_ext->bootloader_areas_xl[ba1].size) >> SECTOR_SHIFT : 0,
			   (ba1 > -1) ? "s)" : "",
			   (da1 > -1) ? " da1 (" : "",
			   (da1 > -1) ? 1 : 0,
			   (da1 > -1) ? xlate64(pvhdr->disk_areas_xl[da1].offset) >> SECTOR_SHIFT : 0,
			   (da1 > -1) ? "s, " : "",
			   (da1 > -1) ? 1 : 0,
			   (da1 > -1) ? xlate64(pvhdr->disk_areas_xl[da1].size) >> SECTOR_SHIFT : 0,
			   (da1 > -1) ? "s)" : "",
			   mda1 ? " mda1 (" : "",
			   mda1 ? 1 : 0,
			   mda1 ? xlate64(pvhdr->disk_areas_xl[mda1].offset) >> SECTOR_SHIFT : 0,
			   mda1 ? "s, " : "",
			   mda1 ? 1 : 0,
			   mda1 ? xlate64(pvhdr->disk_areas_xl[mda1].size) >> SECTOR_SHIFT : 0,
			   mda1 ? "s)" : "",
			   mda2 ? " mda2 (" : "",
			   mda2 ? 1 : 0,
			   mda2 ? xlate64(pvhdr->disk_areas_xl[mda2].offset) >> SECTOR_SHIFT : 0,
			   mda2 ? "s, " : "",
			   mda2 ? 1 : 0,
			   mda2 ? xlate64(pvhdr->disk_areas_xl[mda2].size) >> SECTOR_SHIFT : 0,
			   mda2 ? "s)" : "");

	if (da1 < 0) {
		log_error(INTERNAL_ERROR "%s label header currently requires "
			  "a data area.", dev_name(lvmcache_device(info)));
		return 0;
	}

	return 1;
}

int add_da(struct dm_pool *mem, struct dm_list *das,
	   uint64_t start, uint64_t size)
{
	struct data_area_list *dal;

	if (!mem) {
		if (!(dal = dm_malloc(sizeof(*dal)))) {
			log_error("struct data_area_list allocation failed");
			return 0;
		}
	} else {
		if (!(dal = dm_pool_alloc(mem, sizeof(*dal)))) {
			log_error("struct data_area_list allocation failed");
			return 0;
		}
	}

	dal->disk_locn.offset = start;
	dal->disk_locn.size = size;

	dm_list_add(das, &dal->list);

	return 1;
}

void del_das(struct dm_list *das)
{
	struct dm_list *dah, *tmp;
	struct data_area_list *da;

	dm_list_iterate_safe(dah, tmp, das) {
		da = dm_list_item(dah, struct data_area_list);
		dm_list_del(&da->list);
		dm_free(da);
	}
}

int add_ba(struct dm_pool *mem, struct dm_list *eas,
	   uint64_t start, uint64_t size)
{
	return add_da(mem, eas, start, size);
}

void del_bas(struct dm_list *bas)
{
	del_das(bas);
}

/* FIXME: refactor this function with other mda constructor code */
int add_mda(const struct format_type *fmt, struct dm_pool *mem, struct dm_list *mdas,
	    struct device *dev, uint64_t start, uint64_t size, unsigned ignored)
{
/* FIXME List size restricted by pv_header SECTOR_SIZE */
	struct metadata_area *mdal;
	struct mda_lists *mda_lists = (struct mda_lists *) fmt->private;
	struct mda_context *mdac;

	if (!mem) {
		if (!(mdal = dm_malloc(sizeof(struct metadata_area)))) {
			log_error("struct mda_list allocation failed");
			return 0;
		}

		if (!(mdac = dm_malloc(sizeof(struct mda_context)))) {
			log_error("struct mda_context allocation failed");
			dm_free(mdal);
			return 0;
		}
	} else {
		if (!(mdal = dm_pool_alloc(mem, sizeof(struct metadata_area)))) {
			log_error("struct mda_list allocation failed");
			return 0;
		}

		if (!(mdac = dm_pool_alloc(mem, sizeof(struct mda_context)))) {
			log_error("struct mda_context allocation failed");
			return 0;
		}
	}

	mdal->ops = mda_lists->raw_ops;
	mdal->metadata_locn = mdac;
	mdal->status = 0;

	mdac->area.dev = dev;
	mdac->area.start = start;
	mdac->area.size = size;
	mdac->free_sectors = UINT64_C(0);
	memset(&mdac->rlocn, 0, sizeof(mdac->rlocn));
	mda_set_ignored(mdal, ignored);

	dm_list_add(mdas, &mdal->list);
	return 1;
}

void del_mdas(struct dm_list *mdas)
{
	struct dm_list *mdah, *tmp;
	struct metadata_area *mda;

	dm_list_iterate_safe(mdah, tmp, mdas) {
		mda = dm_list_item(mdah, struct metadata_area);
		dm_free(mda->metadata_locn);
		dm_list_del(&mda->list);
		dm_free(mda);
	}
}

static int _text_initialise_label(struct labeller *l __attribute__((unused)),
				  struct label *label)
{
	strncpy(label->type, LVM2_LABEL, sizeof(label->type));

	return 1;
}

struct _mda_baton {
	struct lvmcache_info *info;
	struct label *label;
	struct label_read_data *ld;
	unsigned int fail_count;
	unsigned int ignore_count;
	unsigned int success_count;
};

/*
 * The return value from this function is not the result;
 * the success/failure of reading metadata is tracked by
 * baton fields.
 *
 * If this function returns 0, no further mdas are read.
 * If this function returns 1, other mdas are read.
 */

static int _read_mda_header_and_metadata_summary(struct metadata_area *mda, void *baton)
{
	struct _mda_baton *p = baton;
	const struct format_type *fmt = p->label->labeller->fmt;
	struct mda_context *mdac = (struct mda_context *) mda->metadata_locn;
	struct mda_header *mdah;
	struct raw_locn *rl;
	int rl_count = 0;
	uint32_t failed_flags = 0;
	struct lvmcache_vgsummary vgsummary = { 0 };

	if (!dev_open_readonly(mdac->area.dev)) {
		log_error("Can't open device %s to read metadata from mda.", dev_name(mdac->area.dev));
		mda->read_failed_flags |= FAILED_INTERNAL;
		p->fail_count++;
		return 1;
	}

	/*
	 * read mda_header
	 *
	 * metadata_area/mda_context/device_area (mda/mdac/area)
	 * is the incore method of getting to the struct disk_locn
	 * that followed the struct pv_header and points to the
	 * struct mda_header.
	 */
	if (!(mdah = raw_read_mda_header(fmt, &mdac->area, p->ld, &failed_flags))) {
		log_debug_metadata("MDA header on %s at %"PRIu64" is not valid.",
				   dev_name(mdac->area.dev), mdac->area.start);
		mda->read_failed_flags |= failed_flags;
		p->fail_count++;
		goto out;
	}

	mda_set_ignored(mda, rlocn_is_ignored(mdah->raw_locns));

	if (mda_is_ignored(mda)) {
		log_debug_metadata("MDA header on %s at %"PRIu64" has ignored metdata.",
				   dev_name(mdac->area.dev), mdac->area.start);
		p->ignore_count++;
		goto out;
	} else {
		rl = &mdah->raw_locns[0];
		while (rl->offset) {
			rl_count++;
			rl++;
		}
		log_debug_metadata("MDA header on %s at %"PRIu64" has %d metadata locations.",
				   dev_name(mdac->area.dev), mdac->area.start, rl_count);
	}

	if (!rl_count)
		goto out;

	/*
	 * read vg metadata, but saves only a few fields from it in vgsummary
	 * (enough to create vginfo/info connections in lvmcache)
	 *
	 * FIXME: this should be unified with the very similar function
	 * that reads metadata for vg_read(): read_metadata_location_vg().
	 *
	 * (This is not using the checksum optimization because a new/zeroed
	 * vgsummary struct is passed for each area, and read_metadata_location
	 * decides to use the checksum optimization based on whether or not
	 * the vgsummary.vgname is already set.)
	 */
	if (!read_metadata_location_summary(fmt, mdah, p->ld, &mdac->area, &vgsummary, &mdac->free_sectors, &failed_flags)) {
		/* A more specific error has been logged prior to returning. */
		log_debug_metadata("Metadata location on %s returned no VG summary.", dev_name(mdac->area.dev));
		mda->read_failed_flags |= failed_flags;
		p->fail_count++;
		goto out;
	}

	/*
	 * Each copy of the metadata for a VG will call this to save the
	 * VG summary fields in lvmcache.  When the checksum optimization
	 * is used, this is a waste of time and could be skipped, because
	 * we'd just be passing the same data again.  When the metadata
	 * checksum doesn't match between copies, we should set a flag
	 * in the vginfo.  Or, if we are not using the checksum optimization,
	 * then we might be passing a metadata summary that doesn't match
	 * the previous metadata summary (we should include the seqno in
	 * the summary to make that clearer.)  If the summaries from
	 * devices doesn't match, we should also set a flag in the vginfo,
	 * as suggested above for mismatching checksums.  Later, the vg_read
	 * code could look for the vginfo flag indicating mismatching
	 * checksums or summary fields (including seqno), and if all the
	 * copies matched, it could simply reuse the label_read_data
	 * from the original scan, and avoid label_scan_devs to rescan.
	 * This optimization would only apply to reporting commands,
	 * but could reduce scanning to one read per device in the
	 * common case.
	 */
	if (!lvmcache_update_vgname_and_id(p->info, &vgsummary)) {
		log_debug_metadata("Metadata summary on %s cannot be saved in lvmcache.", dev_name(mdac->area.dev));
		mda->read_failed_flags |= FAILED_INTERNAL;
		p->fail_count++;
		goto out;
	}

	log_debug_metadata("Metadata summary on %s found for VG %s.", dev_name(mdac->area.dev), vgsummary.vgname);
	p->success_count++;
out:
	if (!dev_close(mdac->area.dev))
		stack;

	return 1;
}

/*
 * When label_read_data *ld is set, it means that we have read the first
 * ld->buf_len bytes of the device and already have that data, so we don't need
 * to do any dev_read's (as long as the desired dev_read offset+size is less
 * then ld->buf_len).
 */

static int _text_read(struct labeller *l, struct device *dev, void *label_buf,
		      struct label_read_data *ld, struct label **label,
		      uint32_t *failed_flags)
{
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	char uuid[64] __attribute__((aligned(8)));
	struct id pv_id_check;
	struct label_header *lh = (struct label_header *) label_buf;
	struct pv_header *pvhdr;
	struct pv_header_extension *pvhdr_ext;
	struct lvmcache_info *info;
	struct disk_locn *dlocn_xl;
	uint64_t offset;
	uint64_t device_size;
	uint32_t ext_version;
	uint32_t ext_flags;
	unsigned int data_area_count = 0;
	unsigned int meta_area_count = 0;
	int add_errors = 0;
	struct _mda_baton baton = { 0 };

	/*
	 * pv_header has uuid and device_size
	 *
	 * pv_header.disk_areas are two variable sequences of disk_locn's:
	 * . first null terminated sequence of disk_locn's are data areas
	 * . second null terminated sequence of disk_locn's are meta areas
	 *
	 * pv_header_extension has version and flags
	 *
	 * pv_header_extension.bootloader_areas is one set of disk_locn's:
	 * . null terminated sequence of disk_locn's are bootloader areas
	 *
	 * Step 1: look through structs to summarize for log message.
	 */
	pvhdr = (struct pv_header *) ((char *) label_buf + xlate32(lh->offset_xl));

	strncpy(pvid_s, (char *)pvhdr->pv_uuid, sizeof(pvid_s) - 1);
	pvid_s[sizeof(pvid_s) - 1] = '\0';

	if (!id_read_format_try(&pv_id_check, pvid_s)) {
		log_debug_metadata("PV header on %s uuid cannot be read.", dev_name(dev));
		*failed_flags |= FAILED_PV_HEADER;
		return_0;
	}

	if (!id_write_format((const struct id *)&pvid_s, uuid, sizeof(uuid))) {
		log_debug_metadata("PV header on %s uuid cannot be written.", dev_name(dev));
		*failed_flags |= FAILED_INTERNAL;
		return_0;
	}

	/*
	 * FIXME: check for invalid values of other pv_header fields.
	 */

	device_size = xlate64(pvhdr->device_size_xl);

	/* Data areas holding the PEs */
	dlocn_xl = pvhdr->disk_areas_xl;
	while ((offset = xlate64(dlocn_xl->offset))) {
		dlocn_xl++;
		data_area_count++;
	}

	/* Metadata area headers */
	dlocn_xl++;
	while ((offset = xlate64(dlocn_xl->offset))) {
		dlocn_xl++;
		meta_area_count++;
	}

	/* PV header extension */
	dlocn_xl++;
	pvhdr_ext = (struct pv_header_extension *) ((char *) dlocn_xl);
	ext_version = xlate32(pvhdr_ext->version);
	ext_flags = xlate32(pvhdr_ext->flags);

	log_debug_metadata("PV header on %s has device_size %llu uuid %s",
			   dev_name(dev), (unsigned long long)device_size, uuid);

	log_debug_metadata("PV header on %s has data areas %d metadata areas %d",
			   dev_name(dev), data_area_count, meta_area_count);

	log_debug_metadata("PV header on %s has extension version %u flags %x",
			   dev_name(dev), ext_version, ext_flags);

	/*
	 * Step 2: look through structs to populate lvmcache
	 * with pv_header/extension info for this device.
	 *
	 * An "info" struct represents a device in lvmcache
	 * and is created by lvmcache_add().  The info struct
	 * in lvmcache is not associated with any vginfo
	 * struct until the VG name is known from the summary.
	 *
	 * lvmcache_add() calls _create_info() which creates
	 * the label struct, saved at info->label.
	 * lvmcache_get_label(info) then returns info->label.
	 */
	if (!(info = lvmcache_add(l, (char *)pvhdr->pv_uuid, dev, FMT_TEXT_ORPHAN_VG_NAME, FMT_TEXT_ORPHAN_VG_NAME, 0))) {
		log_error("PV %s info cannot be saved in cache.", dev_name(dev));
		*failed_flags |= FAILED_INTERNAL;
		return 0;
	}

	/* get the label that lvmcache_add() created */
	if (!(*label = lvmcache_get_label(info))) {
		*failed_flags |= FAILED_INTERNAL;
		lvmcache_del(info);
		return_0;
	}

	lvmcache_set_device_size(info, device_size);

	lvmcache_del_das(info);
	lvmcache_del_mdas(info);
	lvmcache_del_bas(info);

	/*
	 * Following the struct pv_header on disk are two lists of
	 * struct disk_locn (each disk_locn is an offset+size pair).
	 * The first list points to areas of data blocks holding PEs.
	 * The list of disk_locn's is terminated by a disk_locn
	 * holding zeros.
	 */
	dlocn_xl = pvhdr->disk_areas_xl;
	while ((offset = xlate64(dlocn_xl->offset))) {
		if (!lvmcache_add_da(info, offset, xlate64(dlocn_xl->size)))
			add_errors++;
		dlocn_xl++;
	}

	/*
	 * Following the disk_locn structs for data areas is a
	 * series of disk_locn structs each pointing to a
	 * struct mda_header.  dlocn_xl->offset now points
	 * to the first mda_header on disk.
	 */
	dlocn_xl++;
	while ((offset = xlate64(dlocn_xl->offset))) {
		/* this is just a roundabout call to add_mda() */
		if (!lvmcache_add_mda(info, dev, offset, xlate64(dlocn_xl->size), 0))
			add_errors++;
		dlocn_xl++;
	}

	/* PV header extension */
	dlocn_xl++;
	pvhdr_ext = (struct pv_header_extension *) ((char *) dlocn_xl);

	/* version 0 doesn't support extension */
	if (!ext_version)
		goto mda_read;

	/* Extension version */
	lvmcache_set_ext_version(info, ext_version);

	/* Extension flags */
	lvmcache_set_ext_flags(info, ext_flags);

	/* Bootloader areas */
	dlocn_xl = pvhdr_ext->bootloader_areas_xl;
	while ((offset = xlate64(dlocn_xl->offset))) {
		if (!lvmcache_add_ba(info, offset, xlate64(dlocn_xl->size)))
			add_errors++;
		dlocn_xl++;
	}

	if (add_errors) {
		log_error("PV %s disk area info cannot be saved in cache.", dev_name(dev));
		*failed_flags |= FAILED_INTERNAL;
		lvmcache_del(info);
		return 0;
	}

mda_read:
	/*
	 * Step 3: read struct mda_header and vg metadata which is
	 * saved into lvmcache and used to create a vginfo struct
	 * and associate the vginfo with the info structs created
	 * above.  The locations on disk of the mda_header structs
	 * comes from the disk_locn structs above.  Those disk_locn
	 * structs are confusingly not used directly, but were saved
	 * into lvmcache as metadata_area+mda_context.  This means
	 * that instead of looking at disk_locn/offset+size, we look
	 * through a series of incore structs to get offset+size:
	 * metadata_area/mda_context/device_area/start+size.
	 * FIXME: get rid of these excessive abstractions.
	 */
	baton.info = info;
	baton.label = *label;
	baton.ld = ld;

	/*
	 * for each mda on info->mdas.  These metadata_area structs were
	 * created and added to info->mdas above by lvmcache_add_mda().
	 */
	lvmcache_foreach_mda(info, _read_mda_header_and_metadata_summary, &baton);

	/*
	 * Presumably the purpose of having multiple mdas on a device is
	 * to have a backup that can be used in case one is bad.
	 */
	if (baton.fail_count && baton.success_count) {
		log_warn("WARNING: Using device %s with mix of %d good and %d bad mdas.",
			 dev_name(dev), baton.success_count, baton.fail_count);
	}

	if (baton.success_count) {
		/* if any mda was successful, we ignore other failed mdas */
		log_debug_metadata("PV on %s has valid mda header and vg metadata.", dev_name(dev));
		lvmcache_make_valid(info);
		return 1;
	} else if (baton.fail_count) {
		/* FIXME: get failed_flags from mda->read_failed_flags */
		log_debug_metadata("PV on %s has valid mda header and invalid vg metadata.", dev_name(dev));
		*failed_flags |= FAILED_VG_METADATA;
		lvmcache_del(info);
		return 0;
	} else if (baton.ignore_count) {
		log_debug_metadata("PV on %s has valid mda header and ignored vg metadata.", dev_name(dev));
		lvmcache_make_valid(info);
		return 1;
	} else {
		/* no VG metadata */
		log_debug_metadata("PV on %s has valid mda header and unused vg metadata.", dev_name(dev));
		lvmcache_make_valid(info);
		return 1;
	}
}

static void _text_destroy_label(struct labeller *l __attribute__((unused)),
				struct label *label)
{
	struct lvmcache_info *info = (struct lvmcache_info *) label->info;

	lvmcache_del_mdas(info);
	lvmcache_del_das(info);
	lvmcache_del_bas(info);
}

static void _fmt_text_destroy(struct labeller *l)
{
	dm_free(l);
}

struct label_ops _text_ops = {
	.can_handle = _text_can_handle,
	.write = _text_write,
	.read = _text_read,
	.verify = _text_can_handle,
	.initialise_label = _text_initialise_label,
	.destroy_label = _text_destroy_label,
	.destroy = _fmt_text_destroy,
};

struct labeller *text_labeller_create(const struct format_type *fmt)
{
	struct labeller *l;

	if (!(l = dm_zalloc(sizeof(*l)))) {
		log_error("Couldn't allocate labeller object.");
		return NULL;
	}

	l->ops = &_text_ops;
	l->fmt = fmt;

	return l;
}
