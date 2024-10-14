/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2010 Red Hat, Inc. All rights reserved.
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
#include "lib/metadata/lv_alloc.h"
#include "lib/metadata/segtype.h"
#include "lib/metadata/pv_alloc.h"
#include "lib/display/display.h"
#include "lib/activate/activate.h"
#include "lib/commands/toolcontext.h"
#include "lib/format_text/archiver.h"
#include "lib/datastruct/str_list.h"
#include "lib/cache/lvmcache.h"

struct volume_group *alloc_vg(const char *pool_name, struct cmd_context *cmd,
			      const char *vg_name)
{
	struct dm_pool *vgmem;
	struct volume_group *vg;

	if (!(vgmem = dm_pool_create(pool_name, VG_MEMPOOL_CHUNK)) ||
	    !(vg = dm_pool_zalloc(vgmem, sizeof(*vg)))) {
		log_error("Failed to allocate volume group structure");
		if (vgmem)
			dm_pool_destroy(vgmem);
		return NULL;
	}

	if (vg_name && !(vg->name = dm_pool_strdup(vgmem, vg_name))) {
		log_error("Failed to allocate VG name.");
		dm_pool_destroy(vgmem);
		return NULL;
	}

	vg->system_id = "";

	vg->cmd = cmd;
	vg->vgmem = vgmem;
	vg->alloc = ALLOC_NORMAL;

	dm_list_init(&vg->pvs);
	dm_list_init(&vg->pv_write_list);
	dm_list_init(&vg->lvs);
	dm_list_init(&vg->historical_lvs);
	dm_list_init(&vg->tags);
	dm_list_init(&vg->removed_lvs);
	dm_list_init(&vg->removed_historical_lvs);
	dm_list_init(&vg->removed_pvs);
	dm_list_init(&vg->msg_list);

	log_debug_mem("Allocated VG %s at %p.", vg->name ? : "<no name>", (void *)vg);

	return vg;
}

static void _free_vg(struct volume_group *vg)
{
	vg_set_fid(vg, NULL);

	if (vg->cmd && vg->vgmem == vg->cmd->mem) {
		log_error(INTERNAL_ERROR "global memory pool used for VG %s",
			  vg->name);
		return;
	}

	log_debug_mem("Freeing VG %s at %p.", vg->name ? : "<no name>", (void *)vg);

	if (vg->committed_cft) {
		config_destroy(vg->committed_cft);
		lvmcache_forget_cft(vg->name, &vg->id);
	}
	dm_pool_destroy(vg->vgmem);
}

void release_vg(struct volume_group *vg)
{
	if (!vg || is_orphan_vg(vg->name))
		return;

	release_vg(vg->vg_committed);
	release_vg(vg->vg_precommitted);
	_free_vg(vg);
}

/*
 * FIXME out of place, but the main (cmd) pool has been already
 * destroyed and touching the fid (also via release_vg) will crash the
 * program
 *
 * For now quick wrapper to allow destroy of orphan vg
 */
void free_orphan_vg(struct volume_group *vg)
{
	_free_vg(vg);
}

int link_lv_to_vg(struct volume_group *vg, struct logical_volume *lv)
{
	struct lv_list *lvl;

	if (vg_max_lv_reached(vg))
		stack;

	if (!(lvl = dm_pool_zalloc(vg->vgmem, sizeof(*lvl))))
		return_0;

	lvl->lv = lv;
	lv->vg = vg;
	dm_list_add(&vg->lvs, &lvl->list);
	lv->status &= ~LV_REMOVED;

	return 1;
}

int unlink_lv_from_vg(struct logical_volume *lv)
{
	struct lv_list *lvl;

	if (!(lvl = find_lv_in_vg(lv->vg, lv->name)))
		return_0;

	dm_list_move(&lv->vg->removed_lvs, &lvl->list);
	lv->status |= LV_REMOVED;

	return 1;
}

int vg_max_lv_reached(struct volume_group *vg)
{
	if (!vg->max_lv)
		return 0;

	if (vg->max_lv > vg_visible_lvs(vg))
		return 0;

	log_verbose("Maximum number of logical volumes (%u) reached "
		    "in volume group %s", vg->max_lv, vg->name);

	return 1;
}

char *vg_fmt_dup(const struct volume_group *vg)
{
	if (!vg->fid || !vg->fid->fmt)
		return NULL;
	return dm_pool_strdup(vg->vgmem, vg->fid->fmt->name);
}

char *vg_name_dup(const struct volume_group *vg)
{
	return dm_pool_strdup(vg->vgmem, vg->name);
}

char *vg_system_id_dup(const struct volume_group *vg)
{
	return dm_pool_strdup(vg->vgmem, vg->system_id ? : "");
}

char *vg_lock_type_dup(const struct volume_group *vg)
{
	return dm_pool_strdup(vg->vgmem, vg->lock_type ? : vg->lock_type ? : "");
}

char *vg_lock_args_dup(const struct volume_group *vg)
{
	return dm_pool_strdup(vg->vgmem, vg->lock_args ? : vg->lock_args ? : "");
}

char *vg_uuid_dup(const struct volume_group *vg)
{
	return id_format_and_copy(vg->vgmem, &vg->id);
}

char *vg_tags_dup(const struct volume_group *vg)
{
	return tags_format_and_copy(vg->vgmem, &vg->tags);
}

uint32_t vg_seqno(const struct volume_group *vg)
{
	return vg->seqno;
}

uint64_t vg_status(const struct volume_group *vg)
{
	return vg->status;
}

uint64_t vg_size(const struct volume_group *vg)
{
	return (uint64_t) vg->extent_count * vg->extent_size;
}

uint64_t vg_free(const struct volume_group *vg)
{
	return (uint64_t) vg->free_count * vg->extent_size;
}

uint64_t vg_extent_size(const struct volume_group *vg)
{
	return (uint64_t) vg->extent_size;
}

uint64_t vg_extent_count(const struct volume_group *vg)
{
	return (uint64_t) vg->extent_count;
}

uint64_t vg_free_count(const struct volume_group *vg)
{
	return (uint64_t) vg->free_count;
}

uint64_t vg_pv_count(const struct volume_group *vg)
{
	return (uint64_t) vg->pv_count;
}

uint64_t vg_max_pv(const struct volume_group *vg)
{
	return (uint64_t) vg->max_pv;
}

uint64_t vg_max_lv(const struct volume_group *vg)
{
	return (uint64_t) vg->max_lv;
}

unsigned snapshot_count(const struct volume_group *vg)
{
	struct lv_list *lvl;
	unsigned num_snapshots = 0;

	dm_list_iterate_items(lvl, &vg->lvs)
		if (lv_is_cow(lvl->lv))
			num_snapshots++;

	return num_snapshots;
}

