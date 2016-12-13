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

static int _lvconvert_thin_pool_repair(struct cmd_context *cmd,
				       struct logical_volume *pool_lv,
				       struct dm_list *pvh, int poolmetadataspare)
{
	const char *dmdir = dm_dir();
	const char *thin_dump =
		find_config_tree_str_allow_empty(cmd, global_thin_dump_executable_CFG, NULL);
	const char *thin_repair =
		find_config_tree_str_allow_empty(cmd, global_thin_repair_executable_CFG, NULL);
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int ret = 0, status;
	int args = 0;
	const char *argv[19]; /* Max supported 10 args */
	char *dm_name, *trans_id_str;
	char meta_path[PATH_MAX];
	char pms_path[PATH_MAX];
	uint64_t trans_id;
	struct logical_volume *pmslv;
	struct logical_volume *mlv = first_seg(pool_lv)->metadata_lv;
	struct pipe_data pdata;
	FILE *f;

	if (!thin_repair || !thin_repair[0]) {
		log_error("Thin repair commnand is not configured. Repair is disabled.");
		return 0; /* Checking disabled */
	}

	pmslv = pool_lv->vg->pool_metadata_spare_lv;

	/* Check we have pool metadata spare LV */
	if (!handle_pool_metadata_spare(pool_lv->vg, 0, pvh, 1))
		return_0;

	if (pmslv != pool_lv->vg->pool_metadata_spare_lv) {
		if (!vg_write(pool_lv->vg) || !vg_commit(pool_lv->vg))
			return_0;
		pmslv = pool_lv->vg->pool_metadata_spare_lv;
	}

	if (!(dm_name = dm_build_dm_name(cmd->mem, mlv->vg->name,
					 mlv->name, NULL)) ||
	    (dm_snprintf(meta_path, sizeof(meta_path), "%s/%s", dmdir, dm_name) < 0)) {
		log_error("Failed to build thin metadata path.");
		return 0;
	}

	if (!(dm_name = dm_build_dm_name(cmd->mem, pmslv->vg->name,
					 pmslv->name, NULL)) ||
	    (dm_snprintf(pms_path, sizeof(pms_path), "%s/%s", dmdir, dm_name) < 0)) {
		log_error("Failed to build pool metadata spare path.");
		return 0;
	}

	if (!(cn = find_config_tree_array(cmd, global_thin_repair_options_CFG, NULL))) {
		log_error(INTERNAL_ERROR "Unable to find configuration for global/thin_repair_options");
		return 0;
	}

	for (cv = cn->v; cv && args < 16; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Invalid string in config file: "
				  "global/thin_repair_options");
			return 0;
		}
		argv[++args] = cv->v.str;
	}

	if (args == 10) {
		log_error("Too many options for thin repair command.");
		return 0;
	}

	argv[0] = thin_repair;
	argv[++args] = "-i";
	argv[++args] = meta_path;
	argv[++args] = "-o";
	argv[++args] = pms_path;
	argv[++args] = NULL;

	if (pool_is_active(pool_lv)) {
		log_error("Only inactive pool can be repaired.");
		return 0;
	}

	if (!activate_lv_local(cmd, pmslv)) {
		log_error("Cannot activate pool metadata spare volume %s.",
			  pmslv->name);
		return 0;
	}

	if (!activate_lv_local(cmd, mlv)) {
		log_error("Cannot activate thin pool metadata volume %s.",
			  mlv->name);
		goto deactivate_pmslv;
	}

	if (!(ret = exec_cmd(cmd, (const char * const *)argv, &status, 1))) {
		log_error("Repair of thin metadata volume of thin pool %s failed (status:%d). "
			  "Manual repair required!",
			  display_lvname(pool_lv), status);
		goto deactivate_mlv;
	}

	if (thin_dump[0]) {
		argv[0] = thin_dump;
		argv[1] = pms_path;
		argv[2] = NULL;

		if (!(f = pipe_open(cmd, argv, 0, &pdata)))
			log_warn("WARNING: Cannot read output from %s %s.", thin_dump, pms_path);
		else {
			/*
			 * Scan only the 1st. line for transation id.
			 * Watch out, if the thin_dump format changes
			 */
			if (fgets(meta_path, sizeof(meta_path), f) &&
			    (trans_id_str = strstr(meta_path, "transaction=\"")) &&
			    (sscanf(trans_id_str + 13, FMTu64, &trans_id) == 1) &&
			    (trans_id != first_seg(pool_lv)->transaction_id) &&
			    ((trans_id - 1) != first_seg(pool_lv)->transaction_id))
				log_error("Transaction id " FMTu64 " from pool \"%s/%s\" "
					  "does not match repaired transaction id "
					  FMTu64 " from %s.",
					  first_seg(pool_lv)->transaction_id,
					  pool_lv->vg->name, pool_lv->name, trans_id,
					  pms_path);

			(void) pipe_close(&pdata); /* killing pipe */
		}
	}

deactivate_mlv:
	if (!deactivate_lv(cmd, mlv)) {
		log_error("Cannot deactivate thin pool metadata volume %s.",
			  mlv->name);
		return 0;
	}

deactivate_pmslv:
	if (!deactivate_lv(cmd, pmslv)) {
		log_error("Cannot deactivate thin pool metadata volume %s.",
			  mlv->name);
		return 0;
	}

	if (!ret)
		return 0;

	if (pmslv == pool_lv->vg->pool_metadata_spare_lv) {
		pool_lv->vg->pool_metadata_spare_lv = NULL;
		pmslv->status &= ~POOL_METADATA_SPARE;
		lv_set_visible(pmslv);
	}

	/* Try to allocate new pool metadata spare LV */
	if (!handle_pool_metadata_spare(pool_lv->vg, 0, pvh, poolmetadataspare))
		stack;

	if (dm_snprintf(meta_path, sizeof(meta_path), "%s_meta%%d", pool_lv->name) < 0) {
		log_error("Can't prepare new metadata name for %s.", pool_lv->name);
		return 0;
	}

	if (!generate_lv_name(pool_lv->vg, meta_path, pms_path, sizeof(pms_path))) {
		log_error("Can't generate new name for %s.", meta_path);
		return 0;
	}

	if (!detach_pool_metadata_lv(first_seg(pool_lv), &mlv))
		return_0;

	/* Swap _pmspare and _tmeta name */
	if (!swap_lv_identifiers(cmd, mlv, pmslv))
		return_0;

	if (!attach_pool_metadata_lv(first_seg(pool_lv), pmslv))
		return_0;

	/* Used _tmeta (now _pmspare) becomes _meta%d */
	if (!lv_rename_update(cmd, mlv, pms_path, 0))
		return_0;

	if (!vg_write(pool_lv->vg) || !vg_commit(pool_lv->vg))
		return_0;

	log_warn("WARNING: If everything works, remove %s volume.",
		 display_lvname(mlv));

	log_warn("WARNING: Use pvmove command to move %s on the best fitting PV.",
		 display_lvname(first_seg(pool_lv)->metadata_lv));

	return 1;
}

