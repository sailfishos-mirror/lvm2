/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2017 Red Hat, Inc. All rights reserved.
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
#include "metadata.h"
#include "activate.h"
#include "memlock.h"
#include "display.h"
#include "fs.h"
#include "lvm-exec.h"
#include "lvm-file.h"
#include "lvm-string.h"
#include "toolcontext.h"
#include "dev_manager.h"
#include "str_list.h"
#include "config.h"
#include "segtype.h"
#include "sharedlib.h"

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#define _skip(fmt, args...) log_very_verbose("Skipping: " fmt , ## args)

int lvm1_present(struct cmd_context *cmd)
{
	static char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/lvm/global", cmd->proc_dir)
	    < 0) {
		log_error("LVM1 proc global snprintf failed");
		return 0;
	}

	return (path_exists(path)) ? 1 : 0;
}

int list_segment_modules(struct dm_pool *mem, const struct lv_segment *seg,
			 struct dm_list *modules)
{
	unsigned int s;
	struct lv_segment *seg2, *snap_seg;
	struct dm_list *snh;

	if (seg->segtype->ops->modules_needed &&
	    !seg->segtype->ops->modules_needed(mem, seg, modules)) {
		log_error("module string allocation failed");
		return 0;
	}

	if (lv_is_origin(seg->lv))
		dm_list_iterate(snh, &seg->lv->snapshot_segs)
			if (!list_lv_modules(mem,
					     dm_list_struct_base(snh,
							      struct lv_segment,
							      origin_list)->cow,
					     modules))
				return_0;

	if (lv_is_cow(seg->lv)) {
		snap_seg = find_snapshot(seg->lv);
		if (snap_seg->segtype->ops->modules_needed &&
		    !snap_seg->segtype->ops->modules_needed(mem, snap_seg,
							    modules)) {
			log_error("snap_seg module string allocation failed");
			return 0;
		}
	}

	for (s = 0; s < seg->area_count; s++) {
		switch (seg_type(seg, s)) {
		case AREA_LV:
			seg2 = find_seg_by_le(seg_lv(seg, s), seg_le(seg, s));
			if (seg2 && !list_segment_modules(mem, seg2, modules))
				return_0;
			break;
		case AREA_PV:
		case AREA_UNASSIGNED:
			;
		}
	}

	return 1;
}

int list_lv_modules(struct dm_pool *mem, const struct logical_volume *lv,
		    struct dm_list *modules)
{
	struct lv_segment *seg;

	dm_list_iterate_items(seg, &lv->segments)
		if (!list_segment_modules(mem, seg, modules))
			return_0;

	return 1;
}

static int _lv_passes_volumes_filter(struct cmd_context *cmd, const struct logical_volume *lv,
				     const struct dm_config_node *cn, const int cfg_id)
{
	const struct dm_config_value *cv;
	const char *str;
	static char config_path[PATH_MAX];
	size_t len = strlen(lv->vg->name);

	config_def_get_path(config_path, sizeof(config_path), cfg_id);
	log_verbose("%s configuration setting defined: "
		    "Checking the list to match %s.",
		    config_path, display_lvname(lv));

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type == DM_CFG_EMPTY_ARRAY)
			goto out;
		if (cv->type != DM_CFG_STRING) {
			log_print_unless_silent("Ignoring invalid string in config file %s.",
						config_path);
			continue;
		}
		str = cv->v.str;
		if (!*str) {
			log_print_unless_silent("Ignoring empty string in config file %s.",
						config_path);
			continue;
		}

		/* Tag? */
		if (*str == '@') {
			str++;
			if (!*str) {
				log_print_unless_silent("Ignoring empty tag in config file %s",
							config_path);
				continue;
			}
			/* If any host tag matches any LV or VG tag, activate */
			if (!strcmp(str, "*")) {
				if (str_list_match_list(&cmd->tags, &lv->tags, NULL)
				    || str_list_match_list(&cmd->tags,
							   &lv->vg->tags, NULL))
					    return 1;
				else
					continue;
			}
			/* If supplied tag matches LV or VG tag, activate */
			if (str_list_match_item(&lv->tags, str) ||
			    str_list_match_item(&lv->vg->tags, str))
				return 1;
			else
				continue;
		}

		/* If supplied name is vgname[/lvname] */
		if ((strncmp(str, lv->vg->name, len) == 0) &&
		    (!str[len] ||
		     ((str[len] == '/') &&
		      !strcmp(str + len + 1, lv->name))))
			return 1;
	}

out:
	log_verbose("No item supplied in %s configuration setting matches %s.",
		    config_path, display_lvname(lv));

	return 0;
}

int lv_passes_auto_activation_filter(struct cmd_context *cmd, struct logical_volume *lv)
{
	const struct dm_config_node *cn;

	if (!(cn = find_config_tree_array(cmd, activation_auto_activation_volume_list_CFG, NULL))) {
		log_verbose("activation/auto_activation_volume_list configuration setting "
			    "not defined: All logical volumes will be auto-activated.");
		return 1;
	}

	return _lv_passes_volumes_filter(cmd, lv, cn, activation_auto_activation_volume_list_CFG);
}

#ifndef DEVMAPPER_SUPPORT
void set_activation(int act, int silent)
{
	static int warned = 0;

	if (warned || !act)
		return;

	log_error("Compiled without libdevmapper support. "
		  "Can't enable activation.");

	warned = 1;
}
int activation(void)
{
	return 0;
}
int library_version(char *version, size_t size)
{
	return 0;
}
int driver_version(char *version, size_t size)
{
	return 0;
}
int target_version(const char *target_name, uint32_t *maj,
		   uint32_t *min, uint32_t *patchlevel)
{
	return 0;
}
int target_present(struct cmd_context *cmd, const char *target_name,
		   int use_modprobe)
{
	return 0;
}
int lvm_dm_prefix_check(int major, int minor, const char *prefix)
{
	return 0;
}
int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, int use_layer,
	    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	return 0;
}
int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s, int use_layer,
		    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	return 0;
}
int lv_info_with_seg_status(struct cmd_context *cmd, const struct logical_volume *lv,
			    const struct lv_segment *lv_seg, int use_layer,
			    struct lv_with_info_and_seg_status *status,
			    int with_open_count, int with_read_ahead)
{
	return 0;
}
int lv_status(struct cmd_context *cmd, const struct lv_segment *lv_seg,
	      int use_layer, struct lv_seg_status *lv_seg_status)
{
	return 0;
}
int lv_cache_status(const struct logical_volume *cache_lv,
		    struct lv_status_cache **status)
{
	return 0;
}
int lv_check_not_in_use(const struct logical_volume *lv, int error_if_used)
{
        return 0;
}
int lv_snapshot_percent(const struct logical_volume *lv, dm_percent_t *percent)
{
	return 0;
}
int lv_mirror_percent(struct cmd_context *cmd, const struct logical_volume *lv,
		      int wait, dm_percent_t *percent, uint32_t *event_nr)
{
	return 0;
}
int lv_raid_percent(const struct logical_volume *lv, dm_percent_t *percent)
{
	return 0;
}
int lv_raid_data_offset(const struct logical_volume *lv, uint64_t *data_offset)
{
	return 0;
}
int lv_raid_dev_health(const struct logical_volume *lv, char **dev_health)
{
	return 0;
}
int lv_raid_dev_count(const struct logical_volume *lv, uint32_t *dev_cnt)
{
	return 0;
}
int lv_raid_mismatch_count(const struct logical_volume *lv, uint64_t *cnt)
{
	return 0;
}
int lv_raid_sync_action(const struct logical_volume *lv, char **sync_action)
{
	return 0;
}
int lv_raid_message(const struct logical_volume *lv, const char *msg)
{
	return 0;
}
int lv_thin_pool_percent(const struct logical_volume *lv, int metadata,
			 dm_percent_t *percent)
{
	return 0;
}
int lv_thin_percent(const struct logical_volume *lv, int mapped,
		    dm_percent_t *percent)
{
	return 0;
}
int lv_thin_pool_transaction_id(const struct logical_volume *lv,
				uint64_t *transaction_id)
{
	return 0;
}
int lv_thin_device_id(const struct logical_volume *lv, uint32_t *device_id)
{
	return 0;
}
int lvs_in_vg_activated(const struct volume_group *vg)
{
	return 0;
}
int lvs_in_vg_opened(const struct volume_group *vg)
{
	return 0;
}
/******
int lv_suspend(struct cmd_context *cmd, const char *lvid_s)
{
	return 1;
}
*******/
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, unsigned exclusive,
			 const struct logical_volume *lv, const struct logical_volume *lv_pre)
{
	return 1;
}
int lv_resume(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, const struct logical_volume *lv)
{
	return 1;
}
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only,
			unsigned exclusive, unsigned revert, const struct logical_volume *lv)
{
	return 1;
}
int lv_deactivate(struct cmd_context *cmd, const char *lvid_s, const struct logical_volume *lv)
{
	return 1;
}
int lv_activation_filter(struct cmd_context *cmd, const char *lvid_s,
			 int *activate_lv, const struct logical_volume *lv)
{
	return 1;
}
int lv_activate(struct cmd_context *cmd, const char *lvid_s, int exclusive, int noscan,
		int temporary, const struct logical_volume *lv)
{
	return 1;
}
int lv_activate_with_filter(struct cmd_context *cmd, const char *lvid_s, int exclusive,
			    int noscan, int temporary, const struct logical_volume *lv)
{
	return 1;
}
int lv_mknodes(struct cmd_context *cmd, const struct logical_volume *lv)
{
	return 1;
}
int lv_deactivate_any_missing_subdevs(const struct logical_volume *lv)
{
	return 1;
}
int pv_uses_vg(struct physical_volume *pv,
	       struct volume_group *vg)
{
	return 0;
}
void activation_release(void)
{
}
void activation_exit(void)
{
}

