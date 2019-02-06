/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
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
#include "lib/device/device.h"
#include "lib/metadata/metadata.h"
#include "lib/commands/toolcontext.h"
#include "lib/misc/lvm-string.h"
#include "lib/misc/lvm-file.h"
#include "lib/cache/lvmcache.h"
#include "lib/mm/memlock.h"
#include "lib/datastruct/str_list.h"
#include "lib/metadata/pv_alloc.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/display/display.h"
#include "lib/locking/locking.h"
#include "lib/format_text/archiver.h"
#include "lib/format_text/format-text.h"
#include "lib/format_text/layout.h"
#include "lib/format_text/import-export.h"
#include "lib/config/defaults.h"
#include "lib/locking/lvmlockd.h"
#include "lib/notify/lvmnotify.h"

#include <time.h>
#include <math.h>

static int _check_pv_ext(struct cmd_context *cmd, struct volume_group *vg)
{
	struct lvmcache_info *info;
	uint32_t ext_version, ext_flags;
	struct pv_list *pvl;

	if (vg_is_foreign(vg))
		return 1;

	if (vg_is_shared(vg))
		return 1;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (is_missing_pv(pvl->pv))
			continue;

		/* is_missing_pv doesn't catch NULL dev */
		if (!pvl->pv->dev)
			continue;

		if (!(info = lvmcache_info_from_pvid(pvl->pv->dev->pvid, pvl->pv->dev, 0)))
			continue;

		ext_version = lvmcache_ext_version(info);
		if (ext_version < PV_HEADER_EXTENSION_VSN) {
			log_warn("WARNING: PV %s in VG %s is using an old PV header, modify the VG to update.",
				 dev_name(pvl->pv->dev), vg->name);
			continue;
		}

		ext_flags = lvmcache_ext_flags(info);
		if (!(ext_flags & PV_EXT_USED)) {
			log_warn("WARNING: PV %s in VG %s is missing the used flag in PV header.",
				 dev_name(pvl->pv->dev), vg->name);
		}
	}

	return 1;
}

#define DEV_LIST_DELIM ", "

static int _check_devs_used_correspond_with_lv(struct dm_pool *mem, struct dm_list *list, struct logical_volume *lv)
{
	struct device_list *dl;
	int found_inconsistent = 0;
	struct device *dev;
	struct lv_segment *seg;
	uint32_t s;
	int warned_about_no_dev = 0;
	char *used_devnames = NULL, *assumed_devnames = NULL;

	if (!(list = dev_cache_get_dev_list_for_lvid(lv->lvid.s + ID_LEN)))
		return 1;

	dm_list_iterate_items(dl, list) {
		dev = dl->dev;
		if (!(dev->flags & DEV_ASSUMED_FOR_LV)) {
			if (!found_inconsistent) {
				if (!dm_pool_begin_object(mem, 32))
					return_0;
				found_inconsistent = 1;
			} else {
				if (!dm_pool_grow_object(mem, DEV_LIST_DELIM, sizeof(DEV_LIST_DELIM) - 1))
					return_0;
			}
			if (!dm_pool_grow_object(mem, dev_name(dev), 0))
				return_0;
		}
	}

	if (!found_inconsistent)
		return 1;

	if (!dm_pool_grow_object(mem, "\0", 1))
		return_0;
	used_devnames = dm_pool_end_object(mem);

	found_inconsistent = 0;
	dm_list_iterate_items(seg, &lv->segments) {
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) == AREA_PV) {
				if (!(dev = seg_dev(seg, s))) {
					if (!warned_about_no_dev) {
						log_warn("WARNING: Couldn't find all devices for LV %s "
							 "while checking used and assumed devices.",
							  display_lvname(lv));
						warned_about_no_dev = 1;
					}
					continue;
				}
				if (!(dev->flags & DEV_USED_FOR_LV)) {
					if (!found_inconsistent) {
						if (!dm_pool_begin_object(mem, 32))
                                                        return_0;
						found_inconsistent = 1;
					} else {
						if (!dm_pool_grow_object(mem, DEV_LIST_DELIM, sizeof(DEV_LIST_DELIM) - 1))
							return_0;
					}
					if (!dm_pool_grow_object(mem, dev_name(dev), 0))
						return_0;
				}
			}
		}
	}

	if (found_inconsistent) {
		if (!dm_pool_grow_object(mem, "\0", 1))
			return_0;
		assumed_devnames = dm_pool_end_object(mem);
		log_warn("WARNING: Device mismatch detected for %s which is accessing %s instead of %s.",
			 display_lvname(lv), used_devnames, assumed_devnames);
	}

	return 1;
}

