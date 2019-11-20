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
#define DEFAULT_MODE 'J'
#define DEFAULT_INTERNAL_HASH "crc32c"
#define DEFAULT_BLOCK_SIZE 512

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

	log_print_unless_silent("Creating integrity metadata LV %s with size %s.",
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

int lv_extend_integrity_in_raid(struct logical_volume *lv, struct dm_list *pvh)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	const struct segment_type *segtype;
	struct lv_segment *seg_top, *seg_image;
	struct logical_volume *lv_image;
	struct logical_volume *lv_iorig;
	struct logical_volume *lv_imeta;
	struct dm_list allocatable_pvs;
	struct dm_list *use_pvh;
	uint64_t lv_size_bytes, meta_bytes, meta_sectors, prev_meta_sectors;
	uint32_t meta_extents, prev_meta_extents;
	uint32_t area_count, s;

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

		/*
		 * We only allow lv_imeta to exist on a single PV (for now),
		 * so the allocatable_pvs is the one PV currently used by
		 * lv_imeta.
		 */
		dm_list_init(&allocatable_pvs);

		if (!get_pv_list_for_lv(cmd->mem, lv_imeta, &allocatable_pvs)) {
			log_error("Failed to build list of PVs for extending %s.", display_lvname(lv_imeta));
			return 0;
		}

		use_pvh = &allocatable_pvs;

		if (!lv_extend(lv_imeta, segtype, 1, 0, 0, 0,
			       meta_extents - prev_meta_extents,
			       use_pvh, lv_imeta->alloc, 0)) {
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
	    !seg_is_any_raid5(seg_top) && !seg_is_any_raid6(seg_top) &&
	    !seg_is_any_raid10(seg_top)) {
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

int lv_add_integrity_to_raid(struct logical_volume *lv, struct integrity_settings *settings,
			     struct dm_list *pvh, struct logical_volume *lv_imeta_0)
{
	char imeta_name[NAME_LEN];
	char *imeta_name_dup;
	struct lvcreate_params lp;
	struct dm_list allocatable_pvs;
	struct logical_volume *imeta_lvs[DEFAULT_RAID_MAX_IMAGES];
	struct cmd_context *cmd = lv->vg->cmd;
	struct volume_group *vg = lv->vg;
	struct logical_volume *lv_image, *lv_imeta, *lv_iorig;
	struct lv_segment *seg_top, *seg_image;
	const struct segment_type *segtype;
	struct integrity_settings *set;
	struct dm_list *use_pvh = NULL;
	uint32_t area_count, s;
	uint32_t revert_meta_lvs = 0;
	int is_active;

	memset(imeta_lvs, 0, sizeof(imeta_lvs));

	is_active = lv_is_active(lv);

	if (dm_list_size(&lv->segments) != 1)
		return_0;

	if (!dm_list_empty(&lv->segs_using_this_lv)) {
		log_error("Integrity can only be added to top level raid LV.");
		return 0;
	}

	seg_top = first_seg(lv);
	area_count = seg_top->area_count;

	if (!seg_is_raid1(seg_top) && !seg_is_raid4(seg_top) &&
	    !seg_is_any_raid5(seg_top) && !seg_is_any_raid6(seg_top) &&
	    !seg_is_any_raid10(seg_top)) {
		log_error("Integrity can only be added to raid1,4,5,6,10.");
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
			goto_bad;

		lv_image = seg_lv(seg_top, s);

		/*
		 * This function is used to add integrity to new images added
		 * to the raid, in which case old images will already be
		 * integrity.
		 */
		if (seg_is_integrity(first_seg(lv_image)))
			continue;

		if (!seg_is_striped(first_seg(lv_image))) {
			log_error("raid image must be linear to add integrity");
			goto_bad;
		}

		/*
		 * Use an existing lv_imeta from previous linear+integrity LV.
		 * FIXME: is it guaranteed that lv_image_0 is the existing?
		 */
		if (!s && lv_imeta_0) {
			if (dm_snprintf(imeta_name, sizeof(imeta_name), "%s_imeta", lv_image->name) > 0) {
				if ((imeta_name_dup = dm_pool_strdup(vg->vgmem, imeta_name)))
					lv_imeta_0->name = imeta_name_dup;
			}
			imeta_lvs[0] = lv_imeta_0;
			continue;
		}

		dm_list_init(&allocatable_pvs);

		if (!get_pv_list_for_lv(cmd->mem, lv_image, &allocatable_pvs)) {
			log_error("Failed to build list of PVs for %s.", display_lvname(lv_image));
			goto_bad;
		}

		use_pvh = &allocatable_pvs;

		/*
		 * allocate a new linear LV NAME_rimage_N_imeta
		 */
		memset(&lp, 0, sizeof(lp));
		lp.lv_name = lv_image->name;
		lp.pvh = use_pvh;
		lp.extents = lv_image->size / vg->extent_size;

		if (!_lv_create_integrity_metadata(cmd, vg, &lp, &meta_lv))
			goto_bad;

		revert_meta_lvs++;

		/* Used below to set up the new integrity segment. */
		imeta_lvs[s] = meta_lv;

		/*
		 * dm-integrity requires the metadata LV header to be zeroed.
		 */

		if (!activate_lv(cmd, meta_lv)) {
			log_error("Failed to activate LV %s to zero", display_lvname(meta_lv));
			goto_bad;
		}

		if (!wipe_lv(meta_lv, wipe)) {
			log_error("Failed to zero LV for integrity metadata %s", display_lvname(meta_lv));
			if (deactivate_lv(cmd, meta_lv))
				log_error("Failed to deactivate LV %s after zero", display_lvname(meta_lv));
			goto_bad;
		}

		if (!deactivate_lv(cmd, meta_lv)) {
			log_error("Failed to deactivate LV %s after zero", display_lvname(meta_lv));
			goto_bad;
		}
	}

	/*
	 * For each rimage, move its segments to a new rimage_iorig and give
	 * the rimage a new integrity segment.
	 */
	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);

		/* Not adding integrity to this image. */
		if (!imeta_lvs[s])
			continue;

		if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_INTEGRITY)))
			goto_bad;

		log_debug("Adding integrity to raid image %s", lv_image->name);

		/*
		 * "lv_iorig" is a new LV with new id, but with the segments
		 * from "lv_image". "lv_image" keeps the existing name and id,
		 * but gets a new integrity segment, in place of the segments
		 * that were moved to lv_iorig.
		 */
		if (!(lv_iorig = insert_layer_for_lv(cmd, lv_image, INTEGRITY, "_iorig")))
			goto_bad;

		lv_image->status |= INTEGRITY;

		/*
		 * Set up the new first segment of lv_image as integrity.
		 */
		seg_image = first_seg(lv_image);
		seg_image->segtype = segtype;

		lv_imeta = imeta_lvs[s];
		lv_imeta->status |= INTEGRITY_METADATA;
		lv_set_hidden(lv_imeta);
		seg_image->integrity_data_sectors = lv_image->size;
		seg_image->integrity_meta_dev = lv_imeta;
		seg_image->integrity_recalculate = 1;

		memcpy(&seg_image->integrity_settings, settings, sizeof(struct integrity_settings));
		set = &seg_image->integrity_settings;

		if (!set->mode[0])
			set->mode[0] = DEFAULT_MODE;

		if (!set->tag_size)
			set->tag_size = DEFAULT_TAG_SIZE;

		if (!set->block_size)
			set->block_size = DEFAULT_BLOCK_SIZE;

		if (!set->internal_hash)
			set->internal_hash = DEFAULT_INTERNAL_HASH;
	}

	if (is_active) {
		log_debug("Writing VG and updating LV with new integrity LV %s", lv->name);

		/* vg_write(), suspend_lv(), vg_commit(), resume_lv() */
		if (!lv_update_and_reload(lv)) {
			log_error("LV update and reload failed");
			goto_bad;
		}
		revert_meta_lvs = 0;

	} else {
		log_debug("Writing VG with new integrity LV %s", lv->name);

		if (!vg_write(vg) || !vg_commit(vg))
			goto_bad;

		revert_meta_lvs = 0;

		/*
		 * This first activation includes "recalculate" which starts the
		 * kernel's recalculating (initialization) process.
		 */

		log_debug("Activating to start integrity initialization for LV %s", lv->name);

		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate integrity LV to initialize.");
			goto_bad;
		}
	}

	/*
	 * Now that the device is being initialized, update the VG to clear
	 * integrity_recalculate so that subsequent activations will not
	 * include "recalculate" and restart initialization.
	 */

	log_debug("Writing VG with initialized integrity LV %s", lv->name);

	for (s = 0; s < area_count; s++) {
		lv_image = seg_lv(seg_top, s);
		seg_image = first_seg(lv_image);
		seg_image->integrity_recalculate = 0;
	}

	if (!vg_write(vg) || !vg_commit(vg))
		goto_bad;

	return 1;

bad:
	log_error("Failed to add integrity.");

	for (s = 0; s < revert_meta_lvs; s++) {
		if (!lv_remove(imeta_lvs[s]))
			log_error("New integrity metadata LV may require manual removal.");
	}
			       
	if (!vg_write(vg) || !vg_commit(vg))
		log_error("New integrity metadata LV may require manual removal.");

	return 0;
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

int lv_get_raid_integrity_settings(struct logical_volume *lv, struct integrity_settings **isettings)
{
	struct logical_volume *lv_image;
	struct lv_segment *seg, *seg_image;
	uint32_t s;

	seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++) {
			lv_image = seg_lv(seg, s);
			seg_image = first_seg(lv_image);

			if (seg_is_integrity(seg_image)) {
				*isettings = &seg_image->integrity_settings;
				return 1;
			}
		}
	}

	return 0;
}