int raid4_is_supported(struct cmd_context *cmd, const struct segment_type *segtype)
{
	return 1;
}

int lv_is_active(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_locally(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_remotely(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_but_not_locally(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_exclusive(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_exclusive_locally(const struct logical_volume *lv)
{
	return 0;
}
int lv_is_active_exclusive_remotely(const struct logical_volume *lv)
{
	return 0;
}

int lv_check_transient(struct logical_volume *lv)
{
	return 1;
}
int monitor_dev_for_events(struct cmd_context *cmd, const struct logical_volume *lv,
			   const struct lv_activate_opts *laopts, int monitor)
{
	return 1;
}
/* fs.c */
void fs_unlock(void)
{
}
/* dev_manager.c */
#include "targets.h"
int add_areas_line(struct dev_manager *dm, struct lv_segment *seg,
		   struct dm_tree_node *node, uint32_t start_area,
		   uint32_t areas)
{
        return 0;
}
int device_is_usable(struct device *dev, struct dev_usable_check_params check)
{
        return 0;
}
int lv_has_target_type(struct dm_pool *mem, const struct logical_volume *lv,
		       const char *layer, const char *target_type)
{
        return 0;
}
#else				/* DEVMAPPER_SUPPORT */

static int _activation = 1;

void set_activation(int act, int silent)
{
	if (act == _activation)
		return;

	_activation = act;
	if (_activation)
		log_verbose("Activation enabled. Device-mapper kernel "
			    "driver will be used.");
	else if (!silent)
		log_warn("WARNING: Activation disabled. No device-mapper "
			  "interaction will be attempted.");
	else
		log_verbose("Activation disabled. No device-mapper "
			    "interaction will be attempted.");
}

int activation(void)
{
	return _activation;
}

static int _passes_activation_filter(struct cmd_context *cmd,
				     const struct logical_volume *lv)
{
	const struct dm_config_node *cn;

	if (!(cn = find_config_tree_array(cmd, activation_volume_list_CFG, NULL))) {
		log_verbose("activation/volume_list configuration setting "
			    "not defined: Checking only host tags for %s.",
			    display_lvname(lv));

		/* If no host tags defined, activate */
		if (dm_list_empty(&cmd->tags))
			return 1;

		/* If any host tag matches any LV or VG tag, activate */
		if (str_list_match_list(&cmd->tags, &lv->tags, NULL) ||
		    str_list_match_list(&cmd->tags, &lv->vg->tags, NULL))
			return 1;

		log_verbose("No host tag matches %s", display_lvname(lv));

		/* Don't activate */
		return 0;
	}

	return _lv_passes_volumes_filter(cmd, lv, cn, activation_volume_list_CFG);
}

static int _passes_readonly_filter(struct cmd_context *cmd,
				   const struct logical_volume *lv)
{
	const struct dm_config_node *cn;

	if (!(cn = find_config_tree_array(cmd, activation_read_only_volume_list_CFG, NULL)))
		return 0;

	return _lv_passes_volumes_filter(cmd, lv, cn, activation_read_only_volume_list_CFG);
}

int library_version(char *version, size_t size)
{
	if (!activation())
		return 0;

	return dm_get_library_version(version, size);
}

int driver_version(char *version, size_t size)
{
	if (!activation())
		return 0;

	log_very_verbose("Getting driver version");

	return dm_driver_version(version, size);
}

int target_version(const char *target_name, uint32_t *maj,
		   uint32_t *min, uint32_t *patchlevel)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_versions *target, *last_target;

	log_very_verbose("Getting target version for %s", target_name);
	if (!(dmt = dm_task_create(DM_DEVICE_LIST_VERSIONS)))
		return_0;

        if (activation_checks() && !dm_task_enable_checks(dmt))
                goto_out;

	if (!dm_task_run(dmt)) {
		log_debug_activation("Failed to get %s target version", target_name);
		/* Assume this was because LIST_VERSIONS isn't supported */
		*maj = 0;
		*min = 0;
		*patchlevel = 0;
		r = 1;
		goto out;
	}

	target = dm_task_get_versions(dmt);

	do {
		last_target = target;

		if (!strcmp(target_name, target->name)) {
			r = 1;
			*maj = target->version[0];
			*min = target->version[1];
			*patchlevel = target->version[2];
			goto out;
		}

		target = (struct dm_versions *)((char *) target + target->next);
	} while (last_target != target);

      out:
	if (r)
		log_very_verbose("Found %s target "
				 "v%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".",
				 target_name, *maj, *min, *patchlevel);

	dm_task_destroy(dmt);

	return r;
}

int lvm_dm_prefix_check(int major, int minor, const char *prefix)
{
	struct dm_task *dmt;
	const char *uuid;
	int r;

	if (!(dmt = dm_task_create(DM_DEVICE_STATUS)))
		return_0;

	if (!dm_task_set_minor(dmt, minor) ||
	    !dm_task_set_major(dmt, major) ||
	    !dm_task_run(dmt) ||
	    !(uuid = dm_task_get_uuid(dmt))) {
		dm_task_destroy(dmt);
		return 0;
	}

	r = strncasecmp(uuid, prefix, strlen(prefix));
	dm_task_destroy(dmt);

	return r ? 0 : 1;
}

int module_present(struct cmd_context *cmd, const char *target_name)
{
	int ret = 0;
#ifdef MODPROBE_CMD
	char module[128];
	const char *argv[] = { MODPROBE_CMD, module, NULL };
#endif
	struct stat st;
	char path[PATH_MAX];
	int i = dm_snprintf(path, (sizeof(path) - 1), "%smodule/dm_%s",
			    dm_sysfs_dir(), target_name);

	if (i > 0) {
		while (path[--i] != '/')  /* stop on dm_ */
			if (path[i] == '-')
				path[i] = '_'; /* replace '-' with '_' */

		if ((lstat(path, &st) == 0) && S_ISDIR(st.st_mode)) {
			log_debug_activation("Module directory %s exists.", path);
			return 1;
		}
	}

#ifdef MODPROBE_CMD
	if (dm_snprintf(module, sizeof(module), "dm-%s", target_name) < 0) {
		log_error("module_present module name too long: %s",
			  target_name);
		return 0;
	}

	ret = exec_cmd(cmd, argv, NULL, 0);
#endif
	return ret;
}

int target_present_version(struct cmd_context *cmd, const char *target_name,
			   int use_modprobe,
			   uint32_t *maj, uint32_t *min, uint32_t *patchlevel)
{
	if (!activation()) {
		log_error(INTERNAL_ERROR "Target present version called when activation is disabled.");
		return 0;
	}
#ifdef MODPROBE_CMD
	if (use_modprobe) {
		if (target_version(target_name, maj, min, patchlevel))
			return 1;

		if (!module_present(cmd, target_name))
			return_0;
	}
#endif
	return target_version(target_name, maj, min, patchlevel);
}

int target_present(struct cmd_context *cmd, const char *target_name,
		   int use_modprobe)
{
	uint32_t maj, min, patchlevel;

	return target_present_version(cmd, target_name, use_modprobe,
				      &maj, &min, &patchlevel);
}

/*
 * When '*info' is NULL, returns 1 only when LV is active.
 * When '*info' != NULL, returns 1 when info structure is populated.
 */
static int _lv_info(struct cmd_context *cmd, const struct logical_volume *lv,
		    int use_layer, struct lvinfo *info,
		    const struct lv_segment *seg,
		    struct lv_seg_status *seg_status,
		    int with_open_count, int with_read_ahead)
{
	struct dm_info dminfo;

	/*
	 * If open_count info is requested and we have to be sure our own udev
	 * transactions are finished
	 * For non-clustered locking type we are only interested for non-delete operation
	 * in progress - as only those could lead to opened files
	 */
	if (with_open_count) {
		if (locking_is_clustered() && !sync_local_dev_names(cmd)) /* Wait to have udev in sync */
			return_0;
		else if (fs_has_non_delete_ops())
			fs_unlock(); /* For non clustered - wait if there are non-delete ops */
	}

	/* New thin-pool has no layer, but -tpool suffix needs to be queried */
	if (!use_layer && lv_is_new_thin_pool(lv)) {
		/* Check if there isn't existing old thin pool mapping in the table */
		if (!dev_manager_info(cmd, lv, NULL, 0, 0, &dminfo, NULL, NULL))
			return_0;
		if (!dminfo.exists)
			use_layer = 1;
	}

	if (seg_status) {
		/* TODO: for now it's mess with seg_status */
		seg_status->seg = seg;
	}

	if (!dev_manager_info(cmd, lv,
			      (use_layer) ? lv_layer(lv) : NULL,
			      with_open_count, with_read_ahead,
			      &dminfo, (info) ? &info->read_ahead : NULL,
			      seg_status))
		return_0;

	if (!info)
		return dminfo.exists;

	info->exists = dminfo.exists;
	info->suspended = dminfo.suspended;
	info->open_count = dminfo.open_count;
	info->major = dminfo.major;
	info->minor = dminfo.minor;
	info->read_only = dminfo.read_only;
	info->live_table = dminfo.live_table;
	info->inactive_table = dminfo.inactive_table;

	return 1;
}

/*
 * Returns 1 if info structure populated, else 0 on failure.
 * When lvinfo* is NULL, it returns 1 if the device is locally active, 0 otherwise.
 */
int lv_info(struct cmd_context *cmd, const struct logical_volume *lv, int use_layer,
	    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	if (!activation())
		return 0;

	return _lv_info(cmd, lv, use_layer, info, NULL, NULL, with_open_count, with_read_ahead);
}

int lv_info_by_lvid(struct cmd_context *cmd, const char *lvid_s, int use_layer,
		    struct lvinfo *info, int with_open_count, int with_read_ahead)
{
	int r;
	struct logical_volume *lv;

	if (!(lv = lv_from_lvid(cmd, lvid_s, 0)))
		return 0;

	r = lv_info(cmd, lv, use_layer, info, with_open_count, with_read_ahead);
	release_vg(lv->vg);

	return r;
}

/*
 * Returns 1 if lv_with_info_and_seg_status info structure populated,
 * else 0 on failure or if device not active locally.
 *
 * When seg_status parsing had troubles it will set type to SEG_STATUS_UNKNOWN.
 *
 * Using usually one ioctl to obtain info and status.
 * More complex segment do collect info from one device,
 * but status from another device.
 *
 * TODO: further improve with more statuses (i.e. snapshot's origin/merge)
 */
int lv_info_with_seg_status(struct cmd_context *cmd,
			    const struct lv_segment *lv_seg,
			    struct lv_with_info_and_seg_status *status,
			    int with_open_count, int with_read_ahead)
{
	const struct logical_volume *olv, *lv = status->lv = lv_seg->lv;

	if (!activation())
		return 0;

	if (lv_is_used_cache_pool(lv)) {
		/* INFO is not set as cache-pool cannot be active.
		 * STATUS is collected from cache LV */
		lv_seg = get_only_segment_using_this_lv(lv);
		(void) _lv_info(cmd, lv_seg->lv, 1, NULL, lv_seg, &status->seg_status, 0, 0);
		return 1;
	}

	if (lv_is_thin_pool(lv)) {
		/* Always collect status for '-tpool' */
		if (_lv_info(cmd, lv, 1, &status->info, lv_seg, &status->seg_status, 0, 0) &&
		    (status->seg_status.type == SEG_STATUS_THIN_POOL)) {
			/* There is -tpool device, but query 'active' state of 'fake' thin-pool */
			if (!_lv_info(cmd, lv, 0, NULL, NULL, NULL, 0, 0) &&
			    !status->seg_status.thin_pool->needs_check)
				status->info.exists = 0; /* So pool LV is not active */
		}
		return 1;
	} else if (lv_is_external_origin(lv)) {
		if (!_lv_info(cmd, lv, 0, &status->info, NULL, NULL,
			      with_open_count, with_read_ahead))
			return_0;

		(void) _lv_info(cmd, lv, 1, NULL, lv_seg, &status->seg_status, 0, 0);
		return 1;
	} else if (lv_is_origin(lv)) {
		/* Query segment status for 'layered' (-real) device most of the time,
		 * only for merging snapshot, query its progress.
		 * TODO: single LV may need couple status to be exposed at once....
		 *       but this needs more logical background
		 */
		/* Show INFO for actual origin and grab status for merging origin */
		if (!_lv_info(cmd, lv, 0, &status->info, lv_seg,
			      lv_is_merging_origin(lv) ? &status->seg_status : NULL,
			      with_open_count, with_read_ahead))
			return_0;

		if (status->info.exists &&
		    (status->seg_status.type != SEG_STATUS_SNAPSHOT)) /* Not merging */
			/* Grab STATUS from layered -real */
			(void) _lv_info(cmd, lv, 1, NULL, lv_seg, &status->seg_status, 0, 0);
		return 1;
	} else if (lv_is_cow(lv)) {
		if (lv_is_merging_cow(lv)) {
			olv = origin_from_cow(lv);

			if (!_lv_info(cmd, olv, 0, &status->info, first_seg(olv), &status->seg_status,
				      with_open_count, with_read_ahead))
				return_0;

			if (status->seg_status.type == SEG_STATUS_SNAPSHOT) {
				log_debug_activation("Snapshot merge is in progress, querying status of %s instead.",
						     display_lvname(lv));
				/*
				 * When merge is in progress, query merging origin LV instead.
				 * COW volume is already mapped as error target in this case.
				 */
				status->lv = olv;
				return 1;
			}

			/* Merge not yet started, still a snapshot... */
		}
		/* Hadle fictional lvm2 snapshot and query snapshotX volume */
		lv_seg = find_snapshot(lv);
	}

	return _lv_info(cmd, lv, 0, &status->info, lv_seg, &status->seg_status,
			with_open_count, with_read_ahead);
}

#define OPEN_COUNT_CHECK_RETRIES 25
#define OPEN_COUNT_CHECK_USLEEP_DELAY 200000

/* Only report error if error_if_used is set */
int lv_check_not_in_use(const struct logical_volume *lv, int error_if_used)
{
	struct lvinfo info;
	unsigned int open_count_check_retries;

	if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) || !info.exists || !info.open_count)
		return 1;

	/* If sysfs is not used, use open_count information only. */
	if (dm_sysfs_dir()) {
		if (dm_device_has_holders(info.major, info.minor)) {
			if (error_if_used)
				log_error("Logical volume %s is used by another device.",
					  display_lvname(lv));
			else
				log_debug_activation("Logical volume %s is used by another device.",
						     display_lvname(lv));
			return 0;
		}

		if (dm_device_has_mounted_fs(info.major, info.minor)) {
			if (error_if_used)
				log_error("Logical volume %s contains a filesystem in use.",
					  display_lvname(lv));
			else
				log_debug_activation("Logical volume %s contains a filesystem in use.",
						     display_lvname(lv));
			return 0;
		}
	}

	open_count_check_retries = retry_deactivation() ? OPEN_COUNT_CHECK_RETRIES : 1;
	while (info.open_count > 0 && open_count_check_retries--) {
		if (!open_count_check_retries) {
			if (error_if_used)
				log_error("Logical volume %s in use.", display_lvname(lv));
			else
				log_debug_activation("Logical volume %s in use.", display_lvname(lv));
			return 0;
		}

		usleep(OPEN_COUNT_CHECK_USLEEP_DELAY);
		log_debug_activation("Retrying open_count check for %s.",
				     display_lvname(lv));
		if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0)) {
			stack; /* device dissappeared? */
			break;
		}
	}

	return 1;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_check_transient(struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!activation())
		return 0;

	log_debug_activation("Checking transient status for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_transient(dm, lv)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_snapshot_percent(const struct logical_volume *lv, dm_percent_t *percent)
{
	int r;
	struct dev_manager *dm;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking snapshot percent for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_snapshot_percent(dm, lv, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/* FIXME Merge with snapshot_percent */
int lv_mirror_percent(struct cmd_context *cmd, const struct logical_volume *lv,
		      int wait, dm_percent_t *percent, uint32_t *event_nr)
{
	int r;
	struct dev_manager *dm;

	/* If mirrored LV is temporarily shrinked to 1 area (= linear),
	 * it should be considered in-sync. */
	if (dm_list_size(&lv->segments) == 1 && first_seg(lv)->area_count == 1) {
		*percent = DM_PERCENT_100;
		return 1;
	}

	if (!lv_info(cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking mirror percent for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_mirror_percent(dm, lv, wait, percent, event_nr)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

int lv_raid_percent(const struct logical_volume *lv, dm_percent_t *percent)
{
	return lv_mirror_percent(lv->vg->cmd, lv, 0, percent, NULL);
}

int lv_raid_data_offset(const struct logical_volume *lv, uint64_t *data_offset)
{
	int r;
	struct dev_manager *dm;
	struct dm_status_raid *status;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid data offset and dev sectors for LV %s/%s",
			     lv->vg->name, lv->name);
	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_raid_status(dm, lv, &status)))
		stack;

	*data_offset = status->data_offset;

	dev_manager_destroy(dm);

	return r;
}

int lv_raid_dev_health(const struct logical_volume *lv, char **dev_health)
{
	int r;
	struct dev_manager *dm;
	struct dm_status_raid *status;

	*dev_health = NULL;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid device health for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_raid_status(dm, lv, &status)) ||
	    !(*dev_health = dm_pool_strdup(lv->vg->cmd->mem,
					   status->dev_health))) {
		dev_manager_destroy(dm);
		return_0;
	}

	dev_manager_destroy(dm);

	return r;
}

int lv_raid_dev_count(const struct logical_volume *lv, uint32_t *dev_cnt)
{
	struct dev_manager *dm;
	struct dm_status_raid *status;

	*dev_cnt = 0;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid device count for LV %s/%s",
			     lv->vg->name, lv->name);
	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!dev_manager_raid_status(dm, lv, &status)) {
		dev_manager_destroy(dm);
		return_0;
	}
	*dev_cnt = status->dev_count;

	dev_manager_destroy(dm);

	return 1;
}

