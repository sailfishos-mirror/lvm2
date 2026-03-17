/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
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

#include "lib/lvmpolld/polldaemon.h"
#include "lib/display/display.h"
#include "pvmove_poll.h"
#include "lib/lvmpolld/lvmpolld-client.h"

struct pvmove_params {
	char *pv_name_arg; /* original unmodified arg */
	char *lv_name_arg; /* original unmodified arg */
	alloc_policy_t alloc;
	int pv_count;
	char **pv_names;

	union lvid *lvid;
	char *id_vg_name;
	char *id_lv_name;
	unsigned in_progress;
	int setup_result;
	int found_pv;
};

static int _pvmove_target_present(struct cmd_context *cmd, int clustered)
{
	const struct segment_type *segtype;
	int found = 1;

	if (!(segtype = get_segtype_from_string(cmd, SEG_TYPE_NAME_MIRROR)))
		return_0;

	if (activation() && segtype->ops->target_present &&
	    !segtype->ops->target_present(cmd, NULL, NULL))
		found = 0;

	return found;
}


/* Allow /dev/vgname/lvname, vgname/lvname or lvname */
static const char *_extract_lvname(struct cmd_context *cmd, const char *vgname,
				   const char *arg)
{
	const char *lvname;

	/* Is an lvname supplied directly? */
	if (!strchr(arg, '/'))
		return arg;

	lvname = skip_dev_dir(cmd, arg, NULL);
	while (*lvname == '/')
		lvname++;
	if (!strchr(lvname, '/')) {
		log_error("--name takes a logical volume name.");
		return NULL;
	}
	if (strncmp(vgname, lvname, strlen(vgname)) ||
	    (lvname += strlen(vgname), *lvname != '/')) {
		log_error("Named LV and old PV must be in the same VG.");
		return NULL;
	}
	while (*lvname == '/')
		lvname++;
	if (!*lvname) {
		log_error("Incomplete LV name supplied with --name.");
		return NULL;
	}
	return lvname;
}

/* Create list of PVs for allocation of replacement extents */
static struct dm_list *_get_allocatable_pvs(struct cmd_context *cmd, int argc,
					 char **argv, struct volume_group *vg,
					 struct physical_volume *pv,
					 alloc_policy_t alloc)
{
	struct dm_list *allocatable_pvs, *pvht, *pvh;
	struct pv_list *pvl;

	if (argc)
		allocatable_pvs = create_pv_list(cmd->mem, vg, argc, argv, 1);
	else
		allocatable_pvs = clone_pv_list(cmd->mem, &vg->pvs);

	if (!allocatable_pvs)
		return_NULL;

	dm_list_iterate_safe(pvh, pvht, allocatable_pvs) {
		pvl = dm_list_item(pvh, struct pv_list);

		/* Don't allocate onto the PV we're clearing! */
		if ((alloc != ALLOC_ANYWHERE) && (pvl->pv->dev == pv_dev(pv))) {
			dm_list_del(&pvl->list);
			continue;
		}

		/* Remove PV if full */
		if (pvl->pv->pe_count == pvl->pv->pe_alloc_count)
			dm_list_del(&pvl->list);
	}

	if (dm_list_empty(allocatable_pvs)) {
		log_error("No extents available for allocation.");
		return NULL;
	}

	return allocatable_pvs;
}

/*
 * If @lv_name's a RAID SubLV, check for any PVs
 * on @trim_list holding it's sibling (rimage/rmeta)
 * and remove it from the @trim_list in order to allow
 * for pvmove collocation of DataLV/MetaLV pairs.
 */
static int _remove_sibling_pvs_from_trim_list(struct logical_volume *lv,
					      const char *lv_name,
					      struct dm_list *trim_list)
{
	struct logical_volume *sibling_lv = NULL;
	struct lv_segment *raid_seg = first_seg(lv);
	struct dm_list sibling_pvs, *pvh1, *pvh2;
	struct pv_list *pvl1, *pvl2;
	uint32_t s;

	/* Early return for invalid cases */
	if (!lv_name || !*lv_name ||
	    !seg_is_raid(raid_seg) ||
	    seg_is_raid0(raid_seg) ||
	    !strcmp(lv->name, lv_name))
		return 1;

