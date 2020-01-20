/*
 * Copyright (C) 2014-2015 Red Hat, Inc. All rights reserved.
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
#include "lib/metadata/metadata.h"
#include "lib/locking/locking.h"
#include "lib/misc/lvm-string.h"
#include "lib/commands/toolcontext.h"
#include "lib/display/display.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/config/defaults.h"
#include "lib/activate/dev_manager.h"

#define DEFAULT_TAG_SIZE 4 /* bytes */
#define DEFAULT_MODE 'B'
#define DEFAULT_INTERNAL_HASH "crc32c"

#define ONE_MB_IN_BYTES 1048576

int lv_is_integrity_origin(const struct logical_volume *lv)
{
	struct seg_list *sl;

	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		if (!sl->seg || !sl->seg->lv || !sl->seg->origin)
			continue;
		if (lv_is_integrity(sl->seg->lv) && (sl->seg->origin == lv))
			return 1;
	}
	return 0;
}

static struct logical_volume *_lv_integrity_from_origin(const struct logical_volume *lv)
{
	struct seg_list *sl;

	dm_list_iterate_items(sl, &lv->segs_using_this_lv) {
		if (!sl->seg || !sl->seg->lv || !sl->seg->origin)
			continue;
		if (lv_is_integrity(sl->seg->lv) && (sl->seg->origin == lv))
			return sl->seg->lv;
	}
	return NULL;
}

/*
 * Every 500M of data needs 4M of metadata.
 * (From trial and error testing.)
 */
static uint64_t _lv_size_bytes_to_integrity_meta_bytes(uint64_t lv_size_bytes)
{
	return ((lv_size_bytes / (500 * ONE_MB_IN_BYTES)) + 1) * (4 * ONE_MB_IN_BYTES);
}

/*
 * The user wants external metadata, but did not specify an existing
 * LV to hold metadata, so create an LV for metadata.
 */
static int _lv_create_integrity_metadata(struct cmd_context *cmd,
				struct volume_group *vg,
				struct lvcreate_params *lp,
				struct logical_volume **meta_lv)
{
	char metaname[NAME_LEN];
	uint64_t lv_size_bytes, meta_bytes, meta_sectors;
	struct logical_volume *lv;
	struct lvcreate_params lp_meta = {
		.activate = CHANGE_AN,
		.alloc = ALLOC_INHERIT,
		.major = -1,
		.minor = -1,
		.permission = LVM_READ | LVM_WRITE,
		.pvh = &vg->pvs,
		.read_ahead = DM_READ_AHEAD_NONE,
		.stripes = 1,
		.vg_name = vg->name,
		.zero = 0,
		.wipe_signatures = 0,
		.suppress_zero_warn = 1,
	};

	if (lp->lv_name &&
	    dm_snprintf(metaname, NAME_LEN, "%s_imeta", lp->lv_name) < 0) {
		log_error("Failed to create metadata LV name.");
		return 0;
	}

	lp_meta.lv_name = metaname;
	lp_meta.pvh = lp->pvh;

	lv_size_bytes = (uint64_t)lp->extents * (uint64_t)vg->extent_size * 512;
	meta_bytes = _lv_size_bytes_to_integrity_meta_bytes(lv_size_bytes);
	meta_sectors = meta_bytes / 512;
	lp_meta.extents = meta_sectors / vg->extent_size;

	log_print("Creating integrity metadata LV %s with size %s.",
		  metaname, display_size(cmd, meta_sectors));

	dm_list_init(&lp_meta.tags);

