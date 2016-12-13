/*
 * Copyright (C) 2005-2015 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

#include "lvconvert_poll.h"

int lvconvert_mirror_finish(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct logical_volume *lv,
			    struct dm_list *lvs_changed __attribute__((unused)))
{
	if (!lv_is_converting(lv))
		return 1;

	if (!collapse_mirrored_lv(lv)) {
		log_error("Failed to remove temporary sync layer.");
		return 0;
	}

	lv->status &= ~CONVERTING;

	if (!lv_update_and_reload(lv))
		return_0;

	log_print_unless_silent("Logical volume %s converted.", lv->name);

	return 1;
}

/* Swap lvid and LV names */
int swap_lv_identifiers(struct cmd_context *cmd,
			struct logical_volume *a, struct logical_volume *b)
{
	union lvid lvid;
	const char *aname = a->name, *bname = b->name;

	lvid = a->lvid;
	a->lvid = b->lvid;
	b->lvid = lvid;

	/* rename temporarily to 'unused' name */
	if (!lv_rename_update(cmd, a, "pmove_tmeta", 0))
		return_0;
	/* name rename 'b' to unused name of 'a' */
	if (!lv_rename_update(cmd, b, aname, 0))
		return_0;
	/* finish name swapping */
	if (!lv_rename_update(cmd, a, bname, 0))
		return_0;

	return 1;
}

static void _move_lv_attributes(struct logical_volume *to, struct logical_volume *from)
{
	/* Maybe move this code into thin_merge_finish() */
	to->status = from->status; // FIXME maybe some masking ?
	to->alloc = from->alloc;
	to->profile = from->profile;
	to->read_ahead = from->read_ahead;
	to->major = from->major;
	to->minor = from->minor;
	to->timestamp = from->timestamp;
	to->hostname = from->hostname;

	/* Move tags */
	dm_list_init(&to->tags);
	dm_list_splice(&to->tags, &from->tags);

	/* Anything else to preserve? */
}

/* Finalise merging of lv into merge_lv */
int thin_merge_finish(struct cmd_context *cmd,
		      struct logical_volume *merge_lv,
		      struct logical_volume *lv)
{
	if (!swap_lv_identifiers(cmd, merge_lv, lv)) {
		log_error("Failed to swap %s with merging %s.",
			  lv->name, merge_lv->name);
		return 0;
	}

	/* Preserve origins' attributes */
	_move_lv_attributes(lv, merge_lv);

	/* Removed LV has to be visible */
	if (!lv_remove_single(cmd, merge_lv, DONT_PROMPT, 1))
		return_0;

	return 1;
}

int lvconvert_merge_finish(struct cmd_context *cmd,
			   struct volume_group *vg,
			   struct logical_volume *lv,
			   struct dm_list *lvs_changed __attribute__((unused)))
{
	struct lv_segment *snap_seg = find_snapshot(lv);

	if (!lv_is_merging_origin(lv)) {
		log_error("Logical volume %s has no merging snapshot.", lv->name);
		return 0;
	}

	log_print_unless_silent("Merge of snapshot into logical volume %s has finished.", lv->name);

	if (seg_is_thin_volume(snap_seg)) {
		clear_snapshot_merge(lv);

		if (!thin_merge_finish(cmd, lv, snap_seg->lv))
			return_0;

	} else if (!lv_remove_single(cmd, snap_seg->cow, DONT_PROMPT, 0)) {
		log_error("Could not remove snapshot %s merged into %s.",
			  snap_seg->cow->name, lv->name);
		return 0;
	}

	return 1;
}

progress_t poll_merge_progress(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       const char *name __attribute__((unused)),
			       struct daemon_parms *parms)
{
	dm_percent_t percent = DM_PERCENT_0;

	if (!lv_is_merging_origin(lv) ||
	    !lv_snapshot_percent(lv, &percent)) {
		log_error("%s: Failed query for merging percentage. Aborting merge.", lv->name);
		return PROGRESS_CHECK_FAILED;
	} else if (percent == DM_PERCENT_INVALID) {
		log_error("%s: Merging snapshot invalidated. Aborting merge.", lv->name);
		return PROGRESS_CHECK_FAILED;
	} else if (percent == LVM_PERCENT_MERGE_FAILED) {
		log_error("%s: Merge failed. Retry merge or inspect manually.", lv->name);
		return PROGRESS_CHECK_FAILED;
	}

	if (parms->progress_display)
		log_print_unless_silent("%s: %s: %.2f%%", lv->name, parms->progress_title,
					dm_percent_to_float(DM_PERCENT_100 - percent));
	else
		log_verbose("%s: %s: %.2f%%", lv->name, parms->progress_title,
			    dm_percent_to_float(DM_PERCENT_100 - percent));

	if (percent == DM_PERCENT_0)
		return PROGRESS_FINISHED_ALL;

	return PROGRESS_UNFINISHED;
}