int lv_raid_mismatch_count(const struct logical_volume *lv, uint64_t *cnt)
{
	struct dev_manager *dm;
	struct dm_status_raid *status;

	*cnt = 0;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid mismatch count for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!dev_manager_raid_status(dm, lv, &status)) {
		dev_manager_destroy(dm);
		return_0;
	}
	*cnt = status->mismatch_count;

	dev_manager_destroy(dm);

	return 1;
}

int lv_raid_sync_action(const struct logical_volume *lv, char **sync_action)
{
	struct dev_manager *dm;
	struct dm_status_raid *status;
	char *action;

	*sync_action = NULL;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking raid sync_action for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	/* status->sync_action can be NULL if dm-raid version < 1.5.0 */
	if (!dev_manager_raid_status(dm, lv, &status) ||
	    !status->sync_action ||
	    !(action = dm_pool_strdup(lv->vg->cmd->mem,
				      status->sync_action))) {
		dev_manager_destroy(dm);
		return_0;
	}

	*sync_action = action;

	dev_manager_destroy(dm);

	return 1;
}

int lv_raid_message(const struct logical_volume *lv, const char *msg)
{
	int r = 0;
	struct dev_manager *dm;
	struct dm_status_raid *status;

	if (!seg_is_raid(first_seg(lv))) {
		/*
		 * Make it easier for user to know what to do when
		 * they are using thinpool.
		 */
		if (lv_is_thin_pool(lv) &&
		    (lv_is_raid(seg_lv(first_seg(lv), 0)) ||
		     lv_is_raid(first_seg(lv)->metadata_lv))) {
			log_error("Thin pool data or metadata volume "
				  "must be specified. (E.g. \"%s_tdata\")",
				  display_lvname(lv));
			return 0;
		}
		log_error("%s must be a RAID logical volume to perform this action.",
			  display_lvname(lv));
		return 0;
	}

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0)) {
		log_error("Unable to send message to an inactive logical volume.");
		return 0;
	}

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_raid_status(dm, lv, &status))) {
		log_error("Failed to retrieve status of %s.",
			  display_lvname(lv));
		goto out;
	}

	if (!status->sync_action) {
		log_error("Kernel driver does not support this action: %s", msg);
		goto out;
	}

	/*
	 * Note that 'dev_manager_raid_message' allows us to pass down any
	 * currently valid message.  However, this function restricts the
	 * number of user available combinations to a minimum.  Specifically,
	 *     "idle" -> "check"
	 *     "idle" -> "repair"
	 * (The state automatically switches to "idle" when a sync process is
	 * complete.)
	 */
	if (strcmp(msg, "check") && strcmp(msg, "repair")) {
		/*
		 * MD allows "frozen" to operate in a toggling fashion.
		 * We could allow this if we like...
		 */
		log_error("\"%s\" is not a supported sync operation.", msg);
		goto out;
	}
	if (strcmp(status->sync_action, "idle")) {
		log_error("%s state is currently \"%s\".  Unable to switch to \"%s\".",
			  display_lvname(lv), status->sync_action, msg);
		goto out;
	}

	r = dev_manager_raid_message(dm, lv, msg);