	if (!(lp_meta.segtype = get_segtype_from_string(vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (!(lv = lv_create_single(vg, &lp_meta))) {
		log_error("Failed to create integrity metadata LV");
		return 0;
	}

	*meta_lv = lv;
	return 1;
}

static int _get_provided_data_sectors(struct logical_volume *lv, uint64_t *provided_data_sectors)
{
	struct lv_with_info_and_seg_status status;
	uint64_t data_sectors, extra_sectors;

	memset(&status, 0, sizeof(status));
	status.seg_status.type = SEG_STATUS_NONE;

	status.seg_status.seg = first_seg(lv);

	/* FIXME: why reporter_pool? */
	if (!(status.seg_status.mem = dm_pool_create("reporter_pool", 1024))) {
		log_error("Failed to get mem for LV status.");
		return 0;
	}

	if (!lv_info_with_seg_status(lv->vg->cmd, first_seg(lv), &status, 1, 1)) {
		log_error("Failed to get device mapper status for %s", display_lvname(lv));
		goto fail;
	}

	if (!status.info.exists) {
		log_error("No device mapper info exists for %s", display_lvname(lv));
		goto fail;
	}

	if (status.seg_status.type != SEG_STATUS_INTEGRITY) {
		log_error("Invalid device mapper status type (%d) for %s",
			  (uint32_t)status.seg_status.type, display_lvname(lv));
		goto fail;
	}

	data_sectors = status.seg_status.integrity->provided_data_sectors;

	if ((extra_sectors = (data_sectors % lv->vg->extent_size))) {
		data_sectors -= extra_sectors;
		log_debug("Reduce provided_data_sectors by %llu to %llu for extent alignment",
			  (unsigned long long)extra_sectors, (unsigned long long)data_sectors);
	}

	*provided_data_sectors = data_sectors;

	dm_pool_destroy(status.seg_status.mem);
	return 1;

fail:
	dm_pool_destroy(status.seg_status.mem);
	return 0;
}

/*
 * lv_iorig has been extended, now extend the integrity layer
 * above it.
 */
int lv_extend_integrity_for_origin(struct logical_volume *lv_iorig,
				   struct dm_list *pvh, const char *meta_name)
{
	struct cmd_context *cmd = lv_iorig->vg->cmd;
	struct volume_group *vg = lv_iorig->vg;
	const struct segment_type *segtype;
	struct logical_volume *lv;
	struct logical_volume *lv_imeta;
	struct lv_segment *seg;
	struct device *meta_dev;
	struct dm_list *use_pvh = pvh;
	uint64_t lv_size_bytes, meta_bytes, meta_sectors, prev_meta_sectors;
	uint32_t meta_extents, prev_meta_extents;

	if (meta_name) {
		if (!(meta_dev = dev_cache_get(cmd, meta_name, NULL))) {
			log_error("integritymetadata device not found %s.", meta_name);
			return 0;
		}
		if (!(use_pvh = create_pv_list(cmd->mem, vg, 1, (char **)&meta_name, 1))) {
			log_error("integritymetadata not found in VG %s.", meta_name);
			return 0;
		}
	}

	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (!(lv = _lv_integrity_from_origin(lv_iorig)))
		return_0;

	seg = first_seg(lv);

	lv_imeta = seg->integrity_meta_dev;

	/*
	 * Shouldn't happen because we prevent extending an LV with
	 * internal integrity.  To allow that, we'd need to find out
	 * the new provided_data_sectors to set in the metadata.
	 * That would require doing a temporary activation of the
	 * extended LV with provided_data_sectors=1 and then check
	 * what the kernel reports.
	 */
	if (!lv_imeta) {
		log_error("Cannot extend with internal integrity.");
		return_0;
	}

	/* new size in sectors */
	lv->size = lv_iorig->size;
	seg->integrity_data_sectors = lv_iorig->size;
	/* new size in extents */
	lv->le_count = lv_iorig->le_count;
	seg->len = lv_iorig->le_count;
	seg->area_len = lv_iorig->le_count;

	lv_size_bytes = lv_iorig->size * 512;
	meta_bytes = _lv_size_bytes_to_integrity_meta_bytes(lv_size_bytes);
	meta_sectors = meta_bytes / 512;
	meta_extents = meta_sectors / vg->extent_size;

	prev_meta_sectors = lv_imeta->size;
	prev_meta_extents = prev_meta_sectors / vg->extent_size;

	if (meta_extents <= prev_meta_extents) {
		log_debug("extend not needed for imeta LV %s", lv_imeta->name);
		return 1;
	}

	if (!lv_extend(lv_imeta, segtype, 1, 0, 0, 0,
		       meta_extents - prev_meta_extents,
		       use_pvh, lv_imeta->alloc, 0, NULL)) {
		log_error("Failed to extend integrity metadata LV %s", lv_imeta->name);
		return 0;
	}

	return 1;
}

int lv_extend_integrity_in_raid(struct logical_volume *lv,
				struct dm_list *pvh, const char *meta_name)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	const struct segment_type *segtype;
	struct lv_segment *seg_top, *seg_image;
	struct logical_volume *lv_image;
	struct logical_volume *lv_iorig;
	struct logical_volume *lv_imeta;
	struct device *meta_dev;
	struct dm_list *use_pvh = pvh;
	uint64_t lv_size_bytes, meta_bytes, meta_sectors, prev_meta_sectors;
	uint32_t meta_extents, prev_meta_extents;
	uint32_t area_count, s;

	if (meta_name) {
		if (!(meta_dev = dev_cache_get(cmd, meta_name, NULL))) {
			log_error("integritymetadata device not found %s.", meta_name);
			return 0;
		}
		if (!(use_pvh = create_pv_list(cmd->mem, vg, 1, (char **)&meta_name, 1))) {
			log_error("integritymetadata not found in VG %s.", meta_name);
			return 0;
		}
	}

	seg_top = first_seg(lv);
		                
	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	area_count = seg_top->area_count;

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);

		if (!(lv_imeta = seg_image->integrity_meta_dev)) {
			log_error("LV %s segment has no integrity metadata device.", display_lvname(lv));
			return 0;
		}

		if (!(lv_iorig = seg_lv(seg_image, 0))) {
			log_error("LV %s integrity segment has no origin", display_lvname(lv));
			return 0;
		}

		lv_size_bytes = lv_iorig->size * 512;
		meta_bytes = _lv_size_bytes_to_integrity_meta_bytes(lv_size_bytes);
		meta_sectors = meta_bytes / 512;
		meta_extents = meta_sectors / vg->extent_size;

		prev_meta_sectors = lv_imeta->size;
		prev_meta_extents = prev_meta_sectors / vg->extent_size;

		if (meta_extents <= prev_meta_extents) {
			log_debug("extend not needed for imeta LV %s", lv_imeta->name);
			continue;
		}

		if (!lv_extend(lv_imeta, segtype, 1, 0, 0, 0,
			       meta_extents - prev_meta_extents,
			       use_pvh, lv_imeta->alloc, 0, NULL)) {
			log_error("Failed to extend raid image integrity metadata LV %s", lv_imeta->name);
			return 0;
		}
	}