int lvconvert_repair_thinpool(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle)
{
	int poolmetadataspare = arg_int_value(cmd, poolmetadataspare_ARG, DEFAULT_POOL_METADATA_SPARE);
	struct dm_list *use_pvh;
	int ret;

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		use_pvh = &lv->vg->pvs;

	ret = _lvconvert_thin_pool_repair(cmd, lv, use_pvh, poolmetadataspare);

	return ret ? ECMD_PROCESSED : ECMD_FAILED;
}

static int _lvconvert_merge_thin_snapshot(struct cmd_context *cmd,
					  struct logical_volume *lv)
{
	int origin_is_active = 0, r = 0;
	struct lv_segment *snap_seg = first_seg(lv);
	struct logical_volume *origin = snap_seg->origin;

	if (!origin) {
		log_error("%s is not a mergeable logical volume.",
			  display_lvname(lv));
		return 0;
	}

	/* Check if merge is possible */
	if (lv_is_merging_origin(origin)) {
		log_error("Snapshot %s is already merging into the origin.",
			  display_lvname(find_snapshot(origin)->lv));
		return 0;
	}

	if (lv_is_external_origin(origin)) {
		if (!(origin = origin_from_cow(lv)))
			log_error(INTERNAL_ERROR "%s is missing origin.",
				  display_lvname(lv));
		else
			log_error("%s is read-only external origin %s.",
				  display_lvname(lv), display_lvname(origin));
		return 0;
	}

	if (lv_is_origin(origin)) {
		log_error("Merging into the old snapshot origin %s is not supported.",
			  display_lvname(origin));
		return 0;
	}

	if (!archive(lv->vg))
		return_0;

	/*
	 * Prevent merge with open device(s) as it would likely lead
	 * to application/filesystem failure.  Merge on origin's next
	 * activation if either the origin or snapshot LV can't be
	 * deactivated.
	 */
	if (!deactivate_lv(cmd, lv))
		log_print_unless_silent("Delaying merge since snapshot is open.");
	else if ((origin_is_active = lv_is_active(origin)) &&
		 !deactivate_lv(cmd, origin))
		log_print_unless_silent("Delaying merge since origin volume is open.");
	else {
		/*
		 * Both thin snapshot and origin are inactive,
		 * replace the origin LV with its snapshot LV.
		 */
		if (!thin_merge_finish(cmd, origin, lv))
			goto_out;

		if (origin_is_active && !activate_lv(cmd, lv)) {
			log_error("Failed to reactivate origin %s.",
				  display_lvname(lv));
			goto out;
		}

		r = 1;
		goto out;
	}

	init_snapshot_merge(snap_seg, origin);

	/* Commit vg, merge will start with next activation */
	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	r = 1;
out:
	backup(lv->vg);

	if (r)
		log_print_unless_silent("Merging of thin snapshot %s will occur on "
					"next activation of %s.",
					display_lvname(lv), display_lvname(origin));

	return r;
}

static int _lvconvert_split_and_keep_cachepool(struct cmd_context *cmd,
				   struct logical_volume *lv,
				   struct logical_volume *cachepool_lv)
{
	log_debug("Detaching cache pool %s from cache LV %s.",
		  display_lvname(cachepool_lv), display_lvname(lv));

	if (!archive(lv->vg))
		return_0;

	if (!lv_cache_remove(lv))
		return_0;

	if (!vg_write(lv->vg) || !vg_commit(lv->vg))
		return_0;

	backup(lv->vg);

	log_print_unless_silent("Logical volume %s is not cached and cache pool %s is unused.",
				display_lvname(lv), display_lvname(cachepool_lv));

	return 1;
}

static int _lvconvert_split_and_remove_cachepool(struct cmd_context *cmd,
				   struct logical_volume *lv,
				   struct logical_volume *cachepool_lv)
{
	struct lv_segment *seg;
	struct logical_volume *remove_lv;

	seg = first_seg(lv);

	if (lv_is_partial(seg_lv(seg, 0))) {
		log_warn("WARNING: Cache origin logical volume %s is missing.",
			 display_lvname(seg_lv(seg, 0)));
		remove_lv = lv; /* When origin is missing, drop everything */
	} else
		remove_lv = seg->pool_lv;

	if (lv_is_partial(seg_lv(first_seg(seg->pool_lv), 0)))
		log_warn("WARNING: Cache pool data logical volume %s is missing.",
			 display_lvname(seg_lv(first_seg(seg->pool_lv), 0)));

	if (lv_is_partial(first_seg(seg->pool_lv)->metadata_lv))
		log_warn("WARNING: Cache pool metadata logical volume %s is missing.",
			 display_lvname(first_seg(seg->pool_lv)->metadata_lv));

	/* TODO: Check for failed cache as well to get prompting? */
	if (lv_is_partial(lv)) {
		if (first_seg(seg->pool_lv)->cache_mode != CACHE_MODE_WRITETHROUGH) {
			if (!arg_count(cmd, force_ARG)) {
				log_error("Conversion aborted.");
				log_error("Cannot uncache writethrough cache volume %s without --force.",
					  display_lvname(lv));
				return 0;
			}
			log_warn("WARNING: Uncaching of partially missing writethrough cache volume %s might destroy your data.",
				 display_lvname(lv));
		}

		if (!arg_count(cmd, yes_ARG) &&
		    yes_no_prompt("Do you really want to uncache %s with missing LVs? [y/n]: ",
				  display_lvname(lv)) == 'n') {
			log_error("Conversion aborted.");
			return 0;
		}
	}

	if (lvremove_single(cmd, remove_lv, NULL) != ECMD_PROCESSED)
		return_0;

	if (remove_lv != lv)
		log_print_unless_silent("Logical volume %s is not cached.", display_lvname(lv));

	return 1;
}