unsigned vg_visible_lvs(const struct volume_group *vg)
{
	struct lv_list *lvl;
	unsigned lv_count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (lv_is_visible(lvl->lv))
			lv_count++;
	}

	return lv_count;
}

uint32_t vg_mda_count(const struct volume_group *vg)
{
	return dm_list_size(&vg->fid->metadata_areas_in_use) +
		dm_list_size(&vg->fid->metadata_areas_ignored);
}

uint32_t vg_mda_used_count(const struct volume_group *vg)
{
       uint32_t used_count = 0;
       struct metadata_area *mda;

	/*
	 * Ignored mdas could be on either list - the reason being the state
	 * may have changed from ignored to un-ignored and we need to write
	 * the state to disk.
	 */
       dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use)
	       if (!mda_is_ignored(mda))
		       used_count++;

       return used_count;
}

uint32_t vg_mda_copies(const struct volume_group *vg)
{
	return vg->mda_copies;
}

uint64_t vg_mda_size(const struct volume_group *vg)
{
	return find_min_mda_size(&vg->fid->metadata_areas_in_use);
}

uint64_t vg_mda_free(const struct volume_group *vg)
{
	uint64_t freespace = UINT64_MAX, mda_free;
	struct metadata_area *mda;

	dm_list_iterate_items(mda, &vg->fid->metadata_areas_in_use) {
		if (!mda->ops->mda_free_sectors)
			continue;
		mda_free = mda->ops->mda_free_sectors(mda);
		if (mda_free < freespace)
			freespace = mda_free;
	}

	if (freespace == UINT64_MAX)
		freespace = UINT64_C(0);
	return freespace;
}

int vg_set_mda_copies(struct volume_group *vg, uint32_t mda_copies)
{
	vg->mda_copies = mda_copies;

	/* FIXME Use log_verbose when this is due to specific cmdline request. */
	log_debug_metadata("Setting mda_copies to %"PRIu32" for VG %s",
			   mda_copies, vg->name);

	return 1;
}

char *vg_profile_dup(const struct volume_group *vg)
{
	const char *profile_name = vg->profile ? vg->profile->name : "";
	return dm_pool_strdup(vg->vgmem, profile_name);
}

static int _recalc_extents(uint32_t *extents, const char *desc1,
			   const char *desc2, uint32_t old_extent_size,
			   uint32_t new_extent_size)
{
	uint64_t size = (uint64_t) old_extent_size * (*extents);

	if (size % new_extent_size) {
		log_error("New size %" PRIu64 " for %s%s not an exact number "
			  "of new extents.", size, desc1, desc2);
		return 0;
	}

	size /= new_extent_size;

	if (size > MAX_EXTENT_COUNT) {
		log_error("New extent count %" PRIu64 " for %s%s exceeds "
			  "32 bits.", size, desc1, desc2);
		return 0;
	}

	*extents = (uint32_t) size;

	return 1;
}

int vg_check_new_extent_size(const struct format_type *fmt, uint32_t new_extent_size)
{
	if (!new_extent_size) {
		log_error("Physical extent size may not be zero");
		return 0;
	}

	if ((fmt->features & FMT_NON_POWER2_EXTENTS)) {
		if (!is_power_of_2(new_extent_size) &&
		    (new_extent_size % MIN_NON_POWER2_EXTENT_SIZE)) {
			log_error("Physical Extent size must be a multiple of %s when not a power of 2.",
				  display_size(fmt->cmd, (uint64_t) MIN_NON_POWER2_EXTENT_SIZE));
			return 0;
		}
		return 1;
	}

	/* Apply original format1 restrictions */
	if (!is_power_of_2(new_extent_size)) {
		log_error("Metadata format only supports Physical Extent sizes that are powers of 2.");
		return 0;
	}

	if (new_extent_size > MAX_PE_SIZE || new_extent_size < MIN_PE_SIZE) {
		log_error("Extent size must be between %s and %s",
			  display_size(fmt->cmd, (uint64_t) MIN_PE_SIZE),
			  display_size(fmt->cmd, (uint64_t) MAX_PE_SIZE));
		return 0;
	}

	if (new_extent_size % MIN_PE_SIZE) {
		log_error("Extent size must be multiple of %s",
			  display_size(fmt->cmd, (uint64_t) MIN_PE_SIZE));
		return 0;
	}

 	return 1;
}

int vg_set_extent_size(struct volume_group *vg, uint32_t new_extent_size)
{
	uint32_t old_extent_size = vg->extent_size;
	struct pv_list *pvl;
	struct lv_list *lvl;
	struct physical_volume *pv;
	struct logical_volume *lv;
	struct lv_segment *seg;
	struct pv_segment *pvseg;
	uint32_t s;

	if (!vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change PE size", vg->name);
		return 0;
	}

	if (new_extent_size == vg->extent_size)
		return 1;

	if (!vg_check_new_extent_size(vg->fid->fmt, new_extent_size))
		return_0;

	if (new_extent_size > vg->extent_size) {
		if ((uint64_t) vg_size(vg) % new_extent_size) {
			/* FIXME Adjust used PV sizes instead */
			log_error("New extent size is not a perfect fit");
			return 0;
		}
	}

	vg->extent_size = new_extent_size;

	if (vg->fid->fmt->ops->vg_setup &&
	    !vg->fid->fmt->ops->vg_setup(vg->fid, vg))
		return_0;

	if (!_recalc_extents(&vg->extent_count, vg->name, "", old_extent_size,
			     new_extent_size))
		return_0;

	if (!_recalc_extents(&vg->free_count, vg->name, " free space",
			     old_extent_size, new_extent_size))
		return_0;

	/* foreach PV */
	dm_list_iterate_items(pvl, &vg->pvs) {
		pv = pvl->pv;

		pv->pe_size = new_extent_size;
		if (!_recalc_extents(&pv->pe_count, pv_dev_name(pv), "",
				     old_extent_size, new_extent_size))
			return_0;

		if (!_recalc_extents(&pv->pe_alloc_count, pv_dev_name(pv),
				     " allocated space", old_extent_size, new_extent_size))
			return_0;

		/* foreach free PV Segment */
		dm_list_iterate_items(pvseg, &pv->segments) {
			if (pvseg_is_allocated(pvseg))
				continue;

			if (!_recalc_extents(&pvseg->pe, pv_dev_name(pv),
					     " PV segment start", old_extent_size,
					     new_extent_size))
				return_0;
			if (!_recalc_extents(&pvseg->len, pv_dev_name(pv),
					     " PV segment length", old_extent_size,
					     new_extent_size))
				return_0;
		}
	}

	/* foreach LV */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!_recalc_extents(&lv->le_count, lv->name, "", old_extent_size,
				     new_extent_size))
			return_0;

		dm_list_iterate_items(seg, &lv->segments) {
			if (!_recalc_extents(&seg->le, lv->name,
					     " segment start", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->len, lv->name,
					     " segment length", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->area_len, lv->name,
					     " area length", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->extents_copied, lv->name,
					     " extents moved", old_extent_size,
					     new_extent_size))
				return_0;

			if (!_recalc_extents(&seg->vdo_pool_virtual_extents, lv->name,
					     " virtual extents", old_extent_size,
					     new_extent_size))
				return_0;

			/* foreach area */
			for (s = 0; s < seg->area_count; s++) {
				switch (seg_type(seg, s)) {
				case AREA_PV:
					if (!_recalc_extents
					    (&seg_pe(seg, s),
					     lv->name,
					     " pvseg start", old_extent_size,
					     new_extent_size))
						return_0;
					if (!_recalc_extents
					    (&seg_pvseg(seg, s)->len,
					     lv->name,
					     " pvseg length", old_extent_size,
					     new_extent_size))
						return_0;
					break;
				case AREA_LV:
					if (!_recalc_extents
					    (&seg_le(seg, s), lv->name,
					     " area start", old_extent_size,
					     new_extent_size))
						return_0;
					break;
				case AREA_UNASSIGNED:
					log_error("Unassigned area %u found in "
						  "segment", s);
					return 0;
				}
			}
		}

	}

	return 1;
}