	return 1;
}

int lv_remove_integrity_from_raid(struct logical_volume *lv)
{
	struct logical_volume *iorig_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct logical_volume *imeta_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct lv_segment *seg_top, *seg_image;
	struct logical_volume *lv_image;
	struct logical_volume *lv_iorig;
	struct logical_volume *lv_imeta;
	uint32_t area_count, s;
	int is_active = lv_is_active(lv);

	seg_top = first_seg(lv);

	if (!seg_is_raid1(seg_top) && !seg_is_raid4(seg_top) &&
	    !seg_is_any_raid5(seg_top) && !seg_is_any_raid6(seg_top)) {
		log_error("LV %s segment is unsupported raid for integrity.", display_lvname(lv));
		return 0;
	}

	area_count = seg_top->area_count;

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);

		if (!(lv_imeta = seg_image->integrity_meta_dev)) {
			log_error("LV %s segment has no integrity metadata device.", display_lvname(lv));
			return 0;
		}

		if (!(lv_iorig = seg_lv(seg_image, 0))) {
			log_error("LV %s integrity segment has no origin", display_lvname(lv));
			return 0;
		}

		if (!remove_seg_from_segs_using_this_lv(seg_image->integrity_meta_dev, seg_image))
			return_0;

		iorig_lvs[s] = lv_iorig;
		imeta_lvs[s] = lv_imeta;

		lv_image->status &= ~INTEGRITY;
		seg_image->integrity_meta_dev = NULL;
		seg_image->integrity_data_sectors = 0;
		memset(&seg_image->integrity_settings, 0, sizeof(seg_image->integrity_settings));

		if (!remove_layer_from_lv(lv_image, lv_iorig))
			return_0;
	}

	if (is_active) {
		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("Failed to update and reload LV after integrity remove.");
			return 0;
                }
	}

	for (s = 0; s < area_count; s++) {
		lv_iorig = iorig_lvs[s];
		lv_imeta = imeta_lvs[s];

		if (is_active) {
			if (!deactivate_lv(cmd, lv_iorig))
				log_error("Failed to deactivate unused iorig LV %s.", lv_iorig->name);

			if (!deactivate_lv(cmd, lv_imeta))
				log_error("Failed to deactivate unused imeta LV %s.", lv_imeta->name);
		}

		lv_imeta->status &= ~INTEGRITY_METADATA;
		lv_set_visible(lv_imeta);

		if (!lv_remove(lv_iorig))
			log_error("Failed to remove unused iorig LV %s.", lv_iorig->name);

		if (!lv_remove(lv_imeta))
			log_error("Failed to remove unused imeta LV %s.", lv_imeta->name);
	}

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	return 1;
}