static int _lvconvert_to_thin_with_external(struct cmd_context *cmd,
			   struct logical_volume *lv,
			   struct logical_volume *thinpool_lv)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *thin_lv;
	const char *origin_name;

	struct lvcreate_params lvc = {
		.activate = CHANGE_AEY,
		.alloc = ALLOC_INHERIT,
		.major = -1,
		.minor = -1,
		.suppress_zero_warn = 1, /* Suppress warning for this thin */
		.permission = LVM_READ,
		.pool_name = thinpool_lv->name,
		.pvh = &vg->pvs,
		.read_ahead = DM_READ_AHEAD_AUTO,
		.stripes = 1,
		.virtual_extents = lv->le_count,
	};

	if (lv == thinpool_lv) {
		log_error("Can't use same LV %s for thin pool and thin volume.",
			  display_lvname(thinpool_lv));
		return 0;
	}

	if ((origin_name = arg_str_value(cmd, originname_ARG, NULL)))
		if (!validate_restricted_lvname_param(cmd, &vg->name, &origin_name))
			return_0;

	/*
	 * If NULL, an auto-generated 'lvol' name is used.
	 * If set, the lv create code checks the name isn't used.
	 */
	lvc.lv_name = origin_name;

	if (is_lockd_type(vg->lock_type)) {
		/*
		 * FIXME: external origins don't work in lockd VGs.
		 * Prior to the lvconvert, there's a lock associated with
		 * the uuid of the external origin LV.  After the convert,
		 * that uuid belongs to the new thin LV, and a new LV with
		 * a new uuid exists as the non-thin, readonly external LV.
		 * We'd need to remove the lock for the previous uuid
		 * (the new thin LV will have no lock), and create a new
		 * lock for the new LV uuid used by the external LV.
		 */
		log_error("Can't use lock_type %s LV as external origin.",
			  vg->lock_type);
		return 0;
	}

	dm_list_init(&lvc.tags);

	if (!pool_supports_external_origin(first_seg(thinpool_lv), lv))
		return_0;

	if (!(lvc.segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_THIN)))
		return_0;

	if (!archive(vg))
		return_0;

	/*
	 * New thin LV needs to be created (all messages sent to pool) In this
	 * case thin volume is created READ-ONLY and also warn about not
	 * zeroing is suppressed.
	 *
	 * The new thin LV is created with the origin_name, or an autogenerated
	 * 'lvol' name.  Then the names and ids are swapped between the thin LV
	 * and the original/external LV.  So, the thin LV gets the name and id
	 * of the original LV arg, and the original LV arg gets the origin_name
	 * or the autogenerated name.
	 */

	if (!(thin_lv = lv_create_single(vg, &lvc)))
		return_0;

	if (!deactivate_lv(cmd, thin_lv)) {
		log_error("Aborting. Unable to deactivate new LV. "
			  "Manual intervention required.");
		return 0;
	}

	/*
	 * Crashing till this point will leave plain thin volume
	 * which could be easily removed by the user after i.e. power-off
	 */

	if (!swap_lv_identifiers(cmd, thin_lv, lv)) {
		stack;
		goto revert_new_lv;
	}

	/* Preserve read-write status of original LV here */
	thin_lv->status |= (lv->status & LVM_WRITE);

	if (!attach_thin_external_origin(first_seg(thin_lv), lv)) {
		stack;
		goto revert_new_lv;
	}

	if (!lv_update_and_reload(thin_lv)) {
		stack;
		goto deactivate_and_revert_new_lv;
	}

	log_print_unless_silent("Converted %s to thin volume with external origin %s.",
				display_lvname(thin_lv), display_lvname(lv));

	return 1;

deactivate_and_revert_new_lv:
	if (!swap_lv_identifiers(cmd, thin_lv, lv))
		stack;

	if (!deactivate_lv(cmd, thin_lv)) {
		log_error("Unable to deactivate failed new LV. "
			  "Manual intervention required.");
		return 0;
	}

	if (!detach_thin_external_origin(first_seg(thin_lv)))
		return_0;

revert_new_lv:
	/* FIXME Better to revert to backup of metadata? */
	if (!lv_remove(thin_lv) || !vg_write(vg) || !vg_commit(vg))
		log_error("Manual intervention may be required to remove "
			  "abandoned LV(s) before retrying.");
	else
		backup(vg);

	return 0;
}

static int _lvconvert_swap_pool_metadata(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct logical_volume *metadata_lv)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *prev_metadata_lv;
	struct lv_segment *seg;
	struct lv_types *lvtype;
	char meta_name[NAME_LEN];
	const char *swap_name;
	uint32_t chunk_size;
	int is_thinpool;
	int is_cachepool;
	int lvt_enum;

	is_thinpool = lv_is_thin_pool(lv);
	is_cachepool = lv_is_cache_pool(lv);
	lvt_enum = get_lvt_enum(metadata_lv);
	lvtype = get_lv_type(lvt_enum);

	if (lvt_enum != striped_LVT && lvt_enum != linear_LVT && lvt_enum != raid_LVT) {
		log_error("LV %s with type %s cannot be used as a metadata LV.",
			  display_lvname(metadata_lv), lvtype ? lvtype->name : "unknown");
		return 0;
	}

	if (!lv_is_visible(metadata_lv)) {
		log_error("Can't convert internal LV %s.",
			  display_lvname(metadata_lv));
		return 0;
	}

	if (lv_is_locked(metadata_lv)) {
		log_error("Can't convert locked LV %s.",
			  display_lvname(metadata_lv));
		return 0;
	}

	if (lv_is_origin(metadata_lv) ||
	    lv_is_merging_origin(metadata_lv) ||
	    lv_is_external_origin(metadata_lv) ||
	    lv_is_virtual(metadata_lv)) {
		log_error("Pool metadata LV %s is of an unsupported type.",
			  display_lvname(metadata_lv));
		return 0;
	}

	/* FIXME cache pool */
	if (is_thinpool && pool_is_active(lv)) {
		/* If any volume referencing pool active - abort here */
		log_error("Cannot convert pool %s with active volumes.",
			  display_lvname(lv));
		return 0;
	}

	if ((dm_snprintf(meta_name, sizeof(meta_name), "%s%s", lv->name, is_cachepool ? "_cmeta" : "_tmeta") < 0)) {
                log_error("Failed to create internal lv names, pool name is too long.");
                return 0;
        }

	seg = first_seg(lv);

	/* Normally do NOT change chunk size when swapping */

	if (arg_is_set(cmd, chunksize_ARG)) {
		chunk_size = arg_uint_value(cmd, chunksize_ARG, 0);

		if ((chunk_size != seg->chunk_size) && !dm_list_empty(&lv->segs_using_this_lv)) {
			if (arg_count(cmd, force_ARG) == PROMPT) {
				log_error("Chunk size can be only changed with --force. Conversion aborted.");
				return 0;
			}

			if (!validate_pool_chunk_size(cmd, seg->segtype, chunk_size))
				return_0;

			log_warn("WARNING: Changing chunk size %s to %s for %s pool volume.",
				 display_size(cmd, seg->chunk_size),
				 display_size(cmd, chunk_size),
				 display_lvname(lv));

			/* Ok, user has likely some serious reason for this */
			if (!arg_count(cmd, yes_ARG) &&
			    yes_no_prompt("Do you really want to change chunk size for %s pool volume? [y/n]: ",
					  display_lvname(lv)) == 'n') {
				log_error("Conversion aborted.");
				return 0;
			}
		}

		seg->chunk_size = chunk_size;
	}

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Do you want to swap metadata of %s pool with metadata volume %s? [y/n]: ",
			  display_lvname(lv),
			  display_lvname(metadata_lv)) == 'n') {
		log_error("Conversion aborted.");
		return 0;
	}

	if (!deactivate_lv(cmd, metadata_lv)) {
		log_error("Aborting. Failed to deactivate %s.",
			  display_lvname(metadata_lv));
		return 0;
	}

	if (!archive(vg))
		return_0;

	/* Swap names between old and new metadata LV */

	if (!detach_pool_metadata_lv(seg, &prev_metadata_lv))
		return_0;

	swap_name = metadata_lv->name;

	if (!lv_rename_update(cmd, metadata_lv, "pvmove_tmeta", 0))
		return_0;

	/* Give the previous metadata LV the name of the LV replacing it. */

	if (!lv_rename_update(cmd, prev_metadata_lv, swap_name, 0))
		return_0;

	/* Rename deactivated metadata LV to have _tmeta suffix */

	if (!lv_rename_update(cmd, metadata_lv, meta_name, 0))
		return_0;

	if (!attach_pool_metadata_lv(seg, metadata_lv))
		return_0;

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	backup(vg);
	return 1;
}