	/* Search within RAID sub-LVs and find sibling */
	for (s = 0; s < raid_seg->area_count; s++) {
		/* Check if this rimage matches lv_name */
		if (seg_lv(raid_seg, s) && !strcmp(seg_lv(raid_seg, s)->name, lv_name)) {
			/* Found rimage, sibling is rmeta at same index */
			if (raid_seg->meta_areas)
				sibling_lv = seg_metalv(raid_seg, s);
			break;
		}
		/* Check if this rmeta matches lv_name */
		if (raid_seg->meta_areas && seg_metalv(raid_seg, s) &&
		    !strcmp(seg_metalv(raid_seg, s)->name, lv_name)) {
			/* Found rmeta, sibling is rimage at same index */
			sibling_lv = seg_lv(raid_seg, s);
			break;
		}
	}

	if (!sibling_lv) {
		log_debug("No sibling found for %s (not a RAID sub-LV).", lv_name);
		return 1; /* Not an error - might not be a RAID sub-LV */
	}

	/* Get PV list for the sibling LV */
	dm_list_init(&sibling_pvs);
	if (!get_pv_list_for_lv(lv->vg->cmd->mem, sibling_lv, &sibling_pvs)) {
		log_error("Can't find PVs for sibling LV %s.", sibling_lv->name);
		return 0;
	}

	/* Remove sibling PVs from trim_list */
	dm_list_iterate(pvh1, &sibling_pvs) {
		pvl1 = dm_list_item(pvh1, struct pv_list);

		dm_list_iterate(pvh2, trim_list) {
			pvl2 = dm_list_item(pvh2, struct pv_list);

			if (pvl1->pv == pvl2->pv) {
				log_debug("Removing PV %s from trim list (sibling of %s).",
					  pvl2->pv->dev->pvid, lv_name);
				dm_list_del(&pvl2->list);
				break;  /* PV found and removed, continue with next sibling PV */
			}
		}
	}

	return 1;
}

/*
 * _trim_allocatable_pvs
 * @alloc_list
 * @trim_list
 *
 * Remove PVs in 'trim_list' from 'alloc_list'.
 *
 * Returns: 1 on success, 0 on error
 */
static int _trim_allocatable_pvs(struct dm_list *alloc_list,
				 struct dm_list *trim_list,
				 alloc_policy_t alloc)
{
	struct dm_list *pvht, *pvh, *trim_pvh;
	struct pv_list *pvl, *trim_pvl;

	if (!alloc_list) {
		log_error(INTERNAL_ERROR "alloc_list is NULL.");
		return 0;
	}

	if (!trim_list || dm_list_empty(trim_list))
		return 1; /* alloc_list stays the same */

	dm_list_iterate_safe(pvh, pvht, alloc_list) {
		pvl = dm_list_item(pvh, struct pv_list);

		dm_list_iterate(trim_pvh, trim_list) {
			trim_pvl = dm_list_item(trim_pvh, struct pv_list);

			/* Don't allocate onto a trim PV */
			if ((alloc != ALLOC_ANYWHERE) &&
			    (pvl->pv == trim_pvl->pv)) {
				dm_list_del(&pvl->list);
				break;  /* goto next in alloc_list */
			}
		}
	}
	return 1;
}

/*
 * Replace any LV segments on given PV with temporary mirror.
 * Returns list of LVs changed.
 */
static int _insert_pvmove_mirrors(struct cmd_context *cmd,
				  struct logical_volume *lv_mirr,
				  struct dm_list *source_pvl,
				  struct logical_volume *lv,
				  struct dm_list *lvs_changed)

{
	struct pv_list *pvl;
	uint32_t prev_le_count;

	/* Only 1 PV may feature in source_pvl */
	pvl = dm_list_item(source_pvl->n, struct pv_list);

	prev_le_count = lv_mirr->le_count;
	if (!insert_layer_for_segments_on_pv(cmd, lv, lv_mirr, PVMOVE,
					     pvl, lvs_changed))
		return_0;

	/* check if layer was inserted */
	if (lv_mirr->le_count - prev_le_count) {
		lv->status |= LOCKED;

		log_verbose("Moving %u extents of logical volume %s.",
			    lv_mirr->le_count - prev_le_count,
			    display_lvname(lv));
	}

	return 1;
}

/*
 * Is 'lv' a sub_lv of the LV by the name of 'lv_name'?
 *
 * Returns: 1 if true, 0 otherwise
 */
static int _sub_lv_of(struct logical_volume *lv, const char *lv_name)
{
	struct lv_segment *seg;

	/* Sub-LVs only ever have one segment using them */
	if (dm_list_size(&lv->segs_using_this_lv) != 1)
		return 0;

	if (!(seg = get_only_segment_using_this_lv(lv)))
		return_0;

	if (!strcmp(seg->lv->name, lv_name))
		return 1;

	/* Continue up the tree */
	return _sub_lv_of(seg->lv, lv_name);
}