out:
	dev_manager_destroy(dm);

	return r;
}

/*
 * Return dm_status_cache for cache volume, accept also cache pool
 *
 * As there are too many variable for cache volumes, and it hard
 * to make good API - so let's obtain dm_status_cache and return
 * all info we have - user just has to release struct after its use.
 */
int lv_cache_status(const struct logical_volume *cache_lv,
		    struct lv_status_cache **status)
{
	struct dev_manager *dm;
	struct lv_segment *cache_seg;

	if (lv_is_cache_pool(cache_lv)) {
		if (dm_list_empty(&cache_lv->segs_using_this_lv) ||
		    !(cache_seg = get_only_segment_using_this_lv(cache_lv))) {
			log_error(INTERNAL_ERROR "Cannot check status for unused cache pool %s.",
				  display_lvname(cache_lv));
			return 0;
		}
		cache_lv = cache_seg->lv;
	}

	if (lv_is_pending_delete(cache_lv)) {
		log_error("Cannot check status for deleted cache volume %s.",
			  display_lvname(cache_lv));
		return 0;
	}

	if (!lv_info(cache_lv->vg->cmd, cache_lv, 1, NULL, 0, 0)) {
		log_error("Cannot check status for locally inactive cache volume %s.",
			  display_lvname(cache_lv));
		return 0;
	}

	log_debug_activation("Checking status for cache volume %s.",
			     display_lvname(cache_lv));

	if (!(dm = dev_manager_create(cache_lv->vg->cmd, cache_lv->vg->name, 1)))
		return_0;

	if (!dev_manager_cache_status(dm, cache_lv, status)) {
		dev_manager_destroy(dm);
		return_0;
	}
	/* User has to call dm_pool_destroy(status->mem)! */

	return 1;
}

/*
 * Returns data or metadata percent usage, depends on metadata 0/1.
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_thin_pool_percent(const struct logical_volume *lv, int metadata,
			 dm_percent_t *percent)
{
	int r;
	struct dev_manager *dm;

	if (!lv_info(lv->vg->cmd, lv, 1, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking thin %sdata percent for LV %s.",
			     (metadata) ? "meta" : "", display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_pool_percent(dm, lv, metadata, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns 1 if percent set, else 0 on failure.
 */
int lv_thin_percent(const struct logical_volume *lv,
		    int mapped, dm_percent_t *percent)
{
	int r;
	struct dev_manager *dm;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking thin percent for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_percent(dm, lv, mapped, percent)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

/*
 * Returns 1 if transaction_id set, else 0 on failure.
 */
int lv_thin_pool_transaction_id(const struct logical_volume *lv,
				uint64_t *transaction_id)
{
	int r;
	struct dev_manager *dm;
	struct dm_status_thin_pool *status;

	if (!lv_info(lv->vg->cmd, lv, 1, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking thin-pool transaction id for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_pool_status(dm, lv, &status, 0)))
		stack;
	else
		*transaction_id = status->transaction_id;

	dev_manager_destroy(dm);

	return r;
}

int lv_thin_device_id(const struct logical_volume *lv, uint32_t *device_id)
{
	int r;
	struct dev_manager *dm;

	if (!lv_info(lv->vg->cmd, lv, 0, NULL, 0, 0))
		return 0;

	log_debug_activation("Checking device id for LV %s.",
			     display_lvname(lv));

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_thin_device_id(dm, lv, device_id)))
		stack;

	dev_manager_destroy(dm);

	return r;
}

static int _lv_active(struct cmd_context *cmd, const struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 0, 0)) {
		log_debug("Cannot determine activation status of %s%s.",
			  display_lvname(lv),
			  activation() ? "" : " (no device driver)");
		return 0;
	}

	return info.exists;
}

static int _lv_open_count(struct cmd_context *cmd, const struct logical_volume *lv)
{
	struct lvinfo info;

	if (!lv_info(cmd, lv, 0, &info, 1, 0)) {
		stack;
		return -1;
	}

	return info.open_count;
}

static int _lv_activate_lv(const struct logical_volume *lv, struct lv_activate_opts *laopts)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, !lv_is_pvmove(lv))))
		return_0;

	if (!(r = dev_manager_activate(dm, lv, laopts)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_preload(const struct logical_volume *lv, struct lv_activate_opts *laopts,
		       int *flush_required)
{
	int r = 0;
	struct dev_manager *dm;
	int old_readonly = laopts->read_only;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, !lv_is_pvmove(lv))))
		goto_out;

	laopts->read_only = _passes_readonly_filter(lv->vg->cmd, lv);

	if (!(r = dev_manager_preload(dm, lv, laopts, flush_required)))
		stack;

	dev_manager_destroy(dm);

	laopts->read_only = old_readonly;
out:
	return r;
}

static int _lv_deactivate(const struct logical_volume *lv)
{
	int r;
	struct dev_manager *dm;

	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, 1)))
		return_0;

	if (!(r = dev_manager_deactivate(dm, lv)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

static int _lv_suspend_lv(const struct logical_volume *lv, struct lv_activate_opts *laopts,
			  int lockfs, int flush_required)
{
	int r;
	struct dev_manager *dm;

	laopts->read_only = _passes_readonly_filter(lv->vg->cmd, lv);

	/*
	 * When we are asked to manipulate (normally suspend/resume) the PVMOVE
	 * device directly, we don't want to touch the devices that use it.
	 */
	if (!(dm = dev_manager_create(lv->vg->cmd, lv->vg->name, !lv_is_pvmove(lv))))
		return_0;

	if (!(r = dev_manager_suspend(dm, lv, laopts, lockfs, flush_required)))
		stack;

	dev_manager_destroy(dm);
	return r;
}

/*
 * These two functions return the number of visible LVs in the state,
 * or -1 on error.  FIXME Check this.
 */
int lvs_in_vg_activated(const struct volume_group *vg)
{
	struct lv_list *lvl;
	int count = 0;

	if (!activation())
		return 0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_visible(lvl->lv))
			count += (_lv_active(vg->cmd, lvl->lv) == 1);

	log_debug_activation("Counted %d active LVs in VG %s", count, vg->name);

	return count;
}

int lvs_in_vg_opened(const struct volume_group *vg)
{
	const struct lv_list *lvl;
	int count = 0;

	if (!activation())
		return 0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_visible(lvl->lv))
			count += (_lv_open_count(vg->cmd, lvl->lv) > 0);

	log_debug_activation("Counted %d open LVs in VG %s.", count, vg->name);

	return count;
}

/*
 * _lv_is_active
 * @lv:        logical volume being queried
 * @locally:   set if active locally (when provided)
 * @remotely:  set if active remotely (when provided)
 * @exclusive: set if active exclusively (when provided)
 *
 * Determine whether an LV is active locally or in a cluster.
 * In addition to the return code which indicates whether or
 * not the LV is active somewhere, two other values are set
 * to yield more information about the status of the activation:
 *
 *	return	locally	exclusively	status
 *	======	=======	===========	======
 *	   0	   0	    0		not active
 *	   1	   0	    0		active remotely
 *	   1	   0	    1		exclusive remotely
 *	   1	   1	    0		active locally and possibly remotely
 *	   1	   1	    1		exclusive locally (or local && !cluster)
 * The VG lock must be held to call this function.
 *
 * Returns: 0 or 1
 */