/*
 * Create a new pool LV, using the lv arg as the data sub LV.
 * The metadata sub LV is either a new LV created here, or an
 * existing LV specified by --poolmetadata.
 */

static int _lvconvert_to_pool(struct cmd_context *cmd,
			      struct logical_volume *lv,
			      int to_thinpool,
			      int to_cachepool,
			      struct dm_list *use_pvh)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *metadata_lv = NULL;  /* existing or created */
	struct logical_volume *data_lv;             /* lv arg renamed */
	struct logical_volume *pool_lv;             /* new lv created here */
	const char *pool_metadata_name;             /* user-specified lv name */
	const char *pool_name;                      /* name of original lv arg */
	char meta_name[NAME_LEN];                   /* generated sub lv name */
	char data_name[NAME_LEN];                   /* generated sub lv name */
	struct segment_type *pool_segtype;          /* thinpool or cachepool */
	struct lv_segment *seg;
	unsigned int target_attr = ~0;
	unsigned int passed_args = 0;
	unsigned int activate_pool;
	unsigned int zero_metadata;
	uint64_t meta_size;
	uint32_t meta_extents;
	uint32_t chunk_size;
	int chunk_calc;
	int r = 0;

	/* for handling lvmlockd cases */
	char *lockd_data_args = NULL;
	char *lockd_meta_args = NULL;
	char *lockd_data_name = NULL;
	char *lockd_meta_name = NULL;
	struct id lockd_data_id;
	struct id lockd_meta_id;


	if (lv_is_thin_pool(lv) || lv_is_cache_pool(lv)) {
		log_error(INTERNAL_ERROR "LV %s is already a pool.", display_lvname(lv));
		return 0;
	}

	pool_segtype = to_cachepool ? get_segtype_from_string(cmd, SEG_TYPE_NAME_CACHE_POOL) :
				      get_segtype_from_string(cmd, SEG_TYPE_NAME_THIN_POOL);

	if (!pool_segtype->ops->target_present(cmd, NULL, &target_attr)) {
		log_error("%s: Required device-mapper target(s) not detected in your kernel.", pool_segtype->name);
		return 0;
	}

	/* Allow to have only thinpool active and restore it's active state. */
	activate_pool = to_thinpool && lv_is_active(lv);

	/* Wipe metadata_lv by default, but allow skipping this for cache pools. */
	zero_metadata = to_cachepool ? arg_int_value(cmd, zero_ARG, 1) : 1;

	/* An existing LV needs to have its lock freed once it becomes a data LV. */
	if (is_lockd_type(vg->lock_type) && lv->lock_args) {
		lockd_data_args = dm_pool_strdup(cmd->mem, lv->lock_args);
		lockd_data_name = dm_pool_strdup(cmd->mem, lv->name);
		memcpy(&lockd_data_id, &lv->lvid.id[1], sizeof(struct id));
	}

	/*
	 * If an existing LV is to be used as the metadata LV,
	 * verify that it's in a usable state.  These checks are
	 * not done by command def rules because this LV is not
	 * processed by process_each_lv.
	 */

	if ((pool_metadata_name = arg_str_value(cmd, poolmetadata_ARG, NULL))) {
		if (!(metadata_lv = find_lv(vg, pool_metadata_name))) {
			log_error("Unknown pool metadata LV %s.", pool_metadata_name);
			return 0;
		}

		/* An existing LV needs to have its lock freed once it becomes a meta LV. */
		if (is_lockd_type(vg->lock_type) && metadata_lv->lock_args) {
			lockd_meta_args = dm_pool_strdup(cmd->mem, metadata_lv->lock_args);
			lockd_meta_name = dm_pool_strdup(cmd->mem, metadata_lv->name);
			memcpy(&lockd_meta_id, &metadata_lv->lvid.id[1], sizeof(struct id));
		}

		if (metadata_lv == lv) {
			log_error("Can't use same LV for pool data and metadata LV %s.",
				  display_lvname(metadata_lv));
			return 0;
		}

		if (!lv_is_visible(metadata_lv)) {
			log_error("Can't convert internal LV %s.",
				  display_lvname(metadata_lv));
			return 0;
		}

		if (lv_is_locked(metadata_lv)) {
			log_error("Can't convert locked LV %s.",
				  display_lvname(metadata_lv));
			return 0;
		}

		if (lv_is_mirror(metadata_lv)) {
			log_error("Mirror logical volumes cannot be used for pool metadata.");
			log_print_unless_silent("Try \"%s\" segment type instead.", SEG_TYPE_NAME_RAID1);
			return 0;
		}

		/* FIXME Tidy up all these type restrictions. */
		if (lv_is_cache_type(metadata_lv) ||
		    lv_is_thin_type(metadata_lv) ||
		    lv_is_cow(metadata_lv) || lv_is_merging_cow(metadata_lv) ||
		    lv_is_origin(metadata_lv) || lv_is_merging_origin(metadata_lv) ||
		    lv_is_external_origin(metadata_lv) ||
		    lv_is_virtual(metadata_lv)) {
			log_error("Pool metadata LV %s is of an unsupported type.",
				  display_lvname(metadata_lv));
			return 0;
		}
	}

	/*
	 * Determine the size of the metadata LV and the chunk size.  When an
	 * existing LV is to be used for metadata, this introduces some
	 * constraints/defaults.  When chunk_size=0 and/or meta_extents=0 are
	 * passed to the "update params" function, defaults are calculated and
	 * returned.
	 */

	if (arg_is_set(cmd, chunksize_ARG)) {
		passed_args |= PASS_ARG_CHUNK_SIZE;
		chunk_size = arg_uint_value(cmd, chunksize_ARG, 0);
		if (!validate_pool_chunk_size(cmd, pool_segtype, chunk_size))
			return_0;
	} else {
		/* A default will be chosen by the "update" function. */
		chunk_size = 0;
	}

	if (arg_is_set(cmd, poolmetadatasize_ARG)) {
		meta_size = arg_uint64_value(cmd, poolmetadatasize_ARG, UINT64_C(0));
		meta_extents = extents_from_size(cmd, meta_size, vg->extent_size);
		passed_args |= PASS_ARG_POOL_METADATA_SIZE;
	} else if (metadata_lv) {
		meta_extents = metadata_lv->le_count;
		passed_args |= PASS_ARG_POOL_METADATA_SIZE;
	} else {
		/* A default will be chosen by the "update" function. */
		meta_extents = 0;
	}

	/* Tell the "update" function to ignore these, they are handled below. */
	passed_args |= PASS_ARG_DISCARDS | PASS_ARG_ZERO;

	/*
	 * Validate and/or choose defaults for meta_extents and chunk_size,
	 * this involves some complicated calculations.
	 */

	if (to_cachepool) {
		if (!update_cache_pool_params(pool_segtype, vg, target_attr,
					      passed_args, lv->le_count,
					      &meta_extents,
					      &chunk_calc,
					      &chunk_size))
			return_0;
	} else {
		if (!update_thin_pool_params(pool_segtype, vg, target_attr,
					     passed_args, lv->le_count,
					     &meta_extents,
					     &chunk_calc,
					     &chunk_size,
					     NULL, NULL))
			return_0;
	}

	if ((uint64_t)chunk_size > ((uint64_t)lv->le_count * vg->extent_size)) {
		log_error("Pool data LV %s is too small (%s) for specified chunk size (%s).",
			  display_lvname(lv),
			  display_size(cmd, (uint64_t)lv->le_count * vg->extent_size),
			  display_size(cmd, chunk_size));
		return 0;
	}

	if (metadata_lv && (meta_extents > metadata_lv->le_count)) {
		log_error("Pool metadata LV %s is too small (%u extents) for required metadata (%u extents).",
			  display_lvname(metadata_lv), metadata_lv->le_count, meta_extents);
		return 0;
	}

	log_verbose("Pool metadata extents %u chunk_size %u", meta_extents, chunk_size);


	/*
	 * Verify that user wants to use these LVs.
	 */

	log_warn("WARNING: Converting logical volume %s%s%s to %s pool's data%s %s metadata wiping.",
		 display_lvname(lv),
		 metadata_lv ? " and " : "",
		 metadata_lv ? display_lvname(metadata_lv) : "",
		 to_cachepool ? "cache" : "thin",
		 metadata_lv ? " and metadata volumes" : " volume",
		 zero_metadata ? "with" : "WITHOUT");

	if (zero_metadata)
		log_warn("THIS WILL DESTROY CONTENT OF LOGICAL VOLUME (filesystem etc.)");
	else if (to_cachepool)
		log_warn("WARNING: Using mismatched cache pool metadata MAY DESTROY YOUR DATA!");

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Do you really want to convert %s%s%s? [y/n]: ",
			  display_lvname(lv),
			  metadata_lv ? " and " : "",
			  metadata_lv ? display_lvname(metadata_lv) : "") == 'n') {
		log_error("Conversion aborted.");
		return 0;
	}

	/*
	 * The internal LV names for pool data/meta LVs.
	 */

	if ((dm_snprintf(meta_name, sizeof(meta_name), "%s%s", lv->name, to_cachepool ? "_cmeta" : "_tmeta") < 0) ||
	    (dm_snprintf(data_name, sizeof(data_name), "%s%s", lv->name, to_cachepool ? "_cdata" : "_tdata") < 0)) {
		log_error("Failed to create internal lv names, pool name is too long.");
		return 0;
	}

	/*
	 * If a new metadata LV needs to be created, collect the settings for
	 * the new LV and create it.
	 *
	 * If an existing LV is used for metadata, deactivate/activate/wipe it.
	 */

	if (!metadata_lv) {
		uint32_t meta_stripes;
		uint32_t meta_stripe_size;
		uint32_t meta_readahead;
		alloc_policy_t meta_alloc;
		unsigned meta_stripes_supplied;
		unsigned meta_stripe_size_supplied;

		if (!get_stripe_params(cmd, get_segtype_from_string(cmd, SEG_TYPE_NAME_STRIPED),
				       &meta_stripes,
				       &meta_stripe_size,
				       &meta_stripes_supplied,
				       &meta_stripe_size_supplied))
			return_0;

		meta_readahead = arg_uint_value(cmd, readahead_ARG, cmd->default_settings.read_ahead);
		meta_alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);

		if (!archive(vg))
			return_0;

		if (!(metadata_lv = alloc_pool_metadata(lv,
							meta_name,
							meta_readahead,
							meta_stripes,
							meta_stripe_size,
							meta_extents,
							meta_alloc,
							use_pvh)))
			return_0;
	} else {
		if (!deactivate_lv(cmd, metadata_lv)) {
			log_error("Aborting. Failed to deactivate %s.",
				  display_lvname(metadata_lv));
			return 0;
		}

		if (!archive(vg))
			return_0;

		if (zero_metadata) {
			metadata_lv->status |= LV_TEMPORARY;
			if (!activate_lv_local(cmd, metadata_lv)) {
				log_error("Aborting. Failed to activate metadata lv.");
				return 0;
			}

			if (!wipe_lv(metadata_lv, (struct wipe_params) { .do_zero = 1 })) {
				log_error("Aborting. Failed to wipe metadata lv.");
				return 0;
			}
		}
	}

	/*
	 * Deactivate the data LV and metadata LV.
	 * We are changing target type, so deactivate first.
	 */

	if (!deactivate_lv(cmd, metadata_lv)) {
		log_error("Aborting. Failed to deactivate metadata lv. "
			  "Manual intervention required.");
		return 0;
	}

	if (!deactivate_lv(cmd, lv)) {
		log_error("Aborting. Failed to deactivate logical volume %s.",
			  display_lvname(lv));
		return 0;
	}

	/*
	 * When the LV referenced by the original function arg "lv"
	 * is renamed, it is then referenced as "data_lv".
	 *
	 * pool_name    pool name taken from lv arg
	 * data_name    sub lv name, generated
	 * meta_name    sub lv name, generated
	 *
	 * pool_lv      new lv for pool object, created here
	 * data_lv      sub lv, was lv arg, now renamed
	 * metadata_lv  sub lv, existing or created here
	 */

	data_lv = lv;
	pool_name = lv->name; /* Use original LV name for pool name */

	/*
	 * Rename the original LV arg to the internal data LV naming scheme.
	 *
	 * Since we wish to have underlaying devs to match _[ct]data
	 * rename data LV to match pool LV subtree first,
	 * also checks for visible LV.
	 *
	 * FIXME: any more types prohibited here?
	 */

	if (!lv_rename_update(cmd, data_lv, data_name, 0))
		return_0;

	/*
	 * Create LV structures for the new pool LV object,
	 * and connect it to the data/meta LVs.
	 */

	if (!(pool_lv = lv_create_empty(pool_name, NULL,
					(to_cachepool ? CACHE_POOL : THIN_POOL) | VISIBLE_LV | LVM_READ | LVM_WRITE,
					ALLOC_INHERIT, vg))) {
		log_error("Creation of pool LV failed.");
		return 0;
	}

	/* Allocate a new pool segment */
	if (!(seg = alloc_lv_segment(pool_segtype, pool_lv, 0, data_lv->le_count,
				     pool_lv->status, 0, NULL, 1,
				     data_lv->le_count, 0, 0, 0, NULL)))
		return_0;

	/* Add the new segment to the layer LV */
	dm_list_add(&pool_lv->segments, &seg->list);
	pool_lv->le_count = data_lv->le_count;
	pool_lv->size = data_lv->size;

	if (!attach_pool_data_lv(seg, data_lv))
		return_0;

	/*
	 * Create a new lock for a thin pool LV.  A cache pool LV has no lock.
	 * Locks are removed from existing LVs that are being converted to
	 * data and meta LVs (they are unlocked and deleted below.)
	 */
	if (is_lockd_type(vg->lock_type)) {
		if (to_cachepool) {
			data_lv->lock_args = NULL;
			metadata_lv->lock_args = NULL;
		} else {
			data_lv->lock_args = NULL;
			metadata_lv->lock_args = NULL;

			if (!strcmp(vg->lock_type, "sanlock"))
				pool_lv->lock_args = "pending";
			else if (!strcmp(vg->lock_type, "dlm"))
				pool_lv->lock_args = "dlm";
			/* The lock_args will be set in vg_write(). */
		}
	}

	/*
	 * Apply settings to the new pool seg, from command line, from
	 * defaults, sometimes adjusted.
	 */

	seg->transaction_id = 0;
	seg->chunk_size = chunk_size;

	if (to_cachepool) {
		cache_mode_t cache_mode = 0;
		const char *policy_name = NULL;
		struct dm_config_tree *policy_settings = NULL;

		if (!get_cache_params(cmd, &cache_mode, &policy_name, &policy_settings))
			return_0;

		if (cache_mode &&
		    !cache_set_cache_mode(seg, cache_mode))
			return_0;

		if ((policy_name || policy_settings) &&
		    !cache_set_policy(seg, policy_name, policy_settings))
			return_0;

		if (policy_settings)
			dm_config_destroy(policy_settings);
	} else {
		const char *discards_name;

		if (arg_is_set(cmd, zero_ARG))
			seg->zero_new_blocks = arg_int_value(cmd, zero_ARG, 0);
		else
			seg->zero_new_blocks = find_config_tree_bool(cmd, allocation_thin_pool_zero_CFG, vg->profile);

		if (arg_is_set(cmd, discards_ARG))
			seg->discards = (thin_discards_t) arg_uint_value(cmd, discards_ARG, THIN_DISCARDS_PASSDOWN);
		else {
			if (!(discards_name = find_config_tree_str(cmd, allocation_thin_pool_discards_CFG, vg->profile)))
				return_0;
			if (!set_pool_discards(&seg->discards, discards_name))
				return_0;
		}
	}

	/*
	 * Rename deactivated metadata LV to have _tmeta suffix.
	 * Implicit checks if metadata_lv is visible.
	 */
	if (pool_metadata_name &&
	    !lv_rename_update(cmd, metadata_lv, meta_name, 0))
		return_0;

	if (!attach_pool_metadata_lv(seg, metadata_lv))
		return_0;

	if (!handle_pool_metadata_spare(vg,
					metadata_lv->le_count,
					use_pvh,
					arg_int_value(cmd, poolmetadataspare_ARG, DEFAULT_POOL_METADATA_SPARE)))
		return_0;

	if (!vg_write(vg) || !vg_commit(vg))
		return_0;

	if (seg->zero_new_blocks &&
	    seg->chunk_size >= DEFAULT_THIN_POOL_CHUNK_SIZE_PERFORMANCE * 2)
		log_warn("WARNING: Pool zeroing and large %s chunk size slows down provisioning.",
			 display_size(cmd, seg->chunk_size));

	if (activate_pool && !lockd_lv(cmd, pool_lv, "ex", LDLV_PERSISTENT)) {
		log_error("Failed to lock pool LV %s.", display_lvname(pool_lv));
		goto out;
	}

	if (activate_pool &&
	    !activate_lv_excl(cmd, pool_lv)) {
		log_error("Failed to activate pool logical volume %s.",
			  display_lvname(pool_lv));
		/* Deactivate subvolumes */
		if (!deactivate_lv(cmd, seg_lv(seg, 0)))
			log_error("Failed to deactivate pool data logical volume %s.",
				  display_lvname(seg_lv(seg, 0)));
		if (!deactivate_lv(cmd, seg->metadata_lv))
			log_error("Failed to deactivate pool metadata logical volume %s.",
				  display_lvname(seg->metadata_lv));
		goto out;
	}

	r = 1;

