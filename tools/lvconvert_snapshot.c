/*
 * Copyright (C) 2005-2016 Red Hat, Inc. All rights reserved.
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

#include "polldaemon.h"
#include "lv_alloc.h"
#include "lvconvert_poll.h"
#include "command-lines-count.h"

static int _lvconvert_snapshot(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       const char *origin_name)
{
	struct logical_volume *org;
	const char *snap_name = display_lvname(lv);
	uint32_t chunk_size;
	int zero;

	if (!(org = find_lv(lv->vg, origin_name))) {
		log_error("Couldn't find origin volume %s in Volume group %s.",
			  origin_name, lv->vg->name);
		return 0;
	}

	if (org == lv) {
		log_error("Unable to use %s as both snapshot and origin.", snap_name);
		return 0;
	}

	chunk_size = arg_uint_value(cmd, chunksize_ARG, 8);
	if (chunk_size < 8 || chunk_size > 1024 || !is_power_of_2(chunk_size)) {
		log_error("Chunk size must be a power of 2 in the range 4K to 512K.");
		return 0;
	}
	log_verbose("Setting chunk size to %s.", display_size(cmd, chunk_size));

	if (!cow_has_min_chunks(lv->vg, lv->le_count, chunk_size))
		return_0;

	/*
	 * check_lv_rules() checks cannot be done via command definition
	 * rules because this LV is not processed by process_each_lv.
	 */
	if (lv_is_locked(org) || lv_is_pvmove(org)) {
		log_error("Unable to use LV %s as snapshot origin: LV is %s.",
			  display_lvname(lv), lv_is_locked(org) ? "locked" : "pvmove");
		return 0;
	}

	/*
	 * check_lv_types() checks cannot be done via command definition
	 * LV_foo specification because this LV is not processed by process_each_lv.
	 */
	if (lv_is_cache_type(org) ||
	    lv_is_thin_type(org) ||
	    lv_is_mirrored(org) ||
	    lv_is_cow(org)) {
		log_error("Unable to use LV %s as snapshot origin: invald LV type.",
			  display_lvname(lv));
		return 0;
	}

	log_warn("WARNING: Converting logical volume %s to snapshot exception store.",
		 snap_name);
	log_warn("THIS WILL DESTROY CONTENT OF LOGICAL VOLUME (filesystem etc.)");

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Do you really want to convert %s? [y/n]: ",
			  snap_name) == 'n') {
		log_error("Conversion aborted.");
		return 0;
	}

	if (!deactivate_lv(cmd, lv)) {
		log_error("Couldn't deactivate logical volume %s.", snap_name);
		return 0;
	}

	if (first_seg(lv)->segtype->flags & SEG_CANNOT_BE_ZEROED)
		zero = 0;
	else
		zero = arg_int_value(cmd, zero_ARG, 1);

	if (!zero || !(lv->status & LVM_WRITE))
		log_warn("WARNING: %s not zeroed.", snap_name);
	else {
		lv->status |= LV_TEMPORARY;
		if (!activate_lv_local(cmd, lv) ||
		    !wipe_lv(lv, (struct wipe_params) { .do_zero = 1 })) {
			log_error("Aborting. Failed to wipe snapshot exception store.");
			return 0;
		}
		lv->status &= ~LV_TEMPORARY;
		/* Deactivates cleared metadata LV */
		if (!deactivate_lv_local(lv->vg->cmd, lv)) {
			log_error("Failed to deactivate zeroed snapshot exception store.");
			return 0;
		}
	}

	if (!archive(lv->vg))
		return_0;

	if (!vg_add_snapshot(org, lv, NULL, org->le_count, chunk_size)) {
		log_error("Couldn't create snapshot.");
		return 0;
	}

	/* store vg on disk(s) */
	if (!lv_update_and_reload(org))
		return_0;

	log_print_unless_silent("Logical volume %s converted to snapshot.", snap_name);

	return 1;
}