/* Return 0 if lv on source_pvl cannot be pvmoved. source_pvl may be NULL. */
static int _pvmove_lv_check_moveable(const struct logical_volume *lv,
				     struct dm_list *source_pvl)
{
	struct logical_volume *lv_cachevol;
	struct logical_volume *lv_orig;

	if (lv_is_converting(lv) || lv_is_merging(lv)) {
		log_error("Unable to pvmove when %s volume %s is present.",
			  lv_is_converting(lv) ? "converting" : "merging",
			  display_lvname(lv));
		return 0;
	}

	if (lv_is_writecache_cachevol(lv)) {
		log_error("Unable to pvmove device used for writecache.");
		return 0;
	}

	if (lv_is_writecache(lv)) {
		lv_cachevol = first_seg(lv)->writecache;
		if (source_pvl && lv_is_on_pvs(lv_cachevol, source_pvl)) {
			log_error("Unable to move device used for writecache cachevol %s.",
				  display_lvname(lv_cachevol));
			return 0;
		}
	}

	if (lv_is_raid(lv) && lv_raid_has_integrity(lv)) {
		log_error("Unable to pvmove device used for raid with integrity.");
		return 0;
	}

	if (lv_is_cache(lv) || lv_is_writecache(lv)) {
		lv_orig = seg_lv(first_seg(lv), 0);
		if (lv_is_raid(lv_orig) && lv_raid_has_integrity(lv_orig)) {
			log_error("Unable to pvmove raid LV with integrity under cache.");
			return 0;
		}
	}

	return 1;
}

/*
 * Check whether the holder LV's lock state allows pvmove.
 * Returns:  1 moveable,
 *           0 unmoveable but other LVs may proceed (sets *lv_skipped),
 *          -1 unmoveable hard error (named LV).
 *
 * In shared VGs: try to acquire EX lock on the holder.
 *   If the holder is already locally active, we hold EX from activation.
 *   If the holder is inactive, try lockd_lv(EX) to test for remote locks.
 *     lockd_lv fails (-EAGAIN): another host holds a lock.
 *       Named pvmove (-n): hard error (-1) -- user asked for this LV.
 *       Unnamed pvmove: unmoveable (0) -- move other LVs on the PV.
 *     lockd_lv succeeds: no remote lock, add to locked_lvs for later unlock.
 * Inactive holders (no lock, not active): proceed with metadata-only insertion.
 */
static int _pvmove_lv_check_holder_lock(struct cmd_context *cmd,
					struct volume_group *vg,
					struct logical_volume *lv,
					const char *lv_name,
					const struct logical_volume *holder,
					int *lv_skipped,
					struct dm_list *locked_lvs)
{
	if (vg_is_shared(vg) &&
	    !(lv_name && !strcmp(holder->name, lv_name))) {
		/*
		 * Skip the lock attempt when lv_name matches the holder:
		 * the caller already holds an EX lock on the named LV.
		 */
		if (lv_is_active(holder))
			return 1; /* Locally active - we hold EX from activation. */

		/*
		 * Inactive holder in shared VG: try to acquire EX lock.
		 * If another host holds a lock, lockd_lv returns 0 (-EAGAIN).
		 */
		if (!lockd_lv(cmd, (struct logical_volume *)holder, "ex", LDLV_PERSISTENT)) {
			if (lv_name) {
				log_error("Cannot pvmove LV %s: holder %s is locked on another node.",
					  display_lvname(lv), display_lvname(holder));
				return -1;
			}
			*lv_skipped = 1;
			log_print_unless_silent("Skipping LV %s - holder %s is locked on another node.",
						display_lvname(lv), display_lvname(holder));
			return 0;
		}

		/*
		 * Lock acquired - track for later unlock after metadata
		 * commit (LOCKED flag will protect the LV).
		 */
		if (locked_lvs) {
			struct lv_list *lvl;

			if (!(lvl = dm_pool_alloc(cmd->mem, sizeof(*lvl)))) {
				log_error("Failed to allocate LV list entry.");
				lockd_lv(cmd, (struct logical_volume *)holder, "un", LDLV_PERSISTENT);
				return -1;
			}
			lvl->lv = (struct logical_volume *)holder;
			dm_list_add(locked_lvs, &lvl->list);
		}

		return 1;
	}

	if (lv_is_active(holder))
		return 1;