static int _lv_is_active(const struct logical_volume *lv,
			 int *locally, int *remotely, int *exclusive)
{
	int r, l, e; /* remote, local, and exclusive */
	int skip_cluster_query = 0;

	r = l = e = 0;

	if (_lv_active(lv->vg->cmd, lv))
		l = 1;

	if (!vg_is_clustered(lv->vg)) {
		if (l)
			e = 1;  /* exclusive by definition */
		goto out;
	}

	/* Active locally, and the caller doesn't care about exclusive or remotely */
	if (l && !exclusive && !remotely)
		skip_cluster_query = 1;

	if (skip_cluster_query)
		goto out;

	if ((r = cluster_lock_held(lv->lvid.s, "", &e)) >= 0)
		goto out;

	/*
	 * If lock query is not supported (due to interfacing with old
	 * code), then we cannot evaluate exclusivity properly.
	 *
	 * Old users of this function will never be affected by this,
	 * since they are only concerned about active vs. not active.
	 * New users of this function who specifically ask for 'exclusive'
	 * will be given a warning message.
	 */
	log_warn("WARNING: Unable to determine exclusivity of %s.", display_lvname(lv));

	e = 0;

	/* Also set remotely as a precaution, as we don't know */
	r = 1;

	/*
	 * We used to attempt activate_lv_excl_local(lv->vg->cmd, lv) here,
	 * but it's unreliable.
	 */

out:
	if (locally)
		*locally = l;
	if (exclusive)
		*exclusive = e;
	if (remotely)
		*remotely = r;

	log_very_verbose("%s is %sactive%s%s%s%s",
			 display_lvname(lv),
			 (r || l) ? "" : "not ",
			 (exclusive && e) ? " exclusive" : "",
			 l ? " locally" : "",
			 (!skip_cluster_query && l && r) ? " and" : "",
			 (!skip_cluster_query && r) ? " remotely" : "");

	return r || l;
}

/*
 * Check if "raid4" @segtype is supported by kernel.
 *
 * if segment type is not raid4, return 1.
 */
int raid4_is_supported(struct cmd_context *cmd, const struct segment_type *segtype)
{
	unsigned attrs;

	if (segtype_is_raid4(segtype) &&
	    (!segtype->ops->target_present ||
             !segtype->ops->target_present(cmd, NULL, &attrs) ||
             !(attrs & RAID_FEATURE_RAID4))) {
		log_error("RAID module does not support RAID4.");
		return 0;
	}

	return 1;
}

int lv_is_active(const struct logical_volume *lv)
{
	return _lv_is_active(lv, NULL, NULL, NULL);
}

int lv_is_active_locally(const struct logical_volume *lv)
{
	int l;

	return _lv_is_active(lv, &l, NULL, NULL) && l;
}

int lv_is_active_remotely(const struct logical_volume *lv)
{
	int r;

	return _lv_is_active(lv, NULL, &r, NULL) && r;
}

int lv_is_active_but_not_locally(const struct logical_volume *lv)
{
	int l;

	return _lv_is_active(lv, &l, NULL, NULL) && !l;
}

int lv_is_active_exclusive(const struct logical_volume *lv)
{
	int e;

	return _lv_is_active(lv, NULL, NULL, &e) && e;
}

int lv_is_active_exclusive_locally(const struct logical_volume *lv)
{
	int l, e;

	return _lv_is_active(lv, &l, NULL, &e) && l && e;
}

int lv_is_active_exclusive_remotely(const struct logical_volume *lv)
{
	int l, e;

	return _lv_is_active(lv, &l, NULL, &e) && !l && e;
}

#ifdef DMEVENTD
static struct dm_event_handler *_create_dm_event_handler(struct cmd_context *cmd, const char *dmuuid, const char *dso,
							 const int timeout, enum dm_event_mask mask)
{
	struct dm_event_handler *dmevh;

	if (!(dmevh = dm_event_handler_create()))
		return_NULL;

	if (dm_event_handler_set_dmeventd_path(dmevh, find_config_tree_str(cmd, dmeventd_executable_CFG, NULL)))
		goto_bad;

	if (dso && dm_event_handler_set_dso(dmevh, dso))
		goto_bad;

	if (dm_event_handler_set_uuid(dmevh, dmuuid))
		goto_bad;

	dm_event_handler_set_timeout(dmevh, timeout);
	dm_event_handler_set_event_mask(dmevh, mask);

	return dmevh;

bad:
	dm_event_handler_destroy(dmevh);
	return NULL;
}

char *get_monitor_dso_path(struct cmd_context *cmd, const char *libpath)
{
	char *path;

	if (!(path = dm_pool_alloc(cmd->mem, PATH_MAX))) {
		log_error("Failed to allocate dmeventd library path.");
		return NULL;
	}

	get_shared_library_path(cmd, libpath, path, PATH_MAX);

	return path;
}

static char *_build_target_uuid(struct cmd_context *cmd, const struct logical_volume *lv)
{
	const char *layer;

	if (lv_is_thin_pool(lv))
		layer = "tpool"; /* Monitor "tpool" for the "thin pool". */
	else if (lv_is_origin(lv))
		layer = "real"; /* Monitor "real" for "snapshot-origin". */
	else
		layer = NULL;

	return build_dm_uuid(cmd->mem, lv, layer);
}

static int _device_registered_with_dmeventd(struct cmd_context *cmd, const struct logical_volume *lv, int *pending, const char **dso)
{
	char *uuid;
	enum dm_event_mask evmask = 0;
	struct dm_event_handler *dmevh;

	*pending = 0;

	if (!(uuid = _build_target_uuid(cmd, lv)))
		return_0;

	if (!(dmevh = _create_dm_event_handler(cmd, uuid, NULL, 0, DM_EVENT_ALL_ERRORS)))
		return_0;

	if (dm_event_get_registered_device(dmevh, 0)) {
		dm_event_handler_destroy(dmevh);
		return 0;
	}

	evmask = dm_event_handler_get_event_mask(dmevh);
	if (evmask & DM_EVENT_REGISTRATION_PENDING) {
		*pending = 1;
		evmask &= ~DM_EVENT_REGISTRATION_PENDING;
	}

	if (dso && (*dso = dm_event_handler_get_dso(dmevh)) && !(*dso = dm_pool_strdup(cmd->mem, *dso)))
		log_error("Failed to duplicate dso name.");

	dm_event_handler_destroy(dmevh);

	return evmask;
}

int target_registered_with_dmeventd(struct cmd_context *cmd, const char *dso,
				    const struct logical_volume *lv, int *pending)
{
	char *uuid;
	enum dm_event_mask evmask = 0;
	struct dm_event_handler *dmevh;
	*pending = 0;

	if (!dso)
		return_0;

	if (!(uuid = _build_target_uuid(cmd, lv)))
		return_0;

	if (!(dmevh = _create_dm_event_handler(cmd, uuid, dso, 0, DM_EVENT_ALL_ERRORS)))
		return_0;

	if (dm_event_get_registered_device(dmevh, 0)) {
		dm_event_handler_destroy(dmevh);
		return 0;
	}

	evmask = dm_event_handler_get_event_mask(dmevh);
	if (evmask & DM_EVENT_REGISTRATION_PENDING) {
		*pending = 1;
		evmask &= ~DM_EVENT_REGISTRATION_PENDING;
	}

	dm_event_handler_destroy(dmevh);

	return evmask;
}

int target_register_events(struct cmd_context *cmd, const char *dso, const struct logical_volume *lv,
			    int evmask __attribute__((unused)), int set, int timeout)
{
	char *uuid;
	struct dm_event_handler *dmevh;
	int r;

	if (!dso)
		return_0;

	/* We always monitor the "real" device, never the "snapshot-origin" itself. */
	if (!(uuid = _build_target_uuid(cmd, lv)))
		return_0;

	if (!(dmevh = _create_dm_event_handler(cmd, uuid, dso, timeout,
					       DM_EVENT_ALL_ERRORS | (timeout ? DM_EVENT_TIMEOUT : 0))))
		return_0;

	r = set ? dm_event_register_handler(dmevh) : dm_event_unregister_handler(dmevh);

	dm_event_handler_destroy(dmevh);

	if (!r)
		return_0;

	log_very_verbose("%s %s for events", set ? "Monitored" : "Unmonitored", uuid);

	return 1;
}

#endif

/*
 * Returns 0 if an attempt to (un)monitor the device failed.
 * Returns 1 otherwise.
 */