static int _lvconvert_merge_old_snapshot(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct logical_volume **lv_to_poll)
{
	int merge_on_activate = 0;
	struct logical_volume *origin = origin_from_cow(lv);
	struct lv_segment *snap_seg = find_snapshot(lv);
	struct lvinfo info;
	dm_percent_t snap_percent;

	if (lv_is_external_origin(origin_from_cow(lv))) {
		log_error("Cannot merge snapshot \"%s\" into "
			  "the read-only external origin \"%s\".",
			  lv->name, origin_from_cow(lv)->name);
		return 0;
	}

	/* FIXME: test when snapshot is remotely active */
	if (lv_info(cmd, lv, 0, &info, 1, 0)
	    && info.exists && info.live_table &&
	    (!lv_snapshot_percent(lv, &snap_percent) ||
	     snap_percent == DM_PERCENT_INVALID)) {
		log_error("Unable to merge invalidated snapshot LV \"%s\".",
			  lv->name);
		return 0;
	}

	if (snap_seg->segtype->ops->target_present &&
	    !snap_seg->segtype->ops->target_present(cmd, snap_seg, NULL)) {
		log_error("Can't initialize snapshot merge. "
			  "Missing support in kernel?");
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

	/*
	 * Prevent merge with open device(s) as it would likely lead
	 * to application/filesystem failure.  Merge on origin's next
	 * activation if either the origin or snapshot LV are currently
	 * open.
	 *
	 * FIXME testing open_count is racey; snapshot-merge target's
	 * constructor and DM should prevent appropriate devices from
	 * being open.
	 */
	if (lv_is_active_locally(origin)) {
		if (!lv_check_not_in_use(origin, 0)) {
			log_print_unless_silent("Can't merge until origin volume is closed.");
			merge_on_activate = 1;
		} else if (!lv_check_not_in_use(lv, 0)) {
			log_print_unless_silent("Can't merge until snapshot is closed.");
			merge_on_activate = 1;
		}
	} else if (vg_is_clustered(origin->vg) && lv_is_active(origin)) {
		/* When it's active somewhere else */
		log_print_unless_silent("Can't check whether remotely active snapshot is open.");
		merge_on_activate = 1;
	}

	init_snapshot_merge(snap_seg, origin);

	if (merge_on_activate) {
		/* Store and commit vg but skip starting the merge */
		if (!vg_write(lv->vg) || !vg_commit(lv->vg))
			return_0;
		backup(lv->vg);
	} else {
		/* Perform merge */
		if (!lv_update_and_reload(origin))
			return_0;

		*lv_to_poll = origin;
	}

	if (merge_on_activate)
		log_print_unless_silent("Merging of snapshot %s will occur on "
					"next activation of %s.",
					display_lvname(lv), display_lvname(origin));
	else
		log_print_unless_silent("Merging of volume %s started.",
					display_lvname(lv));

	return 1;
}

static int _lvconvert_splitsnapshot(struct cmd_context *cmd, struct logical_volume *cow)
{
	struct volume_group *vg = cow->vg;
	const char *cow_name = display_lvname(cow);

	if (lv_is_virtual_origin(origin_from_cow(cow))) {
		log_error("Unable to split off snapshot %s with virtual origin.", cow_name);
		return 0;
	}

	if (!(vg->fid->fmt->features & FMT_MDAS)) {
		log_error("Unable to split off snapshot %s using old LVM1-style metadata.", cow_name);
		return 0;
	}

	if (is_lockd_type(vg->lock_type)) {
		/* FIXME: we need to create a lock for the new LV. */
		log_error("Unable to split snapshots in VG with lock_type %s.", vg->lock_type);
		return 0;
	}

	if (lv_is_active_locally(cow)) {
		if (!lv_check_not_in_use(cow, 1))
			return_0;

		if ((arg_count(cmd, force_ARG) == PROMPT) &&
		    !arg_count(cmd, yes_ARG) &&
		    lv_is_visible(cow) &&
		    lv_is_active(cow)) {
			if (yes_no_prompt("Do you really want to split off active "
					  "logical volume %s? [y/n]: ", cow_name) == 'n') {
				log_error("Logical volume %s not split.", cow_name);
				return 0;
			}
		}
	}

	if (!archive(vg))
		return_0;

	log_verbose("Splitting snapshot %s from its origin.", cow_name);

	if (!vg_remove_snapshot(cow))
		return_0;

	backup(vg);

	log_print_unless_silent("Logical Volume %s split from its origin.", cow_name);

	return 1;
}

/*
 * Merge a COW snapshot LV into its origin.
 */
int lvconvert_merge_snapshot_single(struct cmd_context *cmd,
                                       struct logical_volume *lv,
                                       struct processing_handle *handle)
{
	struct lvconvert_result *lr = (struct lvconvert_result *) handle->custom_handle;
	struct logical_volume *lv_to_poll = NULL;
	struct convert_poll_id_list *idl;

	if (!_lvconvert_merge_old_snapshot(cmd, lv, &lv_to_poll))
		return_ECMD_FAILED;

	if (lv_to_poll) {
		if (!(idl = convert_poll_id_list_create(cmd, lv_to_poll)))
			return_ECMD_FAILED;
		dm_list_add(&lr->poll_idls, &idl->list);
		lr->need_polling = 1;
	}

	return ECMD_PROCESSED;
}

static int _lvconvert_merge_snapshot_check(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle,
			int lv_is_named_arg)
{
	if (!lv_is_visible(lv)) {
		log_error("Operation not permitted (%s %d) on hidden LV %s.",
			  cmd->command->command_line_id, cmd->command->command_line_enum,
			  display_lvname(lv));
		return 0;
	}

	return 1;
}

int lvconvert_merge_snapshot_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvconvert_result lr = { 0 };
	struct convert_poll_id_list *idl;
	int ret, poll_ret;

	dm_list_init(&lr.poll_idls);

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lr;

	ret = process_each_lv(cmd, cmd->position_argc, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, &_lvconvert_merge_snapshot_check, &lvconvert_merge_snapshot_single);

	if (lr.need_polling) {
		dm_list_iterate_items(idl, &lr.poll_idls) {
			poll_ret = lvconvert_poll_by_id(cmd, idl->id,
						arg_is_set(cmd, background_ARG), 1, 0);
			if (poll_ret > ret)
				ret = poll_ret;
		}
	}

	destroy_processing_handle(cmd, handle);

	return ret;
}