int vg_set_max_lv(struct volume_group *vg, uint32_t max_lv)
{
	if (!vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change MaxLogicalVolume", vg->name);
		return 0;
	}

	if (!(vg->fid->fmt->features & FMT_UNLIMITED_VOLS)) {
		if (!max_lv)
			max_lv = 255;
		else if (max_lv > 255) {
			log_error("MaxLogicalVolume limit is 255");
			return 0;
		}
	}

	if (max_lv && max_lv < vg_visible_lvs(vg)) {
		log_error("MaxLogicalVolume is less than the current number "
			  "%d of LVs for %s", vg_visible_lvs(vg),
			  vg->name);
		return 0;
	}
	vg->max_lv = max_lv;

	return 1;
}

int vg_set_max_pv(struct volume_group *vg, uint32_t max_pv)
{
	if (!vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" must be resizeable "
			  "to change MaxPhysicalVolumes", vg->name);
		return 0;
	}

	if (!(vg->fid->fmt->features & FMT_UNLIMITED_VOLS)) {
		if (!max_pv)
			max_pv = 255;
		else if (max_pv > 255) {
			log_error("MaxPhysicalVolume limit is 255");
			return 0;
		}
	}

	if (max_pv && max_pv < vg->pv_count) {
		log_error("MaxPhysicalVolumes is less than the current number "
			  "%d of PVs for \"%s\"", vg->pv_count,
			  vg->name);
		return 0;
	}
	vg->max_pv = max_pv;
	return 1;
}

int vg_set_alloc_policy(struct volume_group *vg, alloc_policy_t alloc)
{
	if (alloc == ALLOC_INHERIT) {
		log_error("Volume Group allocation policy cannot inherit "
			  "from anything");
		return 0;
	}

	if (alloc == vg->alloc)
		return 1;

	vg->alloc = alloc;
	return 1;
}

/* The input string has already been validated. */

int vg_set_system_id(struct volume_group *vg, const char *system_id)
{
	if (!system_id || !*system_id) {
		vg->system_id = NULL;
		return 1;
	}

	if (!(vg->system_id = dm_pool_strdup(vg->vgmem, system_id))) {
		log_error("Failed to allocate memory for system_id in vg_set_system_id.");
		return 0;
	}

	return 1;
}

int vg_set_lock_type(struct volume_group *vg, const char *lock_type)
{
	if (!lock_type)
		lock_type = "none";

	if (!(vg->lock_type = dm_pool_strdup(vg->vgmem, lock_type))) {
		log_error("vg_set_lock_type %s no mem", lock_type);
		return 0;
	}

	return 1;
}

char *vg_attr_dup(struct dm_pool *mem, const struct volume_group *vg)
{
	char *repstr;

	if (!(repstr = dm_pool_zalloc(mem, 7))) {
		log_error("dm_pool_alloc failed");
		return NULL;
	}

	repstr[0] = (vg->status & LVM_WRITE) ? 'w' : 'r';
	repstr[1] = (vg_is_resizeable(vg)) ? 'z' : '-';
	repstr[2] = (vg_is_exported(vg)) ? 'x' : '-';
	repstr[3] = (vg_missing_pv_count(vg)) ? 'p' : '-';
	repstr[4] = alloc_policy_char(vg->alloc);

	if (vg_is_clustered(vg))
		repstr[5] = 'c';
	else if (vg_is_shared(vg))
		repstr[5] = 's';
	else
		repstr[5] = '-';

	return repstr;
}

int vgreduce_single(struct cmd_context *cmd, struct volume_group *vg,
			    struct physical_volume *pv, int commit)
{
	struct pv_list *pvl;
	struct volume_group *orphan_vg = NULL;
	int r = 0;
	const char *name = pv_dev_name(pv);

	if (!vg) {
		log_error(INTERNAL_ERROR "VG is NULL.");
		return r;
	}

	if (!pv->dev || dm_list_empty(&pv->dev->aliases)) {
		log_error("No device found for PV.");
		return r;
	}

	log_debug("vgreduce_single VG %s PV %s", vg->name, pv_dev_name(pv));

	if (pv_pe_alloc_count(pv)) {
		log_error("Physical volume \"%s\" still in use", name);
		return r;
	}

	if (vg->pv_count == 1) {
		log_error("Can't remove final physical volume \"%s\" from "
			  "volume group \"%s\"", name, vg->name);
		return r;
	}

	pvl = find_pv_in_vg(vg, name);

	log_verbose("Removing \"%s\" from volume group \"%s\"", name, vg->name);

	if (pvl)
		del_pvl_from_vgs(vg, pvl);

	pv->vg_name = vg->fid->fmt->orphan_vg_name;
	pv->status = ALLOCATABLE_PV;

	if (!dev_get_size(pv_dev(pv), &pv->size)) {
		log_error("%s: Couldn't get size.", pv_dev_name(pv));
		goto bad;
	}

	vg->free_count -= pv_pe_count(pv) - pv_pe_alloc_count(pv);
	vg->extent_count -= pv_pe_count(pv);

	/* FIXME: we don't need to vg_read the orphan vg here */
	orphan_vg = vg_read_orphans(cmd, vg->fid->fmt->orphan_vg_name);