	/* Holder is not active anywhere - metadata-only insertion */
	if (holder != lv)
		log_verbose("Holder %s is inactive, inserting pvmove mirrors for %s in metadata only.",
			    display_lvname(holder), display_lvname(lv));
	else
		log_verbose("LV %s is inactive, inserting pvmove mirrors in metadata only.",
			    display_lvname(lv));

	return 1;
}

/* Populate pvmove mirror LV with segments for the required copies */
static int _set_up_pvmove_lv(struct cmd_context *cmd,
			     struct volume_group *vg,
			     struct logical_volume *lv_mirr,
			     struct dm_list *source_pvl,
			     const char *lv_name,
			     struct dm_list *allocatable_pvs,
			     alloc_policy_t alloc,
			     struct dm_list **lvs_changed,
			     struct dm_list *locked_lvs)
{
	struct logical_volume *lv;
	struct lv_segment *seg;
	struct lv_list *lvl;
	struct dm_list trim_list;
	uint32_t log_count = 0;
	int lv_found = 0;
	int lv_skipped = 0;
	const struct logical_volume *holder;
	const char *new_lv_name;

	if (!(*lvs_changed = dm_pool_alloc(cmd->mem, sizeof(**lvs_changed)))) {
		log_error("lvs_changed list struct allocation failed.");
		return 0;
	}

	dm_list_init(*lvs_changed);

	/*
	 * First,
	 * use top-level RAID and mirror LVs to build a list of PVs
	 * that must be avoided during allocation.  This is necessary
	 * to maintain redundancy of those targets, but it is also
	 * sub-optimal.  Avoiding entire PVs in this way limits our
	 * ability to find space for other segment types.  In the
	 * majority of cases, however, this method will suffice and
	 * in the cases where it does not, the user can issue the
	 * pvmove on a per-LV basis.
	 *
	 * FIXME: Eliminating entire PVs places too many restrictions
	 *        on allocation.
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (lv == lv_mirr)
			continue;

		if (lv_name) {
			if (!(new_lv_name = top_level_lv_name(vg, lv_name)))
				return_0;

			if (strcmp(lv->name, new_lv_name))
				continue;
		}

		if (!lv_is_on_pvs(lv, source_pvl))
			continue;

		if (!_pvmove_lv_check_moveable(lv, source_pvl))
			return_0;

		seg = first_seg(lv);
		if (seg_is_raid(seg) || seg_is_mirrored(seg)) {
			dm_list_init(&trim_list);

			if (!get_pv_list_for_lv(vg->cmd->mem, lv, &trim_list))
				return_0;

			/*
			 * Remove any PVs holding SubLV siblings to allow
			 * for collocation (e.g. *rmeta_0 -> *rimage_0).
			 *
			 * Callee checks for lv_name and valid raid segment type.
			 */
			if (!_remove_sibling_pvs_from_trim_list(lv, lv_name, &trim_list))
				return_0;

			if (!_trim_allocatable_pvs(allocatable_pvs,
						   &trim_list, alloc))
				return_0;
		}
	}

	/*
	 * Second,
	 * use bottom-level LVs (like *_mimage_*, *_mlog, *_rmeta_*, etc)
	 * to find segments to be moved and then set up mirrors.
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (lv == lv_mirr)
			continue;

		if (lv_name) {
			if (strcmp(lv->name, lv_name) && !_sub_lv_of(lv, lv_name))
				continue;
			lv_found = 1;
		}

		if (!lv_is_striped(lv))
			continue; /* bottom-level LVs only... */

		if (!lv_is_on_pvs(lv, source_pvl))
			continue;

		if (lv_is_locked(lv)) {
			if (lv_name) {
				log_error("LV %s is locked by another pvmove.",
					  display_lvname(lv));
				return 0;
			}
			lv_skipped = 1;
			log_print_unless_silent("Skipping LV %s: locked by another pvmove.",
						display_lvname(lv));
			continue;
		}

		holder = lv_lock_holder(lv);

		if (!_pvmove_lv_check_moveable(holder, source_pvl))
			return_0;

		/*
		 * Safe against TOCTOU: the VG EX lock (READ_FOR_UPDATE)
		 * blocks activation on other nodes between this check
		 * and _insert_pvmove_mirrors below.
		 */
		{
			int r = _pvmove_lv_check_holder_lock(cmd, vg, lv, lv_name, holder, &lv_skipped, locked_lvs);
			if (r < 0)
				return_0;
			if (!r)
				continue;
		}

		if (!_insert_pvmove_mirrors(cmd, lv_mirr, source_pvl, lv,
					    *lvs_changed))
			return_0;
	}

	if (lv_name && !lv_found) {
		log_error("Logical volume %s not found.", lv_name);
		return 0;
	}

	/* Is temporary mirror empty? */
	if (!lv_mirr->le_count) {
		if (lv_skipped)
			log_error("All data on source PV skipped. "
				  "It contains locked, hidden or "
				  "non-top level LVs only.");
		log_error("No data to move for %s.", vg->name);
		return 0;
	}

	if (!lv_add_mirrors(cmd, lv_mirr, 1, 1, 0,
			    get_default_region_size(cmd),
			    log_count, allocatable_pvs, alloc,
			    (arg_is_set(cmd, atomic_ARG)) ?
			    MIRROR_BY_SEGMENTED_LV : MIRROR_BY_SEG)) {
		log_error("Failed to convert pvmove LV to mirrored.");
		return 0;
	}

	if (!split_parent_segments_for_layer(cmd, lv_mirr)) {
		log_error("Failed to split segments being moved.");
		return 0;
	}

	return 1;
}