static void _check_devs_used_correspond_with_vg(struct volume_group *vg)
{
	struct dm_pool *mem;
	char vgid[ID_LEN + 1];
	struct pv_list *pvl;
	struct lv_list *lvl;
	struct dm_list *list;
	struct device_list *dl;
	int found_inconsistent = 0;

	strncpy(vgid, (const char *) vg->id.uuid, sizeof(vgid));
	vgid[ID_LEN] = '\0';

	/* Mark all PVs in VG as used. */
	dm_list_iterate_items(pvl, &vg->pvs) {
		/*
		 * FIXME: It's not clear if the meaning
		 * of "missing" should always include the
		 * !pv->dev case, or if "missing" is the
		 * more narrow case where VG metadata has
		 * been written with the MISSING flag.
		 */
		if (!pvl->pv->dev)
			continue;
		if (is_missing_pv(pvl->pv))
			continue;
		pvl->pv->dev->flags |= DEV_ASSUMED_FOR_LV;
	}

	if (!(list = dev_cache_get_dev_list_for_vgid(vgid)))
		return;

	dm_list_iterate_items(dl, list) {
		if (!(dl->dev->flags & DEV_OPEN_FAILURE) &&
		    !(dl->dev->flags & DEV_ASSUMED_FOR_LV)) {
			found_inconsistent = 1;
			break;
		}
	}

	if (found_inconsistent) {
		if (!(mem = dm_pool_create("vg_devs_check", 1024)))
			return;

		dm_list_iterate_items(lvl, &vg->lvs) {
			if (!_check_devs_used_correspond_with_lv(mem, list, lvl->lv)) {
				dm_pool_destroy(mem);
				return;
			}
		}

		dm_pool_destroy(mem);
	}

	return;
}

static void _destroy_fid(struct format_instance **fid)
{
	if (*fid) {
		(*fid)->fmt->ops->destroy_instance(*fid);
		*fid = NULL;
	}
}

static int _access_vg_clustered(struct cmd_context *cmd, const struct volume_group *vg)
{
	if (vg_is_clustered(vg)) {
		/*
		 * force_access_clustered is only set when forcibly
		 * converting a clustered vg to lock type none.
		 */
		if (cmd->force_access_clustered) {
			log_debug("Allowing forced access to clustered vg %s", vg->name);
			return 1;
		}

		log_verbose("Skipping clustered VG %s.", vg->name);
		return 0;
	}

	return 1;
}

static int _allow_extra_system_id(struct cmd_context *cmd, const char *system_id)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	const char *str;

	if (!(cn = find_config_tree_array(cmd, local_extra_system_ids_CFG, NULL)))
		return 0;

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type == DM_CFG_EMPTY_ARRAY)
			break;
		/* Ignore invalid data: Warning message already issued by config.c */
		if (cv->type != DM_CFG_STRING)
			continue;
		str = cv->v.str;
		if (!*str)
			continue;

		if (!strcmp(str, system_id))
			return 1;
	}

	return 0;
}