progress_t poll_thin_merge_progress(struct cmd_context *cmd,
				    struct logical_volume *lv,
				    const char *name __attribute__((unused)),
				    struct daemon_parms *parms)
{
	uint32_t device_id;

	if (!lv_thin_device_id(lv, &device_id)) {
		stack;
		return PROGRESS_CHECK_FAILED;
	}

	/*
	 * There is no need to poll more than once,
	 * a thin snapshot merge is immediate.
	 */

	if (device_id != find_snapshot(lv)->device_id) {
		log_error("LV %s is not merged.", lv->name);
		return PROGRESS_CHECK_FAILED;
	}

	return PROGRESS_FINISHED_ALL; /* Merging happend */
}

static struct poll_operation_id *_create_id(struct cmd_context *cmd,
					    const char *vg_name,
					    const char *lv_name,
					    const char *uuid)
{
	struct poll_operation_id *id;
	char lv_full_name[NAME_LEN];

	if (!vg_name || !lv_name || !uuid) {
		log_error(INTERNAL_ERROR "Wrong params for lvconvert _create_id.");
		return NULL;
	}

	if (dm_snprintf(lv_full_name, sizeof(lv_full_name), "%s/%s", vg_name, lv_name) < 0) {
		log_error(INTERNAL_ERROR "Name \"%s/%s\" is too long.", vg_name, lv_name);
		return NULL;
	}

	if (!(id = dm_pool_alloc(cmd->mem, sizeof(*id)))) {
		log_error("Poll operation ID allocation failed.");
		return NULL;
	}

	if (!(id->display_name = dm_pool_strdup(cmd->mem, lv_full_name)) ||
	    !(id->lv_name = strchr(id->display_name, '/')) ||
	    !(id->vg_name = dm_pool_strdup(cmd->mem, vg_name)) ||
	    !(id->uuid = dm_pool_strdup(cmd->mem, uuid))) {
		log_error("Failed to copy one or more poll operation ID members.");
		dm_pool_free(cmd->mem, id);
		return NULL;
	}

	id->lv_name++; /* skip over '/' */

	return id;
}

static struct poll_functions _lvconvert_mirror_fns = {
	.poll_progress = poll_mirror_progress,
	.finish_copy = lvconvert_mirror_finish,
};

static struct poll_functions _lvconvert_merge_fns = {
	.poll_progress = poll_merge_progress,
	.finish_copy = lvconvert_merge_finish,
};

static struct poll_functions _lvconvert_thin_merge_fns = {
	.poll_progress = poll_thin_merge_progress,
	.finish_copy = lvconvert_merge_finish,
};

int lvconvert_poll_by_id(struct cmd_context *cmd, struct poll_operation_id *id,
				 unsigned background,
				 int is_merging_origin,
				 int is_merging_origin_thin)
{
	if (test_mode())
		return ECMD_PROCESSED;

	if (is_merging_origin)
		return poll_daemon(cmd, background,
				(MERGING | (is_merging_origin_thin ? THIN_VOLUME : SNAPSHOT)),
				is_merging_origin_thin ? &_lvconvert_thin_merge_fns : &_lvconvert_merge_fns,
				"Merged", id);
	else
		return poll_daemon(cmd, background, CONVERTING,
				&_lvconvert_mirror_fns, "Converted", id);
}

int lvconvert_poll(struct cmd_context *cmd, struct logical_volume *lv,
		   unsigned background)
{
	int r;
	struct poll_operation_id *id = _create_id(cmd, lv->vg->name, lv->name, lv->lvid.s);
	int is_merging_origin = 0;
	int is_merging_origin_thin = 0;

	if (!id) {
		log_error("Failed to allocate poll identifier for lvconvert.");
		return ECMD_FAILED;
	}

	/* FIXME: check this in polling instead */
	if (lv_is_merging_origin(lv)) {
		is_merging_origin = 1;
		is_merging_origin_thin = seg_is_thin_volume(find_snapshot(lv));
	}

	r = lvconvert_poll_by_id(cmd, id, background, is_merging_origin, is_merging_origin_thin);

	return r;
}

struct convert_poll_id_list *convert_poll_id_list_create(struct cmd_context *cmd, const struct logical_volume *lv)
{
	struct convert_poll_id_list *idl = (struct convert_poll_id_list *) dm_pool_alloc(cmd->mem, sizeof(struct convert_poll_id_list));

	if (!idl) {
		log_error("Convert poll ID list allocation failed.");
		return NULL;
	}

	if (!(idl->id = _create_id(cmd, lv->vg->name, lv->name, lv->lvid.s))) {
		dm_pool_free(cmd->mem, idl);
		return_NULL;
	}

	idl->is_merging_origin = lv_is_merging_origin(lv);
	idl->is_merging_origin_thin = idl->is_merging_origin && seg_is_thin_volume(find_snapshot(lv));

	return idl;
}