/*
 * Validate a resume of an in-progress pvmove: find participating LVs,
 * check that the named LV (if any) matches, warn about ignored args.
 * Returns 1 on success (lvs_changed populated), 0 on error.
 */
static int _pvmove_validate_resume(struct cmd_context *cmd,
				   struct volume_group *vg,
				   struct logical_volume *lv_mirr,
				   const char *lv_name,
				   int pv_count,
				   const char *pv_name,
				   struct dm_list **lvs_changed)
{
	log_print_unless_silent("Detected pvmove in progress for %s.", pv_name);

	if (!(*lvs_changed = lvs_using_lv(cmd, vg, lv_mirr))) {
		log_error("ABORTING: Failed to generate list of moving LVs.");
		return 0;
	}

	if (lv_name) {
		struct lv_list *lvl;
		int name_matches = 0;

		dm_list_iterate_items(lvl, *lvs_changed)
			if (!strcmp(lvl->lv->name, lv_name) ||
			    _sub_lv_of(lvl->lv, lv_name)) {
				name_matches = 1;
				break;
			}

		if (!name_matches) {
			log_error("LV %s is not part of the in-progress pvmove on %s.",
				  lv_name, pv_name);
			return 0;
		}
	}

	if (pv_count || lv_name)
		log_warn("WARNING: Ignoring remaining command line arguments.");

	return 1;
}

/*
 * Called to set up initial pvmove LV only.
 * (Not called after first or any other section completes.)
 */
static int _update_metadata(struct logical_volume *lv_mirr,
			    struct dm_list *lvs_changed)
{
	struct lv_list *lvl;
	struct logical_volume *lv = NULL;

	/*
	 * Find the first active LV from lvs_changed for lv_update_and_reload().
	 * The suspend/resume of an active LV with track_pvmove_deps=1
	 * (via !lv_is_pvmove) discovers and reloads ALL active participating
	 * LVs through the pvmove dependency tree.
	 *
	 * If no LV is active, mirrors exist only in metadata and will
	 * take effect when LVs are activated.
	 */
	dm_list_iterate_items(lvl, lvs_changed) {
		if (lv_is_active(lvl->lv)) {
			lv = lvl->lv;
			break;
		}
	}

	if (!lv) {
		/*
		 * No active LV participates in this pvmove.
		 * Write and commit metadata without suspend/resume:
		 * there are no kernel tables to reload.
		 */
		log_verbose("No active LVs affected by pvmove, committing metadata only.");
		if (!vg_write(lv_mirr->vg))
			return_0;
		if (!vg_commit(lv_mirr->vg))
			return_0;
	} else {
		if (!lv_update_and_reload(lv))
			return_0;
	}

	/* Activate pvmove mirror to start the data copy */
	if (!activate_lv(lv_mirr->vg->cmd, lv_mirr)) {
		if (test_mode())
			return 1;

		/*
		 * FIXME Run --abort internally here.
		 */
		log_error("ABORTING: Temporary pvmove mirror activation failed. Run pvmove --abort.");
		return 0;
	}

	return 1;
}

static int _copy_id_components(struct cmd_context *cmd,
			       const struct logical_volume *lv, char **vg_name,
			       char **lv_name, union lvid *lvid)
{
	if (!(*vg_name = dm_pool_strdup(cmd->mem, lv->vg->name)) ||
	    !(*lv_name = dm_pool_strdup(cmd->mem, lv->name))) {
		log_error("Failed to clone VG or LV name.");
		return 0;
	}

	*lvid = lv->lvid;

	return 1;
}