out:
	backup(vg);

	if (r)
		log_print_unless_silent("Converted %s to %s pool.",
					display_lvname(lv),
					to_cachepool ? "cache" : "thin");

	/*
	 * Unlock and free the locks from existing LVs that became pool data
	 * and meta LVs.
	 */
	if (lockd_data_name) {
		if (!lockd_lv_name(cmd, vg, lockd_data_name, &lockd_data_id, lockd_data_args, "un", LDLV_PERSISTENT))
			log_error("Failed to unlock pool data LV %s/%s", vg->name, lockd_data_name);
		lockd_free_lv(cmd, vg, lockd_data_name, &lockd_data_id, lockd_data_args);
	}

	if (lockd_meta_name) {
		if (!lockd_lv_name(cmd, vg, lockd_meta_name, &lockd_meta_id, lockd_meta_args, "un", LDLV_PERSISTENT))
			log_error("Failed to unlock pool metadata LV %s/%s", vg->name, lockd_meta_name);
		lockd_free_lv(cmd, vg, lockd_meta_name, &lockd_meta_id, lockd_meta_args);
	}

	return r;
#if 0
revert_new_lv:
	/* TBD */
	if (!pool_metadata_lv_name) {
		if (!deactivate_lv(cmd, metadata_lv)) {
			log_error("Failed to deactivate metadata lv.");
			return 0;
		}
		if (!lv_remove(metadata_lv) || !vg_write(vg) || !vg_commit(vg))
			log_error("Manual intervention may be required to remove "
				  "abandoned LV(s) before retrying.");
		else
			backup(vg);
	}

	return 0;