	if (!orphan_vg)
		goto bad;

	if (!vg_split_mdas(cmd, vg, orphan_vg) || !vg->pv_count) {
		log_error("Cannot remove final metadata area on \"%s\" from \"%s\"",
			  name, vg->name);
		goto bad;
	}

	/*
	 * Only write out the needed changes if so requested by caller.
	 */
	if (commit) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Removal of physical volume \"%s\" from "
				  "\"%s\" failed", name, vg->name);
			goto bad;
		}

		if (!pv_write(cmd, pv, 0)) {
			log_error("Failed to clear metadata from physical "
				  "volume \"%s\" "
				  "after removal from \"%s\"", name, vg->name);
			goto bad;
		}

		log_print_unless_silent("Removed \"%s\" from volume group \"%s\"",
				name, vg->name);
	}
	r = 1;
bad:
	/* If we are committing here or we had an error then we will free fid */
	if (pvl && (commit || r != 1))
		free_pv_fid(pvl->pv);
	release_vg(orphan_vg);
	return r;
}

void vg_backup_if_needed(struct volume_group *vg)
{
	if (!vg || !vg->needs_backup)
		return;

	vg->needs_backup = 0;
	backup(vg->vg_committed);
}

void insert_segment(struct logical_volume *lv, struct lv_segment *seg)
{                                    
	struct lv_segment *comp;
 
	dm_list_iterate_items(comp, &lv->segments) {
		if (comp->le > seg->le) {
			dm_list_add(&comp->list, &seg->list);
			return;
		}
	}
 
	lv->le_count += seg->len;
	dm_list_add(&lv->segments, &seg->list);
}

struct logical_volume *get_data_from_pool(struct logical_volume *pool_lv)
{
	/* works for cache pool, thin pool, vdo pool */
	/* first_seg() = dm_list_first_entry(&lv->segments) */
	/* seg_lv(seg, n) = seg->areas[n].u.lv.lv */
	return seg_lv(first_seg(pool_lv), 0);
}

struct logical_volume *get_meta_from_pool(struct logical_volume *pool_lv)
{
	/* works for cache pool, thin pool, vdo pool */
	/* first_seg() = dm_list_first_entry(&lv->segments) */
	/* seg_lv(seg, n) = seg->areas[n].u.lv.lv */
	return first_seg(pool_lv)->metadata_lv;
}

struct logical_volume *get_pool_from_thin(struct logical_volume *thin_lv)
{
	return first_seg(thin_lv)->pool_lv;
}

struct logical_volume *get_pool_from_cache(struct logical_volume *cache_lv)
{
	return first_seg(cache_lv)->pool_lv;
}

struct logical_volume *get_pool_from_vdo(struct logical_volume *vdo_lv)
{
	return seg_lv(first_seg(vdo_lv), 0);
}

struct logical_volume *get_origin_from_cache(struct logical_volume *cache_lv)
{
	return seg_lv(first_seg(cache_lv), 0);
}

struct logical_volume *get_origin_from_writecache(struct logical_volume *writecache_lv)
{
	return seg_lv(first_seg(writecache_lv), 0);
}

struct logical_volume *get_origin_from_integrity(struct logical_volume *integrity_lv)
{
	return seg_lv(first_seg(integrity_lv), 0);
}

struct logical_volume *get_origin_from_thin(struct logical_volume *thin_lv)
{
	return first_seg(thin_lv)->origin;
}

struct logical_volume *get_merge_lv_from_thin(struct logical_volume *thin_lv)
{
	return first_seg(thin_lv)->merge_lv;
}

struct logical_volume *get_external_lv_from_thin(struct logical_volume *thin_lv)
{
	return first_seg(thin_lv)->external_lv;
}

struct logical_volume *get_origin_from_snap(struct logical_volume *snap_lv)
{
	return first_seg(snap_lv)->origin;
}

struct logical_volume *get_cow_from_snap(struct logical_volume *snap_lv)
{
	return first_seg(snap_lv)->cow;
}

struct logical_volume *get_fast_from_writecache(struct logical_volume *writecache_lv)
{
	return first_seg(writecache_lv)->writecache;
}

/*
 * When reading from text:
 * - pv comes from looking up the "pv0" key in pv_hash
 * - pe comes from text field
 * - pv and pe are passed to set_lv_segment_area_pv() to
 *   create the pv_segment structs, and connect them to
 *   the lv_segment.
 *
 * When copying the struct:
 * - pv comes from looking up the pv id in vg->pvs
 * - pe comes from the original pvseg struct
 * - pv and pe are passed to set_lv_segment_area_pv() to
 *   create the pv_segment structs, and connect them to
 *   the lv_segment (same as when reading from text.)
 * 
 * set_lv_segment_area_pv(struct lv_segment *seg, uint32_t s,
 *                        struct physical_volume *pv, uint32_t pe);
 * does:
 *
 * seg_pvseg(seg, s) =
 * 	assign_peg_to_lvseg(pv, pe, seg->area_len, seg, s);
 *
 * does:
 *
 * seg->areas[s].u.pv.pvseg =
 * 	assign_peg_to_lvseg(pv, pe, area_len, seg, s);
 *
 * struct pv_segment *assign_peg_to_lvseg(struct physical_volume *pv,
 * 				uint32_t pe, uint32_t area_len,
 * 				struct lv_segment *seg, uint32_t s);
 *
 * This does multiple things:
 * 1. creates pv_segment and connects it to lv_segment
 * 2. creates pv->segments list of all pv_segments on the pv
 * 3. updates pv->pe_alloc_count, vg->free_count
 */

static int _areas_copy_struct(struct volume_group *vg,
		      struct logical_volume *lv,
		      struct lv_segment *seg,
		      struct volume_group *vgo,
		      struct logical_volume *lvo,
		      struct lv_segment *sego,
		      struct dm_hash_table *pv_hash,
		      struct dm_hash_table *lv_hash)
{
	uint32_t s;

	/* text_import_areas */

	for (s = 0; s < sego->area_count; s++) {
		seg->areas[s].type = sego->areas[s].type;

		if (sego->areas[s].type == AREA_PV) {
			struct physical_volume *area_pvo;
			struct physical_volume *area_pv;

			if (!(area_pvo = sego->areas[s].u.pv.pvseg->pv))
				goto_bad;
			if (!(area_pv = dm_hash_lookup_binary(pv_hash, &area_pvo->id, ID_LEN)))
				goto_bad;
			if (!set_lv_segment_area_pv(seg, s, area_pv, sego->areas[s].u.pv.pvseg->pe))
				goto_bad;

		} else if (sego->areas[s].type == AREA_LV) {
			struct logical_volume *area_lvo;
			struct logical_volume *area_lv;

			if (!(area_lvo = sego->areas[s].u.lv.lv))
				goto_bad;
			if (!(area_lv = dm_hash_lookup(lv_hash, area_lvo->name)))
				goto_bad;
			if (!set_lv_segment_area_lv(seg, s, area_lv, sego->areas[s].u.lv.le, 0))
				goto_bad;
		}
	}