int lv_remove_integrity(struct logical_volume *lv)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct lv_segment *seg = first_seg(lv);
	struct logical_volume *lv_iorig;
	struct logical_volume *lv_imeta;
	int is_active = lv_is_active(lv);

	if (!seg_is_integrity(seg)) {
		log_error("LV %s segment is not integrity.", display_lvname(lv));
		return 0;
	}

	if (!seg->integrity_meta_dev) {
		log_error("Internal integrity cannot be removed.");
		return 0;
	}

	if (!(lv_imeta = seg->integrity_meta_dev)) {
		log_error("LV %s segment has no integrity metadata device.", display_lvname(lv));
		return 0;
	}

	if (!(lv_iorig = seg_lv(seg, 0))) {
		log_error("LV %s integrity segment has no origin", display_lvname(lv));
		return 0;
	}

	if (!remove_seg_from_segs_using_this_lv(seg->integrity_meta_dev, seg))
		return_0;

	lv->status &= ~INTEGRITY;
	seg->integrity_meta_dev = NULL;

	if (!remove_layer_from_lv(lv, lv_iorig))
		return_0;

	if (is_active) {
		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("Failed to update and reload LV after integrity remove.");
			return_0;
		}

		if (!deactivate_lv(cmd, lv_iorig))
			log_error("Failed to deactivate unused iorig LV %s.", lv_iorig->name);

		if (!deactivate_lv(cmd, lv_imeta))
			log_error("Failed to deactivate unused imeta LV %s.", lv_iorig->name);
	}

	lv_imeta->status &= ~INTEGRITY_METADATA;
	lv_set_visible(lv_imeta);

	if (!lv_remove(lv_iorig))
		log_error("Failed to remove unused iorig LV %s.", lv_iorig->name);

	if (!lv_remove(lv_imeta))
		log_error("Failed to remove unused imeta LV %s.", lv_imeta->name);

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	return 1;
}

/*
 * Add integrity to each raid image.
 *
 * for each rimage_N:
 * . create and allocate a new linear LV rimage_N_imeta
 * . move the segments from rimage_N to a new rimage_N_iorig
 * . add an integrity segment to rimage_N with
 *   origin=rimage_N_iorig, meta_dev=rimage_N_imeta
 *
 * Before:
 * rimage_0
 *   segment1: striped: pv0:A
 * rimage_1
 *   segment1: striped: pv1:B
 *
 * After:
 * rimage_0
 *   segment1: integrity: rimage_0_iorig, rimage_0_imeta
 * rimage_1
 *   segment1: integrity: rimage_1_iorig, rimage_1_imeta
 * rimage_0_iorig
 *   segment1: striped: pv0:A
 * rimage_1_iorig
 *   segment1: striped: pv1:B
 * rimage_0_imeta
 *   segment1: striped: pv2:A
 * rimage_1_imeta
 *   segment1: striped: pv2:B
 *    
 */

int lv_add_integrity_to_raid(struct logical_volume *lv, const char *arg,
			     const char *meta_name,
			     struct integrity_settings *settings,
			     struct dm_list *pvh)
{
	struct lvcreate_params lp;
	struct logical_volume *imeta_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_image, *lv_imeta, *lv_iorig;
	struct lv_segment *seg_top, *seg_image;
	const struct segment_type *segtype;
	struct integrity_settings *set;
	struct device *meta_dev = NULL;
	struct dm_list *use_pvh = pvh;
	uint64_t status_data_sectors = 0;
	uint32_t area_count, s;
	int is_active;
	int external = 0, internal = 0;
	int ret = 1;

	memset(imeta_lvs, 0, sizeof(imeta_lvs));

	is_active = lv_is_active(lv);

	if (!arg || !strcmp(arg, "y") || !strcmp(arg, "external"))
		external = 1;
	else if (!strcmp(arg, "internal"))
		internal = 1;
	else {
		log_error("Invalid --integrity arg for lvcreate.");
		return 0;
	}