static int _pvmove_setup_single(struct cmd_context *cmd,
				struct volume_group *vg,
				struct physical_volume *pv,
				struct processing_handle *handle)
{
	struct pvmove_params *pp = (struct pvmove_params *) handle->custom_handle;
	const char *lv_name = NULL;
	struct dm_list *source_pvl;
	struct dm_list *allocatable_pvs;
	struct dm_list *lvs_changed;
	struct dm_list locked_lvs;
	struct logical_volume *lv_mirr = NULL;
	struct logical_volume *lv = NULL;
	const char *pv_name = pv_dev_name(pv);
	struct lv_list *lvl;
	int lv_locked = 0;
	int lv_mirr_locked = 0;
	int r = ECMD_FAILED;

	dm_list_init(&locked_lvs);

	if (!vg) {
		log_error(INTERNAL_ERROR "Missing volume group.");
		return r;
	}

	pp->found_pv = 1;
	pp->setup_result = ECMD_FAILED;

	if (pp->lv_name_arg) {
		if (!(lv_name = _extract_lvname(cmd, vg->name, pp->lv_name_arg))) {
			log_error("Failed to find an LV name.");
			pp->setup_result = EINVALID_CMD_LINE;
			return ECMD_FAILED;
		}

		if (!validate_name(lv_name)) {
			log_error("Logical volume name %s is invalid.", lv_name);
			pp->setup_result = EINVALID_CMD_LINE;
			return ECMD_FAILED;
		}

		if (!(lv = find_lv(vg, lv_name))) {
			log_error("Logical volume %s not found.", lv_name);
			return ECMD_FAILED;
		}

		if (!_pvmove_lv_check_moveable(lv, NULL))
			return ECMD_FAILED;

		if (lv_is_locked(lv)) {
			log_error("LV %s is already locked by another pvmove.",
				  display_lvname(lv));
			return ECMD_FAILED;
		}
	}

	/*
	 * FIXME: The named-LV requirement for shared VGs is artificially
	 * conservative.  pvmove always works on identified per-LV extents, so
	 * moving all LVs from a PV should be safe once each participating LV
	 * is locked exclusively before being moved.  Remove this restriction
	 * once the no-name path acquires per-LV locks in shared VGs.
	 *
	 * We would need to avoid any PEs used by LVs that are active (ex) on
	 * other hosts.  For LVs that are active on multiple hosts (sh), we
	 * would need to used cluster mirrors.
	 */
	if (vg_is_shared(vg)) {
		if (!lv) {
			log_error("pvmove in a shared VG requires a named LV.");
			return ECMD_FAILED;
		}

		if (lv_is_lockd_sanlock_lv(lv)) {
			log_error("pvmove not allowed on internal sanlock LV.");
			return ECMD_FAILED;
		}
	}