#endif
}

static int _lvconvert_to_cache_vol(struct cmd_context *cmd,
			    struct logical_volume *lv,
			    struct logical_volume *cachepool_lv)
{
	struct logical_volume *cache_lv;
	cache_mode_t cache_mode = 0;
	const char *policy_name = NULL;
	struct dm_config_tree *policy_settings = NULL;

	if (!validate_lv_cache_create_pool(cachepool_lv))
		return_0;

	if (!get_cache_params(cmd, &cache_mode, &policy_name, &policy_settings))
		return_0;

	if (!archive(lv->vg))
		return_0;

	if (!(cache_lv = lv_cache_create(cachepool_lv, lv)))
		return_0;

	if (!cache_set_cache_mode(first_seg(cache_lv), cache_mode))
		return_0;

	if (!cache_set_policy(first_seg(cache_lv), policy_name, policy_settings))
		return_0;

	if (policy_settings)
		dm_config_destroy(policy_settings);

	cache_check_for_warns(first_seg(cache_lv));

	if (!lv_update_and_reload(cache_lv))
		return_0;

	log_print_unless_silent("Logical volume %s is now cached.",
				display_lvname(cache_lv));

	return 1;
}

static int _lvconvert_to_pool_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	struct dm_list *use_pvh = NULL;
	int to_thinpool = 0;
	int to_cachepool = 0;

	switch (cmd->command->command_line_enum) {
	case lvconvert_to_thinpool_CMD:
		to_thinpool = 1;
		break;
	case lvconvert_to_cachepool_CMD:
		to_cachepool = 1;
		break;
	default:
		log_error(INTERNAL_ERROR "Invalid lvconvert pool command");
		return 0;
	};

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
	} else
		use_pvh = &lv->vg->pvs;

	if (!_lvconvert_to_pool(cmd, lv, to_thinpool, to_cachepool, use_pvh))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

