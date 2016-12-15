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

/*
 * Reomove missing and empty PVs from VG, if are also in provided list
 */
static void _remove_missing_empty_pv(struct volume_group *vg, struct dm_list *remove_pvs)
{
	struct pv_list *pvl, *pvl_vg, *pvlt;
	int removed = 0;

	if (!remove_pvs)
		return;

	dm_list_iterate_items(pvl, remove_pvs) {
		dm_list_iterate_items_safe(pvl_vg, pvlt, &vg->pvs) {
			if (!id_equal(&pvl->pv->id, &pvl_vg->pv->id) ||
			    !is_missing_pv(pvl_vg->pv) ||
			    pvl_vg->pv->pe_alloc_count != 0)
				continue;

			/* FIXME: duplication of vgreduce code, move this to library */
			vg->free_count -= pvl_vg->pv->pe_count;
			vg->extent_count -= pvl_vg->pv->pe_count;
			del_pvl_from_vgs(vg, pvl_vg);
			free_pv_fid(pvl_vg->pv);

			removed++;
		}
	}

	if (removed) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			stack;
			return;
		}
		log_warn("%d missing and now unallocated Physical Volumes removed from VG.", removed);
	}
}

static int _lvconvert_repair_pvs(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle)
{
	struct dm_list *failed_pvs;
	struct dm_list *use_pvh;
	int ret;

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		use_pvh = &lv->vg->pvs;

	if (lv_is_raid(lv))
		ret = lvconvert_repair_pvs_raid(cmd, lv, handle, use_pvh);
	else if (lv_is_mirror(lv))
		ret = lvconvert_repair_pvs_mirror(cmd, lv, handle, use_pvh);
	else
		ret = 0;

	if (ret && arg_is_set(cmd, usepolicies_ARG)) {
		if ((failed_pvs = failed_pv_list(lv->vg)))
			_remove_missing_empty_pv(lv->vg, failed_pvs);
	}

	return ret ? ECMD_PROCESSED : ECMD_FAILED;
}

static int _lvconvert_repair_pvs_or_thinpool_single(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle)
{
	if (lv_is_thin_pool(lv))
		return lvconvert_repair_thinpool(cmd, lv, handle);
	else if (lv_is_raid(lv) || lv_is_mirror(lv))
		return _lvconvert_repair_pvs(cmd, lv, handle);
	else
		return_ECMD_FAILED;
}

/*
 * FIXME: add option --repair-pvs to call _lvconvert_repair_pvs() directly,
 * and option --repair-thinpool to call _lvconvert_repair_thinpool().
 */
int lvconvert_repair_pvs_or_thinpool_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvconvert_result lr = { 0 };
	struct convert_poll_id_list *idl;
	int saved_ignore_suspended_devices;
	int ret, poll_ret;

	dm_list_init(&lr.poll_idls);

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lr;

	saved_ignore_suspended_devices = ignore_suspended_devices();
	init_ignore_suspended_devices(1);

	cmd->handles_missing_pvs = 1;

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvconvert_repair_pvs_or_thinpool_single);

	init_ignore_suspended_devices(saved_ignore_suspended_devices);

	if (lr.need_polling) {
		dm_list_iterate_items(idl, &lr.poll_idls) {
			poll_ret = lvconvert_poll_by_id(cmd, idl->id,
						arg_is_set(cmd, background_ARG), 0, 0);
			if (poll_ret > ret)
				ret = poll_ret;
		}
	}

	destroy_processing_handle(cmd, handle);

	return ret;
}

static int _lvconvert_replace_pv_single(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle)
{
	struct arg_value_group_list *group;
	const char *tmp_str;
	struct dm_list *use_pvh;
	struct dm_list *replace_pvh;
	char **replace_pvs;
	int replace_pv_count;
	int i;

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		use_pvh = &lv->vg->pvs;

	if (!(replace_pv_count = arg_count(cmd, replace_ARG)))
		return_ECMD_FAILED;

	if (!(replace_pvs = dm_pool_alloc(cmd->mem, sizeof(char *) * replace_pv_count)))
		return_ECMD_FAILED;

	i = 0;
	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, replace_ARG))
			continue;
		if (!(tmp_str = grouped_arg_str_value(group->arg_values, replace_ARG, NULL))) {
			log_error("Failed to get '--replace' argument");
			return_ECMD_FAILED;
		}
		if (!(replace_pvs[i++] = dm_pool_strdup(cmd->mem, tmp_str)))
			return_ECMD_FAILED;
	}

	if (!(replace_pvh = create_pv_list(cmd->mem, lv->vg, replace_pv_count, replace_pvs, 0)))
		return_ECMD_FAILED;

	if (!lv_raid_replace(lv, arg_count(cmd, force_ARG), replace_pvh, use_pvh))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvconvert_replace_pv_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvconvert_result lr = { 0 };
	int ret;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lr;

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvconvert_replace_pv_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

static int _lvconvert_start_poll_single(struct cmd_context *cmd,
                                       struct logical_volume *lv,
                                       struct processing_handle *handle)
{
	struct lvconvert_result *lr = (struct lvconvert_result *) handle->custom_handle;
	struct convert_poll_id_list *idl;

	if (!(idl = convert_poll_id_list_create(cmd, lv)))
		return_ECMD_FAILED;
	dm_list_add(&lr->poll_idls, &idl->list);

	lr->need_polling = 1;

	return ECMD_PROCESSED;
}

int lvconvert_start_poll_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvconvert_result lr = { 0 };
	struct convert_poll_id_list *idl;
	int saved_ignore_suspended_devices;
	int ret, poll_ret;

	dm_list_init(&lr.poll_idls);

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lr;

	saved_ignore_suspended_devices = ignore_suspended_devices();
	init_ignore_suspended_devices(1);

	cmd->handles_missing_pvs = 1;

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvconvert_start_poll_single);

	init_ignore_suspended_devices(saved_ignore_suspended_devices);

	if (lr.need_polling) {
		dm_list_iterate_items(idl, &lr.poll_idls) {
			poll_ret = lvconvert_poll_by_id(cmd, idl->id,
						arg_is_set(cmd, background_ARG), 0, 0);
			if (poll_ret > ret)
				ret = poll_ret;
		}
	}

	destroy_processing_handle(cmd, handle);

	return ret;
}

static int _lvconvert_merge_generic_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	int ret;

	if (lv_is_cow(lv))
		ret = lvconvert_merge_snapshot_single(cmd, lv, handle);

	else if (lv_is_thin_volume(lv))
		ret = lvconvert_merge_thin_single(cmd, lv, handle);

	else
		ret = lvconvert_merge_mirror_images_single(cmd, lv, handle);

	return ret;
}

int lvconvert_merge_cmd(struct cmd_context *cmd, int argc, char **argv)
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

	cmd->command->flags &= ~GET_VGNAME_FROM_OPTIONS;

	ret = process_each_lv(cmd, cmd->position_argc, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvconvert_merge_generic_single);

	/* polling is only used by merge_snapshot */
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

