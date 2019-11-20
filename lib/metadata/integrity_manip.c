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

/*
 * The user wants external metadata, but did not specify an existing
 * LV to hold metadata, so create an LV for metadata.  Save it in
 * lp->integrity_meta_lv and it will be passed to lv_add_integrity().
 */
int lv_create_integrity_metadata(struct cmd_context *cmd,
				struct volume_group *vg,
				struct lvcreate_params *lp)
{
	char metaname[NAME_LEN];
	uint32_t extent_bytes;
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
	};

	if (dm_snprintf(metaname, NAME_LEN, "%s_imeta", lp->lv_name) < 0) {
		log_error("Failed to create metadata LV name.");
		return 0;
	}

	if (!(lp_meta.lv_name = strdup(metaname)))
		return_0;

	/* TODO: set pvh per command line */
	/* TODO: scale size according to LV size. */
	extent_bytes = vg->extent_size * SECTOR_SIZE;
	lp_meta.extents = (4 * ONE_MB_IN_BYTES) / extent_bytes;

	log_debug("Creating integrity metadata LV with %u extents", lp_meta.extents);

	dm_list_init(&lp_meta.tags);

	if (!(lp_meta.segtype = get_segtype_from_string(vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (!(lv = lv_create_single(vg, &lp_meta))) {
		log_error("Failed to create integrity metadata LV");
		return 0;
	}

	lp->integrity_meta_lv = lv;
	return 1;
}

static int _get_provided_data_sectors(struct logical_volume *lv, uint64_t *provided_data_sectors)
{
	struct lv_with_info_and_seg_status status;

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

	*provided_data_sectors = status.seg_status.integrity->provided_data_sectors;

	dm_pool_destroy(status.seg_status.mem);
	return 1;

fail:
	dm_pool_destroy(status.seg_status.mem);
	return 0;
}

int lv_add_integrity(struct logical_volume *lv, const char *arg,
		     struct logical_volume *meta_lv_created,
		     const char *meta_name,
		     struct integrity_settings *settings)
{
	struct cmd_context *cmd = lv->vg->cmd;
	struct integrity_settings *set;
	struct logical_volume *lv_orig;
	struct logical_volume *meta_lv = NULL;
	const struct segment_type *segtype;
	struct lv_segment *seg;
	int ret;

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
	set = &seg->integrity_settings;

	if (!set->mode[0])
		set->mode[0] = DEFAULT_MODE;

	if (!set->tag_size)
		set->tag_size = DEFAULT_TAG_SIZE;

	if (!set->internal_hash)
	       	set->internal_hash = DEFAULT_INTERNAL_HASH;

	/*
	 * --integrity <arg> is y|external|internal
	 */

	if (!arg)
		arg = "external";

	if (!strcmp(arg, "none")) {
		log_error("Invalid --integrity arg for lvcreate.");
		return 0;
	}

	if (!strcmp(arg, "internal") && meta_name) {
		log_error("Internal integrity cannot be used with integritymetadata option.");
		return 0;
	}

	if (meta_lv_created)
		meta_lv = meta_lv_created;
	else if (meta_name) {
		if (!(meta_lv = find_lv(lv->vg, meta_name))) {
			log_error("LV %s not found.", meta_name);
			return_0;
		}
	}

	/*
	 * When not using a meta_dev, dm-integrity needs to tell us what the
	 * usable size of the LV is, which is the dev size minus the integrity
	 * overhead.  To find that, we need to do a special, temporary activation
	 * of the new LV, specifying a dm dev size of 1, then check the dm dev
	 * status field provided_data_sectors, which is the actual size of the
	 * LV.  We need to include provided_data_sectors in the metadata for the
	 * new LV, and use this as the dm dev size for normal LV activation.
	 *
	 * When using a meta_dev, the dm dev size is the size of the data
	 * device.  The necessary size of the meta_dev for the given data
	 * device needs to be estimated.
	 */

	if (meta_lv) {
		if (!sync_local_dev_names(cmd)) {
			log_error("Failed to sync local devices.");
			return 0;
		}

		if (!activate_and_wipe_lv(meta_lv, 0)) {
			log_error("LV %s could not be zeroed.", display_lvname(meta_lv));
			return 0;
		}
		seg->integrity_data_sectors = seg->len;
		seg->integrity_meta_dev = meta_lv;
		lv_set_hidden(meta_lv);
		/* TODO: give meta_lv a suffix? e.g. _imeta */
		ret = 1;
	} else {
		/* dm-integrity wants temp/fake size of 1 to report usable size */
		seg->integrity_data_sectors = 1;

		lv->status |= LV_TEMPORARY;
		lv->status |= LV_NOSCAN;
		lv->status |= LV_UNCOMMITTED;

		log_debug("Activating temporary integrity LV to get data sectors.");

		if (!activate_lv(cmd, lv)) {
			log_error("Failed to activate temporary integrity.");
			ret = 0;
			goto out;
		}

		if (!_get_provided_data_sectors(lv, &seg->integrity_data_sectors)) {
			log_error("Failed to get data sectors from dm-integrity");
			ret = 0;
		} else {
			log_debug("Found integrity provided_data_sectors %llu", (unsigned long long)seg->integrity_data_sectors);
			ret = 1;
		}

		lv->status |= LV_UNCOMMITTED;

		if (!deactivate_lv(cmd, lv)) {
			log_error("Failed to deactivate temporary integrity.");
			ret = 0;
		}

		lv->status &= ~LV_UNCOMMITTED;
		lv->status &= ~LV_NOSCAN;
		lv->status &= ~LV_TEMPORARY;
	}
 out:
	return ret;
}