/*
 * The LV position arg is used as thinpool/cachepool data LV.
 */

int lvconvert_to_pool_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, NULL, &_lvconvert_to_pool_single);
}

/*
 * Reformats non-standard command form into standard command form.
 *
 * In the command variants with no position LV arg, the LV arg is taken from
 * the --thinpool/--cachepool arg, and the position args are modified to match
 * the standard command form.
 */

int lvconvert_to_pool_noarg_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct command *new_command;
	char *pool_data_name;
	int i, p;

	switch (cmd->command->command_line_enum) {
	case lvconvert_to_thinpool_noarg_CMD:
		pool_data_name = (char *)arg_str_value(cmd, thinpool_ARG, NULL);
		new_command = get_command(lvconvert_to_thinpool_CMD);
		break;
	case lvconvert_to_cachepool_noarg_CMD:
		pool_data_name = (char *)arg_str_value(cmd, cachepool_ARG, NULL);
		new_command = get_command(lvconvert_to_cachepool_CMD);
		break;
	default:
		log_error(INTERNAL_ERROR "Unknown pool conversion.");
		return 0;
	};

	log_debug("Changing command line id %s %d to standard form %s %d",
		  cmd->command->command_line_id, cmd->command->command_line_enum,
		  new_command->command_line_id, new_command->command_line_enum);

	/* Make the LV the first position arg. */

	p = cmd->position_argc;
	for (i = 0; i < cmd->position_argc; i++)
		cmd->position_argv[p] = cmd->position_argv[p-1];

	cmd->position_argv[0] = pool_data_name;
	cmd->position_argc++;
	cmd->command = new_command;

	return lvconvert_to_pool_cmd(cmd, argc, argv);
}

static int _lvconvert_to_cache_vol_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *cachepool_lv;
	const char *cachepool_name;
	uint32_t chunk_size = 0;

	if (!(cachepool_name = arg_str_value(cmd, cachepool_ARG, NULL)))
		goto_out;

	if (!validate_lvname_param(cmd, &vg->name, &cachepool_name))
		goto_out;

	if (!(cachepool_lv = find_lv(vg, cachepool_name))) {
		log_error("Cache pool %s not found.", cachepool_name);
		goto out;
	}

	/*
	 * If cachepool_lv is not yet a cache pool, convert it to one.
	 * If using an existing cache pool, wipe it.
	 */

	if (!lv_is_cache_pool(cachepool_lv)) {
		int lvt_enum = get_lvt_enum(cachepool_lv);
		struct lv_types *lvtype = get_lv_type(lvt_enum);

		if (lvt_enum != striped_LVT && lvt_enum != linear_LVT && lvt_enum != raid_LVT) {
			log_error("LV %s with type %s cannot be converted to a cache pool.",
				  display_lvname(cachepool_lv), lvtype ? lvtype->name : "unknown");
			goto out;
		}

		if (!_lvconvert_to_pool(cmd, cachepool_lv, 0, 1, &vg->pvs)) {
			log_error("LV %s could not be converted to a cache pool.",
				  display_lvname(cachepool_lv));
			goto out;
		}

		if (!(cachepool_lv = find_lv(vg, cachepool_name))) {
			log_error("LV %s cannot be found.", display_lvname(cachepool_lv));
			goto out;
		}

		if (!lv_is_cache_pool(cachepool_lv)) {
			log_error("LV %s is not a cache pool.", display_lvname(cachepool_lv));
			goto out;
		}
	} else {
		if (!dm_list_empty(&cachepool_lv->segs_using_this_lv)) {
			log_error("Cache pool %s is already in use.", cachepool_name);
			goto out;
		}

		if (arg_is_set(cmd, chunksize_ARG))
			chunk_size = arg_uint_value(cmd, chunksize_ARG, 0);
		if (!chunk_size)
			chunk_size = first_seg(cachepool_lv)->chunk_size;

		/* FIXME: why is chunk_size read and checked if it's not used? */

		if (!validate_lv_cache_chunk_size(cachepool_lv, chunk_size))
			goto_out;

		/* Note: requires rather deep know-how to skip zeroing */
		if (!arg_is_set(cmd, zero_ARG)) {
		       	if (!arg_is_set(cmd, yes_ARG) &&
			    yes_no_prompt("Do you want wipe existing metadata of cache pool %s? [y/n]: ",
					  display_lvname(cachepool_lv)) == 'n') {
				log_error("Conversion aborted.");
				log_error("To preserve cache metadata add option \"--zero n\".");
				log_warn("WARNING: Reusing mismatched cache pool metadata MAY DESTROY YOUR DATA!");
				goto out;
			}
			/* Wiping confirmed, go ahead */
			if (!wipe_cache_pool(cachepool_lv))
				goto_out;
		} else if (arg_int_value(cmd, zero_ARG, 0)) {
			if (!wipe_cache_pool(cachepool_lv))
				goto_out;
		} else {
			log_warn("WARNING: Reusing cache pool metadata %s for volume caching.",
				 display_lvname(cachepool_lv));
		}

	}

	/* When the lv arg is a thinpool, redirect command to data sub lv. */

	if (lv_is_thin_pool(lv)) {
		lv = seg_lv(first_seg(lv), 0);
		log_verbose("Redirecting operation to data sub LV %s.", display_lvname(lv));
	}

	/* Convert lv to cache vol using cachepool_lv. */

	if (!_lvconvert_to_cache_vol(cmd, lv, cachepool_lv))
		goto_out;

	return ECMD_PROCESSED;

 out:
	return ECMD_FAILED;
}