static int _access_vg_lock_type(struct cmd_context *cmd, struct volume_group *vg,
				uint32_t lockd_state, uint32_t *failure)
{
	if (cmd->lockd_vg_disable)
		return 1;

	/*
	 * Local VG requires no lock from lvmlockd.
	 */
	if (!vg_is_shared(vg))
		return 1;

	/*
	 * When lvmlockd is not used, lockd VGs are ignored by lvm
	 * and cannot be used, with two exceptions:
	 *
	 * . The --shared option allows them to be revealed with
	 *   reporting/display commands.
	 *
	 * . If a command asks to operate on one specifically
	 *   by name, then an error is printed.
	 */
	if (!lvmlockd_use()) {
		/*
	 	 * Some reporting/display commands have the --shared option
		 * (like --foreign) to allow them to reveal lockd VGs that
		 * are otherwise ignored.  The --shared option must only be
		 * permitted in commands that read the VG for report or display,
		 * not any that write the VG or activate LVs.
	 	 */
		if (cmd->include_shared_vgs)
			return 1;

		/*
		 * Some commands want the error printed by vg_read, others by ignore_vg.
		 * Those using ignore_vg may choose to skip the error.
		 */
		if (cmd->vg_read_print_access_error) {
			log_error("Cannot access VG %s with lock type %s that requires lvmlockd.",
				  vg->name, vg->lock_type);
		}

		*failure |= FAILED_LOCK_TYPE;
		return 0;
	}

	/*
	 * The lock request from lvmlockd failed.  If the lock was ex,
	 * we cannot continue.  If the lock was sh, we could also fail
	 * to continue but since the lock was sh, it means the VG is
	 * only being read, and it doesn't hurt to allow reading with
	 * no lock.
	 */
	if (lockd_state & LDST_FAIL) {
		if ((lockd_state & LDST_EX) || cmd->lockd_vg_enforce_sh) {
			log_error("Cannot access VG %s due to failed lock.", vg->name);
			*failure |= FAILED_LOCK_MODE;
			return 0;
		}

		log_warn("Reading VG %s without a lock.", vg->name);
		return 1;
	}

	if (test_mode()) {
		log_error("Test mode is not yet supported with lock type %s.", vg->lock_type);
		return 0;
	}

	return 1;
}

int is_system_id_allowed(struct cmd_context *cmd, const char *system_id)
{
	/*
	 * A VG without a system_id can be accessed by anyone.
	 */
	if (!system_id || !system_id[0])
		return 1;

	/*
	 * Allowed if the host and VG system_id's match.
	 */
	if (cmd->system_id && !strcmp(cmd->system_id, system_id))
		return 1;

	/*
	 * Allowed if a host's extra system_id matches.
	 */
	if (cmd->system_id && _allow_extra_system_id(cmd, system_id))
		return 1;

	/*
	 * Not allowed if the host does not have a system_id
	 * and the VG does, or if the host and VG's system_id's
	 * do not match.
	 */

	return 0;
}

static int _access_vg_systemid(struct cmd_context *cmd, struct volume_group *vg)
{
	/*
	 * A few commands allow read-only access to foreign VGs.
	 */
	if (cmd->include_foreign_vgs)
		return 1;

	if (is_system_id_allowed(cmd, vg->system_id))
		return 1;

	/*
	 * Allow VG access if the local host has active LVs in it.
	 */
	if (lvs_in_vg_activated(vg)) {
		log_warn("WARNING: Found LVs active in VG %s with foreign system ID %s.  Possible data corruption.",
			  vg->name, vg->system_id);
		if (cmd->include_active_foreign_vgs)
			return 1;
		return 0;
	}

	/*
	 * Print an error when reading a VG that has a system_id
	 * and the host system_id is unknown.
	 */
	if (!cmd->system_id || cmd->unknown_system_id) {
		log_error("Cannot access VG %s with system ID %s with unknown local system ID.",
			  vg->name, vg->system_id);
		return 0;
	}

	/*
	 * Some commands want the error printed by vg_read, others by ignore_vg.
	 * Those using ignore_vg may choose to skip the error.
	 */
	if (cmd->vg_read_print_access_error) {
		log_error("Cannot access VG %s with system ID %s with local system ID %s.",
			  vg->name, vg->system_id, cmd->system_id);
		return 0;
	}

	/* Silently ignore foreign vgs. */

	return 0;
}