int monitor_dev_for_events(struct cmd_context *cmd, const struct logical_volume *lv,
			   const struct lv_activate_opts *laopts, int monitor)
{
#ifdef DMEVENTD
	int i, pending = 0, monitored;
	int r = 1;
	struct dm_list *snh, *snht;
	struct lv_segment *seg;
	struct lv_segment *log_seg;
	int (*monitor_fn) (struct lv_segment *s, int e);
	uint32_t s;
	static const struct lv_activate_opts zlaopts = { 0 };
	struct lvinfo info;
	const char *dso = NULL;
	int new_unmonitor;

	if (!laopts)
		laopts = &zlaopts;

	/* skip dmeventd code altogether */
	if (dmeventd_monitor_mode() == DMEVENTD_MONITOR_IGNORE)
		return 1;

	/*
	 * Nothing to do if dmeventd configured not to be used.
	 */
	if (monitor && !dmeventd_monitor_mode())
		return 1;

	/*
	 * Activation of unused cache-pool activates metadata device as
	 * a public LV  for clearing purpose.
	 * FIXME:
	 *  As VG lock is held across whole operation unmonitored volume
	 *  is usually OK since dmeventd couldn't do anything.
	 *  However in case command would have crashed, such LV is
	 *  left unmonitored and may potentially require dmeventd.
	 */
	if ((lv_is_cache_pool_data(lv) || lv_is_cache_pool_metadata(lv)) &&
	    !lv_is_used_cache_pool((find_pool_seg(first_seg(lv))->lv))) {
		log_debug_activation("Skipping %smonitor of %s.%s",
				     (monitor) ? "" : "un", display_lvname(lv),
				     (monitor) ? " Cache pool activation for clearing only." : "");
		return 1;
	}

	/*
	 * Allow to unmonitor thin pool via explicit pool unmonitor
	 * or unmonitor before the last thin pool user deactivation
	 * Skip unmonitor, if invoked via deactivation of thin volume
	 * and there is another thin pool user (open_count > 1)
	 * FIXME  think about watch ruler influence.
	 */
	if (laopts->skip_in_use && lv_is_thin_pool(lv) &&
	    lv_info(lv->vg->cmd, lv, 1, &info, 1, 0) && (info.open_count > 1)) {
		log_debug_activation("Skipping unmonitor of opened %s (open:%d)",
				     display_lvname(lv), info.open_count);
		return 1;
	}

	/* Do not monitor snapshot that already covers origin */
	if (monitor && lv_is_cow_covering_origin(lv)) {
		log_debug_activation("Skipping monitor of snapshot larger "
				     "then origin %s.", display_lvname(lv));
		return 1;
	}

	/*
	 * In case of a snapshot device, we monitor lv->snapshot->lv,
	 * not the actual LV itself.
	 */
	if (lv_is_cow(lv) && (laopts->no_merging || !lv_is_merging_cow(lv))) {
		if (!(r = monitor_dev_for_events(cmd, lv->snapshot->lv, NULL, monitor)))
			stack;
		return r;
	}

	/*
	 * In case this LV is a snapshot origin, we instead monitor
	 * each of its respective snapshots.  The origin itself may
	 * also need to be monitored if it is a mirror, for example,
	 * so fall through to process it afterwards.
	 */
	if (!laopts->origin_only && lv_is_origin(lv))
		dm_list_iterate_safe(snh, snht, &lv->snapshot_segs)
			if (!monitor_dev_for_events(cmd, dm_list_struct_base(snh,
				struct lv_segment, origin_list)->cow, NULL, monitor)) {
				stack;
				r = 0;
			}

	/*
	 * If the volume is mirrored and its log is also mirrored, monitor
	 * the log volume as well.
	 */
	if ((seg = first_seg(lv)) != NULL && seg->log_lv != NULL &&
	    (log_seg = first_seg(seg->log_lv)) != NULL &&
	    seg_is_mirrored(log_seg))
		if (!monitor_dev_for_events(cmd, seg->log_lv, NULL, monitor)) {
			stack;
			r = 0;
		}

	dm_list_iterate_items(seg, &lv->segments) {
		/* Recurse for AREA_LV */
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_LV)
				continue;
			if (!monitor_dev_for_events(cmd, seg_lv(seg, s), NULL,
						    monitor)) {
				log_error("Failed to %smonitor %s",
					  monitor ? "" : "un",
					  display_lvname(seg_lv(seg, s)));
				r = 0;
			}
		}

		/*
		 * If requested unmonitoring of thin volume, preserve skip_in_use flag.
		 *
		 * FIXME: code here looks like _lv_postorder()
		 */
		if (seg->pool_lv &&
		    !monitor_dev_for_events(cmd, seg->pool_lv,
					    (!monitor) ? laopts : NULL, monitor)) {
			stack;
			r = 0;
		}

		if (seg->metadata_lv &&
		    !monitor_dev_for_events(cmd, seg->metadata_lv, NULL, monitor)) {
			stack;
			r = 0;
		}

		if (!seg_monitored(seg) ||
		    (seg->status & PVMOVE) ||
		    !seg->segtype->ops->target_monitored) /* doesn't support registration */
			continue;

		if (!monitor)
			/* When unmonitoring, obtain existing dso being used. */
			monitored = _device_registered_with_dmeventd(cmd, seg_is_snapshot(seg) ? seg->cow : seg->lv, &pending, &dso);
		else
			monitored = seg->segtype->ops->target_monitored(seg, &pending);

		/* FIXME: We should really try again if pending */
		monitored = (pending) ? 0 : monitored;

		monitor_fn = NULL;
		new_unmonitor = 0;

		if (monitor) {
			if (monitored)
				log_verbose("%s already monitored.", display_lvname(lv));
			else if (seg->segtype->ops->target_monitor_events) {
				log_verbose("Monitoring %s%s", display_lvname(lv), test_mode() ? " [Test mode: skipping this]" : "");
				monitor_fn = seg->segtype->ops->target_monitor_events;
			}
		} else {
			if (!monitored)
				log_verbose("%s already not monitored.", display_lvname(lv));
			else if (dso && *dso) {
				/*
				 * Divert unmonitor away from code that depends on the new segment
				 * type instead of the existing one if it's changing.
				 */
				log_verbose("Not monitoring %s with %s%s", display_lvname(lv), dso, test_mode() ? " [Test mode: skipping this]" : "");
				new_unmonitor = 1;
			}
		}

		/* FIXME Test mode should really continue a bit further. */
		if (test_mode())
			continue;

		if (new_unmonitor) {
			if (!target_register_events(cmd, dso, seg_is_snapshot(seg) ? seg->cow : lv, 0, 0, 10)) {
				log_error("%s: segment unmonitoring failed.",
					  display_lvname(lv));
 
				return 0;
			}
		} else if (monitor_fn) {
			/* FIXME specify events */
			if (!monitor_fn(seg, 0)) {
				log_error("%s: %s segment monitoring function failed.",
					  display_lvname(lv), lvseg_name(seg));
				return 0;
			}
		} else
			continue;

		/* Check [un]monitor results */
		/* Try a couple times if pending, but not forever... */
		for (i = 0;; i++) {
			pending = 0;
			monitored = seg->segtype->ops->target_monitored(seg, &pending);
			if (!pending || i >= 40)
				break;
			log_very_verbose("%s %smonitoring still pending: waiting...",
					 display_lvname(lv), monitor ? "" : "un");
			usleep(10000 * i);
		}

		if (r)
			r = (monitored && monitor) || (!monitored && !monitor);
	}

	if (!r && !error_message_produced())
		log_error("%sonitoring %s failed.", monitor ? "M" : "Not m",
			  display_lvname(lv));
	return r;
#else
	return 1;
#endif
}

struct detached_lv_data {
	const struct logical_volume *lv_pre;
	struct lv_activate_opts *laopts;
	int *flush_required;
};

static int _preload_detached_lv(struct logical_volume *lv, void *data)
{
	struct detached_lv_data *detached = data;
	struct logical_volume *lv_pre;

	/* Check and preload removed raid image leg or metadata */
	if (lv_is_raid_image(lv)) {
		if ((lv_pre = find_lv_in_vg_by_lvid(detached->lv_pre->vg, &lv->lvid)) &&
		    !lv_is_raid_image(lv_pre) && lv_is_active(lv) &&
		    !_lv_preload(lv_pre, detached->laopts, detached->flush_required))
			return_0;
	} else if (lv_is_raid_metadata(lv)) {
		if ((lv_pre = find_lv_in_vg_by_lvid(detached->lv_pre->vg, &lv->lvid)) &&
		    !lv_is_raid_metadata(lv_pre) && lv_is_active(lv) &&
		    !_lv_preload(lv_pre, detached->laopts, detached->flush_required))
			return_0;
	}

	/* FIXME: condition here should be far more limiting to really
	 *        detect detached LVs */
	if ((lv_pre = find_lv(detached->lv_pre->vg, lv->name))) {
		if (lv_is_visible(lv_pre) && lv_is_active(lv) &&
		    !lv_is_pool(lv) &&
		    (!lv_is_cow(lv) || !lv_is_cow(lv_pre)) &&
		    !_lv_preload(lv_pre, detached->laopts, detached->flush_required))
			return_0;
	}

	return 1;
}