int lvconvert_to_cache_vol_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, NULL, &_lvconvert_to_cache_vol_single);
}

static int _lvconvert_to_thin_with_external_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *thinpool_lv;
	const char *thinpool_name;

	if (!(thinpool_name = arg_str_value(cmd, thinpool_ARG, NULL)))
		goto_out;

	if (!validate_lvname_param(cmd, &vg->name, &thinpool_name))
		goto_out;

	if (!(thinpool_lv = find_lv(vg, thinpool_name))) {
		log_error("Thin pool %s not found.", thinpool_name);
		goto out;
	}

	/* If thinpool_lv is not yet a thin pool, convert it to one. */

	if (!lv_is_thin_pool(thinpool_lv)) {
		int lvt_enum = get_lvt_enum(thinpool_lv);
		struct lv_types *lvtype = get_lv_type(lvt_enum);

		if (lvt_enum != striped_LVT && lvt_enum != linear_LVT && lvt_enum != raid_LVT) {
			log_error("LV %s with type %s cannot be converted to a thin pool.",
				  display_lvname(thinpool_lv), lvtype ? lvtype->name : "unknown");
			goto out;
		}

		if (!_lvconvert_to_pool(cmd, thinpool_lv, 1, 0, &vg->pvs)) {
			log_error("LV %s could not be converted to a thin pool.",
				  display_lvname(thinpool_lv));
			goto out;
		}

		if (!(thinpool_lv = find_lv(vg, thinpool_name))) {
			log_error("LV %s cannot be found.", display_lvname(thinpool_lv));
			goto out;
		}

		if (!lv_is_thin_pool(thinpool_lv)) {
			log_error("LV %s is not a thin pool.", display_lvname(thinpool_lv));
			goto out;
		}
	}

	/* Convert lv to thin with external origin using thinpool_lv. */

	if (!_lvconvert_to_thin_with_external(cmd, lv, thinpool_lv))
		goto_out;

	return ECMD_PROCESSED;

 out:
	return ECMD_FAILED;
}

int lvconvert_to_thin_with_external_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, NULL, &_lvconvert_to_thin_with_external_single);
}

static int _lvconvert_swap_pool_metadata_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	struct volume_group *vg = lv->vg;
	struct logical_volume *metadata_lv;
	const char *metadata_name;

	if (!(metadata_name = arg_str_value(cmd, poolmetadata_ARG, NULL)))
		goto_out;

	if (!validate_lvname_param(cmd, &vg->name, &metadata_name))
		goto_out;

	if (!(metadata_lv = find_lv(vg, metadata_name))) {
		log_error("Metadata LV %s not found.", metadata_name);
		goto out;
	}

	if (metadata_lv == lv) {
		log_error("Can't use same LV for pool data and metadata LV %s.",
			  display_lvname(metadata_lv));
		goto out;
	}

	if (!_lvconvert_swap_pool_metadata(cmd, lv, metadata_lv))
		goto_out;

	return ECMD_PROCESSED;

 out:
	return ECMD_FAILED;
}

int lvconvert_swap_pool_metadata_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, NULL, &_lvconvert_swap_pool_metadata_single);
}

#if 0
int lvconvert_swap_pool_metadata_noarg_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct command *new_command;
	char *pool_name;

	switch (cmd->command->command_line_enum) {
	case lvconvert_swap_thinpool_metadata_CMD:
		pool_name = (char *)arg_str_value(cmd, thinpool_ARG, NULL);
		break;
	case lvconvert_swap_cachepool_metadata_CMD:
		pool_name = (char *)arg_str_value(cmd, cachepool_ARG, NULL);
		break;
	default:
		log_error(INTERNAL_ERROR "Unknown pool conversion.");
		return 0;
	};

	new_command = get_command(lvconvert_swap_pool_metadata_CMD);

	log_debug("Changing command line id %s %d to standard form %s %d",
		  cmd->command->command_line_id, cmd->command->command_line_enum,
		  new_command->command_line_id, new_command->command_line_enum);

	/* Make the LV the first position arg. */

	cmd->position_argv[0] = pool_name;
	cmd->position_argc++;
	cmd->command = new_command;

	return lvconvert_swap_pool_metadata_cmd(cmd, argc, argv);
}
#endif

int lvconvert_merge_thin_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	if (!_lvconvert_merge_thin_snapshot(cmd, lv))
		return ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvconvert_merge_thin_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	return process_each_lv(cmd, cmd->position_argc, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, NULL, &lvconvert_merge_thin_single);
}

static int _lvconvert_split_cachepool_single(struct cmd_context *cmd,
					 struct logical_volume *lv,
					 struct processing_handle *handle)
{
	struct logical_volume *cache_lv = NULL;
	struct logical_volume *cachepool_lv = NULL;
	struct lv_segment *seg;
	int ret;

	if (lv_is_cache(lv)) {
		cache_lv = lv;
		cachepool_lv = first_seg(cache_lv)->pool_lv;

	} else if (lv_is_cache_pool(lv)) {
		cachepool_lv = lv;

		if ((dm_list_size(&cachepool_lv->segs_using_this_lv) == 1) &&
		    (seg = get_only_segment_using_this_lv(cachepool_lv)) &&
		    seg_is_cache(seg))
			cache_lv = seg->lv;

	} else if (lv_is_thin_pool(lv)) {
		cache_lv = seg_lv(first_seg(lv), 0); /* cached _tdata */
		cachepool_lv = first_seg(cache_lv)->pool_lv;
	}

	if (!cache_lv) {
		log_error("Cannot find cache LV from %s.", display_lvname(lv));
		return ECMD_FAILED;
	}

	if (!cachepool_lv) {
		log_error("Cannot find cache pool LV from %s.", display_lvname(lv));
		return ECMD_FAILED;
	}

	switch (cmd->command->command_line_enum) {
	case lvconvert_split_and_keep_cachepool_CMD:
		ret = _lvconvert_split_and_keep_cachepool(cmd, cache_lv, cachepool_lv);
		break;

	case lvconvert_split_and_remove_cachepool_CMD:
		ret = _lvconvert_split_and_remove_cachepool(cmd, cache_lv, cachepool_lv);
		break;
	default:
		log_error(INTERNAL_ERROR "Unknown cache pool split.");
		ret = 0;
	}

	if (!ret)
		return ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvconvert_split_cachepool_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	if (cmd->command->command_line_enum == lvconvert_split_and_remove_cachepool_CMD) {
		cmd->handles_missing_pvs = 1;
		cmd->partial_activation = 1;
	}

	return process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, NULL, &_lvconvert_split_cachepool_single);
}