static struct volume_group *_vg_read(struct cmd_context *cmd,
				     const char *vgname,
				     const char *vgid,
				     unsigned precommitted)
{
	const struct format_type *fmt = cmd->fmt;
	struct format_instance *fid = NULL;
	struct format_instance_ctx fic;
	struct volume_group *vg, *vg_ret = NULL;
	struct metadata_area *mda, *mda2;
	unsigned use_precommitted = precommitted;
	struct device *mda_dev, *dev_ret;
	struct cached_vg_fmtdata *vg_fmtdata = NULL;	/* Additional format-specific data about the vg */
	int found_old_metadata = 0;
	unsigned use_previous_vg;

	log_debug_metadata("Reading VG %s %s", vgname ?: "<no name>", vgid ?: "<no vgid>");

	/*
	 * Rescan the devices that are associated with this vg in lvmcache.
	 * This repeats what was done by the command's initial label scan,
	 * but only the devices associated with this VG.
	 *
	 * The lvmcache info about these devs is from the initial label scan
	 * performed by the command before the vg lock was held.  Now the VG
	 * lock is held, so we rescan all the info from the devs in case
	 * something changed between the initial scan and now that the lock
	 * is held.
	 *
	 * Some commands (e.g. reporting) are fine reporting data read by
	 * the label scan.  It doesn't matter if the devs changed between
	 * the label scan and here, we can report what was seen in the
	 * scan, even though it is the old state, since we will not be
	 * making any modifications.  If the VG was being modified during
	 * the scan, and caused us to see inconsistent metadata on the
	 * different PVs in the VG, then we do want to rescan the devs
	 * here to get a consistent view of the VG.  Note that we don't
	 * know if the scan found all the PVs in the VG at this point.
	 * We don't know that until vg_read looks at the list of PVs in
	 * the metadata and compares that to the devices found by the scan.
	 *
	 * It's possible that a change made to the VG during scan was
	 * adding or removing a PV from the VG.  In this case, the list
	 * of devices associated with the VG in lvmcache would change
	 * due to the rescan.
	 *
	 * The devs in the VG may be persistently inconsistent due to some
	 * previous problem.  In this case, rescanning the labels here will
	 * find the same inconsistency.  The VG repair (mistakenly done by
	 * vg_read below) is supposed to fix that.
	 *
	 * FIXME: sort out the usage of the global lock (which is mixed up
	 * with the orphan lock), and when we can tell that the global
	 * lock is taken prior to the label scan, and still held here,
	 * we can also skip the rescan in that case.
	 */
	if (!cmd->can_use_one_scan || lvmcache_scan_mismatch(cmd, vgname, vgid)) {
		log_debug_metadata("Rescanning devices for %s", vgname);
		lvmcache_label_rescan_vg(cmd, vgname, vgid);
	} else {
		log_debug_metadata("Skipped rescanning devices for %s", vgname);
	}

	/* Now determine the correct vgname if none was supplied */
	if (!vgname && !(vgname = lvmcache_vgname_from_vgid(cmd->mem, vgid))) {
		log_debug_metadata("Cache did not find VG name from vgid %s", vgid);
		return NULL;
	}

	/* Determine the correct vgid if none was supplied */
	if (!vgid && !(vgid = lvmcache_vgid_from_vgname(cmd, vgname))) {
		log_debug_metadata("Cache did not find VG vgid from name %s", vgname);
		return NULL;
	}

	/*
	 * A "format instance" is an abstraction for a VG location,
	 * i.e. where a VG's metadata exists on disk.
	 *
	 * An fic (format_instance_ctx) is a temporary struct used
	 * to create an fid (format_instance).  The fid hangs around
	 * and is used to create a 'vg' to which it connected (vg->fid).
	 *
	 * The 'fic' describes a VG in terms of fmt/name/id.
	 *
	 * The 'fid' describes a VG in more detail than the fic,
	 * holding information about where to find the VG metadata.
	 *
	 * The 'vg' describes the VG in the most detail representing
	 * all the VG metadata.
	 *
	 * The fic and fid are set up by create_instance() to describe
	 * the VG location.  This happens before the VG metadata is
	 * assembled into the more familiar struct volume_group "vg".
	 *
	 * The fid has one main purpose: to keep track of the metadata
	 * locations for a given VG.  It does this by putting 'mda'
	 * structs on fid->metadata_areas_in_use, which specify where
	 * metadata is located on disk.  It gets this information
	 * (metadata locations for a specific VG) from the command's
	 * initial label scan.  The info is passed indirectly via
	 * lvmcache info/vginfo structs, which are created by the
	 * label scan and then copied into fid by create_instance().
	 *
	 * FIXME: just use the vginfo/info->mdas lists directly instead
	 * of copying them into the fid list.
	 */

	fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vgname;
	fic.context.vg_ref.vg_id = vgid;

	/*
	 * Sets up the metadata areas that we need to read below.
	 * For each info in vginfo->infos, for each mda in info->mdas,
	 * (found during label_scan), copy the mda to fid->metadata_areas_in_use
	 */
	if (!(fid = fmt->ops->create_instance(fmt, &fic))) {
		log_error("Failed to create format instance");
		return NULL;
	}

	/*
	 * We use the fid globally here so prevent the release_vg
	 * call to destroy the fid - we may want to reuse it!
	 */
	fid->ref_count++;


	/*
	 * label_scan found PVs for this VG and set up lvmcache to describe the
	 * VG/PVs that we use here to read the VG.  It created 'vginfo' for the
	 * VG, and created an 'info' attached to vginfo for each PV.  It also
	 * added a metadata_area struct to info->mdas for each metadata area it
	 * found on the PV.  The info->mdas structs are copied to
	 * fid->metadata_areas_in_use by create_instance above, and here we
	 * read VG metadata from each of those mdas.
	 */
	dm_list_iterate_items(mda, &fid->metadata_areas_in_use) {
		mda_dev = mda_get_device(mda);

		/* I don't think this can happen */
		if (!mda_dev) {
			log_warn("Ignoring metadata for VG %s from missing dev.", vgname);
			continue;
		}

		use_previous_vg = 0;

		if (use_precommitted) {
			log_debug_metadata("Reading VG %s precommit metadata from %s %llu",
				 vgname, dev_name(mda_dev), (unsigned long long)mda->header_start);

			vg = mda->ops->vg_read_precommit(fid, vgname, mda, &vg_fmtdata, &use_previous_vg);

			if (!vg && !use_previous_vg) {
				log_warn("WARNING: Reading VG %s precommit on %s failed.", vgname, dev_name(mda_dev));
				vg_fmtdata = NULL;
				continue;
			}
		} else {
			log_debug_metadata("Reading VG %s metadata from %s %llu",
				 vgname, dev_name(mda_dev), (unsigned long long)mda->header_start);

			vg = mda->ops->vg_read(fid, vgname, mda, &vg_fmtdata, &use_previous_vg);

			if (!vg && !use_previous_vg) {
				log_warn("WARNING: Reading VG %s on %s failed.", vgname, dev_name(mda_dev));
				vg_fmtdata = NULL;
				continue;
			}
		}

		if (!vg)
			continue;

		if (vg && !vg_ret) {
			vg_ret = vg;
			dev_ret = mda_dev;
			continue;
		}

		/* 
		 * Use the newest copy of the metadata found on any mdas.
		 * Above, We could check if the scan found an old metadata
		 * seqno in this mda and just skip reading it again; then these
		 * seqno checks would just be sanity checks.
		 */

		if (vg->seqno == vg_ret->seqno) {
			release_vg(vg);
			continue;
		}

		if (vg->seqno > vg_ret->seqno) {
			log_warn("WARNING: ignoring old metadata seqno %u on %s vs new metadata seqno %u on %s for VG %s.",
				 vg_ret->seqno, dev_name(dev_ret),
				 vg->seqno, dev_name(mda_dev), vg->name);
			found_old_metadata = 1;
			release_vg(vg_ret);
			vg_ret = vg;
			dev_ret = mda_dev;
			vg_fmtdata = NULL;
			continue;
		}

		if (vg_ret->seqno > vg->seqno) {
			log_warn("WARNING: ignoring old metadata seqno %u on %s vs new metadata seqno %u on %s for VG %s.",
				 vg->seqno, dev_name(mda_dev),
				 vg_ret->seqno, dev_name(dev_ret), vg->name);
			found_old_metadata = 1;
			release_vg(vg);
			vg_fmtdata = NULL;
			continue;
		}
	}

	if (found_old_metadata)
		log_warn("WARNING: Inconsistent metadata found for VG %s", vgname);

	vg = NULL;

	if (vg_ret)
		set_pv_devices(fid, vg_ret);

	fid->ref_count--;

	if (!vg_ret) {
		_destroy_fid(&fid);
		goto_out;
	}

	/*
	 * Correct the lvmcache representation of the VG using the metadata
	 * that we have chosen above (vg_ret).
	 *
	 * The vginfo/info representation created by label_scan was not
	 * entirely correct since it did not use the full or final metadata.
	 *
	 * In lvmcache, PVs with no mdas were not attached to the vginfo during
	 * label_scan because label_scan didn't know where they should go.  Now
	 * that we have the VG metadata we can tell, so use that to attach those
	 * info's to the vginfo.
	 *
	 * Also, outdated PVs that have been removed from the VG were incorrectly
	 * attached to the vginfo during label_scan, and now need to be detached.
	 */
	lvmcache_update_vg_from_read(vg_ret, vg_ret->status & PRECOMMITTED);

	/*
	 * lvmcache_update_vg identified outdated mdas that we read above that
	 * are not actually part of the VG.  Remove those outdated mdas from
	 * the fid's list of mdas.
	 */
	dm_list_iterate_items_safe(mda, mda2, &fid->metadata_areas_in_use) {
		mda_dev = mda_get_device(mda);
		if (lvmcache_is_outdated_dev(cmd, vg_ret->name, (const char *)&vg_ret->id, mda_dev)) {
			log_debug_metadata("vg_read %s ignore mda for outdated dev %s",
					   vg_ret->name, dev_name(mda_dev));
			dm_list_del(&mda->list);
		}
	}

out:
	return vg_ret;
}

struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name, const char *vgid,
			     uint32_t read_flags, uint32_t lockd_state,
			     uint32_t *error_flags, struct volume_group **error_vg)
{
	struct volume_group *vg = NULL;
	struct lv_list *lvl;
	struct pv_list *pvl;
	int missing_pv_dev = 0;
	int missing_pv_flag = 0;
	uint32_t failure = 0;
	int writing = (read_flags & READ_FOR_UPDATE);

	if (is_orphan_vg(vg_name)) {
		log_very_verbose("Reading orphan VG %s", vg_name);
		vg = vg_read_orphans(cmd, vg_name);
		*error_flags = 0;
		*error_vg = NULL;
		return vg;
	}

	if (!validate_name(vg_name)) {
		log_error("Volume group name \"%s\" has invalid characters.", vg_name);
		return NULL;
	}

	if (!lock_vol(cmd, vg_name, writing ? LCK_VG_WRITE : LCK_VG_READ, NULL)) {
		log_error("Can't get lock for %s", vg_name);
		failure |= FAILED_LOCKING;
		goto_bad;
	}

	if (!(vg = _vg_read(cmd, vg_name, vgid, 0))) {
		/* Some callers don't care if the VG doesn't exist and don't want an error message. */
		if (!(read_flags & READ_OK_NOTFOUND))
			log_error("Volume group \"%s\" not found", vg_name);
		failure |= FAILED_NOTFOUND;
		goto_bad;
	}

	/*
	 * Check and warn if PV ext info is not in sync with VG metadata
	 * (vg_write fixes.)
	 */
	_check_pv_ext(cmd, vg);

	if (!vg_strip_outdated_historical_lvs(vg))
		log_warn("WARNING: failed to strip outdated historical lvs.");

	/*
	 * Check for missing devices in the VG.  In most cases a VG cannot be
	 * changed while it's missing devices.  This restriction is implemented
	 * here in vg_read.  Below we return an error from vg_read if the
	 * vg_read flag indicates that the command is going to modify the VG.
	 * (We should probably implement this restriction elsewhere instead of
	 * returning an error from vg_read.)
	 *
	 * The PV's device may be present while the PV for the device has the
	 * MISSING_PV flag set in the metadata.  This happened because the VG
	 * was written while this dev was missing, so the MISSING flag was
	 * written in the metadata for PV.  Now the device has reappeared.
	 * However, the VG has changed since the device was last present, and
	 * if the device has outdated data it may not be safe to just start
	 * using it again.
	 *
	 * If there were no PE's used on the PV, we can just clear the MISSING
	 * flag, but if there were PE's used we need to continue to treat the
	 * PV as if the device is missing, limiting operations like the VG has
	 * a missing device, and requiring the user to remove the reappeared
	 * device from the VG, like a missing device, with vgreduce
	 * --removemissing.
	 */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!pvl->pv->dev) {
			/* The obvious and common case of a missing device. */

			log_warn("WARNING: VG %s is missing PVID %s.", vg_name, (const char *)&pvl->pv->id);
			missing_pv_dev++;

		} else if (pvl->pv->status & MISSING_PV) {
			/* A device that was missing but has reappeared. */

			if (pvl->pv->pe_alloc_count == 0) {
				log_warn("WARNING: VG %s has unused reappeared PV %s.", vg_name, dev_name(pvl->pv->dev));
				pvl->pv->status &= ~MISSING_PV;
				/* tell vgextend restoremissing that MISSING flag was cleared here */
				pvl->pv->unused_missing_cleared = 1;
			} else {
				log_warn("WARNING: VG %s was missing PV %s.", vg_name, dev_name(pvl->pv->dev));
				missing_pv_flag++;
			}
		}
	}

	if (missing_pv_dev || missing_pv_flag)
		vg_mark_partial_lvs(vg, 1);

	if (!check_pv_segments(vg)) {
		log_error(INTERNAL_ERROR "PV segments corrupted in %s.", vg->name);
		failure |= FAILED_INTERNAL_ERROR;
		goto_bad;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!check_lv_segments(lvl->lv, 0)) {
			log_error(INTERNAL_ERROR "LV segments corrupted in %s.", lvl->lv->name);
			failure |= FAILED_INTERNAL_ERROR;
			goto_bad;
		}
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		/* Checks that cross-reference other LVs. */
		if (!check_lv_segments(lvl->lv, 1)) {
			log_error(INTERNAL_ERROR "LV segments corrupted in %s.", lvl->lv->name);
			failure |= FAILED_INTERNAL_ERROR;
			goto_bad;
		}
	}

	if (!check_pv_dev_sizes(vg))
		log_warn("WARNING: One or more devices used as PVs in VG %s have changed sizes.", vg->name);

	_check_devs_used_correspond_with_vg(vg);

	if (!_access_vg_lock_type(cmd, vg, lockd_state, &failure)) {
		/* Either FAILED_LOCK_TYPE or FAILED_LOCK_MODE were set. */
		goto_bad;
	}

	if (!_access_vg_systemid(cmd, vg)) {
		failure |= FAILED_SYSTEMID;
		goto_bad;
	}

	if (!_access_vg_clustered(cmd, vg)) {
		failure |= FAILED_CLUSTERED;
		goto_bad;
	}

	if (writing && !(read_flags & READ_ALLOW_EXPORTED) && vg_is_exported(vg)) {
		log_error("Volume group %s is exported", vg->name);
		failure |= FAILED_EXPORTED;
		goto_bad;
	}

	if (writing && !(vg->status & LVM_WRITE)) {
		log_error("Volume group %s is read-only", vg->name);
		failure |= FAILED_READ_ONLY;
		goto_bad;
	}

	if (!cmd->handles_missing_pvs && (missing_pv_dev || missing_pv_flag) && writing) {
		log_error("Cannot change VG %s while PVs are missing.", vg->name);
		log_error("See vgreduce --removemissing and vgextend --restoremissing.");
		failure |= FAILED_NOT_ENABLED;
		goto_bad;
	}

	if (!cmd->handles_unknown_segments && vg_has_unknown_segments(vg) && writing) {
		log_error("Cannot change VG %s with unknown segments in it!", vg->name);
		failure |= FAILED_NOT_ENABLED; /* FIXME new failure code here? */
		goto_bad;
	}

	/*
	 * When we are reading the VG with the intention of writing it,
	 * we save a second copy of the VG in vg->vg_committed.  This
	 * copy remains unmodified by the command operation, and is used
	 * later if there is an error and we want to reactivate LVs.
	 * FIXME: be specific about exactly when this works correctly.
	 */
	if (writing) {
		struct dm_config_tree *cft;

		if (dm_pool_locked(vg->vgmem)) {
			/* FIXME: can this happen? */
			log_warn("WARNING: vg_read no vg copy: pool locked");
			goto out;
		}

		if (vg->vg_committed) {
			/* FIXME: can this happen? */
			log_warn("WARNING: vg_read no vg copy: copy exists");
			release_vg(vg->vg_committed);
			vg->vg_committed = NULL;
		}

		if (vg->vg_precommitted) {
			/* FIXME: can this happen? */
			log_warn("WARNING: vg_read no vg copy: pre copy exists");
			release_vg(vg->vg_precommitted);
			vg->vg_precommitted = NULL;
		}

		if (!(cft = export_vg_to_config_tree(vg))) {
			log_warn("WARNING: vg_read no vg copy: copy export failed");
			goto out;
		}

		if (!(vg->vg_committed = import_vg_from_config_tree(cft, vg->fid)))
			log_warn("WARNING: vg_read no vg copy: copy import failed");

		dm_config_destroy(cft);
	} else {
		if (vg->vg_precommitted)
			log_error(INTERNAL_ERROR "vg_read vg %p vg_precommitted %p", vg, vg->vg_precommitted);
		if (vg->vg_committed)
			log_error(INTERNAL_ERROR "vg_read vg %p vg_committed %p", vg, vg->vg_committed);
	}