	return 1;
bad:
	return 0;
}

static int _thin_messages_copy_struct(struct volume_group *vgo, struct volume_group *vg,
			      struct logical_volume *lvo, struct logical_volume *lv,
			      struct lv_segment *sego, struct lv_segment *seg,
			      struct dm_hash_table *lv_hash)
{
	struct lv_thin_message *mso;
	struct lv_thin_message *ms;
	struct logical_volume *ms_lvo;
	struct logical_volume *ms_lv;

        if (dm_list_empty(&sego->thin_messages))
		return 1;

	dm_list_iterate_items(mso, &sego->thin_messages) {
		if (!(ms = dm_pool_alloc(vg->vgmem, sizeof(*ms))))
			goto_bad;

		ms->type = mso->type;

		switch (ms->type) {
		case DM_THIN_MESSAGE_CREATE_SNAP:
		case DM_THIN_MESSAGE_CREATE_THIN:
			if (!(ms_lvo = mso->u.lv))
				goto_bad;
			if (!(ms_lv = dm_hash_lookup(lv_hash, ms_lvo->name)))
				goto_bad;
			ms->u.lv = ms_lv;
			break;
		case DM_THIN_MESSAGE_DELETE:
			ms->u.delete_id = mso->u.delete_id;
                        break;
                default:
                        break;
                }

		dm_list_add(&seg->thin_messages, &ms->list);
	}
	return 1;
bad:
	return 0;
}

static struct lv_segment *_seg_copy_struct(struct volume_group *vg,
				   struct logical_volume *lv,
				   struct volume_group *vgo,
				   struct logical_volume *lvo,
				   struct lv_segment *sego,
				   struct dm_hash_table *pv_hash,
				   struct dm_hash_table *lv_hash)
{
	struct dm_pool *mem = vg->vgmem;
	struct lv_segment *seg;
	uint32_t s;

	if (!(seg = dm_pool_zalloc(mem, sizeof(*seg))))
		return_NULL;

	if (sego->area_count && sego->areas &&
	    !(seg->areas = dm_pool_zalloc(mem, sego->area_count * sizeof(*seg->areas))))
		return_NULL;

	/*
	 * This is a more accurate copy of the original segment:
	 * if (sego->area_count && sego->meta_areas &&
	 *     !(seg->meta_areas = dm_pool_zalloc(mem, sego->area_count * sizeof(*seg->meta_areas))))
	 *         return_NULL;
	 *
	 * But it causes a segfault in for_each_sub_lv, which seems to want meta_areas allocated
	 * in the copy even when it's null in the original.  So, this copies alloc_lv_segment
	 * which always allocates meta_areas.
	 */
	if (segtype_is_raid_with_meta(sego->segtype)) {
		if (!(seg->meta_areas = dm_pool_zalloc(mem, sego->area_count * sizeof(*seg->meta_areas))))
			return_NULL;
	}

	/* see _read_segment, alloc_lv_segment */

	dm_list_init(&seg->tags);
	dm_list_init(&seg->origin_list);
	dm_list_init(&seg->thin_messages);

	seg->lv = lv;

	seg->segtype = sego->segtype;
	seg->le = sego->le;
	seg->len = sego->len;
	seg->status = sego->status;
	seg->area_count = sego->area_count;
	seg->area_len = sego->area_len;

	if (!dm_list_empty(&sego->tags) && !str_list_dup(mem, &seg->tags, &sego->tags))
		goto_bad;

	/*
	 * _read_segment, ->text_import(), i.e. _foo_text_import()
	 */