static int _lv_suspend(struct cmd_context *cmd, const char *lvid_s,
		       struct lv_activate_opts *laopts, int error_if_not_suspended,
	               const struct logical_volume *lv, const struct logical_volume *lv_pre)
{
	const struct logical_volume *pvmove_lv = NULL;
	const struct logical_volume *lv_to_free = NULL;
	const struct logical_volume *lv_pre_to_free = NULL;
	struct logical_volume *lv_pre_tmp;
	struct seg_list *sl;
	struct lv_segment *snap_seg;
	struct lvinfo info;
	int r = 0, lockfs = 0, flush_required = 0;
	struct detached_lv_data detached;

	if (!activation())
		return 1;

	/* lv comes from committed metadata */
	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto_out;

	/* Use precommitted metadata if present */
	if (!lv_pre && !(lv_pre_to_free = lv_pre = lv_from_lvid(cmd, lvid_s, 1)))
		goto_out;

	/* Ignore origin_only unless LV is origin in both old and new metadata */
	/* or LV is thin or thin pool volume */
	if (!lv_is_thin_volume(lv) && !lv_is_thin_pool(lv) &&
	    !(lv_is_origin(lv) && lv_is_origin(lv_pre)))
		laopts->origin_only = 0;

	if (test_mode()) {
		_skip("Suspending %s%s.", display_lvname(lv),
		      laopts->origin_only ? " origin without snapshots" : "");
		r = 1;
		goto out;
	}

	if (!lv_info(cmd, lv, laopts->origin_only, &info, 0, 0))
		goto_out;

	if (!info.exists || info.suspended) {
		if (!error_if_not_suspended) {
			r = 1;
			if (info.suspended)
				critical_section_inc(cmd, "already suspended");
		}
		goto out;
	}

	if (!lv_read_replicator_vgs(lv))
		goto_out;

	lv_calculate_readahead(lv, NULL);

	/*
	 * Preload devices for the LV.
	 * If the PVMOVE LV is being removed, it's only present in the old
	 * metadata and not the new, so we must explicitly add the new
	 * tables for all the changed LVs here, as the relationships
	 * are not found by walking the new metadata.
	 */
	if (lv_is_locked(lv) && !lv_is_locked(lv_pre) &&
	    (pvmove_lv = find_pvmove_lv_in_lv(lv))) {
		/* Preload all the LVs above the PVMOVE LV */
		dm_list_iterate_items(sl, &pvmove_lv->segs_using_this_lv) {
			if (!(lv_pre_tmp = find_lv(lv_pre->vg, sl->seg->lv->name))) {
				log_error(INTERNAL_ERROR "LV %s missing from preload metadata.",
					  display_lvname(sl->seg->lv));
				goto out;
			}
			if (!_lv_preload(lv_pre_tmp, laopts, &flush_required))
				goto_out;
		}
		/* Now preload the PVMOVE LV itself */
		if (!(lv_pre_tmp = find_lv(lv_pre->vg, pvmove_lv->name))) {
			log_error(INTERNAL_ERROR "LV %s missing from preload metadata.",
				  display_lvname(pvmove_lv));
			goto out;
		}
		if (!_lv_preload(lv_pre_tmp, laopts, &flush_required))
			goto_out;
	} else {
		if (!_lv_preload(lv_pre, laopts, &flush_required))
			/* FIXME Revert preloading */
			goto_out;

		/*
		 * Search for existing LVs that have become detached and preload them.
		 */
		detached.lv_pre = lv_pre;
		detached.laopts = laopts;
		detached.flush_required = &flush_required;

		if (!for_each_sub_lv((struct logical_volume *)lv, &_preload_detached_lv, &detached))
			goto_out;

		/*
		 * Preload any snapshots that are being removed.
		 */
		if (!laopts->origin_only && lv_is_origin(lv)) {
			dm_list_iterate_items_gen(snap_seg, &lv->snapshot_segs, origin_list) {
				if (!(lv_pre_tmp = find_lv_in_vg_by_lvid(lv_pre->vg, &snap_seg->cow->lvid))) {
					log_error(INTERNAL_ERROR "LV %s (%s) missing from preload metadata.",
						  display_lvname(snap_seg->cow),
						  snap_seg->cow->lvid.id[1].uuid);
					goto out;
				}
				if (!lv_is_cow(lv_pre_tmp) &&
				    !_lv_preload(lv_pre_tmp, laopts, &flush_required))
					goto_out;
			}
		}
	}

	/* Flush is ATM required for the tested cases
	 * NOTE: Mirror repair requires noflush for proper repair!
	 * TODO: Relax this limiting condition further */
	if (!flush_required &&
	    (lv_is_pvmove(lv) ||
	     (!lv_is_mirror(lv) && !lv_is_thin_pool(lv) && !lv_is_thin_volume(lv)))) {
		log_debug("Requiring flush for LV %s.", display_lvname(lv));
		flush_required = 1;
	}

	if (!monitor_dev_for_events(cmd, lv, laopts, 0))
		/* FIXME Consider aborting here */
		stack;

	critical_section_inc(cmd, "suspending");
	if (pvmove_lv)
		critical_section_inc(cmd, "suspending pvmove LV");

	if (!laopts->origin_only &&
	    (lv_is_origin(lv_pre) || lv_is_cow(lv_pre)))
		lockfs = 1;

	/* Converting non-thin LV to thin external origin ? */
	if (!lv_is_thin_volume(lv) && lv_is_thin_volume(lv_pre))
		lockfs = 1; /* Sync before conversion */

	if (laopts->origin_only && lv_is_thin_volume(lv) && lv_is_thin_volume(lv_pre))
		lockfs = 1;

	/*
	 * Suspending an LV directly above a PVMOVE LV also
 	 * suspends other LVs using that same PVMOVE LV.
	 * FIXME Remove this and delay the 'clear node' until
 	 * after the code knows whether there's a different
 	 * inactive table to load or not instead so lv_suspend
 	 * can be called separately for each LV safely.
 	 */
	if ((lv_pre->vg->status & PRECOMMITTED) &&
	    lv_is_locked(lv_pre) && find_pvmove_lv_in_lv(lv_pre)) {
		if (!_lv_suspend_lv(lv_pre, laopts, lockfs, flush_required)) {
			critical_section_dec(cmd, "failed precommitted suspend");
			if (pvmove_lv)
				critical_section_dec(cmd, "failed precommitted suspend (pvmove)");
			goto_out;
		}
	} else {
		/* Normal suspend */
		if (!_lv_suspend_lv(lv, laopts, lockfs, flush_required)) {
			critical_section_dec(cmd, "failed suspend");
			if (pvmove_lv)
				critical_section_dec(cmd, "failed suspend (pvmove)");
			goto_out;
		}
	}

	r = 1;
out:
	if (lv_pre_to_free)
		release_vg(lv_pre_to_free->vg);
	if (lv_to_free) {
		lv_release_replicator_vgs(lv_to_free);
		release_vg(lv_to_free->vg);
	}

	return r;
}

/*
 * In a cluster, set exclusive to indicate that only one node is using the
 * device.  Any preloaded tables may then use non-clustered targets.
 *
 * Returns success if the device is not active
 */
int lv_suspend_if_active(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only, unsigned exclusive,
			 const struct logical_volume *lv, const struct logical_volume *lv_pre)
{
	struct lv_activate_opts laopts = {
		.origin_only = origin_only,
		.exclusive = exclusive
	};

	return _lv_suspend(cmd, lvid_s, &laopts, 0, lv, lv_pre);
}


static int _lv_resume(struct cmd_context *cmd, const char *lvid_s,
		      struct lv_activate_opts *laopts, int error_if_not_active,
	              const struct logical_volume *lv)
{
	const struct logical_volume *lv_to_free = NULL;
	struct lvinfo info;
	int r = 0;

	if (!activation())
		return 1;

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto_out;

	if (!lv_is_origin(lv) && !lv_is_thin_volume(lv) && !lv_is_thin_pool(lv))
		laopts->origin_only = 0;

	if (test_mode()) {
		_skip("Resuming %s%s%s.", display_lvname(lv),
		      laopts->origin_only ? " without snapshots" : "",
		      laopts->revert ? " (reverting)" : "");
		r = 1;
		goto out;
	}

	log_debug_activation("Resuming LV %s%s%s%s.", display_lvname(lv),
			     error_if_not_active ? "" : " if active",
			     laopts->origin_only ?
			     (lv_is_thin_pool(lv) ? " pool only" :
			      lv_is_thin_volume(lv) ? " thin only" : " without snapshots") : "",
			     laopts->revert ? " (reverting)" : "");

	if (!lv_info(cmd, lv, laopts->origin_only, &info, 0, 0))
		goto_out;

	if (!info.exists || !info.suspended) {
		if (error_if_not_active)
			goto_out;
		r = 1;
		if (!info.suspended)
			critical_section_dec(cmd, "already resumed");
		goto out;
	}

	laopts->read_only = _passes_readonly_filter(cmd, lv);
	laopts->resuming = 1;

	if (!_lv_activate_lv(lv, laopts))
		goto_out;

	critical_section_dec(cmd, "resumed");

	if (!monitor_dev_for_events(cmd, lv, laopts, 1))
		stack;

	r = 1;
out:
	if (lv_to_free)
		release_vg(lv_to_free->vg);

	return r;
}

/*
 * In a cluster, set exclusive to indicate that only one node is using the
 * device.  Any tables loaded may then use non-clustered targets.
 *
 * @origin_only
 * @exclusive   This parameter only has an affect in cluster-context.
 *              It forces local target type to be used (instead of
 *              cluster-aware type).
 * Returns success if the device is not active
 */
int lv_resume_if_active(struct cmd_context *cmd, const char *lvid_s,
			unsigned origin_only, unsigned exclusive,
			unsigned revert, const struct logical_volume *lv)
{
	struct lv_activate_opts laopts = {
		.origin_only = origin_only,
		.exclusive = exclusive,
		.revert = revert
	};

	return _lv_resume(cmd, lvid_s, &laopts, 0, lv);
}

int lv_resume(struct cmd_context *cmd, const char *lvid_s, unsigned origin_only,
	      const struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { .origin_only = origin_only, };

	return _lv_resume(cmd, lvid_s, &laopts, 1, lv);
}

static int _lv_has_open_snapshots(const struct logical_volume *lv)
{
	struct lv_segment *snap_seg;
	int r = 0;

	dm_list_iterate_items_gen(snap_seg, &lv->snapshot_segs, origin_list)
		if (!lv_check_not_in_use(snap_seg->cow, 1))
			r++;

	if (r)
		log_error("LV %s has open %d snapshot(s), not deactivating.",
			  display_lvname(lv), r);

	return r;
}