	if (meta_name) {
		if (!(meta_dev = dev_cache_get(cmd, meta_name, NULL))) {
			log_error("integritymetadata device not found %s.", meta_name);
			return 0;
		}
		if (!(use_pvh = create_pv_list(cmd->mem, vg, 1, (char **)&meta_name, 1))) {
			log_error("integritymetadata not found in VG %s.", meta_name);
			return 0;
		}
		meta_name = NULL;
	}

	if (dm_list_size(&lv->segments) != 1)
		return_0;

	seg_top = first_seg(lv);
	area_count = seg_top->area_count;

	if (!seg_is_raid1(seg_top) && !seg_is_raid4(seg_top) &&
	    !seg_is_any_raid5(seg_top) && !seg_is_any_raid6(seg_top)) {
		log_error("Integrity can only be added to raid1,4,5,6.");
		return 0;
	}

	if (internal) {
		/*
		 * FIXME: raid on internal integrity might not be used widely
		 * enough to enable, given the additional complexity/support.
		 * i.e. nearly everyone may just use external metadata.
		 *
		 * FIXME: to use internal with raid:
		 * _info_run() needs code to adjust the length, like
		 * is done for if (lv_is_integrity()) length = ...
		 * _raid_add_target_line() the len needs to be adjusted
		 * to integrity_data_sectors.
		 */
		/* goto skip_imeta; */
		log_error("Internal integrity metadata is not supported with raid.");
		return 0;
	}

	/*
	 * For each rimage, create an _imeta LV for integrity metadata.
	 * Each needs to be zeroed.
	 */
	for (s = 0; s < area_count; s++) {
		struct logical_volume *meta_lv;
		struct wipe_params wipe = { .do_zero = 1, .zero_sectors = 8 };

		if (s >= DEFAULT_RAID_MAX_IMAGES)
			return_0;

		lv_image = seg_lv(seg_top, s);

		if (!seg_is_striped(first_seg(lv_image))) {
			log_error("raid image must be linear to add integrity");
			return_0;
		}

		/*
		 * allocate a new linear LV NAME_rimage_N_imeta
		 */
		memset(&lp, 0, sizeof(lp));
		lp.lv_name = lv_image->name;
		lp.pvh = use_pvh;
		lp.extents = lv_image->size / vg->extent_size;

		if (!_lv_create_integrity_metadata(cmd, vg, &lp, &meta_lv))
			return_0;

		/*
		 * dm-integrity requires the metadata LV header to be zeroed.
		 */

		if (!activate_lv(cmd, meta_lv)) {
			log_error("Failed to activate LV %s to zero", display_lvname(meta_lv));
			return 0;
		}

		if (!wipe_lv(meta_lv, wipe)) {
			log_error("Failed to zero LV for integrity metadata %s", display_lvname(meta_lv));
			return 0;
		}

		if (!deactivate_lv(cmd, meta_lv)) {
			log_error("Failed to deactivate LV %s after zero", display_lvname(meta_lv));
			return 0;
		}

		/* Used below to set up the new integrity segment. */
		imeta_lvs[s] = meta_lv;
	}

/* skip_imeta: */