	if (seg_is_striped_target(sego)) {

		/* see _striped_text_import, N.B. not "seg_is_striped" */

		seg->stripe_size = sego->stripe_size;

		if (!_areas_copy_struct(vg, lv, seg, vgo, lvo, sego, pv_hash, lv_hash))
			goto_bad;

	} else if (seg_is_cache_pool(sego)) {
		struct logical_volume *data_lvo;
		struct logical_volume *meta_lvo;
		struct logical_volume *data_lv;
		struct logical_volume *meta_lv;

		/* see _cache_pool_text_import */

		seg->cache_metadata_format = sego->cache_metadata_format;
		seg->chunk_size = sego->chunk_size;
		seg->cache_mode = sego->cache_mode;

		if (sego->policy_name)
			seg->policy_name = dm_pool_strdup(mem, sego->policy_name);
		if (sego->policy_settings)
			seg->policy_settings = dm_config_clone_node_with_mem(mem, sego->policy_settings, 0);

		if (!(data_lvo = get_data_from_pool(lvo)))
			goto_bad;
		if (!(meta_lvo = get_meta_from_pool(lvo)))
			goto_bad;
		if (!(data_lv = dm_hash_lookup(lv_hash, data_lvo->name)))
			goto_bad;
		if (!(meta_lv = dm_hash_lookup(lv_hash, meta_lvo->name)))
			goto_bad;
		if (!attach_pool_data_lv(seg, data_lv))
			goto_bad;
		if (!attach_pool_metadata_lv(seg, meta_lv))
			goto_bad;

	} else if (seg_is_cache(sego)) {
		struct logical_volume *pool_lvo;
		struct logical_volume *origin_lvo;
		struct logical_volume *pool_lv;
		struct logical_volume *origin_lv;
		
		/* see _cache_text_import */

		seg->cache_metadata_format = sego->cache_metadata_format;
		seg->chunk_size = sego->chunk_size;
		seg->cache_mode = sego->cache_mode;

		if (sego->policy_name)
			seg->policy_name = dm_pool_strdup(mem, sego->policy_name);
		if (sego->policy_settings)
			seg->policy_settings = dm_config_clone_node_with_mem(mem, sego->policy_settings, 0);

		seg->cleaner_policy = sego->cleaner_policy;
		seg->metadata_start = sego->metadata_start;
		seg->metadata_len = sego->metadata_len;
		seg->data_start = sego->data_start;
		seg->data_len = sego->data_len;

		if (sego->metadata_id) {
			if (!(seg->metadata_id = dm_pool_zalloc(mem, sizeof(struct id))))
				goto_bad;
			memcpy(seg->metadata_id, sego->metadata_id, sizeof(struct id));
		}
		if (sego->data_id) {
			if (!(seg->data_id = dm_pool_zalloc(mem, sizeof(struct id))))
				goto_bad;
			memcpy(seg->data_id, sego->data_id, sizeof(struct id));
		}

		if (!(pool_lvo = get_pool_from_cache(lvo)))
			goto_bad;
		if (!(origin_lvo = get_origin_from_cache(lvo)))
			goto_bad;
		if (!(pool_lv = dm_hash_lookup(lv_hash, pool_lvo->name)))
			goto_bad;
		if (!(origin_lv = dm_hash_lookup(lv_hash, origin_lvo->name)))
			goto_bad;
		if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
			goto_bad;
		if (!attach_pool_lv(seg, pool_lv, NULL, NULL, NULL))
			goto_bad;

	} else if (seg_is_integrity(sego)) {
		struct logical_volume *origin_lvo;
		struct logical_volume *origin_lv;
		struct logical_volume *meta_lvo;
		struct logical_volume *meta_lv;
		const char *hash;

		/* see _integrity_text_import */

		if (!(origin_lvo = get_origin_from_integrity(lvo)))
			goto_bad;
		if (!(origin_lv = dm_hash_lookup(lv_hash, origin_lvo->name)))
			goto_bad;
		if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
			goto_bad;
		seg->origin = origin_lv;

		if ((meta_lvo = sego->integrity_meta_dev)) {
			if (!(meta_lv = dm_hash_lookup(lv_hash, meta_lvo->name)))
				goto_bad;
			seg->integrity_meta_dev = meta_lv;
			if (!add_seg_to_segs_using_this_lv(meta_lv, seg))
				goto_bad;
		}

		seg->integrity_data_sectors = sego->integrity_data_sectors;
		seg->integrity_recalculate = sego->integrity_recalculate;

		memcpy(&seg->integrity_settings, &sego->integrity_settings, sizeof(seg->integrity_settings));

		if ((hash = sego->integrity_settings.internal_hash)) {
			if (!(seg->integrity_settings.internal_hash = dm_pool_strdup(mem, hash)))
				goto_bad;
		}

	} else if (seg_is_mirror(sego)) {
		struct logical_volume *log_lv;

		/* see _mirrored_text_import */

		seg->extents_copied = sego->extents_copied;
		seg->region_size = sego->region_size;

		if (sego->log_lv) {
			if (!(log_lv = dm_hash_lookup(lv_hash, sego->log_lv->name)))
				goto_bad;
			seg->log_lv = log_lv;
		}

		if (!_areas_copy_struct(vg, lv, seg, vgo, lvo, sego, pv_hash, lv_hash))
			goto_bad;

	} else if (seg_is_thin_pool(sego)) {
		struct logical_volume *data_lvo;
		struct logical_volume *meta_lvo;
		struct logical_volume *data_lv;
		struct logical_volume *meta_lv;

		/* see _thin_pool_text_import */

		if (!(data_lvo = get_data_from_pool(lvo)))
			goto_bad;
		if (!(meta_lvo = get_meta_from_pool(lvo)))
			goto_bad;
		if (!(data_lv = dm_hash_lookup(lv_hash, data_lvo->name)))
			goto_bad;
		if (!(meta_lv = dm_hash_lookup(lv_hash, meta_lvo->name)))
			goto_bad;
		if (!attach_pool_data_lv(seg, data_lv))
			goto_bad;
		if (!attach_pool_metadata_lv(seg, meta_lv))
			goto_bad;
		seg->transaction_id = sego->transaction_id;
		seg->chunk_size = sego->chunk_size;
		seg->discards = sego->discards;
		seg->zero_new_blocks = sego->zero_new_blocks;
		seg->crop_metadata = sego->crop_metadata;

		if (!_thin_messages_copy_struct(vgo, vg, lvo, lv, sego, seg, lv_hash))
			goto_bad;

	} else if (seg_is_thin_volume(sego)) {
		struct logical_volume *pool_lvo;
		struct logical_volume *origin_lvo;
		struct logical_volume *merge_lvo;
		struct logical_volume *external_lvo;
		struct logical_volume *pool_lv = NULL;
		struct logical_volume *origin_lv = NULL;
		struct logical_volume *merge_lv = NULL;
		struct logical_volume *external_lv = NULL;

		/* see _thin_text_import */

		if (!(pool_lvo = get_pool_from_thin(lvo)))
			goto_bad;
		if (!(pool_lv = dm_hash_lookup(lv_hash, pool_lvo->name)))
			goto_bad;

		if ((origin_lvo = get_origin_from_thin(lvo))) {
			if (!(origin_lv = dm_hash_lookup(lv_hash, origin_lvo->name)))
				goto_bad;
		}
		if ((merge_lvo = get_merge_lv_from_thin(lvo))) {
			if (!(merge_lv = dm_hash_lookup(lv_hash, merge_lvo->name)))
				goto_bad;
		}
		if ((external_lvo = get_external_lv_from_thin(lvo))) {
			if (!(external_lv = dm_hash_lookup(lv_hash, external_lvo->name)))
				goto_bad;
		}
		if (!attach_pool_lv(seg, pool_lv, origin_lv, NULL, merge_lv))
			goto_bad;
		if (!attach_thin_external_origin(seg, external_lv))
			goto_bad;

		seg->transaction_id = sego->transaction_id;
		seg->device_id = sego->device_id;

	} else if (seg_is_snapshot(sego)) {
		struct logical_volume *origin_lvo;
		struct logical_volume *cow_lvo;
		struct logical_volume *origin_lv;
		struct logical_volume *cow_lv;

		/* see _snap_text_import */

		if (!(origin_lvo = get_origin_from_snap(lvo)))
			goto_bad;
		if (!(cow_lvo = get_cow_from_snap(lvo)))
			goto_bad;
		if  (!(origin_lv = dm_hash_lookup(lv_hash, origin_lvo->name)))
			goto_bad;
		if  (!(cow_lv = dm_hash_lookup(lv_hash, cow_lvo->name)))
			goto_bad;

		init_snapshot_seg(seg, origin_lv, cow_lv, sego->chunk_size,
				  (sego->status & MERGING) ? 1 : 0);

	} else if (seg_is_writecache(sego)) {
		struct logical_volume *origin_lvo;
		struct logical_volume *fast_lvo;
		struct logical_volume *origin_lv;
		struct logical_volume *fast_lv;

		/* see _writecache_text_import */

		if (!(origin_lvo = get_origin_from_writecache(lvo)))
			goto_bad;
		if (!(fast_lvo = get_fast_from_writecache(lvo)))
			goto_bad;
		if  (!(origin_lv = dm_hash_lookup(lv_hash, origin_lvo->name)))
			goto_bad;
		if  (!(fast_lv = dm_hash_lookup(lv_hash, fast_lvo->name)))
			goto_bad;

		if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
			return_0;

		seg->writecache_block_size = sego->writecache_block_size;

		seg->origin = origin_lv;
		seg->writecache = fast_lv;

		if (!add_seg_to_segs_using_this_lv(fast_lv, seg))
			return_0;

		memcpy(&seg->writecache_settings, &sego->writecache_settings, sizeof(seg->writecache_settings));

		if (sego->writecache_settings.new_key &&
		    !(seg->writecache_settings.new_key = dm_pool_strdup(vg->vgmem, sego->writecache_settings.new_key)))
			goto_bad;
		if (sego->writecache_settings.new_val &&
		    !(seg->writecache_settings.new_val = dm_pool_strdup(vg->vgmem, sego->writecache_settings.new_val)))
			goto_bad;

	} else if (seg_is_raid(sego)) {
		struct logical_volume *area_lvo;
		struct logical_volume *area_lv;

		/* see _raid_text_import_areas */

		seg->region_size = sego->region_size;
		seg->stripe_size = sego->stripe_size;
		seg->data_copies = sego->data_copies;
		seg->writebehind = sego->writebehind;
		seg->min_recovery_rate = sego->min_recovery_rate;
		seg->max_recovery_rate = sego->max_recovery_rate;
		seg->data_offset = sego->data_offset;
		seg->reshape_len = sego->reshape_len;

		for (s = 0; s < sego->area_count; s++) {
			if (!(area_lvo = sego->areas[s].u.lv.lv))
				goto_bad;
			if (!(area_lv = dm_hash_lookup(lv_hash, area_lvo->name)))
				goto_bad;
			if (!set_lv_segment_area_lv(seg, s, area_lv, 0, RAID_IMAGE))
				goto_bad;
			if (!sego->meta_areas)
				continue;
			if (!(area_lvo = sego->meta_areas[s].u.lv.lv))
				continue;
			if (!(area_lv = dm_hash_lookup(lv_hash, area_lvo->name)))
				goto_bad;
			if (!set_lv_segment_area_lv(seg, s, area_lv, 0, RAID_META))
				goto_bad;
		}

	} else if (seg_is_vdo_pool(sego)) {
		struct logical_volume *data_lvo;
		struct logical_volume *data_lv;

		if (!(data_lvo = get_data_from_pool(lvo)))
			goto_bad;
		if (!(data_lv = dm_hash_lookup(lv_hash, data_lvo->name)))
			goto_bad;

		seg->vdo_pool_header_size = sego->vdo_pool_header_size;
		seg->vdo_pool_virtual_extents = sego->vdo_pool_virtual_extents;
		memcpy(&seg->vdo_params, &sego->vdo_params, sizeof(seg->vdo_params));

		if (!set_lv_segment_area_lv(seg, 0, data_lv, 0, LV_VDO_POOL_DATA))
			goto_bad;

	} else if (seg_is_vdo(sego)) {
		struct logical_volume *pool_lvo;
		struct logical_volume *pool_lv;
		uint32_t vdo_offset;

		if (!(pool_lvo = get_pool_from_vdo(lvo)))
			goto_bad;
		if (!(pool_lv = dm_hash_lookup(lv_hash, pool_lvo->name)))
			goto_bad;
		vdo_offset = sego->areas[0].u.lv.le; /* or seg_le(seg, 0)) */

		if (!set_lv_segment_area_lv(seg, 0, pool_lv, vdo_offset, LV_VDO_POOL))
			goto_bad;

	} else if (seg_is_zero(sego) || seg_is_error(sego)) {
		/* nothing to copy */

	} else {
		log_error("Missing copy for lv %s segtype %s.",
			   display_lvname(lvo), sego->segtype->name);
		goto bad;
	}