/*
 * Separate a COW snapshot from its origin.
 *
 * lvconvert --splitsnapshot LV_snapshot
 * lvconvert_split_cow_snapshot
 */

static int _lvconvert_split_snapshot_single(struct cmd_context *cmd,
                                       struct logical_volume *lv,
                                       struct processing_handle *handle)
{
	if (!_lvconvert_splitsnapshot(cmd, lv))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvconvert_split_snapshot_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, &lvconvert_generic_check, &_lvconvert_split_snapshot_single);
}

/*
 * Combine two LVs that were once an origin/cow pair of LVs, were then
 * separated with --splitsnapshot, and now with this command are combined again
 * into the origin/cow pair.
 *
 * This is an obscure command that has little to no real uses.
 *
 * The command has unusual handling of position args.  The first position arg
 * will become the origin LV, and is not processed by process_each_lv.  The
 * second position arg will become the cow LV and is processed by
 * process_each_lv.
 *
 * The single function can grab the origin LV from position_argv[0].
 *
 * begin with an ordinary LV foo:
 * lvcreate -n foo -L 1 vg
 *
 * create a cow snapshot of foo named foosnap:
 * lvcreate -s -L 1 -n foosnap vg/foo
 *
 * now, foo is an "origin LV" and foosnap is a "cow LV"
 * (foosnap matches LV_snapshot aka lv_is_cow)
 *
 * split the two LVs apart:
 * lvconvert --splitsnapshot vg/foosnap
 *
 * now, foo is *not* an origin LV and foosnap is *not* a cow LV
 * (foosnap does not match LV_snapshot)
 *
 * now, combine the two LVs again:
 * lvconvert --snapshot vg/foo vg/foosnap
 *
 * after this, foosnap will match LV_snapshot again.
 *
 * FIXME: when splitsnapshot is run, the previous cow LV should be
 * flagged in the metadata somehow, and then that flag should be
 * required here.  As it is now, the first and second args
 * (origin and cow) can be swapped and nothing catches it.
 */

static int _lvconvert_combine_split_snapshot_single(struct cmd_context *cmd,
                                       struct logical_volume *lv,
                                       struct processing_handle *handle)
{
	const char *origin_name = cmd->position_argv[0];

	/* If origin_name includes VG name, the VG name is removed. */
	if (!validate_lvname_param(cmd, &lv->vg->name, &origin_name))
		return_ECMD_FAILED;

	if (!_lvconvert_snapshot(cmd, lv, origin_name))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvconvert_combine_split_snapshot_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, 1, cmd->position_argv + 1, NULL, NULL, READ_FOR_UPDATE,
			       NULL, &lvconvert_generic_check, &_lvconvert_combine_split_snapshot_single);
}