int lv_deactivate(struct cmd_context *cmd, const char *lvid_s, const struct logical_volume *lv)
{
	const struct logical_volume *lv_to_free = NULL;
	struct lvinfo info;
	static const struct lv_activate_opts laopts = { .skip_in_use = 1 };
	struct dm_list *snh;
	int r = 0;

	if (!activation())
		return 1;

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto out;

	if (test_mode()) {
		_skip("Deactivating %s.", display_lvname(lv));
		r = 1;
		goto out;
	}

	log_debug_activation("Deactivating %s.", display_lvname(lv));

	if (!lv_info(cmd, lv, 0, &info, 0, 0))
		goto_out;

	if (!info.exists) {
		r = 1;
		/* Check attached snapshot segments are also inactive */
		dm_list_iterate(snh, &lv->snapshot_segs) {
			if (!lv_info(cmd, dm_list_struct_base(snh, struct lv_segment, origin_list)->cow,
				     0, &info, 0, 0))
				goto_out;
			if (info.exists) {
				r = 0; /* Snapshot left in table? */
				break;
			}
		}

		if (r)
			goto out;
	}

	if (lv_is_visible(lv) || lv_is_virtual_origin(lv) ||
	    lv_is_merging_thin_snapshot(lv)) {
		if (!lv_check_not_in_use(lv, 1))
			goto_out;

		if (lv_is_origin(lv) && _lv_has_open_snapshots(lv))
			goto_out;
	}

	if (!lv_read_replicator_vgs(lv))
		goto_out;

	if (!monitor_dev_for_events(cmd, lv, &laopts, 0))
		stack;

	critical_section_inc(cmd, "deactivating");
	r = _lv_deactivate(lv);
	critical_section_dec(cmd, "deactivated");

	if (!lv_info(cmd, lv, 0, &info, 0, 0) || info.exists) {
		/* Turn into log_error, but we do not log error */
		log_debug_activation("Deactivated volume is still %s present.",
				     display_lvname(lv));
		r = 0;
	}
out:
	if (lv_to_free) {
		lv_release_replicator_vgs(lv_to_free);
		release_vg(lv_to_free->vg);
	}

	return r;
}

/* Test if LV passes filter */
int lv_activation_filter(struct cmd_context *cmd, const char *lvid_s,
			 int *activate_lv, const struct logical_volume *lv)
{
	const struct logical_volume *lv_to_free = NULL;
	int r = 0;

	if (!activation()) {
		*activate_lv = 1;
		return 1;
	}

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto_out;

	if (!_passes_activation_filter(cmd, lv)) {
		log_verbose("Not activating %s since it does not pass "
			    "activation filter.", display_lvname(lv));
		*activate_lv = 0;
	} else
		*activate_lv = 1;
	r = 1;
out:
	if (lv_to_free)
		release_vg(lv_to_free->vg);

	return r;
}

static int _lv_activate(struct cmd_context *cmd, const char *lvid_s,
			struct lv_activate_opts *laopts, int filter,
	                const struct logical_volume *lv)
{
	const struct logical_volume *lv_to_free = NULL;
	struct lvinfo info;
	int r = 0;

	if (!activation())
		return 1;

	if (!lv && !(lv_to_free = lv = lv_from_lvid(cmd, lvid_s, 0)))
		goto out;

	if (filter && !_passes_activation_filter(cmd, lv)) {
		log_verbose("Not activating %s since it does not pass "
			    "activation filter.", display_lvname(lv));
		r = 1;
		goto out;
	}

	if ((!lv->vg->cmd->partial_activation) && lv_is_partial(lv)) {
		if (!lv_is_raid_type(lv) || !partial_raid_lv_supports_degraded_activation(lv)) {
			log_error("Refusing activation of partial LV %s.  "
				  "Use '--activationmode partial' to override.",
				  display_lvname(lv));
			goto out;
		}

		if (!lv->vg->cmd->degraded_activation) {
			log_error("Refusing activation of partial LV %s.  "
				  "Try '--activationmode degraded'.",
				  display_lvname(lv));
			goto out;
		}
	}

	if (lv_has_unknown_segments(lv)) {
		log_error("Refusing activation of LV %s containing "
			  "an unrecognised segment.", display_lvname(lv));
		goto out;
	}

	/*
	 * Check if cmirrord is running for clustered mirrors.
	 */
	if (!laopts->exclusive && vg_is_clustered(lv->vg) &&
	    lv_is_mirror(lv) && !lv_is_raid(lv) &&
	    !cluster_mirror_is_available(lv->vg->cmd)) {
		log_error("Shared cluster mirrors are not available.");
		goto out;
	}

	if (test_mode()) {
		_skip("Activating %s.", display_lvname(lv));
		r = 1;
		goto out;
	}

	if (filter)
		laopts->read_only = _passes_readonly_filter(cmd, lv);

	log_debug_activation("Activating %s%s%s%s%s.", display_lvname(lv),
			     laopts->exclusive ? " exclusively" : "",
			     laopts->read_only ? " read-only" : "",
			     laopts->noscan ? " noscan" : "",
			     laopts->temporary ? " temporary" : "");

	if (!lv_info(cmd, lv, 0, &info, 0, 0))
		goto_out;

	/*
	 * Nothing to do?
	 */
	if (info.exists && !info.suspended && info.live_table &&
	    (info.read_only == read_only_lv(lv, laopts))) {
		r = 1;
		log_debug_activation("LV %s is already active.", display_lvname(lv));
		goto out;
	}

	if (!lv_read_replicator_vgs(lv))
		goto_out;

	lv_calculate_readahead(lv, NULL);

	critical_section_inc(cmd, "activating");
	if (!(r = _lv_activate_lv(lv, laopts)))
		stack;
	critical_section_dec(cmd, "activated");

	if (r && !monitor_dev_for_events(cmd, lv, laopts, 1))
		stack;

out:
	if (lv_to_free) {
		lv_release_replicator_vgs(lv_to_free);
		release_vg(lv_to_free->vg);
	}

	return r;
}

/* Activate LV */
int lv_activate(struct cmd_context *cmd, const char *lvid_s, int exclusive,
		int noscan, int temporary, const struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { .exclusive = exclusive,
					   .noscan = noscan,
					   .temporary = temporary };

	if (!_lv_activate(cmd, lvid_s, &laopts, 0, lv))
		return_0;

	return 1;
}

/* Activate LV only if it passes filter */
int lv_activate_with_filter(struct cmd_context *cmd, const char *lvid_s, int exclusive,
			    int noscan, int temporary, const struct logical_volume *lv)
{
	struct lv_activate_opts laopts = { .exclusive = exclusive,
					   .noscan = noscan,
					   .temporary = temporary };

	if (!_lv_activate(cmd, lvid_s, &laopts, 1, lv))
		return_0;

	return 1;
}

int lv_mknodes(struct cmd_context *cmd, const struct logical_volume *lv)
{
	int r;

	if (!lv) {
		r = dm_mknodes(NULL);
		fs_unlock();
		return r;
	}

	if (!activation())
		return 1;

	r = dev_manager_mknodes(lv);

	fs_unlock();

	return r;
}

/* Remove any existing, closed mapped device by @name */
static int _remove_dm_dev_by_name(const char *name)
{
	int r = 0;
	struct dm_task *dmt;
	struct dm_info info;

	if (!(dmt = dm_task_create(DM_DEVICE_INFO)))
		return_0;

	/* Check, if the device exists. */
	if (dm_task_set_name(dmt, name) && dm_task_run(dmt) && dm_task_get_info(dmt, &info)) {
		dm_task_destroy(dmt);

		/* Ignore non-existing or open dm devices */
		if (!info.exists || info.open_count)
			return 1;

		if (!(dmt = dm_task_create(DM_DEVICE_REMOVE)))
			return_0;

		if (dm_task_set_name(dmt, name))
			r = dm_task_run(dmt);
	}

	dm_task_destroy(dmt);

	return r;
}

/* Work all segments of @lv removing any existing, closed "*-missing_N_0" sub devices. */
static int _lv_remove_any_missing_subdevs(struct logical_volume *lv)
{
	if (lv) {
		uint32_t seg_no = 0;
		char name[257];
		struct lv_segment *seg;

		dm_list_iterate_items(seg, &lv->segments) {
			if (seg->area_count != 1)
				return_0;
			if (dm_snprintf(name, sizeof(name), "%s-%s-missing_%u_0", seg->lv->vg->name, seg->lv->name, seg_no) < 0)
				return 0;
			if (!_remove_dm_dev_by_name(name))
				return 0;

			seg_no++;
		}
	}

	return 1;
}

/* Remove any "*-missing_*" sub devices added by the activation layer for an rmate/rimage missing PV mapping */
int lv_deactivate_any_missing_subdevs(const struct logical_volume *lv)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);

	for (s = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) == AREA_LV &&
		    !_lv_remove_any_missing_subdevs(seg_lv(seg, s)))
			return 0;
		if (seg->meta_areas && seg_metatype(seg, s) == AREA_LV &&
		    !_lv_remove_any_missing_subdevs(seg_metalv(seg, s)))
			return 0;
	}

	return 1;
}

/*
 * Does PV use VG somewhere in its construction?
 * Returns 1 on failure.
 */
int pv_uses_vg(struct physical_volume *pv,
	       struct volume_group *vg)
{
	if (!activation() || !pv->dev)
		return 0;

	if (!dm_is_dm_major(MAJOR(pv->dev->dev)))
		return 0;

	return dev_manager_device_uses_vg(pv->dev, vg);
}

void activation_release(void)
{
	if (critical_section())
		/* May leak stacked operation */
		log_error("Releasing activation in critical section.");

	fs_unlock(); /* Implicit dev_manager_release(); */
}

void activation_exit(void)
{
	activation_release();
	dev_manager_exit();
}
#endif