	return seg;

bad:
	return NULL;
}

/* _read_lvsegs, _read_segments, _read_segment, alloc_lv_segment, ->text_import */

static int _lvsegs_copy_struct(struct volume_group *vg,
		       struct logical_volume *lv,
		       struct volume_group *vgo,
		       struct logical_volume *lvo,
		       struct dm_hash_table *pv_hash,
		       struct dm_hash_table *lv_hash)
{
	struct lv_segment *sego;
	struct lv_segment *seg;

	/* see _read_segment */

	dm_list_iterate_items(sego, &lvo->segments) {
		/* see _read_segment */
		if (!(seg = _seg_copy_struct(vg, lv, vgo, lvo, sego, pv_hash, lv_hash)))
			goto_bad;

		/* last step in _read_segment */
		/* adds seg to lv->segments and sets lv->le_count */
		insert_segment(lv, seg);
	}

	return 1;
bad:
	return 0;
}

static struct logical_volume *_lv_copy_struct(struct volume_group *vg,
				      struct volume_group *vgo,
				      struct logical_volume *lvo,
				      struct dm_hash_table *pv_hash,
				      struct dm_hash_table *lv_hash)
{
	struct dm_pool *mem = vg->vgmem;
	struct logical_volume *lv;

	if (!(lv = alloc_lv(mem)))
		return NULL;
	if (!(lv->name = dm_pool_strdup(mem, lvo->name)))
		goto_bad;
	if (lvo->profile && !(lv->profile = add_profile(lvo->vg->cmd, lvo->profile->name, CONFIG_PROFILE_METADATA)))
		goto_bad;
	if (lvo->hostname && !(lv->hostname = dm_pool_strdup(mem, lvo->hostname)))
		goto_bad;
	if (lvo->lock_args && !(lv->lock_args = dm_pool_strdup(mem, lvo->lock_args)))
		goto_bad;
	if (!dm_list_empty(&lvo->tags) && !str_list_dup(mem, &lv->tags, &lvo->tags))
		goto_bad;

	memcpy(&lv->lvid, &lvo->lvid, sizeof(lvo->lvid));
	lv->vg = vg;
	lv->status = lvo->status;
	lv->alloc = lvo->alloc;
	lv->read_ahead = lvo->read_ahead;
	lv->major = lvo->major;
	lv->minor = lvo->minor;
	lv->size = lvo->size;
	/* lv->le_count = lvo->le_count; */ /* set by calls to insert_segment() */
	lv->origin_count = lvo->origin_count;
	lv->external_count = lvo->external_count;
	lv->timestamp = lvo->timestamp;

	if (!dm_hash_insert(lv_hash, lv->name, lv))
		goto_bad;
	return lv;
bad:
	return NULL;
}

/* _read_pv */

static struct physical_volume *_pv_copy_struct(struct volume_group *vg, struct volume_group *vgo,
				      struct physical_volume *pvo, struct dm_hash_table *pv_hash)
{
	struct dm_pool *mem = vg->vgmem;
	struct physical_volume *pv;