	if ((lv_mirr = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		/* Resume path */
		if (!_pvmove_validate_resume(cmd, vg, lv_mirr, lv_name,
					     pp->pv_count, pv_name, &lvs_changed))
			goto_out;

		/* Re-acquire cluster lock before activating (shared VGs) */
		if (vg_is_shared(vg) && lv_mirr->lock_args) {
			if (!lockd_lv(cmd, lv_mirr, "ex", LDLV_PERSISTENT)) {
				log_error("ABORTING: Failed to re-acquire cluster lock for pvmove LV.");
				goto out;
			}
			lv_mirr_locked = 1;
		}

		/* Ensure mirror LV is active */
		if (!activate_lv(cmd, lv_mirr)) {
			log_error("ABORTING: Temporary mirror activation failed.");
			/*
			 * Keep the cluster lock held: pvmove state is already
			 * committed in metadata from a previous run.  Releasing
			 * the lock would leave the metadata unprotected.
			 */
			lv_mirr_locked = 0;
			goto out;
		}
	} else {
		/* New pvmove path */

		/*
		 * Lock the named LV to verify no other host is using it.
		 * Released after LOCKED status is committed in metadata.
		 */
		if (vg_is_shared(vg)) {
			if (!lockd_lv(cmd, lv, "ex", LDLV_PERSISTENT)) {
				log_error("pvmove in a shared VG requires exclusive lock on named LV.");
				return ECMD_FAILED;
			}
			lv_locked = 1;
		}

		/* Determine PE ranges to be moved */
		if (!(source_pvl = create_pv_list(cmd->mem, vg, 1,
						  &pp->pv_name_arg, 0)))
			goto_out;

		if (pp->alloc == ALLOC_INHERIT)
			pp->alloc = vg->alloc;

		/* Get PVs we can use for allocation */
		if (!(allocatable_pvs = _get_allocatable_pvs(cmd, pp->pv_count, pp->pv_names,
							     vg, pv, pp->alloc)))
			goto_out;

		/* FIXME Cope with non-contiguous => splitting existing segments */
		if (!(lv_mirr = lv_create_empty("pvmove%d", NULL,
						LVM_READ | LVM_WRITE,
						ALLOC_CONTIGUOUS, vg))) {
			log_error("Creation of temporary pvmove LV failed.");
			goto out;
		}

		lv_mirr->status |= (PVMOVE | LOCKED);

		/* In shared VGs, acquire cluster lock for pvmove LV */
		if (vg_is_shared(vg)) {
			if (!lockd_init_lv_args(cmd, vg, lv_mirr,
						vg->lock_type, NULL, &lv_mirr->lock_args)) {
				log_error("Failed to initialize lock for pvmove LV.");
				goto out;
			}
			if (!lockd_lv(cmd, lv_mirr, "ex", LDLV_PERSISTENT)) {
				log_error("Failed to acquire cluster lock for pvmove LV.");
				goto out;
			}
			lv_mirr_locked = 1;
		}

		if (!_set_up_pvmove_lv(cmd, vg, lv_mirr, source_pvl, lv_name,
				       allocatable_pvs, pp->alloc, &lvs_changed,
				       &locked_lvs))
			goto_out;

		if (!_update_metadata(lv_mirr, lvs_changed))
			goto_out;

		/*
		 * LOCKED in metadata protects the LVs now.
		 * Release the named LV lock and any locks acquired
		 * on inactive holder LVs during _pvmove_lv_check_holder_lock().
		 */
		if (lv_locked) {
			if (lockd_lv(cmd, lv, "un", LDLV_PERSISTENT))
				lv_locked = 0;
			else
				log_warn("WARNING: Failed to unlock LV %s.",
					 display_lvname(lv));
		}

		dm_list_iterate_items(lvl, &locked_lvs)
			if (!lockd_lv(cmd, lvl->lv, "un", LDLV_PERSISTENT))
				log_warn("WARNING: Failed to unlock LV %s.",
					 display_lvname(lvl->lv));
		dm_list_init(&locked_lvs);
	}

	if (!_copy_id_components(cmd, lv_mirr, &pp->id_vg_name, &pp->id_lv_name, pp->lvid))
		goto_out;

	pp->setup_result = ECMD_PROCESSED;
	r = ECMD_PROCESSED;
out:
	if (r != ECMD_PROCESSED) {
		if (lv_locked)
			if (!lockd_lv(cmd, lv, "un", LDLV_PERSISTENT))
				log_warn("WARNING: Failed to unlock LV %s.",
					 display_lvname(lv));
		if (lv_mirr_locked)
			if (!lockd_lv(cmd, lv_mirr, "un", LDLV_PERSISTENT))
				log_warn("WARNING: Failed to unlock pvmove LV %s.",
					 display_lvname(lv_mirr));
		dm_list_iterate_items(lvl, &locked_lvs)
			if (!lockd_lv(cmd, lvl->lv, "un", LDLV_PERSISTENT))
				log_warn("WARNING: Failed to unlock LV %s.",
					 display_lvname(lvl->lv));
	}

	return r;
}

static int _pvmove_read_single(struct cmd_context *cmd,
				struct volume_group *vg,
				struct physical_volume *pv,
				struct processing_handle *handle)
{
	struct pvmove_params *pp = (struct pvmove_params *) handle->custom_handle;
	struct logical_volume *lv;
	int ret = ECMD_FAILED;

	if (!vg) {
		log_error(INTERNAL_ERROR "Missing volume group.");
		return ret;
	}

	pp->found_pv = 1;

	if (!(lv = find_pvmove_lv(vg, pv_dev(pv), PVMOVE))) {
		log_print_unless_silent("%s: No pvmove in progress - already finished or aborted.",
					pv_dev_name(pv));
		ret = ECMD_PROCESSED;
		pp->in_progress = 0;
	} else if (_copy_id_components(cmd, lv, &pp->id_vg_name, &pp->id_lv_name, pp->lvid)) {
		ret = ECMD_PROCESSED;
		pp->in_progress = 1;
	}