	/*
	 * For each rimage, move its segments to a new rimage_iorig and give
	 * the rimage a new integrity segment.
	 */
	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);

		if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_INTEGRITY)))
			return_0;

		log_debug("Adding integrity to raid image %s", lv_image->name);

		/*
		 * "lv_iorig" is a new LV with new id, but with the segments
		 * from "lv_image". "lv_image" keeps the existing name and id,
		 * but gets a new integrity segment, in place of the segments
		 * that were moved to lv_iorig.
		 */
		if (!(lv_iorig = insert_layer_for_lv(cmd, lv_image, INTEGRITY, "_iorig")))
			return_0;

		lv_image->status |= INTEGRITY;

		/*
		 * Set up the new first segment of lv_image as integrity.
		 */
		seg_image = first_seg(lv_image);
		seg_image->segtype = segtype;

		if (external) {
			lv_imeta = imeta_lvs[s]; /* external metadata lv created above */
			lv_imeta->status |= INTEGRITY_METADATA;
			lv_set_hidden(lv_imeta);
			seg_image->integrity_data_sectors = lv_image->size;
			seg_image->integrity_meta_dev = lv_imeta;
			seg_image->integrity_recalculate = 1;
		}

		memcpy(&seg_image->integrity_settings, settings, sizeof(struct integrity_settings));
		set = &seg_image->integrity_settings;

		if (!set->mode[0])
			set->mode[0] = DEFAULT_MODE;

		if (!set->tag_size)
			set->tag_size = DEFAULT_TAG_SIZE;

		if (!set->internal_hash)
			set->internal_hash = DEFAULT_INTERNAL_HASH;
	}

	/*
	 * When using internal metadata, we have to temporarily activate the
	 * integrity image with size 1 to get provided_data_sectors from the
	 * dm-integrity module.
	 */
	if (internal) {
		/* Get size from the first image, others will be the same. */
		lv_image = seg_lv(seg_top, 0);

		lv_image->status |= LV_TEMPORARY;
		lv_image->status |= LV_NOSCAN;
		seg_image = first_seg(lv_image);
		seg_image->integrity_data_sectors = 1;

		/* write-commit allows activating the LV to get data_sectors */
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Preliminary internal integrity write commit error");
			ret = 0;
			goto out;
		}

		log_debug("Activating temporary integrity LV to get data sectors.");

		if (!activate_lv(cmd, lv_image)) {
			log_error("Failed to activate temporary integrity.");
			ret = 0;
			goto out;
		}

		if (!_get_provided_data_sectors(lv_image, &status_data_sectors)) {
			log_error("Failed to get data sectors from dm-integrity");
			ret = 0;
		} else {
			log_debug("Found integrity provided_data_sectors %llu", (unsigned long long)status_data_sectors);
			ret = 1;
		}

		if (!status_data_sectors) {
			log_error("LV size too small to include metadata.");
			ret = 0;
		}

		if (!deactivate_lv(cmd, lv_image)) {
			log_error("Failed to deactivate temporary integrity.");
			ret = 0;
		}

		if (!ret)
			goto_out;

		lv_image->status &= ~LV_NOSCAN;
		lv_image->status &= ~LV_TEMPORARY;

		/* The main point, setting integrity_data_sectors. */
		for (s = 0; s < area_count; s++) {
			lv_image = seg_lv(seg_top, s);
			seg_image = first_seg(lv_image);
			seg_image->integrity_data_sectors = status_data_sectors;
			seg_image->integrity_recalculate = 1;
		}
	}

	if (is_active) {
		log_print("Writing VG and updating LV with new integrity LV %s", lv->name);

		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("LV update and reload failed");
			ret = 0;
			goto_out;
		}
	} else {
		log_debug("Writing VG with new integrity LV %s", lv->name);

		if (!vg_write(vg) || !vg_commit(vg)) {
			ret = 0;
			goto_out;
		}

		/*
		 * This first activation includes "recalculate" which starts the
		 * kernel's recalculating (initialization) process.
		 */

		log_print("Activating to start integrity initialization for LV %s", lv->name);

		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate integrity LV to initialize.");
			ret = 0;
			goto_out;
		}
	}

	/*
	 * Now that the device is being initialized, update the VG to clear
	 * integrity_recalculate so that subsequent activations will not
	 * include "recalculate" and restart initialization.
	 */

	log_print("Writing VG with initialized integrity LV %s", lv->name);

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);
		seg_image->integrity_recalculate = 0;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		ret = 0;
		goto_out;
	}

out:
	return ret;
}

int lv_add_integrity(struct logical_volume *lv, const char *arg,
		     const char *meta_name,
		     struct integrity_settings *settings,
		     struct dm_list *pvh)
{
	char imeta_name[NAME_LEN];
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct integrity_settings *segset;
	struct logical_volume *lv_orig;
	struct logical_volume *meta_lv = NULL;
	const struct segment_type *segtype;
	struct device *meta_dev = NULL;
	struct dm_list *use_pvh = pvh;
	struct lv_segment *seg;
	uint64_t lv_size_sectors;
	int is_active;
	int external = 0, internal = 0;
	int ret = 1;

	lv_size_sectors = lv->size;

	is_active = lv_is_active(lv);

	/*
	 * --integrity <arg> is y|external|internal
	 */

	if (!arg || !strcmp(arg, "y") || !strcmp(arg, "external"))
		external = 1;
	else if (!strcmp(arg, "internal"))
		internal = 1;
	else {
		log_error("Invalid --integrity arg for lvcreate.");
		return 0;
	}