	if (!(pv = dm_pool_zalloc(mem, sizeof(*pv))))
		return_NULL;

	if (!(pv->vg_name = dm_pool_strdup(mem, vg->name)))
		goto_bad;
	pv->is_labelled = pvo->is_labelled;
	memcpy(&pv->id, &pvo->id, sizeof(struct id));
	memcpy(&pv->vg_id, &vgo->id, sizeof(struct id));
	pv->status = pvo->status;
	pv->size = pvo->size;

	if (pvo->device_hint && !(pv->device_hint = dm_pool_strdup(mem, pvo->device_hint)))
		goto_bad;
	if (pvo->device_id && !(pv->device_id = dm_pool_strdup(mem, pvo->device_id)))
		goto_bad;
	if (pvo->device_id_type && !(pv->device_id_type = dm_pool_strdup(mem, pvo->device_id_type)))
		goto_bad;

	pv->pe_start = pvo->pe_start;
	pv->pe_count = pvo->pe_count;
	pv->ba_start = pvo->ba_start;
	pv->ba_size = pvo->ba_size;

	dm_list_init(&pv->tags);
	dm_list_init(&pv->segments);

	if (!dm_list_empty(&pvo->tags) && !str_list_dup(mem, &pv->tags, &pvo->tags))
		goto_bad;

	pv->pe_size = vg->extent_size;
	pv->pe_alloc_count = 0;
	pv->pe_align = 0;

	/* Note: text import uses "pv0" style keys rather than pv id. */
	if (!dm_hash_insert_binary(pv_hash, &pv->id, ID_LEN, pv))
		goto_bad;

	return pv;
bad:
	return NULL;
}

/*
 * We only need to copy things that are exported to metadata text.
 * This struct copy is an alternative to text export+import, so the
 * the reference for what to copy are the text export and import
 * functions.
 *
 * There are two parts to copying the struct:
 * 1. Setting the values, e.g. new->field = old->field.
 * 2. Creating the linkages (pointers/lists) among all of
 *    the new structs.
 *
 * Creating the linkages is the complex part, and for that we use
 * most of the same functions that text import uses.
 *
 * In some cases, the functions creating linkage also set values.
 * This is not common, but in those cases we need to be careful.
 *
 * Many parts of the vg struct are not used by the activation code,
 * but it's difficult to know exactly what is or isn't used, so we
 * try to copy everything, except in cases where we know it's not
 * used and implementing it would be complicated.
 */

struct volume_group *vg_copy_struct(struct volume_group *vgo)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	struct pv_list *pvlo;
	struct pv_list *pvl;
	struct lv_list *lvlo;
	struct lv_list *lvl;
	struct dm_hash_table *pv_hash = NULL;
	struct dm_hash_table *lv_hash = NULL;

	if (!(vg = alloc_vg("read_vg", vgo->cmd, vgo->name)))
		return NULL;

	log_debug("Copying vg struct %p to %p", vgo, vg);

	/*
	 * TODO: put hash tables in vg struct, and also use for text import.
	 */
	if (!(pv_hash = dm_hash_create(58)))
		goto_bad;
	if (!(lv_hash = dm_hash_create(8180)))
		goto_bad;

	vg->seqno = vgo->seqno;
	vg->alloc = vgo->alloc;
	vg->status = vgo->status;
	vg->id = vgo->id;
	vg->extent_size = vgo->extent_size;
	vg->max_lv = vgo->max_lv;
	vg->max_pv = vgo->max_pv;
	vg->pv_count = vgo->pv_count;
	vg->open_mode = vgo->open_mode;
	vg->mda_copies = vgo->mda_copies;

	if (vgo->profile && !(vg->profile = add_profile(vgo->cmd, vgo->profile->name, CONFIG_PROFILE_METADATA)))
		goto_bad;
	if (vgo->system_id && !(vg->system_id = dm_pool_strdup(vg->vgmem, vgo->system_id)))
		goto_bad;
	if (vgo->lock_type && !(vg->lock_type = dm_pool_strdup(vg->vgmem, vgo->lock_type)))
		goto_bad;
	if (vgo->lock_args && !(vg->lock_args = dm_pool_strdup(vg->vgmem, vgo->lock_args)))
		goto_bad;
	if (!dm_list_empty(&vgo->tags) && !str_list_dup(vg->vgmem, &vg->tags, &vgo->tags))
		goto_bad;

	dm_list_iterate_items(pvlo, &vgo->pvs) {
		if (!(pvl = dm_pool_zalloc(vg->vgmem, sizeof(struct pv_list))))
			goto_bad;
		if (!(pvl->pv = _pv_copy_struct(vg, vgo, pvlo->pv, pv_hash)))
			goto_bad;
		if (!alloc_pv_segment_whole_pv(vg->vgmem, pvl->pv))
			goto_bad;
		vg->extent_count += pvl->pv->pe_count;
		vg->free_count += pvl->pv->pe_count;
		add_pvl_to_vgs(vg, pvl);
	}

	dm_list_iterate_items(lvlo, &vgo->lvs) {
		if (!(lvl = dm_pool_zalloc(vg->vgmem, sizeof(struct lv_list))))
			goto_bad;
		if (!(lvl->lv = _lv_copy_struct(vg, vgo, lvlo->lv, pv_hash, lv_hash)))
			goto_bad;
		dm_list_add(&vg->lvs, &lvl->list);
	}

	if (vgo->pool_metadata_spare_lv &&
	    !(vg->pool_metadata_spare_lv = dm_hash_lookup(lv_hash, vgo->pool_metadata_spare_lv->name)))
		goto_bad;

	if (vgo->sanlock_lv &&
	    !(vg->sanlock_lv = dm_hash_lookup(lv_hash, vgo->sanlock_lv->name)))
		goto_bad;

	dm_list_iterate_items(lvlo, &vgo->lvs) {
		if (!(lv = dm_hash_lookup(lv_hash, lvlo->lv->name)))
			goto_bad;
		if (!_lvsegs_copy_struct(vg, lv, vgo, lvlo->lv, pv_hash, lv_hash))
			goto_bad;
	}

	/* sanity check */
	if ((vg->free_count != vgo->free_count) || (vg->extent_count != vgo->extent_count)) {
		log_error("vg copy wrong free_count %u %u extent_count %u %u",
			    vgo->free_count, vg->free_count, vgo->extent_count, vg->extent_count);
		goto_bad;
	}

	set_pv_devices(vgo->fid, vg);

	dm_hash_destroy(pv_hash);
	dm_hash_destroy(lv_hash);
	return vg;

bad:
	dm_hash_destroy(pv_hash);
	dm_hash_destroy(lv_hash);
	release_vg(vg);
	return NULL;
}