	return ret;
}

static const struct poll_functions _pvmove_fns = {
	.get_copy_name_from_lv = get_pvmove_pvname_from_lv_mirr,
	.poll_progress = poll_mirror_progress,
	.update_metadata = pvmove_update_metadata,
	.finish_copy = pvmove_finish,
};

int pvmove_poll(struct cmd_context *cmd, const char *pv_name,
		const char *uuid, const char *vg_name,
		const char *lv_name, unsigned background)
{
	const struct poll_operation_id *id = NULL, create_id = {
		.vg_name = vg_name,
		.lv_name = lv_name,
		.display_name = pv_name,
		.uuid = uuid
	};

	if (uuid) {
		if (!vg_name || !lv_name || !pv_name) {
			log_error(INTERNAL_ERROR "Wrong params for creation of pvmove id.");
			return ECMD_FAILED;
		}
		id = &create_id;
	}

	if (test_mode())
		return ECMD_PROCESSED;

	return poll_daemon(cmd, background, PVMOVE, &_pvmove_fns, "Moved", id);
}

int pvmove(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvmove_params pp = { 0 };
	struct processing_handle *handle = NULL;
	union lvid *lvid = NULL;
	char *pv_name = NULL;
	char *colon;
	unsigned is_abort = arg_is_set(cmd, abort_ARG);

	/* dm raid1 target must be present in every case */
	if (!_pvmove_target_present(cmd, 0)) {
		log_error("Required device-mapper target(s) not "
			  "detected in your kernel.");
		return ECMD_FAILED;
	}

	if (lvmlockd_use() && !lvmpolld_use()) {
		/*
		 * Don't want to spend the time making lvmlockd
		 * work without lvmpolld.
		 */
		log_error("Enable lvmpolld when using lvmlockd.");
		return ECMD_FAILED;
	}

	if (lvmlockd_use() && !argc) {
		/*
		 * FIXME: move process_each_vg from polldaemon up to here,
		 * then we can remove this limitation.
		 */
		log_error("Specify pvmove args when using lvmlockd.");
		return ECMD_FAILED;
	}

	if (argc) {
		if (!(lvid = dm_pool_alloc(cmd->mem, sizeof(*lvid)))) {
			log_error("Failed to allocate lvid.");
			return ECMD_FAILED;
		}
		pp.lvid = lvid;

		if (!(pp.pv_name_arg = dm_pool_strdup(cmd->mem, argv[0]))) {
			log_error("Failed to clone PV name.");
			return ECMD_FAILED;
		}

		if (!(pv_name = dm_pool_strdup(cmd->mem, argv[0]))) {
			log_error("Failed to clone PV name.");
			return ECMD_FAILED;
		}

		dm_unescape_colons_and_at_signs(pv_name, &colon, NULL);

		/* Drop any PE lists from PV name */
		if (colon)
			*colon = '\0';

		argc--;
		argv++;

		pp.pv_count = argc;
		pp.pv_names = argv;

		if (arg_is_set(cmd, name_ARG)) {
			if (!(pp.lv_name_arg = dm_pool_strdup(cmd->mem, arg_value(cmd, name_ARG))))  {
				log_error("Failed to clone LV name.");
				return ECMD_FAILED;
			}
		}

		pp.alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);

		pp.in_progress = 1;

		/* Normal pvmove setup requires ex lock from lvmlockd. */
		if (is_abort)
			cmd->lockd_vg_default_sh = 1;

		if (!(handle = init_processing_handle(cmd, NULL))) {
			log_error("Failed to initialize processing handle.");
			return ECMD_FAILED;
		}

		handle->custom_handle = &pp;

		process_each_pv(cmd, 1, &pv_name, NULL, 0,
				is_abort ? 0 : READ_FOR_UPDATE,
				handle,
				is_abort ? &_pvmove_read_single : &_pvmove_setup_single);

		destroy_processing_handle(cmd, handle);

		if (!is_abort) {
			if (!pp.found_pv) {
				stack;
				return EINVALID_CMD_LINE;
			}

			if (pp.setup_result != ECMD_PROCESSED) {
				stack;
				return pp.setup_result;
			}
		} else {
			if (!pp.found_pv)
				return_ECMD_FAILED;

			if (!pp.in_progress)
				return ECMD_PROCESSED;
		}

		/*
		 * The command may sit and report progress for some time,
		 * and we do not want or need the global lock held during
		 * that time.
		 */
		lock_global(cmd, "un");
	}

	return pvmove_poll(cmd, pv_name, lvid ? lvid->s : NULL,
			   pp.id_vg_name, pp.id_lv_name,
			   arg_is_set(cmd, background_ARG));
}