	if (internal && meta_name) {
		log_error("Internal integrity cannot be used with integritymetadata option.");
		return 0;
	}

	if (meta_name) {
		if (!(meta_lv = find_lv(vg, meta_name))) {
			if (!(meta_dev = dev_cache_get(cmd, meta_name, NULL))) {
				log_error("integritymetadata device not found %s.", meta_name);
				return 0;
			}
			if (!(use_pvh = create_pv_list(cmd->mem, vg, 1, (char **)&meta_name, 1))) {
				log_error("integritymetadata not found in VG %s.", meta_name);
				return 0;
			}
			meta_name = NULL;
		}
	}

	if (external && !meta_lv) {
		struct lvcreate_params lp = { 0 };
		lp.lv_name = lv->name;
		lp.pvh = use_pvh;
		lp.extents = lv->size / vg->extent_size;

		if (!_lv_create_integrity_metadata(cmd, vg, &lp, &meta_lv))
			goto_out;

	} else if (external && meta_lv) {
		uint64_t meta_bytes, meta_sectors;

		meta_bytes = _lv_size_bytes_to_integrity_meta_bytes(lv_size_sectors * 512);
		meta_sectors = meta_bytes / 512;

		if (meta_lv->size < meta_sectors) {
			log_error("Integrity metadata needs %s, metadata LV is only %s.",
				 display_size(cmd, meta_sectors), display_size(cmd, meta_lv->size));
			return 0;
		}
	}

	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_INTEGRITY)))
		return_0;

	/*
	 * "lv_orig" is a new LV with new id, but with the segments from "lv".
	 * "lv" keeps the existing name and id, but gets a new integrity segment,
	 * in place of the segments that were moved to lv_orig.
	 */

	if (!(lv_orig = insert_layer_for_lv(cmd, lv, INTEGRITY, "_iorig")))
		return_0;

	seg = first_seg(lv);
	seg->segtype = segtype;

	lv->status |= INTEGRITY;

	memcpy(&seg->integrity_settings, settings, sizeof(struct integrity_settings));
	segset = &seg->integrity_settings;

	if (!segset->mode[0])
		segset->mode[0] = DEFAULT_MODE;

	if (!segset->tag_size)
		segset->tag_size = DEFAULT_TAG_SIZE;

	if (!segset->internal_hash)
		segset->internal_hash = DEFAULT_INTERNAL_HASH;

	/*
	 * When not using a metadata LV, dm-integrity needs to tell us what the
	 * usable size of the LV is, which is the dev size minus the integrity
	 * overhead.  To find that, we need to do a special, temporary activation
	 * of the new LV, specifying a dm dev size of 1, then check the dm dev
	 * status field provided_data_sectors, which is the actual size of the
	 * LV.  We need to include provided_data_sectors in the metadata for the
	 * new LV, and use this as the dm dev size for normal LV activation.
	 *
	 * When using a metadata LV, the dm dev size is the size of the data
	 * device.  The necessary size of the metadata LV for the given data
	 * device needs to be estimated.
	 */

	if (meta_lv) {
		struct wipe_params wipe = { .do_zero = 1, .zero_sectors = 8 };

		if (!sync_local_dev_names(cmd)) {
			log_error("Failed to sync local devices.");
			return 0;
		}

		log_verbose("Zeroing LV for integrity metadata");

		if (!lv_is_active(meta_lv)) {
			if (!activate_lv(cmd, meta_lv)) {
				log_error("Failed to activate LV %s to zero", display_lvname(meta_lv));
				return 0;
			}
		}

		if (!wipe_lv(meta_lv, wipe)) {
			log_error("Failed to zero LV for integrity metadata %s", display_lvname(meta_lv));
			return 0;
		}

		if (!deactivate_lv(cmd, meta_lv)) {
			log_error("Failed to deactivate LV %s after zero", display_lvname(meta_lv));
			return 0;
		}

		if (meta_name) {
			/* An existing LV was specified to use for metaddata. */
			/* LVM tradition to add a suffix to an existing LV when using it. */
			if (dm_snprintf(imeta_name, sizeof(imeta_name), "%s_imeta", meta_lv->name) < 0) {
				log_error("Can't prepare new imeta name for %s", display_lvname(meta_lv));
				return 0;
			}
			if (!lv_rename_update(cmd, meta_lv, imeta_name, 0))
				return_0;
		}

		meta_lv->status |= INTEGRITY_METADATA;
		seg->integrity_data_sectors = lv_size_sectors;
		seg->integrity_recalculate = 1;
		seg->integrity_meta_dev = meta_lv;
		lv_set_hidden(meta_lv);
	} else {
		/* dm-integrity wants temp/fake size of 1 to report usable size */
		seg->integrity_data_sectors = 1;
		seg->integrity_recalculate = 1;

		/* write-commit allows activating the LV to get data_sectors */
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Preliminary internal integrity write commit error");
			return 0;
		}

		lv->status |= LV_TEMPORARY;
		lv->status |= LV_NOSCAN;

		log_debug("Activating temporary integrity LV to get data sectors.");

		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate temporary integrity.");
			return 0;
		}

		if (!_get_provided_data_sectors(lv, &seg->integrity_data_sectors)) {
			log_error("Failed to get data sectors from dm-integrity");
			ret = 0;
		} else {
			log_debug("Found integrity provided_data_sectors %llu", (unsigned long long)seg->integrity_data_sectors);
			ret = 1;
		}

		if (!seg->integrity_data_sectors) {
			log_error("LV size too small to include metadata.");
			ret = 0;
		}

		if (!deactivate_lv(cmd, lv)) {
			log_error("Failed to deactivate temporary integrity.");
			ret = 0;
		}

		lv->status &= ~LV_NOSCAN;
		lv->status &= ~LV_TEMPORARY;

		if (!ret)
			goto_out;
	}

	if (is_active) {
		log_print("Writing VG and updating LV with new integrity LV %s", lv->name);

		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("LV update and reload failed");
			ret = 0;
			goto_out;
		}
	} else {
		log_print("Writing VG with new integrity LV %s", lv->name);

		if (!vg_write(vg) || !vg_commit(vg)) {
			ret = 0;
			goto_out;
		}

		/*
		 * This first activation includes "recalculate" which starts the
		 * kernel's recalculating (initialization) process.
		 */

		log_print("Activating to start integrity initialization for LV %s", lv->name);

		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate integrity LV to initialize.");
			ret = 0;
			goto_out;
		}
	}

	/*
	 * Now that the device is being initialized, update the VG to clear
	 * integrity_recalculate so that subsequent activations will not
	 * include "recalculate" and restart initialization.
	 *
	 * If command fails here, before clearing integrity_recalculate from
	 * the metadata, then the next activation will clear this
	 * (see lv_clear_integrity_recalculate_metadata).
	 */

	log_print("Writing VG with initialized integrity LV %s", lv->name);

	seg->integrity_recalculate = 0;

	if (!vg_write(vg) || !vg_commit(vg)) {
		ret = 0;
		goto_out;
	}

 out:
	return ret;
}