out:
	/* We return with the VG lock held when read is successful. */
	*error_flags = SUCCESS;
	if (error_vg)
		*error_vg = NULL;
	return vg;

bad:
	*error_flags = failure;

	/*
	 * FIXME: get rid of this case so we don't have to return the vg when
	 * there's an error.  It is here for process_each_pv() which wants to
	 * eliminate the VG's devs from the list of devs it is processing, even
	 * when it can't access the VG because of wrong system id or similar.
	 * This could be done by looking at lvmcache info structs intead of 'vg'.
	 * It's also used by process_each_vg/process_each_lv which want to
	 * include error_vg values (like system_id) in error messages.
	 * These values could also be found from lvmcache vginfo.
	 */
	if (error_vg && vg) {
		if (vg->vg_precommitted)
			log_error(INTERNAL_ERROR "vg_read vg %p vg_precommitted %p", vg, vg->vg_precommitted);
		if (vg->vg_committed)
			log_error(INTERNAL_ERROR "vg_read vg %p vg_committed %p", vg, vg->vg_committed);

		/* caller must unlock_vg and release_vg */
		*error_vg = vg;
		return_NULL;
	}

	if (vg) {
		unlock_vg(cmd, vg, vg_name);
		release_vg(vg);
	}
	if (error_vg)
		*error_vg = NULL;
	return_NULL;
}

/*
 * Simply a version of vg_read() that automatically sets the READ_FOR_UPDATE
 * flag, which means the caller intends to write the VG after reading it,
 * so vg_read should acquire an exclusive file lock on the vg.
 */
struct volume_group *vg_read_for_update(struct cmd_context *cmd, const char *vg_name,
			 const char *vgid, uint32_t read_flags, uint32_t lockd_state)
{
	struct volume_group *vg;
	uint32_t error_flags = 0;

	vg = vg_read(cmd, vg_name, vgid, read_flags | READ_FOR_UPDATE, lockd_state, &error_flags, NULL);

	return vg;
}