/*
 * This should rarely if ever be used.  A command that adds integrity
 * to an LV will activate and then clear the flag.  If it fails before
 * clearing the flag, then this function will be used by a subsequent
 * activation to clear the flag.
 */
void lv_clear_integrity_recalculate_metadata(struct logical_volume *lv)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);
			seg_image->integrity_recalculate = 0;
		}
	} else if (seg_is_integrity(seg)) {
		seg->integrity_recalculate = 0;
	} else {
		log_error("Invalid LV type for clearing integrity");
		return;
	}

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_warn("WARNING: failed to clear integrity recalculate flag for %s",
			 display_lvname(lv));
	}
}

int lv_has_integrity_recalculate_metadata(struct logical_volume *lv)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;
	int ret = 0;

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);

			if (!seg_is_integrity(seg_image))
				continue;
			if (seg_image->integrity_recalculate)
				ret = 1;
		}
	} else if (seg_is_integrity(seg)) {
		ret = seg->integrity_recalculate;
	}

	return ret;
}

int lv_has_integrity_internal(struct logical_volume *lv)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);

			if (!seg_is_integrity(seg_image))
				continue;
			if (!seg_image->integrity_meta_dev)
				return 1;
		}
	} else if (seg_is_integrity(seg)) {
		if (!seg->integrity_meta_dev)
			return 1;
	}
	return 0;
}

int lv_raid_has_integrity(struct logical_volume *lv)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);

			if (seg_is_integrity(seg_image))
				return 1;
		}
	}

	return 0;
}
