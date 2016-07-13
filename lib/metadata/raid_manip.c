/*
 * Copyright (C) 2011-2016 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * All raid conversion business in here:
 *
 * - takeover, i.e. raid level change (e.g. striped <-> raid6)
 *
 * - reshaping, i.e. raid algorithm change (e.g. raid5_ls -> raid5_ra)
 *
 * - duplication (e.g. start with a linear LV and add another,
 *                say raid10_offset in a top-level raid1 stack;
 *                N sub LVs possible)
 *
 * - unduplication, i.e. tear down duplicated LV raid1 stack
 *   		    keeping any of the duplicated sub LVs
 *
 * - leg images replacement
 *
 * - leg images repair
 *
 * - raid1 splitting, tracking and merging
 *
 *
 * In general, int functions in this module return 1 on success and 0 on failure.
 */

#include "lib.h"
#include "archiver.h"
#include "metadata.h"
#include "toolcontext.h"
#include "segtype.h"
#include "display.h"
#include "activate.h"
#include "lv_alloc.h"
#include "lvm-string.h"
#include "lvm-signal.h"

#define	ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))

/*
 * Macros to check actual function arguments are being
 * provided, provided correctly and display internal error
 * with @msg on if not
 */
/* True if @arg, else false and display @msg */
#define RETURN_IF_NONZERO(arg, msg) \
{ \
	if ((arg)) { \
		log_error(INTERNAL_ERROR "%s[%u] -- no %s!", __func__, __LINE__, (msg)); \
		return 0; \
	} \
}

/* True if !@arg, else false and display @msg */
#define RETURN_IF_ZERO(arg, msg) \
	RETURN_IF_NONZERO(!(arg), (msg))

/* True if !@arg, display @msg and goto err label */
#define ERR_IF_ZERO(arg, msg) \
{ \
	if (!(arg)) { \
		log_error(INTERNAL_ERROR "%s[%u] -- no %s!", __func__, __LINE__, (msg)); \
		goto err; \
	} \
}

/* Macro to check argument @lv exists */
#define RETURN_IF_LV_ZERO(lv) \
	RETURN_IF_ZERO((lv), "lv argument");

/* Macro to check argument @seg exists */
#define RETURN_IF_SEG_ZERO(seg) \
	RETURN_IF_ZERO((seg), "lv segment argument");

/* False if (@seg)->area_count, else true and display @msg */
#define RETURN_IF_SEG_AREA_INDEX_FALSE(seg, s) \
	RETURN_IF_ZERO((seg)->area_count, "segment areas"); \
	RETURN_IF_ZERO((s) < seg->area_count, "valid segment area index")

/* Macro to check argument @segtype exists */
#define RETURN_IF_SEGTYPE_ZERO(segtype) \
	RETURN_IF_ZERO((segtype), "lv segment type argument");

/* Macro to check @lv and it's first segment @seg exist */
#define RETURN_IF_LV_SEG_ZERO(lv, seg) \
	RETURN_IF_LV_ZERO((lv)); \
	RETURN_IF_SEG_ZERO((seg))

/* Macro to check @lv and the segment type @segtype exist */
#define RETURN_IF_LV_SEGTYPE_ZERO(lv, segtype) \
	RETURN_IF_LV_ZERO((lv)); \
	RETURN_IF_SEGTYPE_ZERO((segtype))

/* Macro to check @lv, it's first segment @seg and the segment type @segtype exist */
#define RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, seg, segtype) \
	RETURN_IF_LV_SEG_ZERO((lv), (seg)); \
	RETURN_IF_SEGTYPE_ZERO((segtype))

/* HM Helper: conditionally return seg_lv(@seg, @s) */
static struct logical_volume *_seg_lv_checked(struct lv_segment *seg, uint32_t s)
{
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, s);

	return seg_type(seg, s) == AREA_LV ? seg_lv(seg, s) : NULL;
}

/* HM Helper: conditionally return seg_metalv(@seg, @s) to prevent oops if no meta areas */
static struct logical_volume *_seg_metalv_checked(struct lv_segment *seg, uint32_t s)
{
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, s);

	return (seg->meta_areas && seg_metatype(seg, s) == AREA_LV) ? seg_metalv(seg, s) : NULL;
}

/* Ensure minimum region size on @lv */
static int _ensure_min_region_size(const struct logical_volume *lv)
{
	struct lv_segment *seg;
	uint32_t min_region_size, region_size;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(lv->size, "LV size set");
	RETURN_IF_ZERO(seg->region_size, "segment region size set");

	/* MD's bitmap is limited to tracking 2^21 regions */
	min_region_size = lv->size / (1 << 21);
	region_size = seg->region_size;

	while (region_size < min_region_size)
		region_size *= 2;

	if (seg->region_size != region_size) {
		log_very_verbose("Setting region_size to %u", seg->region_size);
		seg->region_size = region_size;
	}

	return 1;
}

/* HM Helper: return any alias for segment type of @seg */
static const char *_get_segtype_alias(const struct segment_type *segtype)
{
	const char *r = "\0";

	RETURN_IF_SEG_ZERO(segtype);

	if (!strcmp(segtype->name, SEG_TYPE_NAME_RAID5))
		r = SEG_TYPE_NAME_RAID5_LS;
	else if (!strcmp(segtype->name, SEG_TYPE_NAME_RAID6))
		r = SEG_TYPE_NAME_RAID6_ZR;
	else if (!strcmp(segtype->name, SEG_TYPE_NAME_RAID10))
		r = SEG_TYPE_NAME_RAID10_NEAR;
	else if (!strcmp(segtype->name, SEG_TYPE_NAME_RAID5_LS))
		r = SEG_TYPE_NAME_RAID5;
	else if (!strcmp(segtype->name, SEG_TYPE_NAME_RAID6_ZR))
		r = SEG_TYPE_NAME_RAID6;
	else if (!strcmp(segtype->name, SEG_TYPE_NAME_RAID10_NEAR))
		r = SEG_TYPE_NAME_RAID10;
	
	return r;
}

/* HM Return "linear" for striped @segtype with 1 area instead of "striped" */
static const char *_get_segtype_name(const struct segment_type *segtype, unsigned new_image_count)
{
	if (!segtype ||
	    (segtype_is_striped(segtype) && new_image_count == 1))
		return "linear";

	return segtype->name;
}

/* HM helper: return first hit strstr() of @str for string in @... */
static char *_strstr_strings(const char *str, ...)
{
	char *substr, *r = NULL;
	va_list ap;

	RETURN_IF_ZERO(str, "string argument");

	va_start(ap, str);
	while ((substr = va_arg(ap, char *)))
		if ((r = strstr(str, substr)))
			break;
	va_end(ap);

	return r;
}

/* HM helper: return top-level lv name for given image @lv */
static char *_top_level_lv_name(struct logical_volume *lv)
{
	char *p, *r;

	RETURN_IF_LV_ZERO(lv);
	RETURN_IF_ZERO((r = dm_pool_strdup(lv->vg->vgmem, lv->name)),
		       "lv name allocation possible");
	
	if ((p = _strstr_strings(r, "_rimage_", "_dup_", "_extracted", NULL)))
		*p = '\0'; /* LV name returned is now that of top-level RAID */

	return r;
}

/* HM Helper: return true in case this is a raid1 top-level LV inserted to do synchronization of 2 given sub LVs */
static int _lv_is_duplicating(const struct logical_volume *lv)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	/* Needs to be raid1 with >= 2 legs and the legs must have the proper names suffix */
	if (!seg_is_raid1(seg))
		return 0;

	/* Sub LVs must be present and duplicated ones and "image" infix may not be present in any of their names */
	for (s = 0; s < seg->area_count; s++)
		if (!_seg_lv_checked(seg, s) ||
		    !lv_is_duplicated(seg_lv(seg, s)) ||
		    strstr(seg_lv(seg, s)->name, "image"))
			return 0;

	return 1;
}

/* HM Helper: check if @lv is active and display cluster/local message if not */
static int _lv_is_active(struct logical_volume *lv)
{
	RETURN_IF_LV_ZERO(lv);

	if (vg_is_clustered(lv->vg)) {
		if (!lv_is_active_exclusive_locally(lv)) {
			log_error("%s in clustered VG must be active exclusive "
				  "locally to perform this operation.",
				  display_lvname(lv));
			return 0;
		}

	} else if (!lv_is_active(lv)) {
		log_error("%s must be active to perform this operation.",
			  display_lvname(lv));
		return 0;
	}

	return 1;
}

/*
 * HM Helper:
 *
 * check for @lv if unduplicate request is allowed to proceed based
 * on any @sub_lv_name or @layout_properties_requested provided
 */
static int _valid_name_requested(struct logical_volume **lv, const char **sub_lv_name,
				 int layout_properties_requested, const char *what)
{
	RETURN_IF_ZERO(lv && *lv, "lv argument");
	RETURN_IF_ZERO(sub_lv_name, "sub_lv_name argument");

	/*
	 * If we got a name which identifies the sub LV uniquely per se,
	 * no layout properties (stripes, ...) may be requested
	 */
	if (*sub_lv_name) {
		if (layout_properties_requested) {
			log_error("Rejecting %s request with both sub LV name and layout properties on %s",
				  what, display_lvname(*lv));
			return 0;
		}

	/*
	 * If no *sub_lv_name provided, try deriving it from the provided
	 * LV name, assuming user passed in a duplicated sub LV name.
	 */
	} else {
		struct lv_list *lvl;
		char *lv_name;

		if (!lv_is_duplicated(*lv)) {
			log_error("Rejecting %s request on %s LV %s; use sub LV",
				  what, _lv_is_duplicating(*lv) ? "duplicating" : "non-duplicated",
				  display_lvname(*lv));
			return 0;
		}

		if (!(*sub_lv_name = dm_pool_strdup((*lv)->vg->cmd->mem, (*lv)->name)))
			return_0;

		if (!(lv_name = _top_level_lv_name(*lv)))
			return_0;

		if (!strcmp(lv_name, *sub_lv_name)) {
			log_error("No sub LV name of %s provided", display_lvname(*lv));
			return 0;
		}

		if (!(lvl = find_lv_in_vg((*lv)->vg, lv_name)))
			return_0;

		*lv = lvl->lv;
	}

	return 1;
}

/*
 * HM
 *
 * Default region_size on @lv unless already set i
 */
static int _check_and_init_region_size(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	seg->region_size = seg->region_size ?: get_default_region_size(lv->vg->cmd);

	return _ensure_min_region_size(lv);
}

/* Return data images count for @total_rimages depending on @seg's type */
static uint32_t _data_rimages_count(const struct lv_segment *seg, const uint32_t total_rimages)
{
	RETURN_IF_SEG_ZERO(seg);
	RETURN_IF_NONZERO(!seg_is_thin(seg) && total_rimages <= seg->segtype->parity_devs,
			  "total rimages count > parity devices");

	return total_rimages - seg->segtype->parity_devs;
}

/* HM Helper: return sub LV in @lv by @name and index in areas array in @idx */
static struct logical_volume *_find_lv_in_sub_lvs(struct logical_volume *lv,
						  const char *name, uint32_t *idx)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(idx, "idx argument pointer");

	for (s = 0; s < seg->area_count; s++) {
		RETURN_IF_ZERO(_seg_lv_checked(seg, s), "sub lv");

		if (!strcmp(name, seg_lv(seg, s)->name)) {
			*idx = s;
			return seg_lv(seg, s);
		}
	}

	return NULL;
}

/* HM Helper: suspend+resume LV for table switch */
static int _suspend_resume_lv(struct logical_volume *lv)
{
	RETURN_IF_ZERO(lv, "lv argument");

	/* Suspend+resume LV to switch mapping tables */
	if (!suspend_lv(lv->vg->cmd, lv) || !resume_lv(lv->vg->cmd, lv)) {
		log_error("Failed to suspend+resume LV %s", display_lvname(lv));
		return 0;
	}

	return 1;
}

/* HM Helper: check for @pv listed on @pvs */
static int _pv_on_list(struct physical_volume *pv, struct dm_list *pvs)
{
	struct pv_list *pvl;

	RETURN_IF_ZERO(pv, "pv argument");
	/* pvs may be empty initially but reference must be present */
	RETURN_IF_ZERO(pvs, "failed pvs list argument");

	dm_list_iterate_items(pvl, pvs)
		if (pvl->pv == pv)
			return 1;

	return 0;
}

/*
 * HM helper:
 *
 * Compare the raid levels in segtype @t1 and @t2
 *
 * Return 1 if same, else 0
 */
static int _cmp_level(const struct segment_type *t1, const struct segment_type *t2)
{
	RETURN_IF_ZERO(t1, "first segment");
	RETURN_IF_ZERO(t2, "second segment");

	if ((segtype_is_any_raid10(t1)  && !segtype_is_any_raid10(t2)) ||
	    (!segtype_is_any_raid10(t1) && segtype_is_any_raid10(t2)))
		return 0;

	return !strncmp(t1->name, t2->name, 5);
}

/*
 * HM Helper:
 *
 * Check for same raid levels in segtype @t1 and @t2
 *
 * Return 1 if same, else != 1
 */
static int is_same_level(const struct segment_type *t1, const struct segment_type *t2)
{
	return _cmp_level(t1, t2);
}

static int _lv_is_raid_with_tracking(const struct logical_volume *lv, uint32_t *ss)
{
	uint32_t s;
	const struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (lv_is_raid(lv))
		for (s = 0; s < seg->area_count; s++)
			if (lv_is_visible(seg_lv(seg, s))) {
				if (!(seg_lv(seg, s)->status & LVM_WRITE)) {
					if (ss)
						*ss = s;

					return 1;
				}
	
				RETURN_IF_ZERO(0, "read-only tracking LV!");
			}

	return 0;
}

/* API function to check for @lv to be a tracking one */
int lv_is_raid_with_tracking(const struct logical_volume *lv)
{
	return _lv_is_raid_with_tracking(lv, NULL);
}

/* HM Helper: activate all lvs on list @lvs */
static int _activate_lv_list_excl_local(struct dm_list *lvs)
{
	struct lv_list *lvl;

	/* lvs list may be empty here! */
	RETURN_IF_ZERO(lvs, "lvs list argument");

	dm_list_iterate_items(lvl, lvs)
		if (!activate_lv_excl_local(lvl->lv->vg->cmd, lvl->lv))
			return_0;
	return 1;
}

/* Helper: return image count of @lv depending on segment type */
uint32_t lv_raid_image_count(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	return seg_is_raid(seg) ? seg->area_count : 1;
}

/* Calculate raid rimage extents required based on total @extents for @segtype, @stripes and @data_copies */
uint32_t raid_rimage_extents(const struct segment_type *segtype,
			     uint32_t extents, uint32_t stripes, uint32_t data_copies)
{
	uint64_t r;

	RETURN_IF_ZERO(segtype, "segtype argument");

PFLA("segtype=%s extents=%u stripes=%u, data_copies=%u", segtype->name, extents, stripes, data_copies);
	if (!extents ||
	    segtype_is_mirror(segtype) ||
	    segtype_is_raid1(segtype) ||
	    segtype_is_raid01(segtype))
		return extents;

	r = extents;
	if (segtype_is_any_raid10(segtype))
		r *= (data_copies ?: 1); /* Caller should ensure data_copies > 0 */

	r = dm_div_up(r, stripes ?: 1); /* Caller should ensure stripes > 0 */

PFLA("r=%llu", (unsigned long long) r);
	return r > UINT_MAX ? 0 : (uint32_t) r;
}

/*
 * Calculate total brutto extents required to provide requested @extents
 * based on @segtype, @stripes and @data_copies
 *
 * This is extents allocated and has to be devided by # data copies
 * to get the user visible le count.
 */
uint32_t raid_total_extents(const struct segment_type *segtype,
			    uint32_t extents, uint32_t stripes, uint32_t data_copies)
{
#if 0
	uint32_t multiplier = (segtype_is_mirror(segtype) || segtype_is_raid1(segtype) || segtype_is_raid01(segtype)) ? 1 : stripes;
#else
	uint32_t multiplier = (segtype_is_striped(segtype) || segtype_is_striped_raid(segtype)) ? stripes : 1;
#endif

	RETURN_IF_ZERO(segtype, "segtype argument");
	RETURN_IF_ZERO(extents, "extents > 0");
	RETURN_IF_ZERO(stripes, "stripes > 0");

PFLA("multiplier * raid_rimage_extents(segtype, extents, stripes, data_copies)=%u", multiplier * raid_rimage_extents(segtype, extents, stripes, data_copies));
	return multiplier * raid_rimage_extents(segtype, extents, stripes, data_copies);
}

/* Activate @sub_lv preserving any exclusive local activation given by @top_lv */
static int _activate_sublv_preserving_excl(struct logical_volume *top_lv,
					   struct logical_volume *sub_lv)
{
	struct cmd_context *cmd;

	RETURN_IF_ZERO(top_lv, "top level LV");
	RETURN_IF_ZERO(sub_lv, "sub LV");
	cmd = top_lv->vg->cmd;

	/* If top RAID was EX, use EX */
	if (lv_is_active_exclusive_locally(top_lv)) {
		if (!activate_lv_excl_local(cmd, sub_lv))
			return_0;
	} else if (!activate_lv(cmd, sub_lv))
			return_0;

	return 1;
}

/* Return # of reshape LEs per device for @seg */
static uint32_t _reshape_len_per_dev(struct lv_segment *seg)
{
	RETURN_IF_SEG_ZERO(seg);

	return seg->reshape_len;
}

/* Return # of reshape LEs per @lv (sum of all sub LVs reshape LEs) */
static uint32_t _reshape_len_per_lv(struct logical_volume *lv)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	return _reshape_len_per_dev(seg) * _data_rimages_count(seg, seg->area_count);
}

/*
 * HM Helper:
 *
 * store the allocated reshape length per data image
 * in the only segment of the top-level RAID @lv and
 * in the first segment of each sub lv.
 */
static int _lv_set_reshape_len(struct logical_volume *lv, uint32_t reshape_len)
{
	uint32_t s;
	struct lv_segment *seg, *data_seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
	RETURN_IF_ZERO(reshape_len < lv->le_count - 1, "proper reshape_len argument");

	seg->reshape_len = reshape_len;

	for (s = 0; s < seg->area_count; s++) {
		RETURN_IF_ZERO(_seg_lv_checked(seg, s), "sub lv");

		dm_list_iterate_items(data_seg, &seg_lv(seg, s)->segments) {
			data_seg->reshape_len = reshape_len;
			reshape_len = 0;
		}
	}

	return 1;
}

/* HM Helper:
 *
 * correct segments logical start extents in all sub LVs of @lv
 * after having reordered any segments in sub LVs e.g. because of
 * reshape space (re)allocation.
 */
static int _lv_set_image_lvs_start_les(struct logical_volume *lv)
{
	uint32_t le, s;
	struct lv_segment *data_seg, *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);

	for (s = 0; s < seg->area_count; s++) {
		RETURN_IF_ZERO(_seg_lv_checked(seg, s), "sub lv");

		le = 0;
		dm_list_iterate_items(data_seg, &(seg_lv(seg, s)->segments)) {
			data_seg->reshape_len = le ? 0 : seg->reshape_len;
			data_seg->le = le;
			le += data_seg->len;
		}
	}

	/* At least try merging segments _after_ adjusting start LEs */
	return lv_merge_segments(lv);
}

/* HM Helper: put @lv on @removal_lvs resetting it's raid image state */
static int _lv_reset_raid_add_to_list(struct logical_volume *lv, struct dm_list *removal_lvs)
{
	struct lv_list *lvl;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(removal_lvs, "removal LVs list argument");
	RETURN_IF_ZERO((lvl = dm_pool_alloc(lv->vg->vgmem, sizeof(*lvl))), "LV list item allocated")

	lvl->lv = lv;
	dm_list_add(removal_lvs, &lvl->list);
	lv->status &= ~(RAID_IMAGE | RAID_META | LV_DUPLICATED);
	lv_set_visible(lv);

	return 1;
}

/*
 * HM Helper:
 *
 * Deactivate and remove the LVs on @removal_lvs list from @vg
 *
 * Returns 1 on success or 0 on failure
 */
static int _deactivate_and_remove_lvs(struct volume_group *vg, struct dm_list *removal_lvs)
{
	struct lv_list *lvl;

	RETURN_IF_ZERO(vg, "VG");
	RETURN_IF_ZERO(removal_lvs, "removal LVs list argument");

	dm_list_iterate_items(lvl, removal_lvs)
		if (!deactivate_lv(vg->cmd, lvl->lv) ||
		    !lv_remove(lvl->lv))
			return_0;

	return 1;
}

/*
 * HM Helper:
 *
 * report health string in @*raid_health for @lv from kernel reporting # of devs in @*kernel_devs
 */
static int _get_dev_health(struct logical_volume *lv, uint32_t *kernel_devs,
			   uint32_t *devs_health, uint32_t *devs_in_sync,
			   char **raid_health)
{
	unsigned d;
	char *rh;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(kernel_devs, "kernel_devs");
	RETURN_IF_ZERO(devs_health, "devs_health");
	RETURN_IF_ZERO(devs_in_sync, "devs_in_sync");

	*devs_health = *devs_in_sync = 0;

	if (!lv_raid_dev_count(lv, kernel_devs)) {
		log_error("Failed to get device count");
		return_0;
	}

	if (!lv_raid_dev_health(lv, &rh)) {
		log_error("Failed to get device health");
		return_0;
	}

	d = (unsigned) strlen(rh);
	while (d--) { 
		(*devs_health)++;
		if (rh[d] == 'A')
			(*devs_in_sync)++;
	}

	if (raid_health)
		*raid_health = rh;

	return 1;
}

/* HM Helper: return 1 in case raid device with @idx is alive and in sync */
static int _dev_in_sync(struct logical_volume *lv, const uint32_t idx)
{
	uint32_t kernel_devs, devs_health, devs_in_sync;
	char *raid_health;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, idx);

	if (!seg_is_raid(seg))
		return seg->area_count;

	if (!_get_dev_health(lv, &kernel_devs, &devs_health, &devs_in_sync, &raid_health) ||
	    idx >= kernel_devs)
		return 0;

	return raid_health[idx] == 'A';
}

/* HM Helper: return number of devices in sync for (raid) @lv */
static int _devs_in_sync_count(struct logical_volume *lv)
{
	uint32_t kernel_devs, devs_health, devs_in_sync;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (!seg_is_raid(seg))
		return seg->area_count;

	if (!_get_dev_health(lv, &kernel_devs, &devs_health, &devs_in_sync, NULL))
		return 0;

	return (int) devs_in_sync;
}

/* Return 1 if @lv is degraded, else 0 */
static int _lv_is_degraded(struct logical_volume *lv)
{
	struct lv_segment *seg;

	RETURN_IF_ZERO(lv, "lv argument");
	if (!(seg = first_seg(lv)))
		return 0;

	return _devs_in_sync_count(lv) < seg->area_count;
}

/*
 * _raid_in_sync
 * @lv
 *
 * _raid_in_sync works for all types of RAID segtypes, as well
 * as 'mirror' segtype.  (This is because 'lv_raid_percent' is
 * simply a wrapper around 'lv_mirror_percent'.
 *
 * Returns: 1 if in-sync, 0 otherwise.
 */
#define RAID_IN_SYNC_RETRIES	6
static int _raid_in_sync(const struct logical_volume *lv)
{
	int retries = RAID_IN_SYNC_RETRIES;
	dm_percent_t sync_percent = DM_PERCENT_0;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (seg_is_striped(seg) || seg_is_any_raid0(seg))
		return 1;

	do {
		/*
		 * FIXME We repeat the status read here to workaround an
		 * unresolved kernel bug when we see 0 even though the 
		 * the array is 100% in sync.
		 * https://bugzilla.redhat.com/1210637
		 */
		if (!lv_raid_percent(lv, &sync_percent)) {
			log_error("Unable to determine sync status of %s/%s.",
				  lv->vg->name, lv->name);
			return 0;
		}
PFLA("sync_percent=%d DM_PERCENT_100=%d", sync_percent, DM_PERCENT_100);
		if (sync_percent > DM_PERCENT_0)
			break;
		if (!retries)
			log_warn("WARNING: Sync status for %s is inconsistent.",
				 display_lvname(lv));
		if (sync_percent)
			break;
		usleep(500000);
	} while (retries--);

	return (sync_percent == DM_PERCENT_100) ? 1 : 0;
}

/*
 * HM API function:
 *
 * return 1 if raid @lv is in sync or no RAID @lv else 0.
 *
 * Called via lv_extend/lv_reduce API to prevent
 * reshaping @lv from being resized.
 */
int lv_raid_in_sync(const struct logical_volume *lv)
{
	struct lv_segment *seg;

	RETURN_IF_LV_ZERO(lv);

	if (!(seg = first_seg(lv)))
		return 1;

	if (seg_is_reshapable_raid(seg)) {
		if (!lv_is_active(lv)) {
			log_error("RAID LV %s has to be active to resize it!", display_lvname(lv));
			return 0;
		}

		return _raid_in_sync(lv);
	}

	return 1;
}

/* HM Helper: start repair on idle/frozen @lv */
static int _lv_cond_repair(struct logical_volume *lv)
{
	char *action;

	RETURN_IF_ZERO(lv, "lv argument");

	if (!lv_raid_sync_action(lv, &action))
		return 0;

	return (strcmp(action, "idle") &&
		strcmp(action, "frozen")) ? 1 : lv_raid_message(lv, "repair");
}

/*
 * HM Helper:
 *
 * report current number of redundant disks for @segtype depending on @total_images and @data_copies in @*nr
 */
static int _seg_get_redundancy(const struct segment_type *segtype, unsigned total_images,
			       unsigned data_copies, unsigned *nr)
{
	if (segtype_is_thin(segtype)) {
		*nr = 0;
		return 1;
	}

	RETURN_IF_ZERO(total_images, "total images");
	RETURN_IF_ZERO(data_copies, "data copies");
	RETURN_IF_ZERO(nr, "nr argument");

	if (!segtype)
		*nr = 0;

	else if (segtype_is_any_raid10(segtype)) {
		if (!total_images % data_copies &&
		    !segtype_is_raid10_far(segtype))
			/* HM FIXME: this is the ideal case if (data_copies - 1) fail per 'mirror group' */
			*nr = total_images / data_copies;
		else
			*nr = data_copies - 1;

	} else if (segtype_is_raid01(segtype) ||
		   segtype_is_raid1(segtype) ||
		   segtype_is_mirror(segtype))
		*nr = total_images - 1;

	else if (segtype_is_raid4(segtype) ||
		 segtype_is_any_raid5(segtype) ||
		 segtype_is_any_raid6(segtype))
		*nr = segtype->parity_devs;

	else
		*nr = 0;

	return 1;
}

/*
 * HM Helper:
 *
 * in case of any resilience related conversions on @lv -> ask the user unless "-y/--yes" on command line
 */
static int _yes_no_conversion(const struct logical_volume *lv,
			      const struct segment_type *new_segtype,
			      int yes, int force, int duplicate,
			      unsigned new_image_count,
			      const unsigned new_data_copies,
			      const unsigned new_stripes,
			      const unsigned new_stripe_size)
{
	int data_copies_change, segtype_change, stripes_change, stripe_size_change;
	unsigned cur_redundancy, new_redundancy;
	struct lv_segment *seg;
	struct segment_type *new_segtype_tmp;
	const struct segment_type *segtype;
	struct lvinfo info = { 0 };
	struct cmd_context *cmd;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(new_data_copies, "new data copies");
	RETURN_IF_ZERO((new_segtype_tmp = (struct segment_type *) new_segtype), /* Drop const */
		       "segment type argument");
	cmd = lv->vg->cmd;

	if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
		log_error("Unable to retrieve logical volume information: aborting");
		return 0;
	}

	/* If this is a duplicating LV with raid1 on top, the segtype of the respective leg is relevant */
	if (_lv_is_duplicating(lv)) {

		if (first_seg(seg_lv(seg, 0))->segtype == new_segtype)
			segtype = first_seg(seg_lv(seg, 1))->segtype;
		else
			segtype = first_seg(seg_lv(seg, 0))->segtype;

	} else
		segtype = seg->segtype;

	segtype_change = new_segtype != segtype;
	data_copies_change = new_data_copies && (new_data_copies != seg->data_copies);
	stripes_change = new_stripes && (new_stripes != _data_rimages_count(seg, seg->area_count));
	stripe_size_change = new_stripe_size && (new_stripe_size != seg->stripe_size);
	new_image_count = new_image_count ?: lv_raid_image_count(lv);

	/* Get number of redundant disk for current and new segtype */
	if (!_seg_get_redundancy(segtype, seg->area_count, seg->data_copies, &cur_redundancy) ||
	    !_seg_get_redundancy(new_segtype, new_image_count, new_data_copies, &new_redundancy))
		return 0;

PFLA("yes=%d cur_redundancy=%u new_redundancy=%u", yes, cur_redundancy, new_redundancy);
	if (duplicate)
		;
	else if (new_redundancy == cur_redundancy) {
		if (stripes_change)
			log_print_unless_silent("Converting active%s %s %s%s%s%s will keep "
				 "resilience of %u disk failure%s",
				 info.open_count ? " and open" : "", display_lvname(lv),
				 segtype != new_segtype ? "from " : "",
				 segtype != new_segtype ? _get_segtype_name(segtype, seg->area_count) : "",
				 segtype != new_segtype ? " to " : "",
				 segtype != new_segtype ? _get_segtype_name(new_segtype, new_image_count) : "",
				 cur_redundancy,
				 (!cur_redundancy || cur_redundancy > 1) ? "s" : "");

	} else if (new_redundancy > cur_redundancy)
		log_print_unless_silent("Converting active%s %s %s%s%s%s will enhance "
			 "resilience from %u disk failure%s to %u",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 segtype != new_segtype ? "from " : "",
			 segtype != new_segtype ? _get_segtype_name(segtype, seg->area_count) : "",
			 segtype != new_segtype ? " to " : "",
			 segtype != new_segtype ? _get_segtype_name(new_segtype, new_image_count) : "",
			 cur_redundancy,
			 (!cur_redundancy || cur_redundancy > 1) ? "s" : "",
			 new_redundancy);

	else if (new_redundancy &&
		 new_redundancy < cur_redundancy)
		log_warn("WARNING: Converting active%s %s %s%s%s%s will degrade "
			 "resilience from %u disk failures to just %u",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 segtype != new_segtype ? "from " : "",
			 segtype != new_segtype ? _get_segtype_name(segtype, seg->area_count) : "",
			 segtype != new_segtype ? " to " : "",
			 segtype != new_segtype ? _get_segtype_name(new_segtype, new_image_count) : "",
			 cur_redundancy, new_redundancy);

	else if (!new_redundancy && cur_redundancy)
		log_warn("WARNING: Converting active%s %s from %s to %s will remove "
			 "all resilience to disk failures",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 _get_segtype_name(segtype, seg->area_count),
			 _get_segtype_name(new_segtype, new_image_count));


	/****************************************************************************/
	/* No --type arg */
	/* Linear/raid0 with 1 image to raid1 via "-mN" option */
	if (segtype == new_segtype &&
	    (seg_is_linear(seg) || (seg_is_any_raid0(seg) && seg->area_count == 1)) &&
    	    new_image_count > 1 &&
	    !(new_segtype_tmp = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return_0;

	if (!yes) {
		if (_lv_is_duplicating(lv)) {
			// const char *add_remove, *to_from;
			struct add_remove {
				const char *ar;
				const char *tf;
			};
			static struct add_remove add_remove[2] = {
				{ "add", "to" },
				{ "remove", "from" },
			};
			struct add_remove *ar = add_remove + (new_image_count > seg->area_count ? 0 : 1);

PFLA("new_image_count=%u seg->area_count=%u", new_image_count, seg->area_count);

			if (yes_no_prompt("Do you really want to %s a %s duplicated "
					  "sub_lv %s duplicating %s? [y/n]: ",
					  ar->ar, new_segtype->name, ar->tf, display_lvname(lv)) == 'n') {
				log_error("Logical volume %s NOT converted", display_lvname(lv));
				return 0;
			}

			return 1;
		}

		/* HM FIXME: all these checks or just the add/remove one above? */
		if (segtype_change &&
		    yes_no_prompt("Do you really want to %s %s with type %s to %s? [y/n]: ",
				  duplicate ? "duplicate" : "convert",
				  display_lvname(lv),
				  _get_segtype_name(segtype, seg->area_count),
				  _get_segtype_name(new_segtype_tmp, new_image_count)) == 'n') {
			log_error("Logical volume %s NOT converted", display_lvname(lv));
			return 0;
		}

		if (data_copies_change &&
		    yes_no_prompt("Do you really want to %s %s with %u to %u data copies %s resilience%s? [y/n]: ",
				  duplicate ? "duplicate" : "convert",
				  display_lvname(lv), seg->data_copies, new_data_copies,
				  seg->data_copies < new_data_copies ? "enhancing" : "reducing",
				  duplicate ? " on the copy": "") == 'n') {
			log_error("Logical volume %s NOT converted", display_lvname(lv));
			return 0;
		}

		if (!duplicate) {
			if (stripes_change &&
			    yes_no_prompt("Do you really want to %s %s with %u stripes to %u stripes? [y/n]: ",
					  duplicate ? "duplicate" : "convert",
					  display_lvname(lv), _data_rimages_count(seg, seg->area_count), new_stripes) == 'n') {
				log_error("Logical volume %s NOT converted", display_lvname(lv));
				return 0;
			}

			if (stripe_size_change &&
			    yes_no_prompt("Do you really want to %s %s with stripesize %s to stripesize %s? [y/n]: ",
					  duplicate ? "duplicate" : "convert",
					  display_lvname(lv),
					  display_size(cmd, seg->stripe_size),
					  display_size(cmd, new_stripe_size)) == 'n') {
				log_error("Logical volume %s NOT converted", display_lvname(lv));
				return 0;
			}
		}
	}

	if (sigint_caught())
		return_0;

	/* Now archive metadata after the user has confirmed */
	if (!archive(lv->vg))
		return_0;

	return 1;
}

/* HM helper: allow allocaion on list of @pvs */
static void _allow_pvs(struct dm_list *pvs)
{
	struct pv_list *pvl;

	if (pvs)
		dm_list_iterate_items(pvl, pvs)
			pvl->pv->status &= ~PV_ALLOCATION_PROHIBITED;
}

/* HM Helper: prohibit allocation on @pv if @lv already has segments allocated on it */
static int _avoid_pv_of_lv(struct logical_volume *lv, struct physical_volume *pv)
{
	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(pv, "PV");

PFL();
	if (!(lv->status & PARTIAL_LV) &&
	    lv_is_on_pv(lv, pv))
		pv->status |= PV_ALLOCATION_PROHIBITED;
PFL();

	return 1;
}

/* HM Helper: prohibit allocation on any listed PVs via @data if @lv already has segments allocated on those */
static int _avoid_pvs_of_lv(struct logical_volume *lv, void *data)
{
	struct dm_list *allocate_pvs = (struct dm_list *) data;
	struct pv_list *pvl;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(allocate_pvs, "allocate pv list argument");

#if 0
	if (!first_seg(lv))
		return 1;
#endif

PFLA("lv=%s", display_lvname(lv));
	dm_list_iterate_items(pvl, allocate_pvs)
		if (!_avoid_pv_of_lv(lv, pvl->pv))
			return 0;
PFL();

	return 1;
}

/*
 * Prevent any PVs holding other image components of @lv from being used for allocation,
 * I.e. remove respective PVs from @allocatable_pvs
 */
static int _avoid_pvs_with_other_images_of_lv(struct logical_volume *lv, struct dm_list *allocate_pvs)
{
	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(allocate_pvs, "allocate pv list argument");

#if 0
	if (!first_seg(lv))
		return 1;
#endif

	log_debug_metadata("Preventing allocation on PVs of LV %s", display_lvname(lv));
PFL();
	/* HM FIXME: check fails in case we will ever have mixed AREA_PV/AREA_LV segments */
	if ((seg_type(first_seg(lv), 0) == AREA_PV ? _avoid_pvs_of_lv(lv, allocate_pvs):
						     for_each_sub_lv(lv, _avoid_pvs_of_lv, allocate_pvs)))
		return 1;
PFL();

	log_error("Failed to prevent PVs holding image components "
	  	  "from LV %s being used for allocation.",
	 	  display_lvname(lv));
	return 0;
}

/*
 * Helper:
 *
 * _convert_raid_to_linear
 * @lv
 * @removal_lvs
 *
 * Remove top layer of RAID LV in order to convert to linear.
 * This function makes no on-disk changes.  The residual LVs
 * returned in 'removal_lvs' must be freed by the caller.
 *
 * Returns: 1 on succes, 0 on failure
 */
static int _extract_image_component_list(struct lv_segment *seg,
					 uint64_t type, uint32_t idx,
					 struct dm_list *removal_lvs);
static int _convert_raid_to_linear(struct logical_volume *lv,
				   struct dm_list *removal_lvs)
{
	struct logical_volume *lv_tmp;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(removal_lvs, "removal LV list argument");

	if (!seg_is_any_raid0(seg) &&
	    !seg_is_mirrored(seg) &&
	    !seg_is_raid4(seg) && !seg_is_any_raid5(seg) &&
	    !seg_is_raid01(seg)) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer from segment type %s",
			  lvseg_name(seg));
		return 0;
	}

	/* Only one area may result from the check! */
	if (seg->area_count != 1) {
		log_error(INTERNAL_ERROR
			  "Unable to remove RAID layer when there"
			  " is more than one sub LV");
		return 0;
	}

	if (seg->meta_areas &&
	    !_extract_image_component_list(seg, RAID_META, 0 /* idx */, removal_lvs))
		return 0;

	/* Add remaining last image LV to removal_lvs */
	lv_tmp = seg_lv(seg, 0);
	if (!_lv_reset_raid_add_to_list(lv_tmp, removal_lvs))
		return 0;

	if (!remove_layer_from_lv(lv, lv_tmp))
		return_0;

	seg = first_seg(lv);
	if (!(seg->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	seg->region_size = 0;
	lv->status &= ~(MIRRORED | RAID | RAID_IMAGE | LV_DUPLICATED);

	return 1;
}

/* HM Helper: convert raid01 @lv to striped */
static int _convert_raid01_to_striped(struct logical_volume *lv,
				      struct dm_list *removal_lvs)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (!seg_is_raid01(seg))
		return 0;

	return _convert_raid_to_linear(lv, removal_lvs);
}

/*
 * HM Helper
 *
 * clear first 4K of @lv
 *
 * We're holding an exclusive lock, so we can clear the
 * first block of the (metadata) LV directly on the respective
 * PV avoiding activation of the metadata lv altogether and
 * hence superfluous latencies.
 *
 * Returns: 1 on success, 0 on failure
 *
 * HM FIXME: share with lv_manip.c!
 */
static int _clear_lv(struct logical_volume *lv)
{
	struct lv_segment *seg;
	struct physical_volume *pv;
	uint64_t offset;

	if (test_mode())
		return 1;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(seg->area_count == 1, "area count == 1");
	RETURN_IF_ZERO(seg_type(seg, 0) == AREA_PV, "area PV");
	RETURN_IF_ZERO((pv = seg_pv(seg, 0)), "physical volume");
	RETURN_IF_ZERO(pv->pe_start, "no PE start address");

	/*
	 * Rather than wiping lv->size, we can simply wipe the first 4KiB
	 * to remove the superblock of any previous RAID devices.  It is much
	 * quicker than wiping a potentially larger metadata device completely.
	 */
	log_verbose("Clearing metadata area of %s", display_lvname(lv));
	offset = (pv->pe_start + seg_pe(seg, 0) * lv->vg->extent_size) << 9;

	return dev_set(pv->dev, offset, 4096, 0);
}

/*
 * HM Helper:
 *
 * wipe all LVs first 4 KiB on @lv_list
 *
 * Does _not_ make any on-disk metadata changes!
 *
 * Returns 1 on success or 0 on failure
 */
static int _clear_lvs(struct dm_list *lv_list)
{
	struct lv_list *lvl;

	RETURN_IF_ZERO(lv_list, "lv list argument");
	RETURN_IF_NONZERO(dm_list_empty(lv_list), "LVs given for clearing");

	dm_list_iterate_items(lvl, lv_list)
		if (!_clear_lv(lvl->lv))
			return 0;

	return 1;
}

/*
 * HM
 *
 * Check for maximum supported raid/mirror devices imposed by the kernel
 * maximum device limits _and_ dm-raid superblock bitfield constraints
 *
 * Returns 1 on success or 0 on failure
 */
static int _check_max_devices(uint32_t image_count, unsigned max)
{
	RETURN_IF_ZERO(image_count, "image count");
	RETURN_IF_ZERO(max, "maximum image count");

	if (image_count > max) {
		log_error("Unable to handle %s with more than %u devices",
			  max == DEFAULT_MIRROR_MAX_IMAGES ? "mirrors" : "raid arrays", max);
		return 0;
	}

	return 1;
}

static int _check_max_raid_devices(uint32_t image_count)
{
	return _check_max_devices(image_count, DEFAULT_RAID_MAX_IMAGES);
}

static int _check_max_mirror_devices(uint32_t image_count)
{
	return _check_max_devices(image_count, DEFAULT_MIRROR_MAX_IMAGES);
}

/* Replace @lv with error segment */
static int _replace_lv_with_error_segment(struct logical_volume *lv)
{
	RETURN_IF_ZERO(lv, "lv argument");

	if (lv->status & PARTIAL_LV) {
		log_debug("Replacing %s segments with error target", lv->name);

		if (!replace_lv_with_error_segment(lv)) {
			log_error("Failed to replace %s's extents with error target.", display_lvname(lv));
			return 0;
		}
	}

	return 1;
}

/* Retrieve index from @*lv_name and add it to @prefix; set the result in @*lv_name */
static int _lv_name_add_string_index(struct cmd_context *cmd, const char **lv_name, const char *prefix)
{
	size_t len;
	char *b, *e, *newname, *tmpname;

	RETURN_IF_ZERO(cmd, "command context argument");
	RETURN_IF_ZERO(lv_name, "lv name argument");
	RETURN_IF_ZERO(prefix, "name prefix argument");

	if (!(tmpname = dm_pool_strdup(cmd->mem, *lv_name)))
		return 0;

	if (!(e = strrchr(tmpname, '_')))
		return 0;

	*e = '\0';
	if (!(b = strrchr(tmpname, '_')))
		return 0;

	len = strlen(prefix) + (e - b) + 1;
	if (!(newname = dm_pool_zalloc(cmd->mem, len))) {
		log_error("Failed to allocate new LV name");
		return 0;
	}

	strcpy(newname, prefix);
	strcat(newname, b);
	*lv_name = newname;

	return 1;
}

/*
 * HM Helper:
 *
 * get @index from @lv names string number suffix
 */
static int _lv_name_get_string_index(struct logical_volume *lv, unsigned *index)
{
	char *numptr, *n;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(index, "index argument");

	if (!(numptr = strrchr(lv->name, '_')))
		goto err;

	n = ++numptr;

	while (*n)
		if (!isdigit(*(n++)))
			goto err;

	*index = atoi(numptr);
	return 1;

err:
	log_error("Malformatted image name");
	return 0;
}

/*
 * Shift image @*name (suffix) from @s to (@s - @missing)
 *
 * E.g. s=5, missing=2 -> change "*_r(image,meta)_5" to "*_r(image,meta)_3" 
 * - or -
 *      s=5, missing=2 -> change "*_dup_5_*" to "*_dup_3_*" 
 */
static int __shift_lv_name(char *shift_name, char **name, unsigned s, unsigned missing)
{
	int r = 0;
	unsigned num;
	ssize_t len, len1;
	char *numptr;

	RETURN_IF_ZERO(shift_name, "shift name argument");
	RETURN_IF_ZERO(name, "name argument");

log_very_verbose("Before shifting %s", *name);
	/* Handle duplicating sub LV names */
	if ((numptr = strstr(shift_name, "_dup_")) &&
	    (_strstr_strings(shift_name, "_rimage_", "_rmeta_", NULL))) {
		char *suffix;
log_very_verbose("shifting duplicating sub LV %s", shift_name);

		numptr += strlen("_dup_");
		if ((suffix = strchr(numptr, '_')) &&
		    (num = atoi(numptr)) == s) {
			len = suffix - numptr + 1;
log_very_verbose("shifting duplicating sub LV %s numptr=%s suffix=%s len=%ld", shift_name, numptr, suffix, len);
			if ((len1 = dm_snprintf(numptr, len, "%u", num - missing)) < 0)
				goto out;

			if (len1 < len) {
				strncpy(*name, shift_name, numptr - shift_name + len1);
				strcat(*name, suffix);

			} else
				*name = shift_name;

			r = 1;
		}
log_very_verbose("shifting s=%u num=%u", s, num);

	/* Handle regular (sub) LV names */
	} else {
		if ((numptr = strrchr(shift_name, '_')) &&
		    (num = atoi(numptr + 1)) == s) {
			*name = shift_name;
			len = strlen(++numptr) + 1;
			r = dm_snprintf(numptr, len, "%u", num - missing) < 0 ? 0 : 1;
		}
	}

	log_very_verbose("After shifting %s", *name);
	return r;
out:
	log_error("Malformatted image name");
	return 0;
}

static int _shift_lv_name(struct logical_volume *lv, unsigned s, unsigned missing)
{
	char *shift_name;

	RETURN_IF_ZERO(lv, "lv argument");

	if (!(shift_name = dm_pool_strdup(lv->vg->cmd->mem, lv->name))) {
		log_error("Memory allocation failed.");
		return 0;
	}

	return __shift_lv_name(shift_name, (char **) &lv->name, s, missing);
}

/* Change name of  @lv with # @s to # (@s - @missing) */
static int _shift_image_name(struct logical_volume *lv, unsigned s, unsigned missing)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (lv_is_duplicated(lv) &&
	    (seg_is_raid(seg) || seg_is_mirror(seg))) {
		uint32_t ss;
		struct lv_segment *fseg = first_seg(lv);

		for (ss = 0; ss < fseg->area_count; ss++) {
			if (!_shift_image_name(seg_lv(fseg, ss), s, missing))
				return 0;

			if (fseg->meta_areas &&
			    (seg_metatype(fseg, ss) != AREA_LV ||
			     !_shift_image_name(seg_metalv(fseg, ss), s, missing)))
				return 0;
		}
	}
		
	return _shift_lv_name(lv, s, missing);
}

/*
 * _shift_image_components
 * @seg: Top-level RAID segment
 *
 * Shift all higher indexed segment areas down to fill in gaps where
 * there are 'AREA_UNASSIGNED' areas.
 *
 * We don't need to bother with name reallocation,
 * because the name length will be less or equal
 * when shifting down as opposed to shifting up.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _shift_image_components(struct lv_segment *seg)
{
	uint32_t s, missing;

	RETURN_IF_ZERO(seg, "lv segment argument");

	if (!seg_is_raid(seg))
		return_0;

	RETURN_IF_ZERO(seg->meta_areas, "meta areas");

	/* Should not be possible here, but... */
	if (!_check_max_raid_devices(seg->area_count))
		return 0;

	log_very_verbose("Shifting images in %s", lvseg_name(seg));

	for (s = missing = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) == AREA_UNASSIGNED) {
			RETURN_IF_ZERO(seg_metatype(seg, s) == AREA_UNASSIGNED, " unassigned metadata segment area");
			missing++;

		} else if (missing) {
			RETURN_IF_ZERO(_seg_lv_checked(seg, s), "data lv");
			RETURN_IF_ZERO(_seg_metalv_checked(seg, s), "meta lv");

			log_very_verbose("Shifting %s and %s by %u",
					 seg_metalv(seg, s)->name,
					 seg_lv(seg, s)->name, missing);

			seg->areas[s - missing] = seg->areas[s];
			seg_type(seg, s) = AREA_UNASSIGNED;
			RETURN_IF_ZERO(_shift_image_name(seg_lv(seg, s - missing), s, missing), "data LV name shift")

			seg->meta_areas[s - missing] = seg->meta_areas[s];
			seg_metatype(seg, s) = AREA_UNASSIGNED;
			RETURN_IF_ZERO(_shift_image_name(seg_metalv(seg, s - missing), s, missing), "meta LV name shift")
		}
	}

	seg->area_count -= missing;
	return 1;
}

/* Generate raid subvolume name and validate it */
static char *_generate_raid_name(struct logical_volume *lv,
				 const char *suffix, int count)
{
	const char *format = (count < 0) ? "%s_%s" : "%s_%s%u";
	size_t len = strlen(lv->name) + strlen(suffix) + ((count < 0) ? 2 : 5);
	char *name;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(suffix, "name suffix argument");

	if (!(name = dm_pool_alloc(lv->vg->vgmem, len))) {
		log_error("Failed to allocate new name.");
		return NULL;
	}

	if (dm_snprintf(name, len, format, lv->name, suffix, count) < 0)
		return_NULL;

	if (count < 0)
		name[strlen(name) - 1] = '\0';

	if (!validate_name(name)) {
		log_error("New logical volume name \"%s\" is not valid.", name);
		return NULL;
	}

	if (find_lv_in_vg(lv->vg, name)) {
		dm_pool_free(lv->vg->vgmem, name);
		return NULL;
	}

	return name;
}

/* HM Helper: write, optionally suspend @lv, commit and optionally backup metadata of @vg */
static int _vg_write_lv_suspend_commit_backup(struct volume_group *vg, struct logical_volume *lv, int do_backup)
{
	RETURN_IF_ZERO(vg, "vg argument");

PFL();
	if (!vg_write(vg)) {
		log_error("Write of VG %s failed", vg->name);
		return_0;
	}

PFL();
	if (lv && !suspend_lv(vg->cmd, lv)) {
		log_error("Failed to suspend %s before committing changes",
			  display_lvname(lv));
		vg_revert(lv->vg);
		return 0;
	}

PFL();
	if (!vg_commit(vg)) {
		log_error("Commit of VG %s failed", vg->name);
		return_0;
	}

PFL();
	if (do_backup && !backup(vg))
		log_error("Backup of VG %s failed; continuing", vg->name);

	return 1;
}

static int _vg_write_commit_backup(struct volume_group *vg)
{
	return _vg_write_lv_suspend_commit_backup(vg, NULL, 1);
}

static int _vg_write_commit(struct volume_group *vg)
{
	return _vg_write_lv_suspend_commit_backup(vg, NULL, 0);
}

/* Write vg of @lv, suspend @lv and commit the vg */
static int _vg_write_lv_suspend_vg_commit(struct logical_volume *lv)
{
	RETURN_IF_ZERO(lv, "lv argument");

	return _vg_write_lv_suspend_commit_backup(lv->vg, lv, 0);
}

/*
 * Eliminate the extracted LVs on @removal_lvs from @vg incl. vg write, commit and backup 
 */
static int _eliminate_extracted_lvs_optional_write_vg(struct volume_group *vg,
						      struct dm_list *removal_lvs,
						      int vg_write_requested)
{
	RETURN_IF_ZERO(vg, "vg argument");

	if (!removal_lvs || dm_list_empty(removal_lvs))
		return 1;

PFL();
	if (!_deactivate_and_remove_lvs(vg, removal_lvs))
		return 0;

	sync_local_dev_names(vg->cmd);
	dm_list_init(removal_lvs);
PFL();
	if (vg_write_requested &&
	    !_vg_write_commit_backup(vg))
		return 0;
PFL();
	return 1;
}

static int _eliminate_extracted_lvs(struct volume_group *vg, struct dm_list *removal_lvs)
{
	return _eliminate_extracted_lvs_optional_write_vg(vg, removal_lvs, 1);
}

/*
 * Reallocate segment areas given by @type (i.e. data or metadata areas)
 * in first segment of @lv to amount in @areas copying the minimum of common areas across
 */
static int _realloc_seg_areas(struct logical_volume *lv,
			      uint32_t areas, uint64_t type)
{
	uint32_t s;
	struct lv_segment *seg;
	struct lv_segment_area **seg_areas;
	struct lv_segment_area *new_areas;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(areas, "areas count");

	switch (type) {
	case RAID_META:
		seg_areas = &seg->meta_areas;
		break;
	case RAID_IMAGE:
		seg_areas = &seg->areas;
		break;
	default:
		log_error(INTERNAL_ERROR "Called with bogus type argument");
		return 0;
	}

	if (!(new_areas = dm_pool_zalloc(lv->vg->vgmem, areas * sizeof(*new_areas)))) {
		log_error("Allocation of new areas array failed.");
		return 0;
	}

	for (s = 0; s < areas; s++)
		new_areas[s].type = AREA_UNASSIGNED;

	if (*seg_areas)
		memcpy(new_areas, *seg_areas, min(areas, seg->area_count) * sizeof(*new_areas));

	*seg_areas = new_areas;

	return 1;
}

/*
 * HM
 *
 * Reallocate both data and metadata areas of first segment of segment of @lv to new amount in @areas
 */
static int _realloc_meta_and_data_seg_areas(struct logical_volume *lv, uint32_t areas)
{
	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(areas, "areas count");

	return (_realloc_seg_areas(lv, areas, RAID_META) &&
		_realloc_seg_areas(lv, areas, RAID_IMAGE)) ? 1 : 0;
}

/*
 * _extract_image_component
 * @seg
 * @idx:  The index in the areas array to remove
 * @data: != 0 to extract data dev / 0 extract metadata_dev
 * @extracted_lv:  The displaced metadata/data LV
 * @set_error_seg: if set, replace LV of @type at @idx with error segment
 */
static int _extract_image_component_error_seg(struct lv_segment *seg,
					      uint64_t type, uint32_t idx,
					      struct logical_volume **extracted_lv,
					      int set_error_seg)
{
	struct logical_volume *lv;

	RETURN_IF_ZERO(seg, "lv segment argument");
	RETURN_IF_ZERO(extracted_lv, "extracted LVs argument");
	RETURN_IF_NONZERO(set_error_seg < 0 || set_error_seg > 1,
			  "set error segment argument");

	switch (type) {
		case RAID_META:
			lv = seg_metalv(seg, idx);
			seg_metalv(seg, idx) = NULL;
			seg_metatype(seg, idx) = AREA_UNASSIGNED;
			break;
		case RAID_IMAGE:
			lv = seg_lv(seg, idx);
			seg_lv(seg, idx) = NULL;
			seg_type(seg, idx) = AREA_UNASSIGNED;
			break;
		default:
			log_error(INTERNAL_ERROR "Bad type provided to %s.", __func__);
			return 0;
	}

	RETURN_IF_ZERO(lv, "sub lv");

	log_very_verbose("Extracting image component %s from %s", lv->name, lvseg_name(seg));
	lv->status &= ~(type | RAID);
	lv_set_visible(lv);

	/* remove reference from @seg to @lv */
	if (!remove_seg_from_segs_using_this_lv(lv, seg))
		return_0;

	if (!(lv->name = _generate_raid_name(lv, "extracted_", -1)))
		return_0;

PFLA("set_error_seg=%d", set_error_seg);
	if (set_error_seg &&
	    !replace_lv_with_error_segment(lv))
		return_0;

	*extracted_lv = lv;

	return 1;
}

static int _extract_image_component(struct lv_segment *seg,
				    uint64_t type, uint32_t idx,
				    struct logical_volume **extracted_lv,
				    int set_error_seg)
{
	RETURN_IF_ZERO(seg, "lv segment argument");
	RETURN_IF_ZERO(extracted_lv, "extracted LVs argument");
	RETURN_IF_NONZERO(set_error_seg < 0 || set_error_seg > 1, "set error segment argument");

	return _extract_image_component_error_seg(seg, type, idx, extracted_lv, set_error_seg);
}

/*
 * @seg
 * @idx:  The index in the areas array to remove
 * @lvl_array: The displaced metadata + data LVs
 *
 * These functions extracts _one_  image component pair - setting the respective
 * 'lvl_array' pointers.  It appends '_extracted' to the LVs' names, so that
 * there are not future conflicts.  It does /not/ commit the results.
 * (IOW, erroring-out requires no unwinding of operations.)
 *
 * This function does /not/ attempt to:
 *    1) shift the 'areas' or 'meta_areas' arrays.
 *       The '[meta_]areas' are left as AREA_UNASSIGNED.
 *    2) Adjust the seg->area_count
 *    3) Name the extracted LVs appropriately (appends '_extracted' to names)
 * These actions must be performed by the caller.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _extract_image_component_pair(struct lv_segment *seg, uint32_t idx,
					 struct lv_list *lvl_array,
					 struct dm_list *extracted_meta_lvs,
					 struct dm_list *extracted_data_lvs,
					 int set_error_seg)
{
	RETURN_IF_ZERO(seg, "lv segment argument");
	RETURN_IF_ZERO(extracted_meta_lvs, "extracted meta LVs list argument");
	RETURN_IF_ZERO(extracted_data_lvs, "extracted data LVs list argument");
	RETURN_IF_NONZERO(set_error_seg < 0 || set_error_seg > 1, "set error segment argument");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, idx);

	/* Don't change extraction sequence; callers are relying on it */
	if (extracted_meta_lvs) {
		if (!_extract_image_component(seg, RAID_META, idx, &lvl_array[0].lv, set_error_seg))
			return_0;

		dm_list_add(extracted_meta_lvs, &lvl_array[0].list);
	}

	if (extracted_data_lvs) {
		if (!_extract_image_component(seg, RAID_IMAGE, idx, &lvl_array[1].lv, set_error_seg))
			return_0;

		dm_list_add(extracted_data_lvs, &lvl_array[1].list);
	}

	return 1;
}

/*
 * Extract all sub LVs of @type from @seg starting at @idx excluding @end and
 * put them on @removal_lvs setting mappings to "error" if @error_seg
 */
static int _extract_image_component_sublist(struct lv_segment *seg,
					    uint64_t type, uint32_t idx, uint32_t end,
					    struct dm_list *removal_lvs,
					    int error_seg)
{
	uint32_t s;
	struct lv_list *lvl;

	RETURN_IF_ZERO(seg, "seg argument");
PFLA("seg->area_count=%u end=%u idx=%u", seg->area_count, end, idx);
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, idx);
	RETURN_IF_NONZERO(end > seg->area_count || end <= idx, "area index wrong for segment");
	RETURN_IF_ZERO(seg_lv(seg, idx), "seg LV");
	if (!(lvl = dm_pool_alloc(seg_lv(seg, idx)->vg->vgmem, sizeof(*lvl) * (end - idx))))
		return_0;

	for (s = idx; s < end; s++) {
		if (!_extract_image_component_error_seg(seg, type, s, &lvl->lv, error_seg))
			return 0;

		dm_list_add(removal_lvs, &lvl->list);
		lvl++;
	}

	if (!idx && end == seg->area_count) {
		if (type == RAID_IMAGE)
			seg->areas = NULL;
		else
			seg->meta_areas = NULL;
	}

	return 1;
}

/* Extract all sub LVs of @type from @seg starting with @idx and put them on @removal_Lvs */
static int _extract_image_component_list(struct lv_segment *seg,
					 uint64_t type, uint32_t idx,
					 struct dm_list *removal_lvs)
{
	return _extract_image_component_sublist(seg, type, idx, seg->area_count, removal_lvs, 1);
}

/* Add new @lvs to @lv at @area_offset */
static int _add_image_component_list(struct lv_segment *seg, int delete_from_list,
				     uint64_t lv_flags, struct dm_list *lvs, uint32_t area_offset)
{
	uint32_t s = area_offset;
	struct lv_list *lvl, *tmp;

	dm_list_iterate_items_safe(lvl, tmp, lvs) {
		if (delete_from_list)
			dm_list_del(&lvl->list);

		if (lv_flags & VISIBLE_LV)
			lv_set_visible(lvl->lv);
		else
			lv_set_hidden(lvl->lv);

		if (lv_flags & LV_REBUILD)
			lvl->lv->status |= LV_REBUILD;
		else
			lvl->lv->status &= ~LV_REBUILD;

		if (!set_lv_segment_area_lv(seg, s++, lvl->lv, 0 /* le */, lvl->lv->status)) {
			log_error("Failed to add sublv %s", lvl->lv->name);
			return 0;
		}
	}

	return 1;
}

/*
 * Create an LV of specified type.  Set visible after creation.
 * This function does not make metadata changes.
 */
static struct logical_volume *_alloc_striped_image_component(struct logical_volume *lv,
							     const char *alt_base_name,
							     struct alloc_handle *ah,
							     uint32_t first_area, uint32_t stripes,
							     uint64_t type)
{
	uint64_t status = RAID | LVM_READ | LVM_WRITE | type;
	char img_name[NAME_LEN];
	const char *type_suffix;
	struct logical_volume *r;
	const struct segment_type *segtype;

	switch (type) {
		case RAID_META:
			type_suffix = "rmeta";
			break;
		case RAID_IMAGE:
			type_suffix = "rimage";
			status |= LV_REBUILD;
			break;
		default:
			log_error(INTERNAL_ERROR "Bad type provided to %s.", __func__);
			return 0;
	}

	if (dm_snprintf(img_name, sizeof(img_name), "%s_%s_%%d",
				alt_base_name ?: lv->name, type_suffix) < 0)
		return_0;

	if (!(r = lv_create_empty(img_name, NULL, status, ALLOC_INHERIT, lv->vg))) {
		log_error("Failed to allocate new raid component, %s.", img_name);
		return 0;
	}

	/* If no allocation requested, leave it to the empty LV (needed for striped -> raid0 takeover) */
	if (ah) {
		if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
			return_0;

		if (!lv_add_segment(ah, first_area, stripes /* areas */, 1 /* data_copies */,
				    r, segtype, 0, status, 0)) {
			log_error("Failed to add segment to LV, %s", img_name);
			return 0;
		}
	}

	lv_set_visible(r);

	return r;
}

static struct logical_volume *_alloc_image_component(struct logical_volume *lv, const char *alt_base_name,
						     struct alloc_handle *ah, uint32_t first_area,
						     uint64_t type)
{
	return _alloc_striped_image_component(lv, alt_base_name, ah, first_area, 1 /* stripes */, type);
}

/* Calculate absolute amount of metadata device extens based on @rimage_extents, @region_size and @extens_size */
static uint32_t _raid_rmeta_extents(struct cmd_context *cmd, uint32_t rimage_extents,
				    uint32_t region_size, uint32_t extent_size)
{
	uint64_t bytes, regions, sectors;

	region_size = region_size ?: get_default_region_size(cmd);
	regions = (uint64_t) rimage_extents * extent_size / region_size;

	/* raid and bitmap superblocks + region bytes */
	bytes = 2 * 4096 + dm_div_up(regions, 8);
	sectors = dm_div_up(bytes, 512);

	return dm_div_up(sectors, extent_size);
}

/*
 * Returns raid metadata device size _change_ in extents, algorithm from dm-raid ("raid" target) kernel code.
 */
uint32_t raid_rmeta_extents_delta(struct cmd_context *cmd,
		uint32_t rimage_extents_cur, uint32_t rimage_extents_new,
		uint32_t region_size, uint32_t extent_size)
{
	uint32_t rmeta_extents_cur = _raid_rmeta_extents(cmd, rimage_extents_cur, region_size, extent_size);
	uint32_t rmeta_extents_new = _raid_rmeta_extents(cmd, rimage_extents_new, region_size, extent_size);

PFLA("rimage_extents_cur=%u rmeta_extents_cur=%u rimage_extents_new=%u rmeta_extents_new=%u region_size=%u extent_size=%u", rimage_extents_cur, rmeta_extents_cur,  rimage_extents_new, rmeta_extents_new, region_size, extent_size);
	/* Need minimum size on LV creation */
	if (!rimage_extents_cur)
		return rmeta_extents_new;

	/* Need current size on LV deletion */
	if (!rimage_extents_new)
		return rmeta_extents_cur;

	if (rmeta_extents_new == rmeta_extents_cur)
		return 0;

	/* Extending/reducing... */
	return rmeta_extents_new > rmeta_extents_cur ?
		rmeta_extents_new - rmeta_extents_cur :
		rmeta_extents_cur - rmeta_extents_new;
}

/*
 * __alloc_rmeta_for_lv
 * @data_lv
 *
 * Allocate  RAID metadata device for the given LV (which is or will
 * be the associated RAID data device).
 */
static int __alloc_rmeta_for_lv(struct logical_volume *data_lv,
				struct logical_volume **meta_lv,
				struct dm_list *allocate_pvs)
{
	int r = 1;
	uint32_t rmeta_extents;
	char *lv_name;
	struct alloc_handle *ah;
	struct lv_segment *seg;
	struct dm_list pvs;
	struct segment_type *striped_segtype;

	RETURN_IF_LV_SEG_ZERO(data_lv, (seg = first_seg(data_lv)));
	RETURN_IF_ZERO(meta_lv, "mate LV argument");
	RETURN_IF_ZERO((striped_segtype = get_segtype_from_string(data_lv->vg->cmd, SEG_TYPE_NAME_STRIPED)),
		       "striped segtype");

	if (allocate_pvs) {
		RETURN_IF_NONZERO(dm_list_empty(allocate_pvs), "allocate pvs listed");

	} else {
		allocate_pvs = &pvs;
		dm_list_init(allocate_pvs);
		if (!get_pv_list_for_lv(data_lv->vg->cmd->mem,
					data_lv, allocate_pvs)) {
			log_error("Failed to build list of PVs for %s", display_lvname(data_lv));
			return 0;
		}
	}

	if (!_check_and_init_region_size(data_lv))
		return 0;

	if (!(lv_name = _top_level_lv_name(data_lv)))
		return 0;

PFLA("lv_name=%s", lv_name);
	rmeta_extents = _raid_rmeta_extents(data_lv->vg->cmd, data_lv->le_count,
					    0 /* region_size */, data_lv->vg->extent_size);
	if (!(ah = allocate_extents(data_lv->vg, NULL, striped_segtype,
				    0 /* stripes */, 1 /* mirrors */,
				    0 /* log_count */ , 0 /* region_size */, rmeta_extents,
				    allocate_pvs, data_lv->alloc, 0, NULL)))
		return_0;

	if ((*meta_lv = _alloc_image_component(data_lv, lv_name, ah, 0, RAID_META))) {
		/*
		 * Wipe metadata device at beginning in order to avoid
		 * discovering a valid, but unrelated superblock in the kernel.
		 */
		if (!_clear_lv(*meta_lv))
			r = 0;

	} else
		r = 0;

	alloc_destroy(ah);

	return r;
}

/* HM Helper: try coallocating a meta image with ithe data image LV first and if that fails, use @allocate_pvs */
static int _alloc_rmeta_for_lv(struct logical_volume *data_lv,
			       struct logical_volume **meta_lv,
			       struct dm_list *allocate_pvs)
{
	RETURN_IF_LV_SEG_ZERO(data_lv, first_seg(data_lv));
	RETURN_IF_ZERO(meta_lv, "meta_lv argument");

	if (__alloc_rmeta_for_lv(data_lv, meta_lv, NULL))
		return 1;

	return __alloc_rmeta_for_lv(data_lv, meta_lv, allocate_pvs);
}

/* HM Helper: allocate a metadata LV for @data_lv, set hidden and set @*meta_lv to it */
static int _alloc_rmeta_for_lv_add_set_hidden(struct logical_volume *lv, uint32_t area_offset,
					      struct dm_list *allocate_pvs)
{
	struct lv_segment *seg;
	struct dm_list meta_lvs;
	struct lv_list lvl;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	dm_list_init(&meta_lvs);

	if (!_alloc_rmeta_for_lv(seg_lv(seg, area_offset), &lvl.lv, allocate_pvs))
		return 0;

	dm_list_add(&meta_lvs, &lvl.list);
	lv_set_hidden(lvl.lv);

	return _add_image_component_list(seg, 1, 0, &meta_lvs, area_offset);
}

/*
 * HM
 *
 * Allocate metadata devs for all @new_data_devs and link them to list @new_meta_lvs
 */
static int _alloc_rmeta_devs_for_rimage_devs(struct logical_volume *lv,
					     struct dm_list *new_data_lvs,
					     struct dm_list *new_meta_lvs,
					     struct dm_list *allocate_pvs)
{
	uint32_t a = 0, raid_devs = 0;
	struct dm_list *l;
	struct lv_list *lvl, *lvl1, *lvl_array;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(new_data_lvs, "new data LVs argument");
	RETURN_IF_NONZERO(dm_list_empty(new_data_lvs), "new data LVs listed");
	RETURN_IF_ZERO(new_meta_lvs, "new meta LVs argument");
	RETURN_IF_NONZERO(!dm_list_empty(new_meta_lvs), "new meta LVs may be listed");

	dm_list_iterate(l, new_data_lvs)
		raid_devs++;

PFLA("lv=%s raid_devs=%u", display_lvname(lv), raid_devs);

	if (!raid_devs)
		return 0;

	if (!(lvl_array = dm_pool_zalloc(lv->vg->vgmem, raid_devs * sizeof(*lvl_array))))
		return 0;

	dm_list_iterate_items(lvl, new_data_lvs) {
		log_debug_metadata("Allocating new metadata LV for %s",
				lvl->lv->name);
		if (!_alloc_rmeta_for_lv(lvl->lv, &lvl_array[a].lv, allocate_pvs)) {
			log_error("Failed to allocate metadata LV for %s in %s",
					lvl->lv->name, lv->vg->name);
			return 0;
		}

		dm_list_add(new_meta_lvs, &lvl_array[a].list);
		
		dm_list_iterate_items(lvl1, new_meta_lvs)
			if (!_avoid_pvs_with_other_images_of_lv(lvl1->lv, allocate_pvs))
				return 0;

		a++;
	}

	_allow_pvs(allocate_pvs);

	return 1;
}

/*
 * HM
 *
 * Allocate metadata devs for all data devs of an LV
 */
static int _alloc_rmeta_devs_for_lv(struct logical_volume *lv,
				    struct dm_list *meta_lvs,
				    struct dm_list *allocate_pvs)
{
	uint32_t s;
	struct lv_list *lvl_array;
	struct dm_list data_lvs;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(meta_lvs, "mate LVs list argument");
	RETURN_IF_NONZERO(seg->meta_areas, "meta LVs may exist");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);

	dm_list_init(&data_lvs);

	if (!(seg->meta_areas = dm_pool_zalloc(lv->vg->vgmem,
					       seg->area_count * sizeof(*seg->meta_areas))))
		return 0;

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem, seg->area_count * sizeof(*lvl_array))))
		return_0;

	for (s = 0; s < seg->area_count; s++) {
		lvl_array[s].lv = seg_lv(seg, s);
		dm_list_add(&data_lvs, &lvl_array[s].list);
	}

	if (!_alloc_rmeta_devs_for_rimage_devs(lv, &data_lvs, meta_lvs, allocate_pvs)) {
		log_error("Failed to allocate metadata LVs for %s", lv->name);
		return 0;
	}

	return 1;
}

/* Get total area len of @lv, i.e. sum of area_len of all segments */
static uint32_t _lv_total_rimage_len(struct logical_volume *lv)
{
	uint32_t s;
	struct lv_segment *seg = first_seg(lv);

	if (seg_is_raid(seg)) {
		for (s = 0; s < seg->area_count; s++)
			if (_seg_lv_checked(seg, s))
				return seg_lv(seg, s)->le_count;
	} else
		return lv->le_count;

	return_0;
}

/* HM Helper: allocate count number of extents in parallel areas to optional @lv, optionally @stripes striped */
static struct alloc_handle *_allocate_extents(struct logical_volume *lv,
					      uint32_t count,
					      uint32_t metadata_area_count,
					      uint32_t region_size,
					      uint32_t stripes,
					      uint32_t stripe_size,
					      uint32_t extents,
					      struct dm_list *allocate_pvs)
{
	struct dm_list *parallel_areas;
	const struct segment_type *segtype;

	/* lv may be naught */
	RETURN_IF_ZERO(count, "count requested");
	RETURN_IF_NONZERO(metadata_area_count > count, "valid metadata_area_count requested");
	RETURN_IF_ZERO(stripes, "stripes requested");
	RETURN_IF_NONZERO(stripe_size & (stripe_size - 1), "valid stripe size");
	/* extents may be naught */
	RETURN_IF_ZERO(allocate_pvs , "allocate pvs list argument");
	RETURN_IF_NONZERO(dm_list_empty(allocate_pvs), "allocate pvs listed");
	RETURN_IF_NONZERO(stripes == 1 && stripe_size, "stripes == 1 and stripe_size != 0");

	/*
	 * The number of extents is based on the RAID type.  For RAID1,
	 * each of the rimages is the same size - 'le_count'.  However
	 * for RAID 0/4/5/6/10, the stripes add together (NOT including
	 * any parity devices) to equal 'le_count'.  Thus, when we are allocating
	 * individual devices, we must specify how large the individual device
	 * is along with the number we want ('count').
	 */
	if (lv) {
		if (!(parallel_areas = build_parallel_areas_from_lv(lv, 0, 1)))
			return_NULL;
	} else
		parallel_areas = allocate_pvs;

	/* Amount of extents for the rimage device(s) */
	extents = extents ?: _lv_total_rimage_len(lv);

	/* Use raid1 segtype for allocation to get images of the same size as the given ones in @lv */
	if (!(segtype = (stripes > 1) ? get_segtype_from_flag(lv->vg->cmd, SEG_RAID01) :
					get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return_NULL;

PFLA("extents=%u", extents);
	return allocate_extents(lv->vg, NULL, segtype,
				stripes, count /* mirrors */,
				metadata_area_count,
				region_size, extents / stripes,
				allocate_pvs, lv->alloc, 0, parallel_areas);
}

/*
 * Create @count new image component pairs for @lv and return them in
 * @new_meta_lvs and @new_data_lvs allocating space if @allocate is set.
 *
 * Use @pvs list for allocation if set, else just create empty image LVs.
 */
static int _alloc_striped_image_components(struct logical_volume *lv,
					   uint32_t count,
					   uint32_t metadata_area_count,
					   uint32_t stripes,
					   uint32_t stripe_size,
					   uint32_t extents,
					   struct dm_list *meta_lvs,
					   struct dm_list *data_lvs,
					   struct dm_list *allocate_pvs)
{
	int r = 0;
	uint32_t m, region_size, s;
	struct alloc_handle *ah;
	// struct dm_list *parallel_areas;
	struct lv_list *lvl_array;

	RETURN_IF_ZERO(lv, "LV argument");
	RETURN_IF_NONZERO(!meta_lvs && !data_lvs, "data and no meta LVs list argument");

	if (!(lvl_array = dm_pool_alloc(lv->vg->vgmem, 2 * count * sizeof(*lvl_array))))
		return_0;

	if (!_check_and_init_region_size(lv))
		return 0;

	region_size = first_seg(lv)->region_size;
PFL();
	/*
	 * The number of extents is based on the RAID type.  For RAID1,
	 * each of the rimages is the same size - 'le_count'.  However
	 * for RAID 0/4/5/6/10, the stripes add together (NOT including
	 * any parity devices) to equal 'le_count'.  Thus, when we are allocating
	 * individual devices, we must specify how large the individual device
	 * is along with the number we want ('count').
	 */
	if (allocate_pvs && !dm_list_empty(allocate_pvs)) {
		if (!(ah = _allocate_extents(lv, count, metadata_area_count, region_size,
					     stripes, stripe_size, extents, allocate_pvs)))
			return 0;

	} else
		ah = NULL;

PFLA("count=%u extents=%u", count, extents);

	for (s = 0, m = count * stripes; s < count; s++, m++) {
		/*
		 * The allocation areas are grouped together.  First
		 * come the rimage allocated areas, then come the metadata
		 * allocated areas.  Thus, the metadata areas are pulled
		 * from 's + count'.
		 */

		/*
		 * If the segtype is raid0, we may avoid allocating metadata LVs
		 * to accompany the data LVs by not passing in @meta_lvs
		 */
		if (meta_lvs) {
			if (!(lvl_array[s + count].lv = _alloc_image_component(lv, NULL, ah, m, RAID_META)))
				goto_bad;

			dm_list_add(meta_lvs, &lvl_array[s + count].list);
		}

		if (data_lvs) {
			if (!(lvl_array[s].lv = _alloc_striped_image_component(lv, NULL, ah, s * stripes, stripes, RAID_IMAGE)))
				goto_bad;

			dm_list_add(data_lvs, &lvl_array[s].list);
			if (first_seg(lvl_array[s].lv))
				first_seg(lvl_array[s].lv)->stripe_size = stripe_size;
		}
	}

	r = 1;
bad:
	if (ah)
		alloc_destroy(ah);

	return r;
}

static int _alloc_image_components(struct logical_volume *lv,
				   uint32_t count,
				   struct dm_list *meta_lvs,
				   struct dm_list *data_lvs,
				   struct dm_list *allocate_pvs)
{
	return _alloc_striped_image_components(lv, count, count, 1, 0, 0, meta_lvs, data_lvs, allocate_pvs);
}

/*
 * _raid_extract_images
 * @lv
 * @new_image_count:  The absolute count of images (e.g. '2' for a 2-way mirror)
 * @target_pvs:  The list of PVs that are candidates for removal
 * @shift:  If set, use _shift_image_components().
 *          Otherwise, leave the [meta_]areas as AREA_UNASSIGNED and
 *          seg->area_count unchanged.
 * @extracted_[meta|data]_lvs:  The LVs removed from the array.  If 'shift'
 *                              is set, then there will likely be name conflicts.
 * This function extracts _both_ portions of the indexed image.  It
 * does /not/ commit the results.  (IOW, erroring-out requires no unwinding
 * of operations.)
 *
 * Returns: 1 on success, 0 on failure
 */
static int _raid_extract_images(struct logical_volume *lv, uint32_t new_image_count,
				struct dm_list *target_pvs, int shift,
				struct dm_list *extracted_meta_lvs,
				struct dm_list *extracted_data_lvs)
{
	int inc;
	unsigned s, extract;
	struct lv_list *lvl_pairs;
	struct lv_segment *seg;
	struct segment_type *error_segtype;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(target_pvs, "target pvs list argument");
	RETURN_IF_ZERO(extracted_meta_lvs, "extracted meta LVs list argument");
	RETURN_IF_ZERO(extracted_data_lvs, "extracted data LVs list argument");

	extract = seg->area_count - new_image_count;

	if ((s = dm_list_size(target_pvs)) < extract) {
		log_error("Unable to remove %d images:  Only %d device%s given.",
			  extract, s, s == 1 ? "" : "s");
		return 0;
	}

	log_verbose("Extracting %u image%s from %s", extract,
		    extract > 1 ? "s" : "", display_lvname(lv));

	if (!(lvl_pairs = dm_pool_alloc(lv->vg->vgmem, 2 * extract * sizeof(*lvl_pairs))))
		return_0;
PFL();

	if (!(error_segtype = get_segtype_from_string(lv->vg->cmd, "error")))
		return_0;
PFL();

	/*
	 * We make two passes over the devices.
	 * - The first pass we look for error LVs to handle them first
	 * - The second pass we look for PVs that match target_pvs and extract them
	 */
	/* First pass */
	for (s = seg->area_count; s-- && extract; ) {
PFLA("s=%u", s);
		/* Conditions for first pass */
		if (!((seg->meta_areas && first_seg(seg_metalv(seg, s))->segtype == error_segtype) ||
		      first_seg(seg_lv(seg, s))->segtype == error_segtype))
			continue;
PFL();
		if (!dm_list_empty(target_pvs) && target_pvs != &lv->vg->pvs) {
			/*
			 * User has supplied a list of PVs, but we
			 * cannot honor that list because error LVs
			 * must come first.
			 */
			log_error("%s has components with error targets"
				  " that must be removed first: %s.",
				  display_lvname(lv), display_lvname(seg_lv(seg, s)));
			log_error("Try removing the PV list and rerun the command.");
			return 0;
		}

PFL();
		log_debug("LVs with error segments to be removed: %s %s",
			  display_lvname(seg_metalv(seg, s)), display_lvname(seg_lv(seg, s)));
PFL();
		if (!_extract_image_component_pair(seg, s, lvl_pairs, extracted_meta_lvs, extracted_data_lvs, 0))
			return_0;

		lvl_pairs += 2;
		extract--;
	}

	/* Second pass */
	for (s = seg->area_count; target_pvs && s-- && extract; ) {
		/* Conditions for second pass */
		if (!_raid_in_sync(lv) &&
		    (!seg_is_mirrored(seg) || !s)) {
			log_error("Unable to extract %sRAID image"
				  " while RAID array is not in-sync",
				  seg_is_mirrored(seg) ? "primary " : "");
			return 0;
		}

		inc = 0;
#if 1
		if (seg->meta_areas &&
 		    lv_is_on_pvs(seg_metalv(seg, s), target_pvs)) {
#else
		/* HM FIXME: PARTIAL_LV not set for LVs on replacement PVs ("lvconvert --replace $PV $LV") */
		if (seg->meta_areas &&
		    (seg_metalv(seg, s)->status & PARTIAL_LV) &&
 		    lv_is_on_pvs(seg_metalv(seg, s), target_pvs)) {
#endif
			if (!_extract_image_component(seg, RAID_META, s, &lvl_pairs[0].lv, 0))
				return_0;

			dm_list_add(extracted_meta_lvs, &lvl_pairs[0].list);
			inc++;
		}

#if 1
		if (lv_is_on_pvs(seg_lv(seg, s), target_pvs)) {
#else
		/* HM FIXME: PARTIAL_LV not set for LVs on replacement PVs ("lvconvert --replace $PV $LV") */
		if ((seg_lv(seg, s)->status & PARTIAL_LV) &&
		    lv_is_on_pvs(seg_lv(seg, s), target_pvs)) {
#endif
			if (!_extract_image_component(seg, RAID_IMAGE, s, &lvl_pairs[1].lv, 0))
				return_0;

			dm_list_add(extracted_data_lvs, &lvl_pairs[1].list);
			inc++;
		}

		if (inc) {
			lvl_pairs += 2;
			extract--;
		}
	}

	if (extract) {
		log_error("Unable to extract enough images to satisfy request");
		return 0;
	}

#if 1
	if (shift) {
	}
#else
	if (shift && !_shift_image_components(seg)) {
		log_error("Failed to shift and rename image components");
		return 0;
	}
#endif

	return 1;
}

/*
 * Change the image count of the raid @lv to @new_image_count
 * allocating from list @allocate_pvs and putting any removed
 * LVs on the @removal_lvs list
 */
static int _lv_change_image_count(struct logical_volume *lv,
				  uint32_t new_image_count,
				  struct dm_list *allocate_pvs,
				  struct dm_list *removal_lvs)
{
	struct lv_segment *seg;
	struct dm_list meta_lvs, data_lvs;
	uint32_t old_image_count;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(new_image_count, "new image count");

	old_image_count = seg->area_count;
	if (old_image_count == new_image_count) {
		log_print_unless_silent("%s already has image count of %d.",
					display_lvname(lv), new_image_count);
		return 1;
	}

	/* Check for maximum supported raid devices */
	if (!_check_max_raid_devices(new_image_count))
		return 0;

	dm_list_init(&meta_lvs);
	dm_list_init(&data_lvs);

	if (old_image_count < new_image_count) {
		/* Allocate additional meta and data LVs pair(s) */
		RETURN_IF_ZERO(allocate_pvs , "allocate pvs list argument");
		RETURN_IF_NONZERO(dm_list_empty(allocate_pvs), "allocate pvs listed");

		log_debug_metadata("Allocating additional data and metadata LV pair%s for LV %s",
				   new_image_count - old_image_count > 1 ? "s" : "", display_lvname(lv));
		if (!_alloc_image_components(lv, new_image_count - old_image_count,
					     &meta_lvs, &data_lvs, allocate_pvs)) {
			log_error("Failed to allocate additional data and metadata LV pair for %s", display_lvname(lv));
			return_0;
		}

		log_debug_metadata("Clearing newly allocated metadata LVs of %s", display_lvname(lv));
		if (!_clear_lvs(&meta_lvs)) {
			log_error("Failed to clear newly allocated metadata LVs of %s", display_lvname(lv));
			return_0;
		}

		/* Grow areas arrays for metadata and data devs for adding ne image component pairs */
		log_debug_metadata("Reallocating areas arrays of %s", display_lvname(lv));
		if (!_realloc_meta_and_data_seg_areas(lv, new_image_count)) {
			log_error("Relocation of areas arrays for %s failed", display_lvname(lv));
			return_0;
		}

		log_debug_metadata("Adding new data and metadata LVs to %s", display_lvname(lv));
		if (!_add_image_component_list(seg, 1, 0, &meta_lvs, old_image_count) ||
		    !_add_image_component_list(seg, 1, LV_REBUILD, &data_lvs, old_image_count)) {
			log_error("Failed to add new data and metadata LVs to %s", display_lvname(lv));
			return_0;
		} 


	} else {
		RETURN_IF_ZERO(removal_lvs, "removal LVs list argument");

		/*
		 * Extract all image and any metadata LVs past new_image_count 
		 *
		 * No need to reallocate data and metadata areas
		 * on reduction of image component pairs
		 */
		log_debug_metadata("Extracting data and metadata LVs from %s", display_lvname(lv));
		if (!_raid_extract_images(lv, new_image_count, allocate_pvs,
					  0 /* Don't shift */, removal_lvs, removal_lvs)) {
			log_error("Failed to extract data and metadata LVs from %s", display_lvname(lv));
			return 0;
		}
	}

	/* Must update area count after resizing it */
	seg->area_count = new_image_count;

	if (old_image_count < new_image_count)
		/* Set any reshape length on new sub LV */
		_lv_set_reshape_len(lv, _reshape_len_per_dev(seg));


	return 1;
}

/*
 * Relocate @out_of_place_les_per_disk from @lv's data images  begin <-> end depending on @where
 *
 * @where:
 * alloc_begin: end -> begin
 * alloc_end:   begin -> end
 */
enum alloc_where { alloc_begin, alloc_end, alloc_anywhere, alloc_none };
static int _lv_relocate_reshape_space(struct logical_volume *lv, enum alloc_where where)
{
	uint32_t le, begin, end, s;
	struct logical_volume *dlv;
	struct dm_list *insert;
	struct lv_segment *data_seg, *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(_reshape_len_per_dev(seg), "reshape space to relocate");

	/*
	 * Move the reshape LEs of each stripe (i.e. the data image sub lv)
	 * in the first/last segment(s) across to the opposite end of the
	 * address space
	 */
PFLA("seg->area_count=%u", seg->area_count);
	for (s = 0; s < seg->area_count; s++) {
		RETURN_IF_ZERO((dlv = _seg_lv_checked(seg, s)), "sub lv");

		switch (where) {
		case alloc_begin:
			/* Move to the beginning -> start moving to the beginning from "end - reshape LEs" to end  */
			begin = dlv->le_count - _reshape_len_per_dev(seg);
			end = dlv->le_count;
			break;
		case alloc_end:
			/* Move to the end -> start moving to the end from 0 and end with reshape LEs */
			begin = 0;
			end = _reshape_len_per_dev(seg);
			break;
		default:
			log_error(INTERNAL_ERROR "bogus reshape space reallocation request [%d]", where);
			return 0;
		}

		/* Ensure segment boundary at begin/end of reshape space */
		if (!lv_split_segment(dlv, begin ?: end))
			return_0;

		/* Select destination to move to (begin/end) */
		insert = begin ? dlv->segments.n : &dlv->segments;
		RETURN_IF_ZERO((data_seg = find_seg_by_le(dlv, begin)), "data segment found");
		le = begin;
		while (le < end) {
			struct dm_list *n = data_seg->list.n;

			le += data_seg->len;

			dm_list_move(insert, &data_seg->list);

			/* If moving to the begin, adjust insertion point so that we don't reverse order */
			if (begin)
				insert = data_seg->list.n;

			data_seg = dm_list_item(n, struct lv_segment);
		}

		le = 0;
		dm_list_iterate_items(data_seg, &dlv->segments) {
			data_seg->reshape_len = le ? 0 : _reshape_len_per_dev(seg);
			data_seg->le = le;
			le += data_seg->len;
		}
	}

	return 1;
}

/*
 * Check if we've got out of space reshape
 * capacity in @lv and allocate if necessary.
 *
 * We inquire the targets status interface to retrieve
 * the current data_offset and the device size and
 * compare that to the size of the component image LV
 * to tell if an extension of the LV is needed or
 * existing space can just be used,
 *
 * Three different scenarios need to be covered:
 *
 *  - we have to reshape forwards
 *    (true for adding disks to a raid set) ->
 *    add extent to each component image upfront
 *    or move an existing one at the end across;
 *    kernel will set component devs data_offset to
 *    the passed in one and new_data_offset to 0,
 *    i.e. the data starts at offset 0 after the reshape
 *
 *  - we have to reshape backwards
 *    (true for removing disks form a raid set) ->
 *    add extent to each component image by the end
 *    or use already existing one from a previous reshape;
 *    kernel will leave the data_offset of each component dev
 *    at 0 and set new_data_offset to the passed in one,
 *    i.e. the data will be at offset new_data_offset != 0
 *    after the reshape
 *
 *  - we are free to reshape either way
 *    (true for layout changes keeping number of disks) ->
 *    let the kernel identify free out of place reshape space
 *    and select the appropriate data_offset and 
 *    reshape direction
 *
 * Kernel will always be told to put data offset
 * on an extent boundary.
 * When we convert to mappings outside MD ones such as linear,
 * striped and mirror _and_ data_offset != 0, split the first segment
 * and adjust the rest to remove the reshape space.
 * If it's at the end, just lv_reduce() and set seg->reshape_len to 0.
 *
 * Does not write metadata!
 */
static int _lv_alloc_reshape_space(struct logical_volume *lv,
				   enum alloc_where where,
				   enum alloc_where *where_it_was,
				   struct dm_list *allocate_pvs)
{
	uint32_t out_of_place_les_per_disk;
	uint64_t data_offset;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(seg->stripe_size, "bogus stripe size zero");

	/* Ensure min out-of-place reshape space 1 MiB */
	out_of_place_les_per_disk = max(2048U, (unsigned) seg->stripe_size);
	out_of_place_les_per_disk = (uint32_t) max(out_of_place_les_per_disk / (unsigned long long) lv->vg->extent_size, 1ULL);

	/* Get data_offset and dev_sectors from the kernel */
	/* FIXME: dev_sectors superfluous? */
	if (!lv_raid_data_offset(lv, &data_offset)) {
		log_error("Can't get data offset and dev size for %s from kernel",
			  display_lvname(lv));
		return 0;
	}

PFLA("data_offset=%llu seg->reshape_len=%u out_of_place_les_per_disk=%u lv->le_count=%u", (unsigned long long) data_offset, seg->reshape_len, out_of_place_les_per_disk, lv->le_count);

	/*
	 * If we have reshape space allocated and it has to grow,
	 * relocate it to the end if kernel says it is at the
	 * beginning in order to grow the LV.
	 */
	if (_reshape_len_per_dev(seg)) {
		if (out_of_place_les_per_disk > _reshape_len_per_dev(seg)) {
			/* Kernel says data is at data_offset > 0 -> relocate reshape space at the begin to the end */
			if (data_offset && !_lv_relocate_reshape_space(lv, alloc_end))
				return_0;

			data_offset = 0;
			out_of_place_les_per_disk -= _reshape_len_per_dev(seg);
		} else
			out_of_place_les_per_disk = 0;
			
	}

	/*
	 * If we don't reshape space allocated extend the LV.
	 *
	 * first_seg(lv)->reshape_len (only segment of top level raid LV)
	 * is accounting for the data rimages so that unchanged
	 * lv_extend()/lv_reduce() can be used to allocate/free,
	 * because seg->len etc. still holds the whole size as before
	 * including the reshape space
	 */
	if (out_of_place_les_per_disk) {
		uint32_t data_rimages = _data_rimages_count(seg, seg->area_count);
		uint32_t reshape_len = out_of_place_les_per_disk * data_rimages;
		uint32_t prev_rimage_len = _lv_total_rimage_len(lv);
		uint64_t lv_size = lv->size;

		RETURN_IF_ZERO(allocate_pvs , "allocate pvs list argument");
		RETURN_IF_NONZERO(dm_list_empty(allocate_pvs), "allocate pvs listed");

PFLA("lv->le_count=%u seg->len=%u seg->area_len=%u", lv->le_count, seg->len, seg->area_len);
PFLA("data_rimages=%u area_count=%u reshape_len=%u", data_rimages, seg->area_count, reshape_len);
PFLA("first_seg(seg_lv(seg, 0)->reshape_len=%u", first_seg(seg_lv(seg, 0))->reshape_len);
PFLA("first_seg(seg_lv(seg, 0)->len=%u", first_seg(seg_lv(seg, 0))->len);

		if (!lv_extend(lv, seg->segtype, data_rimages,
			       seg->stripe_size, 1, seg->region_size,
			       reshape_len /* # of reshape LEs to add */,
			       allocate_pvs, lv->alloc, 0)) {
			log_error("Failed to allocate out-of-place reshape space for %s.",
				  display_lvname(lv));
			return 0;
		}
PFL();
		lv->size = lv_size;

		/* pay attention to lv_extend maybe having allocated more because of layout specific rounding */
		if (!_lv_set_reshape_len(lv, _lv_total_rimage_len(lv) - prev_rimage_len))
			return 0;
	}

	/* Preset data offset in case we fail relocating reshape space below */
	seg->data_offset = 0;

	/*
	 * Handle reshape space relocation
	 */
PFLA("data_offset=%llu where=%d", (unsigned long long) data_offset, where);
	switch (where) {
	case alloc_begin:
		/* Kernel says data is at data_offset == 0 -> relocate reshape space at the end to the begin */
		if (!data_offset && !_lv_relocate_reshape_space(lv, where))
			return_0;
		break;

	case alloc_end:
		/* Kernel says data is at data_offset > 0 -> relocate reshape space at the begin to the end */
		if (data_offset && !_lv_relocate_reshape_space(lv, where))
			return_0;
		break;

	case alloc_anywhere:
		/* We don't care where the space is, kernel will just toggle data_offset accordingly */
		break;

	default:
		log_error(INTERNAL_ERROR "Bogus reshape space allocation request");
		return 0;
	}

	if (where_it_was)
		*where_it_was = data_offset ? alloc_begin : alloc_end;

	/* Inform kernel about the reshape length in sectors */
	seg->data_offset = _reshape_len_per_dev(seg) * lv->vg->extent_size;
PFLA("seg->data_offset=%llu", (unsigned long long) seg->data_offset);

	return _lv_set_image_lvs_start_les(lv);
}

/* Remove any reshape space from the data LVs of @lv */
static int _lv_free_reshape_space_with_status(struct logical_volume *lv, enum alloc_where *where_it_was)
{
	uint32_t total_reshape_len;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if ((total_reshape_len = _reshape_len_per_lv(lv))) {
		enum alloc_where where;
PFL();
		/*
		 * raid10:
		 *
		 * the allocator will have added times #data_copies stripes,
		 * so we need to lv_reduce() less visible size.
		 */
		if (seg_is_any_raid10(seg)) {
			RETURN_IF_NONZERO(total_reshape_len % seg->data_copies, "divisibility by # of data copies");
			total_reshape_len /= seg->data_copies;
		}

		/*
		 * Got reshape space on request to free it.
		 *
		 * If it happens to be at the beginning of
		 * the data LVs, remap it to the end in order
		 * to be able to free it via lv_reduce().
		 */
		if (!_lv_alloc_reshape_space(lv, alloc_end, &where, NULL))
			return_0;

		if (!lv_reduce(lv, total_reshape_len))
			return_0;

		if (!_lv_set_reshape_len(lv, 0))
			return 0;

		/*
		 * Only in case reshape space was freed at the beginning,
		 * which is indicated by "where == alloc_begin",
		 * tell kernel to adjust data_offsets on raid devices to 0.
		 *
		 * The special value '1' for seg->data_offset will cause
		 * "data_offset 0" to be emitted in the segment line.
		 */
PFLA("seg->data_offset=%llu where=%d", (unsigned long long) seg->data_offset, where);
		seg->data_offset = (where == alloc_begin) ? 1 : 0;
PFLA("seg->data_offset=%llu where=%d", (unsigned long long) seg->data_offset, where);

	} else if (where_it_was)
		*where_it_was = alloc_none;

	return 1;
}

static int _lv_free_reshape_space(struct logical_volume *lv)
{
	return _lv_free_reshape_space_with_status(lv, NULL);
}

/*
 * Convert @lv to raid1 by making the linear lv
 * the one data sub LV of a new top-level lv
 */
static struct lv_segment *_convert_lv_to_raid1(struct logical_volume *lv, const char *suffix)
{
	int thin;
	struct lv_segment *seg, *seg1;;
	uint32_t le_count;
	uint64_t flags;

	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));
	thin = lv_is_thin_volume(lv);

	le_count = lv->le_count - _reshape_len_per_lv(lv);
	flags = RAID | LVM_READ | (lv->status & LVM_WRITE);

	log_debug_metadata("Inserting layer LV on top of %s", display_lvname(lv));
	if (!insert_layer_for_lv(lv->vg->cmd, lv, flags, suffix))
		return NULL;

	if (!_check_and_init_region_size(lv))
		return 0;

	/* First segment has changed because of layer insertion */
	RETURN_IF_ZERO((seg = first_seg(lv)), "lv raid segment after layer insertion");
	RETURN_IF_ZERO(seg_lv(seg, 0), "first sub LV");
	RETURN_IF_ZERO((seg1 = first_seg(seg_lv(seg, 0))), "first sub LV segment");

	seg->status |= SEG_RAID;
PFLA("seg1->lv=%s", display_lvname(seg1->lv));
	seg1->lv->status |= RAID_IMAGE | flags;
	seg1->lv->status &= ~LV_REBUILD;

	/* Correct thin volume flags not covered by insert_layer_for_lv() */
	if (thin) {
		seg1->status |= SEG_THIN_VOLUME;
		seg1->lv->status |= THIN_VOLUME;
		lv->status &= ~THIN_VOLUME;
	}

	/* Set raid1 segtype, so that the following image allocation works */
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return NULL;

	lv->status |= RAID;
	lv->le_count = seg->len = seg->area_len = le_count;

	return seg;
}

/* Reset any rebuild or reshape disk flags on @lv, first segment already passed to the kernel */
static int _reset_flags_passed_to_kernel(struct logical_volume *lv, int *flags_reset)
{
	uint32_t lv_count = 0, s;
	struct logical_volume *slv;
	struct lv_segment *seg;
	uint64_t reset_flags = LV_REBUILD | LV_RESHAPE_DELTA_DISKS_PLUS | LV_RESHAPE_DELTA_DISKS_MINUS;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(flags_reset, "flags_reset pointer argument")

	for (s = 0; s < seg->area_count; s++) {
		if (seg_type(seg, s) == AREA_PV)
			continue;

		RETURN_IF_ZERO((slv = _seg_lv_checked(seg, s)), "sub LV");

		/* Recurse into sub LVs */
		if (!_reset_flags_passed_to_kernel(slv, flags_reset))
			return 0;

PFLA("slv=%s", display_lvname(slv));
		if (slv->status & LV_RESHAPE_DELTA_DISKS_MINUS) {
			*flags_reset = 1;
			slv->status |= LV_RESHAPE_REMOVED;
			if (_seg_metalv_checked(seg, s))
				seg_metalv(seg, s)->status |= LV_RESHAPE_REMOVED;
		}

		if (slv->status & reset_flags) {
			*flags_reset = 1;
			slv->status &= ~reset_flags;
		}

		lv_count++;
	}

	/* Reset passed in data offset (reshaping) */
	if (lv_count)
		seg->data_offset = 0;

	return 1;
}

/*
 * HM Helper:
 *
 * Updates and reloads metadata, clears any flags passed to the kerne,l
 * eliminates any residual LVs and updates and reloads metadata again.
 *
 * @lv mandatory argument, rest variable:
 *
 * @lv [ @removal_lvs [ @fn_pre_on_lv @fn_pre_data [ @fn_post_on_lv @fn_post_data ] ] ]
 *
 * Run optional variable args function fn_pre_on_lv with fn_pre_data on @lv before first metadata update
 * and optional variable args function fn_post_on_lv with fn_post_data on @lv before second metadata update
 *
 * This minimaly involves 2 metadata commits or more, depending on
 * pre and post functions carrying out any additional ones or not.
 *
 * WARNING: needs to be called with at least 3 arguments to suit va_list processing!
 */
typedef int (*fn_on_lv_t)(struct logical_volume *lv, void *data);
static int _lv_update_reload_fns_reset_eliminate_lvs(struct logical_volume *lv, ...)
{
	int flags_reset = 0, r = 0;
	va_list ap;
	fn_on_lv_t fn_pre_on_lv = NULL, fn_post_on_lv;
	void *fn_pre_data, *fn_post_data;
	struct lv_segment *seg;
	struct dm_list *removal_lvs;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	va_start(ap, lv);
	removal_lvs = va_arg(ap, struct dm_list *);

	/* Retrieve post/pre functions and post/pre data reference from variable arguments, if any */
	if ((fn_post_on_lv = va_arg(ap, fn_on_lv_t))) {
		fn_post_data = va_arg(ap, void *);
		if ((fn_pre_on_lv = va_arg(ap, fn_on_lv_t)))
			fn_pre_data = va_arg(ap, void *);
	}
PFL();

	/* Call any @fn_pre_on_lv before the first update and reload call (e.g. to rename LVs) */
	if (fn_pre_on_lv && !(r = fn_pre_on_lv(lv, fn_pre_data))) {
		log_error("Pre callout function failed");
		goto err;
	}
PFL();
	if (r == 2) {
		/* Returning 2 -> lv is suspended and metadata got updated in pre function above, don't need to do it again */
		if (!resume_lv(lv->vg->cmd, lv)) {
			log_error("Failed to resume %s", display_lvname(lv));
			goto err;
		}

	/* Update metadata and reload mappings including flags (e.g. LV_REBUILD) */
	} else if (!lv_update_and_reload(lv))
		goto err;
PFL();
	/* Eliminate any residual LV and don't commit the metadata */
	if (!_eliminate_extracted_lvs_optional_write_vg(lv->vg, removal_lvs, 0))
		goto err;
PFL();
	/*
	 * Now that any 'REBUILD' or 'RESHAPE_DELTA_DISKS' etc.
	 * has/have made its/their way to the kernel, we must
	 * remove the flag(s) so that the individual devices are
	 * not rebuilt/reshaped/taken over upon every activation.
	 *
	 * Writes and commits metadata if any flags have been reset
	 * and if successful, performs metadata backup.
	 */
	/* Avoid vg write+commit in vvv and do it here _once_ in case of fn_on_lv() being called */
	log_debug_metadata("Clearing any flags for %s passed to the kernel", display_lvname(lv));
	if (!_reset_flags_passed_to_kernel(lv, &flags_reset))
		goto err;
PFL();
	/* Call any @fn_post_on_lv before the second update and reload call (e.g. to rename LVs back) */
	if (fn_post_on_lv && !(r = fn_post_on_lv(lv, fn_post_data))) {
		log_error("Post callout function failed");
		goto err;
	}
PFL();
	log_debug_metadata("Updating metadata and reloading mappings for %s", display_lvname(lv));
	if ((r != 2 || flags_reset) && !lv_update_and_reload(lv)) {
		log_error(INTERNAL_ERROR "Update and reload of LV %s failed", display_lvname(lv));
		goto err;
	}
PFL();
	r = 1;
err:
	va_end(ap);
	return r;
}

/* Area reorder helper: swap 2 LV segment areas @a1 and @a2 */
static int _swap_areas(struct lv_segment_area *a1, struct lv_segment_area *a2)
{
	struct lv_segment_area tmp;
#if 0
	char *tmp_name;
#endif

	RETURN_IF_ZERO(a1, "first segment area argument");
	RETURN_IF_ZERO(a2, "first segment area argument");

	tmp = *a1;
	*a1 = *a2;
	*a2 = tmp;
#if 0
	/* Rename LVs ? */
	tmp_name = a1->u.lv.lv->name;
	a1->u.lv.lv->name = a2->u.lv.lv->name;
	a2->u.lv.lv->name = tmp_name;
#endif
	return 1;
}

/*
 * Reorder the areas in the first segment of @seg to suit raid10_{near,far}/raid0 layout.
 *
 * raid10_{near,far} can only be reordered to raid0 if !mod(#total_devs, #mirrors)
 *
 * Examples with 6 disks indexed 0..5 with 3 stripes:
 * raid0             (012345) -> raid10_{near,far} (031425) order
 * idx                024135
 * raid10_{near,far} (012345) -> raid0  (024135/135024) order depending on mirror leg selection (TBD)
 * idx                031425
 * _or_ (variations possible)
 * idx                304152
 *
 * Examples 3 stripes with 9 disks indexed 0..8 to create a 3 striped raid0 with 3 data_copies per leg:
 *         vvv
 * raid0  (012345678) -> raid10 (034156278) order
 *         v  v  v
 * raid10 (012345678) -> raid0  (036124578) order depending on mirror leg selection (TBD)
 *
 */
enum raid0_raid10_conversion { reorder_to_raid10_near, reorder_from_raid10_near };
static int _reorder_raid10_near_seg_areas(struct lv_segment *seg, enum raid0_raid10_conversion conv)
{
	unsigned dc, idx1, idx1_sav, idx2, s, ss, str, xchg;
	uint32_t *idx, stripes;
	unsigned i = 0;

	/* Internal sanity checks... */
	RETURN_IF_ZERO(seg, "lv segment argument");
	RETURN_IF_ZERO(conv == reorder_to_raid10_near || conv == reorder_from_raid10_near,
		       "supported reordering requested");
	RETURN_IF_NONZERO((conv == reorder_to_raid10_near && !(seg_is_striped(seg) || seg_is_any_raid0(seg))) ||
			  (conv == reorder_from_raid10_near && !seg_is_raid10_near(seg)),
			  "proper segment types to reorder");
	RETURN_IF_ZERO(seg->data_copies > 1, "#data_copies > 1");
	if ((stripes = seg->area_count) % seg->data_copies) {
		log_error("Can't convert raid10_near LV %s with number of stripes not divisable by number of data copies",
			  display_lvname(seg->lv));
		return 0;
	}

	stripes /= seg->data_copies;
PFLA("seg->data_copies=%u stripes=%u", seg->data_copies, stripes);

	if (!(idx = dm_pool_zalloc(seg_lv(seg, 0)->vg->vgmem, seg->area_count * sizeof(*idx))))
		return 0;

	/* Set up positional index array */
	switch (conv) {
	case reorder_to_raid10_near:
		/*
		 * raid0  (012 345) with 3 stripes/2 data copies     -> raid10 (031425)
		 *
		 * _reorder_raid10_near_seg_areas 2137 idx[0]=0
		 * _reorder_raid10_near_seg_areas 2137 idx[1]=2
		 * _reorder_raid10_near_seg_areas 2137 idx[2]=4
		 * _reorder_raid10_near_seg_areas 2137 idx[3]=1
		 * _reorder_raid10_near_seg_areas 2137 idx[4]=3
		 * _reorder_raid10_near_seg_areas 2137 idx[5]=5
		 *
		 * raid0  (012 345 678) with 3 stripes/3 data copies -> raid10 (036147258)
		 *
		 * _reorder_raid10_near_seg_areas 2137 idx[0]=0
		 * _reorder_raid10_near_seg_areas 2137 idx[1]=3
		 * _reorder_raid10_near_seg_areas 2137 idx[2]=6
		 *
		 * _reorder_raid10_near_seg_areas 2137 idx[3]=1
		 * _reorder_raid10_near_seg_areas 2137 idx[4]=4
		 * _reorder_raid10_near_seg_areas 2137 idx[5]=7
		 * _reorder_raid10_near_seg_areas 2137 idx[6]=2
		 * _reorder_raid10_near_seg_areas 2137 idx[7]=5
		 * _reorder_raid10_near_seg_areas 2137 idx[8]=8
		 */
		/* idx[from] = to */
		for (s = ss = 0; s < seg->area_count; s++)
			if (s < stripes)
				idx[s] = s * seg->data_copies;

			else {
				uint32_t factor = s % stripes;

				if (!factor)
					ss++;

				idx[s] = ss + factor * seg->data_copies;
			}

		break;

	case reorder_from_raid10_near:
		/*
		 * Order depending on mirror leg selection (TBD)
		 *
		 * raid10 (012345) with 3 stripes/2 data copies    -> raid0  (024135/135024)
		 * raid10 (012345678) with 3 stripes/3 data copies -> raid0  (036147258/147036258/...)
		 */
		/* idx[from] = to */
PFL();
		for (s = 0; s < seg->area_count; s++)
			idx[s] = -1; /* = unused */

		idx1 = 0;
		idx2 = stripes;
		for (str = 0; str < stripes; str++) {
PFL();
			idx1_sav = idx1;
			for (dc = 0; dc < seg->data_copies; dc++) {
				struct logical_volume *slv;
PFL();
				s = str * seg->data_copies + dc;
				slv = seg_lv(seg, s);
				idx[s] = ((slv->status & PARTIAL_LV) || idx1 != idx1_sav) ? idx2++ : idx1++;
			}

			if (idx1 == idx1_sav) {
				log_error("Failed to find a valid mirror in stripe %u!", str);
				return 0;
			}
		}

		break;

	default:
		return 0;
	}
PFL()
for (s = 0; s < seg->area_count ; s++)
PFLA("idx[%u]=%d", s, idx[s]);

	/* Sort areas */
	do {
		xchg = seg->area_count;

		for (s = 0; s < seg->area_count ; s++)
			if (idx[s] == s)
				xchg--;

			else {
				if (!_swap_areas(seg->areas + s, seg->areas + idx[s]) ||
				    !_swap_areas(seg->meta_areas + s, seg->meta_areas + idx[s]))
					return 0;

				ss = idx[idx[s]];
				idx[idx[s]] = idx[s];
				idx[s] = ss;
			}
		i++;
	} while (xchg);

for (s = 0; s < seg->area_count ; s++)
PFLA("s=%u idx[s]=%u", s, idx[s]);
PFLA("%d iterations", i);
for (s = 0; s < seg->area_count; s++)
PFLA("seg_lv(seg, %u)->name=%s", s, seg_lv(seg, s)->name);

	return 1;
}

/****************************************************************************/
/*
 * HM
 *
 * Add/remove metadata areas to/from raid0
 *
 * Update metadata and reload mappings if @update_and_reload
 */
static int _alloc_and_add_rmeta_devs_for_lv(struct logical_volume *lv, struct dm_list *allocate_pvs)
{
	struct lv_segment *seg;
	struct dm_list meta_lvs;
struct lv_list *lvl;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	dm_list_init(&meta_lvs);

	log_debug_metadata("Allocating metadata LVs for %s", display_lvname(lv));
	if (!_alloc_rmeta_devs_for_lv(lv, &meta_lvs, allocate_pvs)) {
		log_error("Failed to allocate metadata LVs for %s", display_lvname(lv));
		return_0;
	}

dm_list_iterate_items(lvl, &meta_lvs)
PFLA("meta_lv=%s", lvl->lv->name);
	/* Metadata LVs must be cleared before being added to the array */
	log_debug_metadata("Clearing newly allocated metadata LVs for %s", display_lvname(lv));
	if (!_clear_lvs(&meta_lvs)) {
		log_error("Failed to initialize metadata LVs for %s", display_lvname(lv));
		return_0;
	}

	/* Set segment areas for metadata sub_lvs */
	log_debug_metadata("Adding newly allocated metadata LVs to %s", display_lvname(lv));
	if (!_add_image_component_list(seg, 1, 0, &meta_lvs, 0)) {
		log_error("Failed to add newly allocated metadata LVs to %s", display_lvname(lv));
		return_0;
	}

	return 1;
}

/*
 * HM
 *
 * Add/remove metadata areas to/from raid0
 *
 * Update metadata and reload mappings if @update_and_reload
 */
static int _raid0_add_or_remove_metadata_lvs(struct logical_volume *lv,
					     int update_and_reload,
					     struct dm_list *allocate_pvs,
					     struct dm_list *removal_lvs)
{
	uint64_t raid_type_flag;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (seg->meta_areas) {
PFL();
		RETURN_IF_ZERO(removal_lvs, "removal LVs list argument");
		log_debug_metadata("Extracting metadata LVs");
		if (!_extract_image_component_list(seg, RAID_META, 0, removal_lvs)) {
			log_error(INTERNAL_ERROR "Failed to extract metadata LVs");
			return 0;
		}
PFL();
		raid_type_flag = SEG_RAID0;
		seg->meta_areas = NULL;

	} else {
		if (!_alloc_and_add_rmeta_devs_for_lv(lv, allocate_pvs))
			return 0;

		raid_type_flag = SEG_RAID0_META;
	}

	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, raid_type_flag)))
		return_0;
PFL();

	if (update_and_reload) {
		if (!lv_update_and_reload_origin(lv))
			return_0;

		/* If any residual LVs, eliminate them, write VG, commit it and take a backup */
		return _eliminate_extracted_lvs(lv->vg, removal_lvs);
	}

PFL();
	return 1;
}

/* Set segment area data image LVs from @data_lvs with @status in @lv and give them proper names */
static int _set_lv_areas_from_data_lvs_and_create_names(struct logical_volume *lv,
							struct dm_list *data_lvs,
							uint64_t status)
{
	uint32_t s = 0;
	char **name;
	const char *suffix = (status & RAID_IMAGE) ? "rimage_" : "rmeta_";
	struct lv_list *lvl, *tlvl;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(data_lvs, "data LVs list argument");

	dm_list_iterate_items_safe(lvl, tlvl, data_lvs) {
PFLA("lv=%s", display_lvname(lvl->lv));
		dm_list_del(&lvl->list);
		lv_set_hidden(lvl->lv);

		if (!set_lv_segment_area_lv(seg, s, lvl->lv, 0, status | RAID_IMAGE))
			return_0;

		name = (status & RAID_IMAGE) ? (char **) &seg_lv(seg, s)->name :
					       (char **) &seg_metalv(seg, s)->name;
		if (!(*name = _generate_raid_name(lv, suffix, s++))) {
			log_error("Failed to allocate new data image LV name for %s", display_lvname(lv));
			return 0;
		}
	}

	return 1;
}

/*
 * HM API function:
 *
 * split off raid1 images of @lv, prefix with @split_name or selecet duplicated LV by @split_name,
 * leave @new_image_count in the raid1 set and find them on @splittable_pvs
 *
 * HM FIXME: enhance to split off selectable sub LV (for raid01)
 */
static int _raid_split_duplicate(struct logical_volume *lv, int yes,
				 const char *split_name, uint32_t new_image_count);
int lv_raid_split(struct logical_volume *lv, int yes,
		  const char *split_name, uint32_t new_image_count,
		  struct dm_list *splittable_pvs)
{
	uint32_t split_count, s;
	struct lv_list *lvl;
	struct dm_list meta_lvs, data_lvs;
	struct cmd_context *cmd;
	struct logical_volume *tracking, *split_lv = NULL;
	struct lv_segment *seg;
	struct dm_list tracking_pvs;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_NONZERO(!seg_is_mirrored(seg) && !seg_is_raid01(seg),
			  "mirrored/raid10 segment to split off");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, new_image_count);
	RETURN_IF_ZERO(new_image_count, "images left, rejecting request");
	RETURN_IF_ZERO(split_name, "split name argument");
	cmd = lv->vg->cmd;

	if (!_lv_is_active((lv)))
		return 0;

	/* Special case for splitting off an image of a duplicating LV */
	if (_lv_is_duplicating(lv))
		return _raid_split_duplicate(lv, yes, split_name, new_image_count);

	/* raid1 leg split from here... */
	if (find_lv_in_vg(lv->vg, split_name)) {
		log_error("Logical Volume \"%s\" already exists in %s",
			  split_name, lv->vg->name);
		return 0;
	}

	if (!_raid_in_sync(lv)) {
		log_error("Unable to split %s while it is not in-sync.",
				display_lvname(lv));
		return 0;
	}

	/*
	 * We only allow a split while there is tracking if it is to
	 * complete the split of the tracking sub-LV
	 */
	log_debug_metadata("Check if LV %s is tracking changes", display_lvname(lv));
	if (_lv_is_raid_with_tracking(lv, &s)) {
		if (!lv_is_on_pvs((tracking = seg_lv(seg, s)), splittable_pvs)) {
			log_error("Unable to split additional image from %s "
				  "while tracking changes for %s",
				  lv->name, tracking->name);
			return 0;
		}

		/* Ensure we only split the tracking image */
		dm_list_init(&tracking_pvs);
		splittable_pvs = &tracking_pvs;
		if (!get_pv_list_for_lv(tracking->vg->cmd->mem,
					tracking, splittable_pvs))
			return_0;
	}

	split_count = seg->area_count - new_image_count;

	dm_list_init(&meta_lvs);
	dm_list_init(&data_lvs);

	log_debug_metadata("Extracting image components from LV %s", display_lvname(lv));
	if (!_raid_extract_images(lv, new_image_count, splittable_pvs, 1 /* Don't shift */,
				  &meta_lvs, &data_lvs)) {
		log_error("Failed to extract images from %s",
				display_lvname(lv));
		return 0;
	}

	/* Rename all extracted rimages with @split_name prefix */
	log_debug_metadata("Rename all extracted LVs of LV %s to split name", display_lvname(lv));
	dm_list_iterate_items(lvl, &data_lvs)
		if (!_lv_name_add_string_index(cmd, &lvl->lv->name, split_name))
			return 0;

	/* Split off multiple images as a new raid1 LV */
	if (split_count > 1) {
		uint64_t status = RAID | LVM_READ | LVM_WRITE;
		struct lv_segment *raid1_seg;

		log_print_unless_silent("Splitting off %u images into new raid1 LV %s/%s",
					split_count, lv->vg->name, split_name);

		/* Create empty LV with @split_name to add segment and images */
		log_debug_metadata("Creating new raid1 LV");
		if (!(split_lv = lv_create_empty(split_name, NULL, status | VISIBLE_LV, ALLOC_INHERIT, lv->vg))) {
			log_error("Failed to create new raid1 LV %s/%s.", lv->vg->name, split_name);
			return_0;
		}

		/* Create the one top-level segment for our new raid1 split LV and add it to the LV  */
		log_debug_metadata("Creating new raid1 segment for slit off image component pairs of %s",
				   display_lvname(lv));
		if (!(raid1_seg = alloc_lv_segment(seg->segtype, split_lv, 0, seg->len, 0, status,
						   seg->stripe_size, NULL,
						   split_count, seg->area_len,
						   split_count, 0, seg->region_size, 0, NULL))) {
			log_error("Failed to create raid1 segment for %s", display_lvname(split_lv));
			return_0;
		}
		dm_list_add(&split_lv->segments, &raid1_seg->list);
		raid1_seg->data_copies = split_count;

		/* Set new raid1 segment area data and metadata image LVs and give them proper names */
		log_debug_metadata("setting areas of new raid1 segment");
		if(!_set_lv_areas_from_data_lvs_and_create_names(split_lv, &data_lvs, RAID_IMAGE) ||
		   !_set_lv_areas_from_data_lvs_and_create_names(split_lv, &meta_lvs, RAID_META))
			return 0;

		split_lv->le_count = seg->len;
		split_lv->size = seg->len * lv->vg->extent_size;
	} 

	/* Adjust numbers of raid1 areas and data copies (i.e. sub LVs) */
	seg->area_count = seg->data_copies = new_image_count;

	/* Convert to linear? */
	if (new_image_count == 1) {
		log_debug_metadata("Converting LV %s with one image to linear", display_lvname(lv));
		if (!_convert_raid_to_linear(lv, &meta_lvs)) {
			log_error("Failed to remove RAID layer after linear conversion");
			return 0;
		}
	}

	log_debug_metadata("Supending LV %s and commiting metadata", display_lvname(lv));
	if (!_vg_write_lv_suspend_vg_commit(lv))
		return 0;

	log_debug_metadata("Activating split off LVs");
	if (!_activate_lv_list_excl_local(&data_lvs) ||
	    !_activate_lv_list_excl_local(&meta_lvs))
		return_0;

	log_debug_metadata("Resuming LV %s after split", display_lvname(lv));
	if (!resume_lv(cmd, lv_lock_holder(lv))) {
		log_error("Failed to resume %s after committing changes",
			  display_lvname(lv));
		return 0;
	}

	log_debug_metadata("Removing extracted metadata LVs of LV %s", display_lvname(lv));
	if (!_eliminate_extracted_lvs(lv->vg, &meta_lvs))
		return 0;

	if (split_lv && !activate_lv_excl_local(cmd, split_lv))
		return 0;

	log_print_unless_silent("LV %s split off successfully from %s",
				display_lvname(split_lv ?: dm_list_item(data_lvs.n, struct lv_list)->lv), display_lvname(lv));
	return 1;
}

/*
 * API function:
 *
 * lv_raid_split_and_track
 * @lv
 * @splittable_pvs
 *
 * Only allows a single image to be split while tracking.  The image
 * never actually leaves the mirror.  It is simply made visible.  This
 * action triggers two things: 1) users are able to access the (data) image
 * and 2) lower layers replace images marked with a visible flag with
 * error targets.
 *
 * Returns: 1 on success, 0 on error
 */
int lv_raid_split_and_track(struct logical_volume *lv,
			    int yes,
			    const char *sub_lv_name,
			    struct dm_list *splittable_pvs)
{
	int s;
	struct logical_volume *split_lv;
	struct lv_segment *seg;
	struct volume_group *vg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_NONZERO(!seg_is_mirrored(seg) && !seg_is_raid01(seg),
			  "mirrored/raid10 segment to split off");
	vg = lv->vg;

	if (!_raid_in_sync(lv)) {
		log_error("Unable to split image from %s while not in-sync",
			  display_lvname(lv));
		return 0;
	}

	/* Cannot track two split images at once */
	if (lv_is_raid_with_tracking(lv)) {
		log_error("Cannot track more than one split image at a time");
		return 0;
	}

	/* Find sub LV by name if given, else select last raid1 leg */
	for (s = seg->area_count - 1; s >= 0; s--) {
		if (sub_lv_name &&
		    !strstr(sub_lv_name, seg_lv(seg, s)->name))
			continue;

		if (lv_is_on_pvs(seg_lv(seg, s), splittable_pvs)) {
			split_lv = seg_lv(seg, s);
			split_lv->status &= ~LVM_WRITE;
			lv_set_visible(split_lv);
			break;
		}
	}

	if (s < 0) {
		log_error("No image found to satisfy request");
		return 0;
	}

	if (!yes && yes_no_prompt("Do you really want to split off tracking LV %s from %s [y/n]: ",
				  display_lvname(split_lv), display_lvname(lv)) == 'n')
		return 0;

	if (sigint_caught())
		return_0;


	/*
	 * Check restrictions to keep resilience with just 2 raid1 legs
	 *
	 * A 2-legged raid1 flat mirror will loose all resilience if we allow one leg to
	 * be tracked, because only one remaining leg will receive any writes.
	 *
	 * A duplicating LV (i.e. raid1 top-level with variations of layouts as 2 sub LVs)
	 * _may_ still keep resilience, presumably the remaining leg is raid1/4/5/10, because
	 * data redundancy is being ensured within the reaming raid sub LV.
	 */
	if (seg->area_count < 3) {
		int duplicating = _lv_is_duplicating(lv), redundant = 0;

		if (duplicating) {
			struct lv_segment *seg1;

			RETURN_IF_LV_SEG_ZERO(seg_lv(seg, !s), (seg1 = first_seg(seg_lv(seg, !s))));

			/* Allow for 2-legged tracking, presumably the duplicated LV left is resilient */
			if (!_lv_is_degraded(lv) &&
			    seg_is_raid(seg1) &&
		    	    !seg_is_any_raid0(seg1))
				redundant = 1;
			else
				log_error("Split would cause complete loss of redundancy with "
					  "one %s%s duplicating leg %s",
					  _lv_is_degraded(lv) ? "degraded " : "",
					  lvseg_name(seg1), display_lvname(seg_lv(seg, !s)));

		} else
			log_error("Tracking an image in 2-way raid1 LV %s will cause loss of redundancy!",
				  display_lvname(lv));
	
		if (!redundant) {	
			log_error("Run \"lvconvert %s %s\" to have 3 legs before splitting of %s and redo",
				  duplicating ? "--dup ..." : "-m2",
				  display_lvname(lv), display_lvname(split_lv));
			return 0;
		}
	}

	if (_lv_is_degraded(lv)) {
		log_error("Splitting off degraded LV %s rejected to limit danger of data loss; repair first",
			  display_lvname(lv));
		return 0;
	}

	if (!_vg_write_commit_backup(vg))
		return 0;

	/* Suspend+resume the tracking LV to create its devnode */
	if (!_suspend_resume_lv(split_lv))
		return 0;

	log_print_unless_silent("%s split from %s for read-only purposes.",
				split_lv->name, lv->name);
	log_print_unless_silent("Use 'lvconvert --merge %s' to merge back into %s",
				display_lvname(split_lv), display_lvname(lv));
	return 1;
}

/* HM Helper: */
static int _lv_update_and_reload(struct logical_volume *lv, int recurse)
{
	uint32_t s;
	struct logical_volume *lv1;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (!recurse && !_vg_write_lv_suspend_vg_commit(lv))
		return_0;

	for (s = 0; s < seg->area_count; s++) {
		if ((lv1 = seg_lv(seg, s))) {
PFLA("lv1=%s recurse=%d", display_lvname(lv1), recurse);
			if (_seg_lv_checked(first_seg(lv1), 0) && lv_is_duplicated(lv1)) {
				if (!_lv_update_and_reload(lv1, 1))
					return_0;

			} else if (!_activate_sublv_preserving_excl(lv, lv1))
				return_0;
		}

		if ((lv1 = _seg_metalv_checked(seg, s)) &&
		    !_activate_sublv_preserving_excl(lv, lv1))
			return_0;
	}

	if (!resume_lv(lv->vg->cmd, lv_lock_holder(lv))) {
		log_error("Failed to resume %s after committing changes",
			  display_lvname(lv));
		return 0;
	}

	return 1;
}

/*
 * API function:
 *
 * merge split of tracking @image_lv back into raid1 set
 */
int lv_raid_merge(struct logical_volume *image_lv)
{
	uint32_t s;
	char *lv_name;
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct logical_volume *meta_lv = NULL;
	struct lv_segment *seg;
	struct volume_group *vg = image_lv->vg;
	struct logical_volume *tracking;

	RETURN_IF_ZERO(image_lv, "image LV argument");

	if (!(lv_name = _top_level_lv_name(image_lv))) {
		log_error("Unable to merge non-{mirror,duplicating} image %s.",
			  display_lvname(image_lv));
		return 0;
	}

	if (!(lvl = find_lv_in_vg(vg, lv_name))) {
		log_error("Unable to find containing RAID array for %s.",
			  display_lvname(image_lv));
		return 0;
	}

	lv = lvl->lv;
	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_NONZERO(!seg_is_mirrored(seg) && !seg_is_raid01(seg),
			  "mirrored/raid10 to merge into, rejecting request");
	RETURN_IF_ZERO(seg->meta_areas, "metadata LV areas");

	if (!_lv_is_raid_with_tracking(lv, &s)) {
		log_error("%s is not a tracking LV.", display_lvname(lv));
		return 0;
	}

	if ((tracking = seg_lv(seg, s)) != image_lv) {
		log_error("%s is not the tracking LV of %s but %s is.",
			  display_lvname(image_lv), display_lvname(lv), display_lvname(tracking));
		return 0;
	}

	/* Image LVs should not be resizable directly, but... */
	/* HM FIXME: duplicating sub LVs can have different size! */
	if (seg->len > image_lv->le_count)  {
		log_error(INTERNAL_ERROR "The image LV %s of %s has invalid size!",
			  display_lvname(image_lv), display_lvname(lv));
		return 0;
	}

	if (image_lv->status & LVM_WRITE) {
		log_error("%s is not read-only - refusing to merge.",
			  display_lvname(image_lv));
	}

	if (!(meta_lv = _seg_metalv_checked(seg, s))) {
		log_error("Failed to find metadata LV for %s in %s.",
			  display_lvname(image_lv), display_lvname(lv));
		return 0;
	}

	image_lv->status |= (LVM_WRITE | RAID_IMAGE);
	lv_set_hidden(image_lv);

	if (!lv_update_and_reload(lv))
		return_0;

	/* Suspend+resume the tracking LV to remove its devnode */
	if (!_suspend_resume_lv(lv))
		return 0;

	log_print_unless_silent("LV %s successfully merged back into %s",
				display_lvname(image_lv), display_lvname(lv));
	return 1;
}

/*
 * Adjust all data sub LVs of @lv to mirror
 * or raid name depending on @direction
 * adjusting their LV status
 */
enum mirror_raid_conv { mirror_to_raid1 = 0, raid1_to_mirror };
static int _adjust_data_lvs(struct logical_volume *lv, enum mirror_raid_conv direction)
{
	uint32_t s;
	char *p;
	struct lv_segment *seg;
	static struct {
		char type_char;
		uint64_t set_flag;
		uint64_t reset_flag;
	} conv[] = {
		{ 'r', RAID_IMAGE  , MIRROR_IMAGE },
		{ 'm', MIRROR_IMAGE, RAID_IMAGE }
	};

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	for (s = 0; s < seg->area_count; ++s) {
		struct logical_volume *dlv = seg_lv(seg, s);

		if (!(p = _strstr_strings(dlv->name, "_mimage_", "_rimage_", NULL))) {
			log_error(INTERNAL_ERROR "name lags image part");
			return 0;
		}

		*(p + 1) = conv[direction].type_char;
		log_debug_metadata("data LV renamed to %s", dlv->name);

		dlv->status &= ~conv[direction].reset_flag;
		dlv->status |= conv[direction].set_flag;
	}

	return 1;
}

/*
 * Convert @lv with "mirror" mapping to "raid1"
 * opitonally changing number of data_copies
 * defined by @new_image_count.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _convert_mirror_to_raid(struct logical_volume *lv,
				   const struct segment_type *new_segtype,
				   uint32_t new_image_count,
				   uint32_t new_region_size,
				   struct dm_list *allocate_pvs,
				   int update_and_reload,
				   struct dm_list *removal_lvs)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (!seg_is_mirrored(seg)) {
		log_error(INTERNAL_ERROR "mirror conversion supported only");
		return 0;
	}

	new_image_count = new_image_count ?: seg->area_count;
	if (new_image_count < 2) {
		log_error("can't convert %s to fewer than 2 data_copies", display_lvname(lv));
		return 0;
	}

	/* Remove any mirror log */
	if (seg->log_lv) {
		log_debug_metadata("Removing mirror log, %s", seg->log_lv->name);
		if (!remove_mirror_log(lv->vg->cmd, lv, NULL, 0)) {
			log_error("Failed to remove mirror log");
			return 0;
		}
	}


	/* Rename all data sub LVs from "*_mimage_*" to "*_rimage_*" and set their status */
	log_debug_metadata("Adjust data LVs of %s", display_lvname(lv));
	if (!_adjust_data_lvs(lv, mirror_to_raid1))
		return 0;

	seg->region_size = new_region_size;

	/* Allocate metadata devs for all mimage ones (writes+commits metadata) */
	if (!_alloc_and_add_rmeta_devs_for_lv(lv, allocate_pvs))
		return 0;

	init_mirror_in_sync(new_image_count > seg->area_count ? 0 : 1);

	seg->segtype = new_segtype;
	seg->data_copies = new_image_count;
	lv->status &= ~(MIRROR | MIRRORED | LV_NOTSYNCED);
	lv->status |= RAID;
	seg->status |= RAID;

	/* Change image pair count to requested # of images */
	if (new_image_count != seg->area_count) {
		log_debug_metadata("Changing image count to %u on %s",
				   new_image_count, display_lvname(lv));
		if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, removal_lvs))
			return 0;
	}

	return update_and_reload ? _lv_update_reload_fns_reset_eliminate_lvs(lv, removal_lvs, NULL) : 1;
}

/*
 * Convert @lv with "raid1" mapping to "mirror"
 * optionally changing number of data_copies
 * defined by @new_image_count.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _convert_raid1_to_mirror(struct logical_volume *lv,
				    const struct segment_type *new_segtype,
				    uint32_t new_image_count,
				    uint32_t new_region_size,
				    struct dm_list *allocate_pvs,
				    int update_and_reload,
				    struct dm_list *removal_lvs)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (!seg_is_raid1(seg)) {
		log_error(INTERNAL_ERROR "raid1 conversion supported only");
		return 0;
	}

	if ((new_image_count = new_image_count ?: seg->area_count) < 2) {
		log_error("can't convert %s to fewer than 2 data_copies", display_lvname(lv));
		return 0;
	}

	if (!_check_max_mirror_devices(new_image_count)) {
		log_error("Unable to convert %s LV %s with %u images to %s",
			  SEG_TYPE_NAME_RAID1, display_lvname(lv), new_image_count, SEG_TYPE_NAME_MIRROR);
		log_error("At least reduce to the maximum of %u images with \"lvconvert -m%u %s\"",
			  DEFAULT_MIRROR_MAX_IMAGES, DEFAULT_MIRROR_MAX_IMAGES - 1, display_lvname(lv));
		return 0;
	}

	init_mirror_in_sync(new_image_count > seg->area_count ? 0 : 1);

	/* Change image pair count to requested # of images */
	if (new_image_count != seg->area_count) {
		log_debug_metadata("Changing image count to %u on %s",
				   new_image_count, display_lvname(lv));
		if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, removal_lvs))
			return 0;
	}


	/* Remove rmeta LVs */
	log_debug_metadata("Extracting and renaming metadata LVs");
	if (!_extract_image_component_list(seg, RAID_META, 0, removal_lvs))
		return 0;

	seg->meta_areas = NULL;

	/* Rename all data sub LVs from "*_rimage_*" to "*_mimage_*" and set their status */
	log_debug_metadata("Adjust data LVs of %s", display_lvname(lv));
	if (!_adjust_data_lvs(lv, raid1_to_mirror))
		return 0;

	seg->segtype = new_segtype;
	seg->region_size = new_region_size;
	seg->data_copies = new_image_count;
	lv->status &= ~RAID;
	seg->status &= ~RAID;
	lv->status |= (MIRROR | MIRRORED);

PFL();
	/* Add mirror_log LV (should happen in wih image allocation */
	if (!add_mirror_log(lv->vg->cmd, lv, 1, seg->region_size, allocate_pvs, lv->vg->alloc)) {
		log_error("Unable to add mirror log to %s", display_lvname(lv));
		return 0;
	}

PFL();
	return update_and_reload ? _lv_update_reload_fns_reset_eliminate_lvs(lv, removal_lvs, NULL) : 1;
}

/* BEGIN: striped -> raid0 conversion */
/*
 * HM
 *
 * Helper convert striped to raid0
 *
 * For @lv, empty hidden LVs in @data_lvs have been created by the caller.
 *
 * All areas from @lv segments are being moved to new
 * segments allocated with area_count=1 for @data_lvs.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _striped_to_raid0_move_segs_to_raid0_lvs(struct logical_volume *lv,
						    struct dm_list *data_lvs)
{
	uint32_t s = 0, le;
	struct logical_volume *dlv;
	struct lv_segment *seg_from, *seg_new;
	struct lv_list *lvl;
	struct segment_type *segtype;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(data_lvs, "data LVs list pointer argument");
	RETURN_IF_NONZERO(dm_list_empty(data_lvs), "data LVs listed");

	if (!(segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* Move segment areas across to the N data LVs of the rnew raid0 LV */
	dm_list_iterate_items(lvl, data_lvs)  {
		dlv = lvl->lv;
		le = 0;
		dm_list_iterate_items(seg_from, &lv->segments) {
			uint64_t status = RAID | SEG_RAID | (seg_from->status & (LVM_READ | LVM_WRITE));

			/* Allocate a data LV segment with one area for each segment in the striped LV */
			if (!(seg_new = alloc_lv_segment(segtype, dlv,
							 le, seg_from->area_len,
							 0 /* reshape_len */, status,
							 0 /* stripe_size */, NULL, 1 /* area_count */,
							 seg_from->area_len, 1 /* data_copies */,
							 0 /* chunk_size */, 0 /* region_size */, 0, NULL)))
				return_0;

			seg_type(seg_new, 0) = AREA_UNASSIGNED;
			dm_list_add(&dlv->segments, &seg_new->list);
			le += seg_from->area_len;

			/* Move the respective area across to our new segment */
			if (!move_lv_segment_area(seg_new, 0, seg_from, s))
				return_0;
		}

		/* Adjust le count and LV size */
		dlv->le_count = le;
		dlv->size = (uint64_t) le * lv->vg->extent_size;
		s++;
	}

	/* Remove the empty segments from the striped LV */
	dm_list_init(&lv->segments);

	return 1;
}

/*
 * HM Helper: check that @lv has one stripe one, i.e. same stripe count in all of its segments
 *
 * Superfluous if different stripe zones will ever be supported
 */
static int _lv_has_one_stripe_zone(struct logical_volume *lv)
{
	struct lv_segment *seg;
	unsigned area_count;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	area_count = seg->area_count;
	dm_list_iterate_items(seg, &lv->segments)
		if (seg->area_count != area_count)
			return 0;

	return 1;
}

/* HM Helper: check that @lv has segments with just @areas */
static int _lv_has_segments_with_n_areas(struct logical_volume *lv, unsigned areas)
{
	struct lv_segment *seg;

	RETURN_IF_ZERO(lv, "lv argument");

	dm_list_iterate_items(seg, &lv->segments)
		if (seg->area_count != areas) {
			log_error("Called on %s with segments != %u area", display_lvname(lv), areas);
			return 0;
		}

	return 1;
}


/*
 * HM
 *
 * Helper: convert striped to raid0
 *
 * Inserts hidden LVs for all segments and the parallel areas in @lv and moves 
 * given segments and areas across.
 *
 * Optionally allocates metadata devs if @alloc_metadata_devs
 * Optionally updates metadata and reloads mappings if @update_and_reload
 *
 * Returns: 1 on success, 0 on failure
 */
static struct lv_segment *_convert_striped_to_raid0(struct logical_volume *lv,
						    int alloc_metadata_devs,
						    int update_and_reload,
						    struct dm_list *allocate_pvs)
{
	uint32_t area_count, area_len = 0, stripe_size;
	struct lv_segment *seg, *raid0_seg;
	struct segment_type *segtype;
	struct dm_list data_lvs;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, (area_count = seg->area_count) - 1);
	RETURN_IF_ZERO(allocate_pvs || dm_list_empty(allocate_pvs), "PVs to allocate");

	if (!seg_is_striped(seg)) {
		log_error(INTERNAL_ERROR "Cannot convert non-%s LV %s to %s",
			  SEG_TYPE_NAME_STRIPED, display_lvname(lv), SEG_TYPE_NAME_RAID0);
		return NULL;
	}

	dm_list_iterate_items(seg, &lv->segments)
		area_len += seg->area_len;

	seg = first_seg(lv);
	stripe_size = seg->stripe_size;

	/* Check for not (yet) supported varying area_count on multi-segment striped LVs */
	if (!_lv_has_one_stripe_zone(lv)) {
		log_error("Cannot convert striped LV %s with varying stripe count to raid0",
			  display_lvname(lv));
		log_error("Please use \"lvconvert --duplicate ...\"");
		return NULL;
	}

	if (!seg->stripe_size ||
	    (seg->stripe_size & (seg->stripe_size - 1))) {
		log_error("Cannot convert striped LV %s with non-power of 2 stripe size %u",
			  display_lvname(lv), seg->stripe_size);
		log_error("Please use \"lvconvert --duplicate ...\"");
	}

	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0)))
		return_NULL;

	/* Allocate empty rimage components */
	dm_list_init(&data_lvs);
	if (!_alloc_image_components(lv, area_count, NULL, &data_lvs, NULL)) {
		log_error("Failed to allocate empty image components for raid0 LV %s.",
			  display_lvname(lv));
		return_NULL;
	}

	/* Move the AREA_PV areas across to the new rimage components; empties lv->segments */
	if (!_striped_to_raid0_move_segs_to_raid0_lvs(lv, &data_lvs)) {
		log_error("Failed to insert linear LVs underneath %s.", display_lvname(lv));
		return_NULL;
	}

	/*
	 * Allocate single segment to hold the image component
	 * areas based on the first data LVs properties derived
	 * from the first new raid0 LVs first segment
	 */
	seg = first_seg(dm_list_item(dm_list_first(&data_lvs), struct lv_list)->lv);
	if (!(raid0_seg = alloc_lv_segment(segtype, lv,
					   0 /* le */, lv->le_count /* len */,
					   0 /* reshape_len */, seg->status | SEG_RAID,
					   stripe_size, NULL /* log_lv */,
					   area_count, area_len,
					   1 /* data_copies */, 0 /* chunk_size */,
					   0 /* seg->region_size */, 0u /* extents_copied */ ,
					   NULL /* pvmove_source_seg */))) {
		log_error("Failed to allocate new raid0 segement for LV %s.", display_lvname(lv));
		return_NULL;
	}

	/* Add new single raid0 segment to emptied LV segments list */
	dm_list_add(&lv->segments, &raid0_seg->list);

	/* Add data LVs to the top-level LVs segment; resets LV_REBUILD flag on them */
	if (!_add_image_component_list(raid0_seg, 1, 0, &data_lvs, 0))
		return NULL;

	lv->status |= RAID;

	/* Allocate metadata LVs if requested */
	if (alloc_metadata_devs &&
	    !_raid0_add_or_remove_metadata_lvs(lv, 0, allocate_pvs, NULL))
		return NULL;

	if (update_and_reload && !lv_update_and_reload(lv))
		return NULL;

	return raid0_seg;
}
/* END: striped -> raid0 conversion */

/* BEGIN: raid0 -> striped conversion */
/* HM Helper: walk the segment LVs of a segment @seg and find smallest area at offset @area_le */
static uint32_t _smallest_segment_lvs_area(struct lv_segment *seg,
					   uint32_t area_le, uint32_t *area_len)
{
	uint32_t s;

	RETURN_IF_ZERO(seg, "lv segment argument");
	RETURN_IF_NONZERO(area_le >= seg->area_len, "area logical extent argument");
	RETURN_IF_ZERO(area_len, "area length argument");

	*area_len = ~0U;

	/* Find smallest segment of each of the data image LVs at offset area_le */
	for (s = 0; s < seg->area_count; s++) {
		struct lv_segment *seg1 = find_seg_by_le(seg_lv(seg, s), area_le);

		if (!seg1) {
			log_error(INTERNAL_ERROR "Segment at logical extent %u not found in LV %s!",
				  area_le, display_lvname(seg_lv(seg, s)));
			return 0;
		}

		*area_len = min(*area_len, seg1->len);

PFLA("Segment at logical extent %u, len=%u found in LV %s, area_len=%u!",
area_le, seg1->len, display_lvname(seg_lv(seg, s)), *area_len);

	}

	return 1;
}

/* HM Helper: Split segments in segment LVs in all areas of @seg at offset @area_le */
static int _split_area_lvs_segments(struct lv_segment *seg, uint32_t area_le)
{
	uint32_t s;

	RETURN_IF_ZERO(seg, "lv segment argument");

	/* Make sure that there's segments starting at area_le in all data LVs */
	for (s = 0; s < seg->area_count; s++)
		if (area_le < seg_lv(seg, s)->le_count &&
		    !lv_split_segment(seg_lv(seg, s), area_le))
			return_0;

	return 1;
}

/* HM Helper: allocate a new striped segment and add it to list @new_segments */
static int _alloc_and_add_new_striped_segment(struct logical_volume *lv,
					      uint32_t le, uint32_t area_len,
					      struct dm_list *new_segments)
{
	struct lv_segment *seg, *new_seg;
	struct segment_type *striped_segtype;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(new_segments, "new segments argument");

	if (!(striped_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* Allocate a segment with seg->area_count areas */
	if (!(new_seg = alloc_lv_segment(striped_segtype, lv, le, area_len * seg->area_count,
					 0 /* seg->reshape_len */, seg->status & ~RAID,
					 seg->stripe_size, NULL, seg->area_count,
					 area_len, 1 /* data_copies */ , seg->chunk_size, 0, 0, NULL)))
		return_0;

	dm_list_add(new_segments, &new_seg->list);

	return 1;
}

/*
 * HM
 *
 * All areas from @lv image component LV's segments are
 * being split at "striped" compatible boundaries and
 * moved to @new_segments allocated.
 *
 * The metadata+data component LVs are being mapped to an
 * error target and linked to @removal_lvs for disposal
 * by the caller.
 *
 * Returns: 1 on success, 0 on failure
 */
static int _raid0_to_striped_retrieve_segments_and_lvs(struct logical_volume *lv,
						       struct dm_list *removal_lvs)
{
	uint32_t s, area_le, area_len, le;
	struct lv_segment *data_seg, *seg, *seg_to;
	struct dm_list new_segments;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	dm_list_init(&new_segments);

	/*
	 * Walk all segments of all data LVs splitting them up at proper boundaries
	 * and create the number of new striped segments we need to move them across
	 */
	area_le = le = 0;
	while (le < lv->le_count) {
		if (!_smallest_segment_lvs_area(seg, area_le, &area_len))
			return_0;

PFLA("le=%u area_len=%u area_le=%u area_count=%u", le, area_len, area_le, seg->area_count);
		area_le += area_len;

		if (!_split_area_lvs_segments(seg, area_le) ||
		    !_alloc_and_add_new_striped_segment(lv, le, area_len, &new_segments))
			return 0;

		le = area_le * seg->area_count;
	}

	/* Now move the prepared split areas across to the new segments */
	area_le = 0;
	dm_list_iterate_items(seg_to, &new_segments) {
		for (s = 0; s < seg->area_count; s++) {
			data_seg = find_seg_by_le(seg_lv(seg, s), area_le);

			/* Move the respective area across to our new segments area */
			if (!move_lv_segment_area(seg_to, s, data_seg, 0))
				return_0;
		}

		/* Presumes all data LVs have equal size */
		area_le += data_seg->len;
	}

	/* Extract any metadata LVs and the empty data LVs for disposal by the caller */
	log_debug_metadata("Extracting image comonent pairs");
	if ((seg->meta_areas && !_extract_image_component_list(seg, RAID_META,  0, removal_lvs)) ||
	    !_extract_image_component_list(seg, RAID_IMAGE, 0, removal_lvs))
		return_0;

	/*
	 * Remove the one segment holding the image component areas
	 * from the top-level LV, then add the new segments to it
	 */
	dm_list_del(&seg->list);
	dm_list_splice(&lv->segments, &new_segments);

	return 1;
}

/*
 * HM
 *
 * Helper convert raid0 to striped
 *
 * Convert a RAID0 set to striped
 *
 * Returns: 1 on success, 0 on failure
 *
 */
static int _convert_raid0_to_striped(struct logical_volume *lv,
				     int update_and_reload,
				     struct dm_list *removal_lvs)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	/* Caller should ensure, but... */
	if (!seg_is_any_raid0(seg)) {
		log_error(INTERNAL_ERROR "Cannot convert non-%s LV %s to %s",
			  SEG_TYPE_NAME_RAID0, display_lvname(lv), SEG_TYPE_NAME_STRIPED);
		return 0;
	}

	/* Reshape space should be freed already, but... */
	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space of %s", display_lvname(lv));
		return 0;
	}

	/* Remove metadata devices */
	if (seg_is_raid0_meta(seg) &&
	    !_raid0_add_or_remove_metadata_lvs(lv, 0 /* update_and_reload */, NULL, removal_lvs))
		return_0;

	/* Move the AREA_PV areas across to new top-level segments of type "striped" */
	if (!_raid0_to_striped_retrieve_segments_and_lvs(lv, removal_lvs)) {
		log_error("Failed to retrieve raid0 segments from %s.", lv->name);
		return_0;
	}

	lv->status &= ~RAID;

	if (!(first_seg(lv)->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	if (update_and_reload) {
		if (!lv_update_and_reload(lv))
			return_0;

		/* Eliminate the residual LVs, write VG, commit it and take a backup */
		return _eliminate_extracted_lvs(lv->vg, removal_lvs);
	} 
	return 1;
}
/* END: raid0 -> striped conversion */

/* BEGIN: raid <-> raid conversion */
/*
 *
 * HM
 *
 * Compares current raid disk count of active RAID set @lv to
 * requested @dev_count returning number of disks as of healths
 * string in @devs_health and synced disks in @devs_in_sync
 *
 * Returns:
 *
 * 	0: error
 * 	1: kernel dev count = @dev_count
 * 	2: kernel dev count < @dev_count
 * 	3: kernel dev count > @dev_count
 *
 */
static int _reshaped_state(struct logical_volume *lv, const unsigned dev_count,
			   unsigned *devs_health, unsigned *devs_in_sync)
{
	uint32_t kernel_devs;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(devs_health, "sevices health argument");
	RETURN_IF_ZERO(devs_in_sync, "sevices in-sync argument");

	if (!_get_dev_health(lv, &kernel_devs, devs_health, devs_in_sync, NULL))
		return 0;

	if (kernel_devs == dev_count)
		return 1;

	return kernel_devs < dev_count ? 2 : 3;
}

/*
 * Return new length for @lv based on @old_image_count and @new_image_count in @*len
 *
 * Subtracts any reshape space and provide data length only!
 */
static int _lv_reshape_get_new_len(struct logical_volume *lv,
				   uint32_t old_image_count, uint32_t new_image_count,
				   uint32_t *len)
{
	struct lv_segment *seg;
	uint32_t di_old, di_new;
	uint32_t old_lv_reshape_len, new_lv_reshape_len;
	uint64_t r;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(len, "reshape length pointer argument");
	RETURN_IF_ZERO((di_old = _data_rimages_count(seg, old_image_count)), "old data images");
	RETURN_IF_ZERO((di_new = _data_rimages_count(seg, new_image_count)), "new data images");

	old_lv_reshape_len = di_old * _reshape_len_per_dev(seg);
	new_lv_reshape_len = di_new *_reshape_len_per_dev(seg);

	r = (uint64_t) lv->le_count;
	r -= old_lv_reshape_len;
	RETURN_IF_NONZERO((r = new_lv_reshape_len + r * di_new / di_old) > UINT_MAX, "proper new segment length!");
	*len = (uint32_t) r;

	return 1;
}

/*
 * Extend/reduce size of @lv and it's first segment during reshape to @extents
 */
static int _reshape_adjust_to_size(struct logical_volume *lv,
				   uint32_t old_image_count, uint32_t new_image_count)
{
	struct lv_segment *seg;
	uint32_t new_le_count;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

PFLA("%s lv->le_count=%u seg->len=%u\n", __func__, lv->le_count, seg->len);
	if (!_lv_reshape_get_new_len(lv, old_image_count, new_image_count, &new_le_count))
		return 0;
PFLA("%s lv->le_count=%u seg->len=%u\n", __func__, lv->le_count, seg->len);

	/* Externally visible LV size w/o reshape space */
	lv->le_count = seg->len = new_le_count;
	lv->size = (lv->le_count - new_image_count * _reshape_len_per_dev(seg)) * lv->vg->extent_size;
	/* seg->area_len does not change */

	if (old_image_count < new_image_count) {
PFLA("lv->size=%s seg->len=%u seg->area_len=%u seg->area_count=%u old_image_count=%u new_image_count=%u", display_size(lv->vg->cmd, lv->size), seg->len, seg->area_len, seg->area_count, old_image_count, new_image_count);
		/* Extend from raid1 mapping */
		if (old_image_count == 2 &&
		    !seg->stripe_size)
			seg->stripe_size = DEFAULT_STRIPESIZE;

	/* Reduce to raid1 mapping */
	} else if (new_image_count == 2)
		seg->stripe_size = 0;

	return 1;
}

/*
 * HM Helper:
 *
 * Reshape: add immages to existing raid lv
 *
 */
static int _raid_reshape_add_images(struct logical_volume *lv,
				    const struct segment_type *new_segtype,
				    int yes, int force,
				    uint32_t old_image_count, uint32_t new_image_count,
			 	    const unsigned new_stripes, const unsigned new_stripe_size,
		 	 	    struct dm_list *allocate_pvs)
{
	uint32_t grown_le_count, current_le_count, s;
	struct volume_group *vg;
	struct logical_volume *slv;
	struct lv_segment *seg;
	struct lvinfo info = { 0 };

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	vg = lv->vg;

	if (!lv_info(vg->cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
		log_error("lv_info failed: aborting");
		return 0;
	}

	if (seg->segtype != new_segtype)
		log_print_unless_silent("Ignoring layout change on device adding reshape");
PFL();
	if (!_lv_reshape_get_new_len(lv, old_image_count, new_image_count, &grown_le_count))
		return 0;
PFL();
	current_le_count = lv->le_count - _reshape_len_per_lv(lv);
	grown_le_count -= _reshape_len_per_dev(seg) * _data_rimages_count(seg, new_image_count);
	log_warn("WARNING: Adding stripes to active%s logical volume %s "
		 "will grow it from %u to %u extents!",
		 info.open_count ? " and open" : "",
		 display_lvname(lv), current_le_count, grown_le_count);
	log_print_unless_silent("Run \"lvresize -l%u %s\" to shrink it or use the additional capacity",
				current_le_count, display_lvname(lv));
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count,
				seg->data_copies, new_stripes, new_stripe_size))
		return 0;
PFL();
	/* Allocate new image component pairs for the additional stripes and grow LV size */
	log_debug_metadata("Adding %u data and metadata image LV pair%s to %s",
			   new_image_count - old_image_count, new_image_count - old_image_count > 1 ? "s" : "",
			   display_lvname(lv));
	if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, NULL))
		return 0;
PFL();
	/* Reshape adding image component pairs -> change sizes/counters accordingly */
	if (!_reshape_adjust_to_size(lv, old_image_count, new_image_count)) {
		log_error("Failed to adjust LV %s to new size!", display_lvname(lv));
		return 0;
	}
PFL();
	/* Allocate forward out of place reshape space at the beginning of all data image LVs */
	log_debug_metadata("(Re)allocating reshape space for %s", display_lvname(lv));
	if (!_lv_alloc_reshape_space(lv, alloc_begin, NULL, allocate_pvs))
		return 0;

PFLA("lv->size=%s", display_size(vg->cmd, lv->size));
PFLA("lv->le_count=%u", lv->le_count);
PFLA("seg->len=%u", first_seg(lv)->len);
	/*
 	 * Reshape adding image component pairs:
 	 *
 	 * - reset rebuild flag on new image LVs
 	 * - set delta disks plus flag on new image LVs
 	 */
	if (old_image_count < seg->area_count) {
		log_debug_metadata("Setting delta disk flag on new data LVs of %s",
				   display_lvname(lv));
		for (s = old_image_count; s < seg->area_count; s++) {
			slv = seg_lv(seg, s);
PFLA("seg_lv(seg, %u)=%s", s, display_lvname(slv));
			slv->status &= ~LV_REBUILD;
			slv->status |= LV_RESHAPE_DELTA_DISKS_PLUS;
		}
	}

	return 1;
}

/*
 * HM Helper:
 *
 * Reshape: remove images from existing raid lv
 *
 */
static int _raid_reshape_remove_images(struct logical_volume *lv,
				       const struct segment_type *new_segtype,
				       int yes, int force,
				       uint32_t old_image_count, uint32_t new_image_count,
			 	       const unsigned new_stripes, const unsigned new_stripe_size,
		 		       struct dm_list *allocate_pvs, struct dm_list *removal_lvs)
{
	uint32_t active_lvs, current_le_count, reduced_le_count, removed_lvs, s;
	uint64_t extend_le_count;
	unsigned devs_health, devs_in_sync;
	struct lv_segment *seg;
	struct lvinfo info = { 0 };

PFL();
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
PFL();
	switch (_reshaped_state(lv, new_image_count, &devs_health, &devs_in_sync)) {
	case 3:
		/*
		 * Disk removal reshape step 1:
		 *
		 * we got more disks active than requested via @new_stripes
		 *
		 * -> flag the ones to remove
		 *
		 */
PFL();
		if (seg->segtype != new_segtype)
			log_print_unless_silent("Ignoring layout change on device removing reshape");

		if (!lv_info(lv->vg->cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
			log_error("lv_info failed: aborting");
			return 0;
		}

		if (!_lv_reshape_get_new_len(lv, old_image_count, new_image_count, &reduced_le_count))
			return 0;

		reduced_le_count -= seg->reshape_len * _data_rimages_count(seg, new_image_count);
		current_le_count = lv->le_count - seg->reshape_len * _data_rimages_count(seg, old_image_count);
		extend_le_count = current_le_count * current_le_count / reduced_le_count;
PFLA("new_image_count=%u _data_rimages_count(seg, new_image_count)=%u current_le_count=%u", new_image_count, _data_rimages_count(seg, new_image_count), current_le_count);
		log_warn("WARNING: Removing stripes from active%s logical "
			 "volume %s will shrink it from %s to %s!",
			 info.open_count ? " and open" : "", display_lvname(lv),
			 display_size(lv->vg->cmd, (uint64_t) current_le_count * lv->vg->extent_size),
			 display_size(lv->vg->cmd, (uint64_t) reduced_le_count * lv->vg->extent_size));
		log_warn("THIS MAY DESTROY (PARTS OF) YOUR DATA!");
		if (!yes)
			log_warn("Interrupt the conversion and run \"lvresize -y -l%u %s\" to "
				 "keep the current size if not done already!",
				 (uint32_t) extend_le_count, display_lvname(lv));
		log_print_unless_silent("If that leaves the logical volume larger than %u extents due to stripe rounding,",
			 reduced_le_count);
		log_print_unless_silent("you may want to grow the content afterwards (filesystem etc.)");
		log_warn("WARNING: too remove freed stripes after the conversion has finished, you have to run \"lvconvert --stripes %u %s\"",
			 new_stripes, display_lvname(lv));

		if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count,
					seg->data_copies, new_stripes, new_stripe_size))
			return 0;

		if (!force) {
			log_warn("WARNING: Can't remove stripes without --force option");
			return 0;
		}

		/*
		 * Allocate backward out of place reshape space at the
		 * _end_ of all data image LVs, because MD reshapes backwards
		 * to remove disks from a raid set
		 */
		if (!_lv_alloc_reshape_space(lv, alloc_end, NULL, allocate_pvs))
			return 0;

		/* Flag all disks past new images as delta disks minus to kernel */
		for (s = new_image_count; s < old_image_count; s++)
			seg_lv(seg, s)->status |= LV_RESHAPE_DELTA_DISKS_MINUS;

		if (seg_is_any_raid5(seg) && new_image_count == 2)
			seg->data_copies = 2;

		break;

	case 1:
		/*
	 	* Disk removal reshape step 2:
	 	*
	 	* we got the proper (smaller) amount of devices active
	 	* for a previously finished disk removal reshape
	 	*
	 	* -> remove the freed up images and reduce LV size
	 	*
	 	*/
PFL();
		for (active_lvs = removed_lvs = s = 0; s < seg->area_count; s++) {
			struct logical_volume *slv;

			RETURN_IF_NONZERO(!_seg_lv_checked(seg, s) ||
					  !(slv = seg_lv(seg, s)), "image sub lv");
			if (slv->status & LV_RESHAPE_REMOVED)
				removed_lvs++;
			else
				active_lvs++;
		}

PFLA("devs_in_sync=%u active_lvs=%u removed_lvs=%u", devs_in_sync, active_lvs, removed_lvs);
		RETURN_IF_ZERO(devs_in_sync == new_image_count, "correct kernel/lvm active LV count");
		// RETURN_IF_ZERO(devs_in_sync == active_lvs, "correct kernel/lvm active LV count");
		RETURN_IF_ZERO(active_lvs + removed_lvs == old_image_count, "correct kernel/lvm total LV count");

		/* Reshape removing image component pairs -> change sizes accordingly */
		if (!_reshape_adjust_to_size(lv, old_image_count, new_image_count)) {
			log_error("Failed to adjust LV %s to new size!", display_lvname(lv));
			return 0;
		}

		log_debug_metadata("Removing %u data and metadata image LV pair%s from %s",
				   old_image_count - new_image_count, old_image_count - new_image_count > 1 ? "s" : "",
				   display_lvname(lv));
		if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, removal_lvs))
			return 0;

		break;

	default:
PFL();
		log_error(INTERNAL_ERROR "Bad return provided to %s.", __func__);
		return 0;
	}

	return 1;
}

/*
 * HM Helper:
 *
 * adjust @lv size for lv_{extend/reduce} to work.
 *
 * Set all segments type (should be just one) to raid0_meta
 * for resize (for extend, last one would suffice, but...).
 */
static int _adjust_raid10_lv_size(struct logical_volume *lv,
				  const struct segment_type *new_segtype,
				  const struct segment_type *raid0_meta_segtype,
				  const uint32_t data_copies, int to_exposed_size)
{
	struct lv_segment *seg, *seg1;
	const struct segment_type *segtype;

	if (data_copies == 1)
		return 1;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(raid0_meta_segtype, "no valid raid0_meta_segtype argument");
	RETURN_IF_ZERO(data_copies > 1, "valid #data_copies");
	RETURN_IF_ZERO(to_exposed_size == 0 || to_exposed_size == 1,
		       "valid to_exposed_size argument");

	if (to_exposed_size) {
		lv->le_count /= data_copies;
		segtype = new_segtype;
		seg->data_copies = data_copies;
	} else  {
		lv->le_count *= data_copies;
		segtype = raid0_meta_segtype;
		seg->data_copies = 1;
	}

	seg->len = lv->le_count;
	dm_list_iterate_items(seg1, &lv->segments)
		seg1->segtype = segtype;

	return 1;
}

/*
 * HM Helper:
 *
 * resize striped/raid0/raid10 number of slices in @lv to hold raid10 @new_data_copies >= 2 and <= stripes
 *
 * This function could be used to change number of data copies
 * on raid10_{new,offset} as well, but MD kernel does not
 * supprt that (yet).
 */
static int _lv_raid10_resize_data_copies(struct logical_volume *lv,
					 const struct segment_type *new_segtype,
					 const uint32_t new_data_copies,
					 struct dm_list *allocate_pvs)
{
	uint32_t data_copies, extents_per_data_copy, extents;
	struct lv_segment *seg;
	const struct segment_type *raid0_meta_segtype, *segtype;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	/* HM FIXME: accept raid10* once (if ever) MD kernel supports it */
	RETURN_IF_ZERO(seg_is_striped(seg) || seg_is_any_raid0(seg) || seg_is_raid10_far(seg),
		       "processable segment type");
	RETURN_IF_ZERO(seg->area_count > 1, "area count > 1");
	RETURN_IF_NONZERO((seg_is_striped(seg) || seg_is_any_raid0(seg)) && seg->data_copies != 1,
			  "#data_copies == 1 with striped/raid0");
	RETURN_IF_NONZERO(seg_is_raid10(seg) && seg->data_copies < 2,
			  "#data_copies < 2 with raid10_");
	RETURN_IF_NONZERO(new_data_copies == seg->data_copies, "change in #data_copies");
	RETURN_IF_NONZERO(lv->le_count % seg->area_count, "divisibility of LV size by stripes");
	RETURN_IF_ZERO((raid0_meta_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)),
		       "raid0_meta segment type found?");

	if (new_data_copies < 1 || new_data_copies > seg->area_count) {
		log_error("Number of data copies invalid for %s LV %s", lvseg_name(seg), display_lvname(lv));
		return 0;
	}

	segtype = seg->segtype;
	data_copies = seg->data_copies;
	extents_per_data_copy = lv->le_count; /* Netto extents exposed = size per data copy needed */
	extents = (new_data_copies > data_copies ? /* Extents to extend/reduce LV */
		   new_data_copies - data_copies :
		   data_copies - new_data_copies) * extents_per_data_copy;

	/* Adjust to total (internal) LV size */
	if (!_adjust_raid10_lv_size(lv, new_segtype, raid0_meta_segtype, data_copies, 0))
		return 0;

	log_debug_metadata("%sing %s LV %s before conversion to %s",
			   new_data_copies > seg->data_copies ? "Extend": "Reduc",
			   segtype->name, display_lvname(lv), new_segtype->name);

	if (new_data_copies > data_copies) {
		if (!lv_extend(lv, raid0_meta_segtype,
			       seg->area_count, seg->stripe_size, 1, 0,
			       extents, allocate_pvs, lv->alloc, 0)) {
			log_error("Failed to extend %s LV %s before conversion to %s",
				  segtype->name, display_lvname(lv), new_segtype->name);
			return 0;
		}

	} else if (!lv_reduce(lv, extents)) {
		log_error("Failed to reduce %s LV %s", segtype->name, display_lvname(lv));
		return 0;
	}

	/* Adjust to externally LV exposed size */
	return _adjust_raid10_lv_size(lv, new_segtype, raid0_meta_segtype, new_data_copies, 1);
}

/*
 * HM Helper:
 *
 * Reshape: keep images in RAID @lv but change stripe size or data copies
 *
 */
static int _raid_reshape_keep_images(struct logical_volume *lv,
				     const struct segment_type *new_segtype,
				     int yes, int force,
				     int *force_repair,
				     const int new_data_copies, const unsigned new_stripe_size,
				     struct dm_list *allocate_pvs)
{
	int alloc_reshape_space = 1;
	enum alloc_where where = alloc_anywhere;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(force_repair, "force repair pointer argument");
	RETURN_IF_ZERO(allocate_pvs || dm_list_empty(allocate_pvs), "PVs to allocate");

PFLA("seg->data_copies=%u new_data_copies=%u", seg->data_copies, new_data_copies);
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0,
				seg->area_count, seg->data_copies,
				seg->area_count - seg->segtype->parity_devs, new_stripe_size))
			return 0;
PFL();
	/* Check a request to change the number of data copies in a raid10 LV */
	if (seg_is_raid10_far(seg)) {
		if (seg->data_copies != new_data_copies) {
			/*
		 	* Ensure resynchronisation of new data copies
		 	* No reshape space needed, because raid10_far uses distinct stripe zones
		 	* for its data copies rather than rotating them in individual stripes.
		 	*/
			*force_repair = new_data_copies > seg->data_copies;
			alloc_reshape_space = 0;

			if (!_lv_raid10_resize_data_copies(lv, new_segtype, new_data_copies, allocate_pvs))
				return 0;
		}
	} else
		seg->stripe_size = new_stripe_size;

	/*
	 * Reshape layout or chunksize:
	 *
	 * Allocate free out of place reshape space unless raid10_far.
	 *
	 * If other raid10, allocate it appropriatly.
	 *
	 * Allocate it anywhere for raid4/5 to avoid remapping
	 * it in case it is already allocated.
	 *
	 * The dm-raid target is able to use the space whereever it
	 * is found by appropriately selecting forward or backward reshape.
	 */
	if (alloc_reshape_space &&
	    !_lv_alloc_reshape_space(lv, where, NULL, allocate_pvs))
		return 0;

	seg->segtype = new_segtype;

	return 1;
}

/* Helper: callback function to activate any new image component pairs of @lv starting at area @start_idx */
static int _activate_sub_lvs(struct logical_volume *lv, uint32_t start_idx)
{
	uint32_t s;
	struct logical_volume *lv1;
	struct lv_segment *seg;
	struct cmd_context *cmd;

	/* seg->area_count may be 0 here! */
	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	cmd = lv->vg->cmd;

	log_debug_metadata("Activating %u image component%s of LV %s",
			   seg->area_count - start_idx, seg->meta_areas ? "pairs" : "s",
			   display_lvname(lv));
	for (s = start_idx; s < seg->area_count; s++) {
		if ((lv1 = _seg_lv_checked(seg, s)) &&
#if 1
		    !activate_lv_excl_local(cmd, lv1))
#else
		    !_suspend_resume_lv(lv1))
#endif
			return 0;
		if ((lv1 = _seg_metalv_checked(seg, s)) &&
#if 1
		    !activate_lv_excl_local(cmd, lv1))
#else
		    !_suspend_resume_lv(lv1))
#endif
			return 0;
	}

	return 1;
}

/* Helper: nord fn to make _lv_update_reload_fns_reset_eliminate_lvs() happy */
static int _post_dummy(struct logical_volume *lv, void *data)
{
	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_NONZERO(data, "data argument allowed");

	return 1;
}

/* Helper: callback function to activate any new image component pairs @lv */
static int _pre_raid_reshape(struct logical_volume *lv, void *data)
{
	uint32_t old_image_count;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(data, "proper data pointer argument");
	RETURN_IF_ZERO((old_image_count = *((uint32_t *) data)), "proper old image count");

PFL();
	/* Activate any new image component pairs */
	if (!_vg_write_lv_suspend_vg_commit(lv))
		return 0;
PFL();
	if (!_activate_sub_lvs(lv, old_image_count))
		return 0;
PFL();

	return 2; /* 1: ok, 2: metadata commited */
}

/*
 * Reshape logical volume @lv by adding/removing stripes
 * (absolute new stripes given in @new_stripes), changing
 * layout (e.g. raid5_ls -> raid5_ra) or changing
 * stripe size to @new_stripe_size.
 *
 * In case of disk addition, any PVs listed in mandatory
 * @allocate_pvs will be used for allocation of new stripes.
 */
static int _raid_reshape(struct logical_volume *lv,
			 const struct segment_type *new_segtype,
			 int yes, int force,
			 const unsigned new_data_copies,
			 const unsigned new_region_size,
			 const unsigned new_stripes,
			 const unsigned new_stripe_size,
		 	 struct dm_list *allocate_pvs)
{
	int force_repair = 0, r, too_few = 0;
	unsigned devs_health, devs_in_sync;
	uint32_t new_image_count, old_image_count;
	enum alloc_where where_it_was;
	struct lv_segment *seg;
	struct dm_list removal_lvs;
	fn_on_lv_t fn_pre_on_lv = NULL;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	if (!seg_is_reshapable_raid(seg))
		return 0;

	RETURN_IF_ZERO(is_same_level(seg->segtype, new_segtype), "reshape request");
	RETURN_IF_NONZERO(!seg_is_reshapable_raid(seg) && !seg_is_raid1(seg), "reshapable/processable segment");
	RETURN_IF_ZERO((old_image_count = seg->area_count), "old device count calculated");
	RETURN_IF_NONZERO((new_image_count = new_stripes + seg->segtype->parity_devs) < 2 && !seg_is_raid1(seg),
		  	  "raid set with less than parity devices");
	RETURN_IF_ZERO(allocate_pvs , "allocate pvs list argument");

	if (!_raid_in_sync(lv)) {
		log_error("Unable to convert %s while it is not in-sync",
			  display_lvname(lv));
		return 0;
	}

PFLA("old_image_count=%u new_image_count=%u new_region_size=%u", old_image_count, new_image_count, new_region_size);
	dm_list_init(&removal_lvs);

	/* No change in layout requested ? */
	if (seg->segtype == new_segtype &&
	    seg->data_copies == new_data_copies &&
	    seg->region_size == new_region_size &&
	    old_image_count == new_image_count &&
	    seg->stripe_size == new_stripe_size) {
		/*
		 * No change in segment type, image count, region or stripe size has been requested ->
		 * user requests this to remove any reshape space from the @lv
		 */
		if (!_lv_free_reshape_space_with_status(lv, &where_it_was)) {
			log_error(INTERNAL_ERROR "Failed to free reshape space of %s",
				  display_lvname(lv));
			return 0;
		}

		log_print_unless_silent("No change in RAID LV %s layout, freeing reshape space", display_lvname(lv));
PFLA("seg->data_offset=%llu", (unsigned long long) seg->data_offset);

		if (where_it_was == alloc_none) {
			log_print_unless_silent("LV %s does not have reshape space allocated",
						display_lvname(lv));
			return 0;
		}

		if (!_lv_update_reload_fns_reset_eliminate_lvs(lv, NULL, NULL))
			return_0;

		log_print_unless_silent("%s", display_lvname(lv));
		return 1;
	}

	/* raid4/5 with N image component pairs (i.e. N-1 stripes): allow for raid4/5 reshape to 2 devices, i.e. raid1 layout */
	if (seg_is_raid4(seg) || seg_is_any_raid5(seg)) {
		if (new_stripes < 1)
			too_few = 1;

	/* raid6 (raid10 can't shrink reshape) device count: check for 2 stripes minimum */
	} else if (new_stripes < 2)
		too_few = 1;

	if (too_few) {
		log_error("Too few stripes requested");
		return 0;
	}

	switch ((r = _reshaped_state(lv, old_image_count, &devs_health, &devs_in_sync))) {
	case 1:
		/*
		 * old_image_count == kernel_dev_count
		 *
		 * Check for device health
		 */
		if (devs_in_sync < devs_health) {
			log_error("Can't reshape out of sync LV %s", display_lvname(lv));
			return 0;
		}
PFL();
		/* device count and health are good -> ready to go */
		break;

	case 2:
PFLA("devs_in_sync=%u old_image_count=%u new_image_count=%u", devs_in_sync,old_image_count, new_image_count);
		if (devs_in_sync == new_image_count)
			break;

		/* Possible after a shrinking reshape and forgotten device removal */
		log_error("Device count is incorrect. "
			  "Forgotten \"lvconvert --stripes %d %s\" to remove %u images after reshape?",
			  devs_in_sync - seg->segtype->parity_devs, display_lvname(lv),
			  old_image_count - devs_in_sync);
		return 0;

	default:
		log_error(INTERNAL_ERROR "Bad return=%d provided to %s.", r, __func__);
		return 0;
	}

	/* Handle disk addition reshaping */
	if (old_image_count < new_image_count) {
PFL();
		if (!_raid_reshape_add_images(lv, new_segtype, yes, force,
					      old_image_count, new_image_count,
					      new_stripes, new_stripe_size, allocate_pvs))
			return 0;

		fn_pre_on_lv = _pre_raid_reshape;

	/* Handle disk removal reshaping */
	} else if (old_image_count > new_image_count) {
		if (!_raid_reshape_remove_images(lv, new_segtype, yes, force,
						 old_image_count, new_image_count,
						 new_stripes, new_stripe_size,
						 allocate_pvs, &removal_lvs))
			return 0;

	/*
	 * Handle raid set layout reshaping
	 * (e.g. raid5_ls -> raid5_n or stripe size change or change #data_copies on raid10)
	 */
	} else if (!_raid_reshape_keep_images(lv, new_segtype, yes, force, &force_repair,
					      new_data_copies, new_stripe_size, allocate_pvs))
		return 0;

PFL();

	seg->stripe_size = new_stripe_size;

	/* HM FIXME: workaround for not resetting "nosync" flag */
	init_mirror_in_sync(0);
PFLA("new_segtype=%s seg->area_count=%u", new_segtype->name, seg->area_count);

	/* _pre_raid_reshape to acivate any added image component pairs to avoid unsafe table loads */
	if (!_lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs,
						       _post_dummy, NULL,
						       fn_pre_on_lv, &old_image_count))
		return 0;

	return force_repair ? _lv_cond_repair(lv) : 1;
}

/*
 * Check for reshape request defined by:
 *
 * - raid type is reshape capable
 * - no raid level change
 * - # of stripes requested to change
 *   (i.e. add/remove disks from a striped raid set)
 *   -or-
 * - stripe size change requestd
 *   (e.g. 32K -> 128K)
 *
 * Returns:
 *
 * 0 -> no reshape request
 * 1 -> allowed reshape request
 * 2 -> prohibited reshape request
 * 3 -> allowed region size change request
 */
static int _reshape_requested(const struct logical_volume *lv, const struct segment_type *segtype,
			      const int data_copies, const uint32_t region_size,
			      const uint32_t stripes, const uint32_t stripe_size)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), segtype);

	/* Possible takeover in case #data_copies == #stripes */
	if (seg_is_raid10_near(seg) && segtype_is_raid1(segtype))
		return 0;

	/* No layout change -> allow for removal of reshape space */
	if (seg->segtype == segtype &&
	    data_copies == seg->data_copies &&
	    region_size == seg->region_size &&
	    stripes == _data_rimages_count(seg, seg->area_count) &&
	    stripe_size == seg->stripe_size)
		return 1;

	/* Ensure region size is >= stripe size */
	if (!seg_is_striped(seg) &&
	    !seg_is_any_raid0(seg) &&
	    (region_size || stripe_size) &&
	    ((region_size ?: seg->region_size) < (stripe_size ?: seg->stripe_size))) {
		log_error("region size may not be smaller than stripe size on LV %s",
			  display_lvname(lv));
		return 2;
	}

PFLA("data_copies=%u seg->data_copies=%u stripes=%u seg->area_count=%u", data_copies, seg->data_copies, stripes, seg->area_count);
	if ((_lv_is_duplicating(lv) || lv_is_duplicated(lv)) &&
	    ((seg_is_raid1(seg) ? 0 : (stripes != _data_rimages_count(seg, seg->area_count))) ||
	     data_copies != seg->data_copies))
		goto err;
PFL();
	if ((!seg_is_striped(seg) && segtype_is_raid10_far(segtype)) ||
	    (seg_is_raid10_far(seg) && !segtype_is_striped(segtype))) {
		if (data_copies == seg->data_copies &&
		    region_size == seg->region_size) {
			log_error("Can't convert %sraid10_far",
	    			  seg_is_raid10_far(seg) ? "" : "to ");
			goto err;
		}
	}

	if (seg_is_raid10_far(seg)) {
		if (stripes != _data_rimages_count(seg, seg->area_count)) {
			log_error("Can't change stripes in raid10_far");
			goto err;
		}

		if (stripe_size != seg->stripe_size) {
			log_error("Can't change stripe size in raid10_far");
			goto err;
		}
	}
PFL();
	/* region_size may change on any raid LV but raid0 including raid10_far */
	if (region_size &&
	    region_size != seg->region_size &&
	    segtype == seg->segtype) {
		int may = 1;

		if (seg_is_raid10_far(seg) &&
		    (stripes != _data_rimages_count(seg, seg->area_count) ||
		     stripe_size != seg->stripe_size))
			may = 0;
PFLA("may=%d", may);
		return may ? 3 : 2;
	}
PFL();
	if (seg_is_any_raid10(seg) && seg->area_count > 2 &&
	    stripes && stripes < seg->area_count - seg->segtype->parity_devs) {
		log_error("Can't remove stripes from raid10");
		goto err;
	}
PFL();
	/* This segment type is not reshapable */
	if (!seg_is_reshapable_raid(seg))
		return 0;
PFL();
	/* Switching raid levels is a takeover, no reshape */
	if (!is_same_level(seg->segtype, segtype))
		return 0;
PFL();
	if (data_copies != seg->data_copies) {
		if (seg_is_raid10_near(seg))
			return 0;

		if (seg_is_raid10_far(seg))
			return segtype_is_raid10_far(segtype) ? 1 : 0;

		if (seg_is_raid10_offset(seg)) {
			log_error("Can't change number of data copies on %s LV %s",
				  lvseg_name(seg), display_lvname(lv));
			goto err;
		}
	}

	/* raid10_{near,offset} case */
	if ((seg_is_raid10_near(seg) && segtype_is_raid10_offset(segtype)) ||
	    (seg_is_raid10_offset(seg) && segtype_is_raid10_near(segtype))) {
		if (stripes >= seg->area_count)
			return 1;

		goto err;
	}
PFL();
	/*
	 * raid10_far is not reshapable in MD at all;
	 * lvm/dm adds reshape capability to add/remove data_copies
	 */
	if (seg_is_raid10_far(seg) && segtype_is_raid10_far(segtype)) {
		if (stripes && stripes == seg->area_count &&
		    data_copies > 1 &&
		    data_copies <= seg->area_count &&
		    data_copies != seg->data_copies)
			return 1;

		goto err;

	} else if (seg_is_any_raid10(seg) && segtype_is_any_raid10(segtype) &&
		   data_copies > 1 && data_copies != seg->data_copies)
		goto err;
PFL();
	/* raid10_{near,offset} can't reshape removing devices, just add some */
	if (seg_is_any_raid10(seg) &&
	    seg->segtype == segtype) {
		if (stripes &&
		    (stripes < seg->area_count || stripes < seg->data_copies)) {
			log_error("Can't reshape %s LV %s removing devices.",
				  lvseg_name(seg), display_lvname(lv));
			goto err;

		} else
			return 1;
	}

PFL();
	/* Change layout (e.g. raid5_ls -> raid5_ra) keeping # of stripes */
	if (seg->segtype != segtype) {
PFL();
		if (stripes && stripes != _data_rimages_count(seg, seg->area_count))
			goto err;
PFL();
		if (stripe_size && stripe_size != seg->stripe_size)
			goto err;

PFL();
		return 1;
	}
PFL();

	if (stripes && stripes == _data_rimages_count(seg, seg->area_count) &&
	    stripe_size == seg->stripe_size) {
		log_error("LV %s already has %u stripes.",
			  display_lvname(lv), stripes);
		return 2;
	}
PFL();
	return (region_size || stripes || stripe_size) ? 1 : 0;

err:
	if (lv_is_duplicated(lv))
		log_error("Conversion of duplicating sub LV %s rejected", display_lvname(lv));
	else
		log_error("Use \"lvconvert --duplicate --type %s ... %s", segtype->name, display_lvname(lv));

	return 2;
}

/*
 * HM
 *
 * TAKEOVER: copes with all raid level switches aka takeover of @lv
 *
 * Overwrites the users "--type level_algorithm" request (e.g. --type raid6_zr)
 * with the appropriate, constrained one to allow for takeover (e.g. raid6_n_6).
 *
 * raid0 can take over:
 *  raid4
 *  raid5
 *  raid10_{near,far} - assuming we have all necessary active disks
 *  raid1
 *
 * raid1 can take over:
 *  raid5 with 2 devices, any layout or chunk size
 *
 * raid10_{near,far} can take over:
 *  raid0 - with any number of drives
 *
 * raid4 can take over:
 *  raid0 - if there is only one stripe zone
 *  raid5 - if layout is right (parity on last disk)
 *
 * raid5 can take over:
 *  raid0 - if there is only one stripe zone - make it a raid4 layout
 *  raid1 - if there are two drives.  We need to know the chunk size
 *  raid4 - trivial - just use a raid4 layout.
 *  raid6 - Providing it is a *_6 layout
 *
 * raid6 currently can only take over a (raid4/)raid5.  We map the
 * personality to an equivalent raid6 personality
 * with the Q block at the end.
 *
 *
 * DUPLICATE:
 *
 * restrictions on --mirrors/--stripes/--stripesize are checked
 */
#define	ALLOW_NONE		0x0
#define	ALLOW_DATA_COPIES	0x1
#define	ALLOW_STRIPES		0x2
#define	ALLOW_STRIPE_SIZE	0x4
#define	ALLOW_REGION_SIZE	0x8

struct possible_takeover_reshape_type {
	/* First 2 have to stay... */
	const uint64_t possible_types;
	const uint32_t options;
	const uint64_t current_types;
	const uint32_t current_areas;
};

struct possible_duplicate_type {
	/* First 2 have to stay... */
	const uint64_t possible_types;
	const uint32_t options;
	const uint32_t new_areas;
};

struct possible_type {
	/* ..to be handed back via this struct */
	const uint64_t possible_types;
	const uint32_t options;
};

static struct possible_takeover_reshape_type _possible_takeover_reshape_types[] = {
	/* striped -> */
	{ .current_types  = SEG_AREAS_STRIPED, /* linear, i.e. seg->area_count = 1 */
	  .possible_types = SEG_RAID1,
	  .current_areas = 1,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },
	{ .current_types  = SEG_AREAS_STRIPED, /* linear, i.e. seg->area_count = 1 */
	  .possible_types = SEG_RAID0|SEG_RAID0_META,
	  .current_areas = 1,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_AREAS_STRIPED, /* linear, i.e. seg->area_count = 1 */
	  .possible_types = SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
	  .current_areas = 1,
	  .options = ALLOW_REGION_SIZE },
	{ .current_types  = SEG_AREAS_STRIPED, /* linear, i.e. seg->area_count = 1 */
	  .possible_types = SEG_RAID10_NEAR,
	  .current_areas = 1,
	  .options = ALLOW_REGION_SIZE },
	{ .current_types  = SEG_AREAS_STRIPED,
	  .possible_types = SEG_RAID10_NEAR|SEG_RAID10_FAR,
	  .current_areas = ~0,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },
	{ .current_types  = SEG_AREAS_STRIPED, /* striped, i.e. seg->area_count > 1 */
	  .possible_types = SEG_RAID01,
	  .current_areas = ~0U,
	  .options = ALLOW_REGION_SIZE|ALLOW_DATA_COPIES },
	{ .current_types  = SEG_AREAS_STRIPED, /* striped, i.e. seg->area_count > 1 */
	  .possible_types = SEG_RAID0|SEG_RAID0_META,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_AREAS_STRIPED, /* striped, i.e. seg->area_count > 1 */
	  .possible_types = SEG_RAID4|SEG_RAID5_N|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_REGION_SIZE },

	/* raid0* -> */
	{ .current_types  = SEG_RAID0|SEG_RAID0_META, /* seg->area_count = 1 */
	  .possible_types = SEG_RAID1,
	  .current_areas = 1,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },
	{ .current_types  = SEG_RAID0|SEG_RAID0_META, /* seg->area_count > 1 */
	  .possible_types = SEG_RAID10_NEAR|SEG_RAID10_FAR|SEG_RAID01,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },
	{ .current_types  = SEG_RAID0|SEG_RAID0_META, /* seg->area_count > 1 */
	  .possible_types = SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_REGION_SIZE },
	{ .current_types  = SEG_RAID0|SEG_RAID0_META, /* raid0 striped, i.e. seg->area_count > 0 */
	  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid1 -> */
	{ .current_types  = SEG_RAID1,
	  .possible_types = SEG_RAID1|SEG_MIRROR,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },
	{ .current_types  = SEG_RAID1, /* Only if seg->area_count = 2 */
	  .possible_types = SEG_RAID10_NEAR|SEG_RAID4| \
			    SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
	  .current_areas = 2,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID1, /* seg->area_count != 2 */
	  .possible_types = SEG_RAID10_NEAR,
	  .current_areas = ~0U,
	  .options = ALLOW_REGION_SIZE },
	{ .current_types  = SEG_RAID1, /* seg->area_count != 2 allowing for -m0 */
	  .possible_types = SEG_AREAS_STRIPED,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES },

	/* mirror -> raid1 with arbitrary number of legs */
	{ .current_types  = SEG_MIRROR,
	  .possible_types = SEG_MIRROR|SEG_RAID1,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },

	/* mirror -> raid4/5 with 2 legs */
	{ .current_types  = SEG_MIRROR,
	  .possible_types = SEG_RAID10_NEAR|SEG_RAID4| \
			    SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
	  .current_areas = 2,
	  .options = ALLOW_REGION_SIZE },

	/* raid4 */
	{ .current_types  = SEG_RAID4,
	  .possible_types = SEG_MIRROR|SEG_RAID1,
	  .current_areas = 2,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID4,
	  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META|SEG_RAID5_N|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid5 -> */
	{ .current_types  = SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
	  .possible_types = SEG_RAID1,
	  .current_areas = 2,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
	  .possible_types = SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPES },

	{ .current_types  = SEG_RAID5_LS,
	  .possible_types = SEG_RAID6_LS_6,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID5_LS,
	  .possible_types = SEG_RAID5_LS|SEG_RAID5_N|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID5_RS,
	  .possible_types = SEG_RAID6_RS_6,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID5_RS,
	  .possible_types = SEG_RAID5_RS|SEG_RAID5_N|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RA,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID5_LA,
	  .possible_types = SEG_RAID6_LA_6,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID5_LA,
	  .possible_types = SEG_RAID5_LA|SEG_RAID5_N|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_RA,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID5_RA,
	  .possible_types = SEG_RAID6_RA_6,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },
	{ .current_types  = SEG_RAID5_RA,
	  .possible_types = SEG_RAID5_RA|SEG_RAID5_N|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID5_N,
	  .possible_types = SEG_RAID5_N|SEG_RAID4| \
			    SEG_RAID5_LA|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_RA,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID5_N,
	  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid6 -> */
	{ .current_types  = SEG_RAID6_ZR,
	  .possible_types = SEG_RAID6_ZR|SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_NC,
	  .possible_types = SEG_RAID6_NC|SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_NR,
	  .possible_types = SEG_RAID6_NR|SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NC|SEG_RAID6_ZR|SEG_RAID6_N_6,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_LS_6,
	  .possible_types = SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_LS,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_RS_6,
	  .possible_types = SEG_RAID6_RS_6|SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_RS,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_LA_6,
	  .possible_types = SEG_RAID6_LA_6|SEG_RAID6_LS_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_LA,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_RA_6,
	  .possible_types = SEG_RAID6_RA_6|SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6| \
			    SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6|SEG_RAID5_RA,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_N_6,
	  .possible_types = SEG_RAID6_N_6|SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
			    SEG_RAID6_NR|SEG_RAID6_NC|SEG_RAID6_ZR,
	  .current_areas = ~0U,
	  .options = ALLOW_STRIPE_SIZE },
	{ .current_types  = SEG_RAID6_N_6,
	  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META|SEG_RAID4|SEG_RAID5_N,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid10_near <-> raid10_near */
	{ .current_types  = SEG_RAID10_NEAR,
	  .possible_types = SEG_RAID10_NEAR,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES|ALLOW_STRIPES },

	/* raid10_near <-> raid1 */
	{ .current_types  = SEG_RAID10_NEAR,
	  .possible_types = SEG_RAID1,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid10_far <-> raid10_far */
	{ .current_types  = SEG_RAID10_FAR,
	  .possible_types = SEG_RAID10_FAR,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES },

	/* raid10 -> striped/raid0 */
	{ .current_types  = SEG_RAID10_NEAR|SEG_RAID10_FAR,
	  .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid10 (2 legs) -> raid1 */
	{ .current_types  = SEG_RAID10_NEAR,
	  .possible_types = SEG_RAID1,
	  .current_areas = 2,
	  .options = ALLOW_REGION_SIZE },

	/* raid01 -> striped */
	{ .current_types  = SEG_RAID01,
	  .possible_types = SEG_AREAS_STRIPED,
	  .current_areas = ~0U,
	  .options = ALLOW_NONE },

	/* raid01 -> raid01 */
	{ .current_types  = SEG_RAID01,
	  .possible_types = SEG_RAID01,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES },

	/* raid01 -> raid10 */
	{ .current_types  = SEG_RAID01,
	  .possible_types = SEG_RAID10,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },

	/* raid10 -> raid01 */
	{ .current_types  = SEG_RAID10_NEAR,
	  .possible_types = SEG_RAID01,
	  .current_areas = ~0U,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE },

	/* END */
	{ .current_types  = 0 }
};

static struct possible_duplicate_type _possible_duplicate_types[] = {
	{ .possible_types = SEG_RAID1|SEG_MIRROR,
	  .options = ALLOW_DATA_COPIES|ALLOW_REGION_SIZE,
	  .new_areas = ~0U },
	{ .possible_types = SEG_THIN_VOLUME,
	  .options = ALLOW_NONE,
	  .new_areas = ~0 },
	{ .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META,
	  .options = ALLOW_NONE,
	  .new_areas = 1 },
	{ .possible_types = SEG_AREAS_STRIPED|SEG_RAID0|SEG_RAID0_META,
	  .options = ALLOW_STRIPES|ALLOW_STRIPE_SIZE,
	  .new_areas = ~0U },
	{ .possible_types = SEG_RAID01|SEG_RAID10_NEAR|SEG_RAID10_FAR|SEG_RAID10_OFFSET,
	  .options = ALLOW_DATA_COPIES|ALLOW_STRIPES|ALLOW_STRIPE_SIZE|ALLOW_REGION_SIZE,
	  .new_areas = ~0U },
	{ .possible_types = SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N| \
	  		    SEG_RAID6_ZR|SEG_RAID6_NC|SEG_RAID6_NR| \
	  		    SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6|SEG_RAID6_N_6,
	  .options = ALLOW_STRIPES|ALLOW_STRIPE_SIZE|ALLOW_REGION_SIZE,
	  .new_areas = ~0U },

	/* END */
	{ .possible_types  = 0 }
};

/*
 * Return possible_type struct for current type in @seg 
 *
 * HM FIXME: complete?
 */
static struct possible_takeover_reshape_type *__get_possible_takeover_reshape_type(const struct lv_segment *seg_from,
										   const struct segment_type *segtype_to,
										   struct possible_type *last_pt)
{
	struct possible_takeover_reshape_type *lpt = (struct possible_takeover_reshape_type *) last_pt;
	struct possible_takeover_reshape_type *pt = lpt ? lpt + 1 : _possible_takeover_reshape_types;

	RETURN_IF_ZERO(seg_from, "segment from argument");

PFLA("seg_from=%s segtype_to=%s", lvseg_name(seg_from), segtype_to ? segtype_to->name : "NIL");

	for ( ; pt->current_types; pt++) {
		if ((seg_from->segtype->flags & pt->current_types) &&
		    (segtype_to ? (segtype_to->flags & pt->possible_types) : 1))
			if (seg_from->area_count <= pt->current_areas)
				return pt;
	}

	return NULL;
}

static struct possible_duplicate_type *__get_possible_duplicate_type(const struct segment_type *segtype_to,
								     uint32_t new_image_count,
								     struct possible_type *last_pt)
{
	struct possible_duplicate_type *lpt = (struct possible_duplicate_type *) last_pt;
	struct possible_duplicate_type *pt = lpt ? lpt + 1 : _possible_duplicate_types;

	RETURN_IF_ZERO(segtype_to, "segment type to argument");

	for ( ; pt->possible_types; pt++) {
		if (segtype_to->flags & pt->possible_types)
			if (new_image_count <= pt->new_areas)
				return pt;
	}

	return NULL;
}

static struct possible_type *_get_possible_type(const struct lv_segment *seg_from,
						const struct segment_type *segtype_to,
						uint32_t new_image_count,
						struct possible_type *last_pt)
{
	RETURN_IF_ZERO(seg_from, "segment from argument");

	return new_image_count ?
	       (struct possible_type *) __get_possible_duplicate_type(segtype_to, new_image_count, last_pt) :
	       (struct possible_type *) __get_possible_takeover_reshape_type(seg_from, segtype_to, last_pt);
}

/*
 * Return allowed options (--stripes, ...) for conversion from @seg_from -> @seg_to
 */
static int _get_allowed_conversion_options(const struct lv_segment *seg_from,
					   const struct segment_type *segtype_to,
					   uint32_t new_image_count, uint32_t *options)
{
	struct possible_type *pt;

	RETURN_IF_ZERO(seg_from, "segment from argument");
	RETURN_IF_ZERO(options, "options argument");

	if ((pt = _get_possible_type(seg_from, segtype_to, new_image_count, NULL))) {
		*options = pt->options;
		return 1;
	}

	return 0;
}

/*
 * Log any possible conversions for @lv
 */
/* HM FIXME: use log_info? */
typedef int (*type_flag_fn_t)(void *data);
/* HM Helper: loop through @pt->flags calling @tfn with argument @data */
static int _process_type_flags(struct logical_volume *lv, struct possible_type *pt, type_flag_fn_t tfn, void *data)
{
	unsigned i;
	uint64_t t;
	const struct lv_segment *seg;
	const struct segment_type *segtype;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	for (i = 0; i < 64; i++) {
		t = 1ULL << i;
		if ((t & pt->possible_types) &&
		    !(t & seg->segtype->flags) &&
		     ((segtype = get_segtype_from_flag(lv->vg->cmd, t))))
			if (!tfn(data ?: (void *) segtype))
				return 0;
	}

	return 1;
}

/* HM Helper: callback to increment unsigned  possible conversion types in @*data */
static int _count_possible_conversions(void *data)
{
	unsigned *possible_conversions;

	RETURN_IF_ZERO((possible_conversions = data), "data pointer argument");

	(*possible_conversions)++;

	return 1;
}

/* HM Helper: callback to log possible conversion to segment type in @*data */
static int _log_possible_conversion(void *data)
{
	struct segment_type *segtype = data;

	RETURN_IF_SEGTYPE_ZERO(segtype);

	log_warn("%s%s%s", segtype->name, segtype->descr ? " -> " : "", segtype->descr ?: "");

	return 1;
}

static int _log_possible_conversion_types(struct logical_volume *lv, const struct segment_type *new_segtype)
{
	int duplicating;
	unsigned possible_conversions = 0;
	const struct lv_segment *seg;
	struct possible_type *pt = NULL;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	duplicating = _lv_is_duplicating(lv);

	/* Count any possible segment types @seg an be directly converted to */
	while ((pt = _get_possible_type(seg, NULL, 0, pt)))
		RETURN_IF_ZERO(_process_type_flags(lv, pt, _count_possible_conversions, &possible_conversions),
			       "flags processed");

	if (possible_conversions) {
		if (!duplicating && !silent_mode()) {
			const char *alias = _get_segtype_alias(seg->segtype);

			log_print("Converting %s from %s%s%s%c (without --duplicate) is "
				  "directly possible to the following layout%s:",
				  display_lvname(lv), _get_segtype_name(seg->segtype, seg->area_count),
				  *alias ? " (same as " : "", alias, *alias ? ')' : 0,
				  possible_conversions > 1 ? "s" : "");

			pt = NULL;

			/* Print any possible segment types @seg can be directly converted to */
			while ((pt = _get_possible_type(seg, NULL, 0, pt)))
				RETURN_IF_ZERO(_process_type_flags(lv, pt, _log_possible_conversion, NULL),
					       "flags processed");
		}

	} else
		log_warn("Direct conversion of %s LV %s is not possible",
			 lvseg_name(seg), display_lvname(lv));

	if (duplicating)
		log_warn("To add more duplicated sub LVs, use \"lvconvert --duplicate ...\"");
	else
		log_warn("To convert to other arbitrary layouts by duplication, use \"lvconvert --duplicate ...\"");

	return 0;
}

/*
 * Find takeover raid flag for segment type flag of @seg
 */
/* Segment type flag correspondence raid5 <-> raid6 conversions */
static uint64_t _r5_to_r6[][2] = {
	{ SEG_RAID5_LS, SEG_RAID6_LS_6 },
	{ SEG_RAID5_LA, SEG_RAID6_LA_6 },
	{ SEG_RAID5_RS, SEG_RAID6_RS_6 },
	{ SEG_RAID5_RA, SEG_RAID6_RA_6 },
	{ SEG_RAID5_N,  SEG_RAID6_N_6 },
};


/* Return segment type flag for raid5 -> raid6 conversions */
static uint64_t _get_r56_flag(const struct lv_segment *seg, unsigned idx)
{
	unsigned elems = ARRAY_SIZE(_r5_to_r6);

	RETURN_IF_ZERO(seg, "lv segment argument");
	RETURN_IF_NONZERO(idx > 1, "proper index");

	while (elems--)
		if (seg->segtype->flags & _r5_to_r6[elems][idx])
			return _r5_to_r6[elems][!idx];

	return 0;
}

/* Return segment type flag for raid5 -> raid6 conversions */
static uint64_t _raid_seg_flag_5_to_6(const struct lv_segment *seg)
{
	RETURN_IF_ZERO(seg, "lv segment argument");

	return _get_r56_flag(seg, 0);
}

/* Return segment type flag for raid6 -> raid5 conversions */
static uint64_t _raid_seg_flag_6_to_5(const struct lv_segment *seg)
{
	RETURN_IF_ZERO(seg, "lv segment argument");

PFL();
	return _get_r56_flag(seg, 1);
}
/******* END: raid <-> raid conversion *******/



/****************************************************************************/
/****************************************************************************/
/****************************************************************************/
/* Construction site of takeover handler function jump table solution */

/* Display error message and return 0 if @lv is not synced, else 1 */
static int _lv_is_synced(struct logical_volume *lv)
{
	RETURN_IF_ZERO(lv, "lv argument");

	if (lv->status & LV_NOTSYNCED) {
		log_error("Can't convert out-of-sync LV %s"
			  " use 'lvchange --resync %s' first",
			  display_lvname(lv), display_lvname(lv));
		return 0;
	}

	return 1;
}

/* Begin: various conversions between layers (aka MD takeover) */
/*
 * takeover function argument list definition
 *
 * All takeover functions and helper functions
 * to support them have this list of arguments
 */
#define TAKEOVER_FN_ARGUMENTS			\
	struct logical_volume *lv,		\
	const struct segment_type *new_segtype,	\
	int yes, int force,			\
	unsigned new_image_count,		\
	const unsigned new_data_copies,		\
	const unsigned new_stripes,		\
	unsigned new_stripe_size,		\
	unsigned new_region_size,		\
	struct dm_list *allocate_pvs

#if 0
	unsigned new_extents,
#endif
/*
 * a matrix with types from -> types to holds
 * takeover function pointers this prototype
 */
typedef int (*takeover_fn_t)(TAKEOVER_FN_ARGUMENTS);

/* Return takeover function table index for @segtype */
static unsigned _takeover_fn_idx(const struct segment_type *segtype, uint32_t area_count)
{
	static uint64_t _segtype_to_idx[] = {
		0, /* linear, seg->area_count = 1 */
		SEG_AREAS_STRIPED,
		SEG_MIRROR,
		SEG_RAID0,
		SEG_RAID0_META,
		SEG_RAID1,
		SEG_RAID4|SEG_RAID5_LS|SEG_RAID5_LA|SEG_RAID5_LS|SEG_RAID5_RS|SEG_RAID5_RA|SEG_RAID5_N,
		SEG_RAID6_LS_6|SEG_RAID6_LA_6|SEG_RAID6_RS_6|SEG_RAID6_RA_6| \
		SEG_RAID6_NC|SEG_RAID6_NR|SEG_RAID6_ZR|SEG_RAID6_N_6,
		SEG_RAID10_NEAR|SEG_RAID10_FAR|SEG_RAID10_OFFSET,
		SEG_RAID01
	};
	unsigned r = ARRAY_SIZE(_segtype_to_idx);

	RETURN_IF_ZERO(segtype, "segment type argument");
	RETURN_IF_ZERO(area_count, "area count != 0 argument");

PFLA("segtype=%s area_count=%u", segtype->name, area_count);
	/* Linear special case */
	if (segtype_is_striped(segtype) && area_count == 1)
		return 0;

	while (r-- > 0)
		if (segtype->flags & _segtype_to_idx[r])
{
PFLA("r=%u", r);
			return r;
}

	return 0;
}

/* Macro to define raid takeover helper function header */
#define TAKEOVER_FN(function_name) \
static int function_name(TAKEOVER_FN_ARGUMENTS)

/* Macro to spot takeover helper functions easily */
#define TAKEOVER_HELPER_FN(function_name) TAKEOVER_FN(function_name)
#define TAKEOVER_HELPER_FN_REMOVAL_LVS(function_name) \
static int function_name(TAKEOVER_FN_ARGUMENTS, struct dm_list *removal_lvs)

/*
 * noop and error takoover handler functions
 * to allow for logging that an LV already
 * has the requested type or that the requested
 * conversion is not possible
 */
/* Noop takeover handler for @lv: logs that LV already is of the requested type */
TAKEOVER_FN(_noop)
{
	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));

	log_warn("Logical volume %s already is of requested type %s",
		 display_lvname(lv), lvseg_name(first_seg(lv)));

	return 0;
}

/* Error takeover handler for @lv: logs what's (im)possible to convert to (and mabye added later) */
TAKEOVER_FN(_error)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	_log_possible_conversion_types(lv, new_segtype);

	return 0;
}

/****************************************************************************/
/*
 * Conversion via creation of a new LV to put
 * top-level raid1 on top of initial maping and
 * N addtitional ones with arbitrayr supported layout.
 */
/* Create a new LV with type @segtype */
static struct logical_volume *_lv_create(struct volume_group *vg, const char *lv_name,
					 const struct segment_type *segtype,
					 const uint32_t data_copies, const uint32_t region_size,
					 const uint32_t stripes, const uint32_t stripe_size,
					 uint32_t extents, enum activation_change change,
					 int zero,
					 const char *pool_lv_name,
					 struct dm_list *pvs)
{
	struct logical_volume *r;
	struct lvcreate_params lp = {
		.activate = change,
		.alloc = ALLOC_INHERIT,
		.extents = pool_lv_name ? 0 : extents,
		.virtual_extents = pool_lv_name ? extents : 0,
		.major = -1,
		.minor = -1,
		.log_count = 0,
		.lv_name = lv_name,
		.mirrors = data_copies,
		.nosync = 0,
		.permission = LVM_READ | LVM_WRITE,
		/* HM FIXME: inherit on stacking LVs? */
		.read_ahead = DM_READ_AHEAD_AUTO,
		.region_size = region_size,
		.segtype = segtype,
		.stripes = stripes,
		.stripe_size = stripe_size,
		.tags = DM_LIST_HEAD_INIT(lp.tags),
		.temporary = 0,
		.zero = zero,
		.pool_name = pool_lv_name,
	};

	RETURN_IF_ZERO(vg, "vg argument");
	RETURN_IF_ZERO(lv_name, "lv name argument");
	RETURN_IF_ZERO(extents, "extents != 0 argument");
	RETURN_IF_ZERO(data_copies, "data copies argument");
	RETURN_IF_ZERO(stripes, "stripes argument");
	RETURN_IF_ZERO(segtype, "new segment argument");

	lp.pvh = pvs ?: &vg->pvs;

PFLA("lv_name=%s segtype=%s data_copies=%u stripes=%u region_size=%u stripe_size=%u extents=%u",
     lv_name, segtype->name, data_copies, stripes, region_size, stripe_size, extents);

	if (segtype_is_striped(segtype) && stripes == 1) {
		lp.mirrors = lp.stripes = 1;
		lp.stripe_size = 0;

	/* Caller should ensure all this... */
	} else if (segtype_is_raid1(segtype) && stripes != 1) {
		log_warn("Adjusting stripes to 1i for raid1");
		lp.stripes = 1;
	}

	else if (segtype_is_striped_raid(segtype)) {
		if (stripes < 2) {
			log_warn("Adjusting stripes to the minimum of 2");
			lp.stripes = 2;
		}
		if (!lp.stripe_size) {
			log_warn("Adjusting stripesize to 32KiB");
			lp.stripe_size = 64;
		}
	}

	else if (segtype_is_any_raid10(segtype)) {
		if (data_copies < 2)
			lp.mirrors = 2;

		if (data_copies > stripes) {
			log_error("raid10 data_copies may not be more than stripes (i.e. -mN with N < #stripes)");
			return_NULL;
		}

	} else if (segtype_is_mirror(segtype)) {
		lp.mirrors = data_copies > 1 ? data_copies : 2;
		lp.log_count = 1;
		lp.stripes = 1;
		lp.stripe_size = 0;
	}

	log_debug_metadata("Creating new logical volume %s/%s.", vg->name, lp.lv_name);

	init_silent(1);
	r = lv_create_single(vg, &lp);
	init_silent(0);

	return r;
}

/* Helper: create a unique name from @lv->name and string @(suffix + 1) adding a number */
static char *_generate_unique_raid_name(struct logical_volume *lv, const char *suffix)
{
	char *name;
	uint32_t s = 0;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(suffix, "name suffix argument");

	/* Loop until we found an available one */
	while (!(name = _generate_raid_name(lv, suffix, s)))
		s++;

	if (!name)
		log_error("Failed to create unique sub LV name for %s", display_lvname(lv));

	return name;
}

/* Helper: rename single @lv from sub string @from to @to; strings must have the same length */
static int _rename_lv(struct logical_volume *lv, const char *from, const char *to)
{
	size_t sz;
	char *name, *p;
 
	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO((name = (char *) lv->name), "lv name");

	/* Catch being called on non-raid namespace */
	if (!(p = strstr(lv->name, from)))
		return 1;
 
	sz = p - lv->name + strlen(to) + (strlen(p) - strlen(from)) + 1;
	RETURN_IF_ZERO((name = dm_pool_alloc(lv->vg->vgmem, sz)), "space for name");
	        
	sz = p - lv->name;
	strncpy(name, lv->name, sz);
	strncpy(name + sz, to, strlen(to));
	strcpy(name + sz + strlen(to), p + strlen(from));
	lv->name = name;
 
	return 1;
}
 
/* HM Helper: rename @lv to string @to and replace all its sub LV names substring @from to @to */
static int _rename_lv_and_sub_lvs(struct logical_volume *lv, const char *from, const char *to)
{
	uint32_t s;
	struct logical_volume *lv1;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (seg->area_count > 1)
		for (s = 0; s < seg->area_count; s++) {
			if ((lv1 = _seg_metalv_checked(seg, s)) &&
			    !_rename_lv(lv1, from, to))
				return 0;
			if ((lv1 = _seg_lv_checked(seg, s)) &&
			    !_rename_lv(lv1, from, to))
				return 0;
		}

	lv->name = to;

	return 1;
}

/* Get maximum name index suffix from all sub LVs of @lv and report in @*max_idx */
static int _get_max_sub_lv_name_index(struct logical_volume *lv, uint32_t *max_idx)
{
	uint32_t s, idx;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
	RETURN_IF_ZERO(max_idx, "max index argument");

	*max_idx = 0;

	for (s = 0; s < seg->area_count; s++) {
		if (!_seg_lv_checked(seg, s))
			return 0;

		if (!_lv_name_get_string_index(seg_lv(seg, s), &idx))
			return 0;

		if (*max_idx < idx)
			*max_idx = idx;
	}

	return 1;
}

/*
 * Prepare first segment of @lv to suit _shift_image_components()
 * 
 * Being called with areas arrays one larger than seg->area_count
 * and all slots shifted to the front with the last one unassigned.
 *
 * HM FIXME: simplify
 */
static int _prepare_seg_for_name_shift(struct logical_volume *lv)
{
	int s;
	uint32_t idx, max_idx;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	if (!_get_max_sub_lv_name_index(lv, &max_idx))
		return 0;

	seg->area_count++;
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, max_idx);

	for (s = seg->area_count - 1; s > -1; s--) {
		if (!_seg_lv_checked(seg, s))
			continue;

		RETURN_IF_ZERO(seg_metatype(seg, s) == AREA_LV, "valid metadata sub LV")

		if (!_lv_name_get_string_index(seg_lv(seg, s), &idx))
			return 0;

		RETURN_IF_SEG_AREA_INDEX_FALSE(seg, idx);

		if (idx != s) {
			seg->areas[idx] = seg->areas[s];
			seg_type(seg, s) = AREA_UNASSIGNED;

			if (seg->meta_areas) {
				seg->meta_areas[idx] = seg->meta_areas[s];
				seg_metatype(seg, s) = AREA_UNASSIGNED;
			}
		}
	}

	return 1;
}

/* HM Helper: rename sub LVs to avoid conflict on creation of new metadata LVs */
enum rename_dir { to_flat = 0, from_flat, from_dup, to_dup };
static int _rename_sub_lvs(struct logical_volume *lv, enum rename_dir dir)
{
	int type;
	static const int *ft, from_to[][2] = { { 0, 2 }, { 2, 0 } };
	uint32_t s;
	static const char *names[][4] = {
		{ "_rimage_", "_rmeta_", "__rimage_", "__rmeta_" }, /* flat */
		{ "_dup_",    "_rmeta_", "__dup_",    "__rmeta_" }, /* dup */
	};
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	if (seg_is_thin(seg))
		return 1;

	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
PFL();
	if (!lv_is_raid(lv) && !lv_is_raid_image(lv))
		return 1;
PFL();
	RETURN_IF_NONZERO(dir < to_flat || dir > to_dup, "valid rename request");
	type = dir / 2; /* flat or dup names */
	RETURN_IF_ZERO(type < ARRAY_SIZE(names), "valid type");
	ft = from_to[!(dir % 2)]; /* from or to indexes */

	log_debug_metadata("Renaming %s sub LVs to avoid name collision", display_lvname(lv));
	for (s = 0; s < seg->area_count; s++) {
		if (_seg_metalv_checked(seg, s) &&
		    !_rename_lv(seg_metalv(seg, s), names[type][ft[0]+1], names[type][ft[1]+1]))
			return 0;

		if (_seg_lv_checked(seg, s) &&
		    !_rename_lv(seg_lv(seg, s), names[type][ft[0]], names[type][ft[1]]))
			return 0;
	}
PFL();
	return 1;
}

/* rename @dlv and @mlv to / from ddup to avoid name collisions during name shift */
static int _rename_split_duplicate_lv_and_sub_lvs(struct logical_volume *dlv,
						  struct logical_volume *mlv,
						  enum rename_dir dir)
{
	const char *in[] = { "_dup_", "__dup_" };
	const char *mn[] = { "_rmeta_", "__rmeta_" };
	int d;

	switch (dir) {
	case to_dup:
		d = 1; break;
	case from_dup:
		d = 0; break;
	default:
		RETURN_IF_ZERO(0, "proper direction to rename");
	}

	return _rename_sub_lvs(dlv, dir) &&
	       _rename_lv(dlv, in[!d], in[d]) &&
	       mlv ? _rename_lv(mlv, mn[!d], mn[d]) : 1;
}

/*
 * HM Helper:
 *
 * remove layer from @lv keeping first sub LV,
 * add top-level lv to list @removal_lvs,
 * rename from flat "_rimage_|_rmeta_" namespace 
 * to "__rimage_|__rmeta_" to avoid name collision,
 * reset duplicated flag and make visible
 */
static int _remove_duplicating_layer(struct logical_volume *lv,
				     struct dm_list *removal_lvs)
{
	struct logical_volume *slv;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(seg->area_count == 1, "no single area");
	RETURN_IF_ZERO((slv = seg_lv(seg, 0)), "no first sub LV");

	/* Ensure proper size of LV, sub LV may be larger due to rounding. */
	lv->le_count = slv->le_count;
	lv->size = lv->le_count * lv->vg->extent_size;

	if (!_lv_reset_raid_add_to_list(slv, removal_lvs))
		return 0;

	/* Remove the raid1 layer from the LV */
	if (!remove_layer_from_lv(lv, slv))
		return_0;

	/* Adjust any thin volume flag which is not handled by remove_layer_from_lv() */
	slv->status &= ~THIN_VOLUME;

	/* HM FIXME: in case of _lv_reduce() recursion bugs, this may hit */
	RETURN_IF_ZERO((seg = first_seg(lv)), "first segment!?");

	if (!_rename_sub_lvs(lv, from_flat))
		return 0;

	lv->status &= ~LV_DUPLICATED;
	if (seg_is_thin(seg))
		lv->status |= THIN_VOLUME;

	lv_set_visible(lv);

	return 1;
}

/*
 * HM Helper:
 *
 * _raid_split_duplicate() callback to rename duplicated
 * @lv and its sub LVs to __ namespace to avoid collisions
 */
static int _pre_raid_split_duplicate_rename_lv_and_sub_lvs(struct logical_volume *lv, void *data)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_NONZERO(data, "data argument allowed");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0); /* area_count > 0 check */

	/* Rename all remaning sub LVs temporarilly to allow for name shift w/o name collision */
	log_debug_metadata("Renaming split duplicate LV and sub LVs of %s", display_lvname(lv));
	for (s = 0; s < seg->area_count; s++)
		if (_seg_lv_checked(seg, s) &&
		    !_rename_split_duplicate_lv_and_sub_lvs(seg_lv(seg, s), _seg_metalv_checked(seg, s), to_dup))
			return 0;

	return _vg_write_lv_suspend_vg_commit(lv) ? 2 : 0;
}

/*
 * HM Helper:
 * 
 * _raid_split_duplicate() callback to rename duplicated
 * @lv and its sub LVs back from "__" infixed namespace
 */
static int _post_raid_split_duplicate_rename_lv_and_sub_lvs(struct logical_volume *lv, void *data)
{
	uint32_t s;
	struct lv_segment *seg;
	struct logical_volume *split_lv, *lv1;
	struct volume_group *vg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO((split_lv = data), "valid split LV");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0); /* area_count > 0 check */

	vg = lv->vg;

	/* Make @split_lv visible */
	if (!_suspend_resume_lv(split_lv))
		return_0;

for (s = 0; s < seg->area_count; s++)
PFLA("seg_lv(seg, %u)=%s", s, _seg_lv_checked(seg, s) ? display_lvname(seg_lv(seg, s)) : "");

	/* Rename all remaning sub LVs temporarilly to allow for name shift w/o name collision */
	log_debug_metadata("Renaming duplicate LV and sub LVs of %s", display_lvname(lv));
	for (s = 0; s < seg->area_count; s++)
		if ((lv1 = _seg_lv_checked(seg, s)) &&
		    !_rename_split_duplicate_lv_and_sub_lvs(lv1, _seg_metalv_checked(seg, s), from_dup))
			return 0;
PFL();
	/* Shift area numerical indexes down */
	/* HM FIXME: avoid _prepare_seg_for_name_shift() in favour of better _shift_image_components */
	log_debug_metadata("Shifting image components of %s", display_lvname(lv));
	if (!_prepare_seg_for_name_shift(lv) ||
	    !_shift_image_components(seg))
		return 0;
for (s = 0; s < seg->area_count; s++)
PFLA("seg_lv(seg, %u)=%s", s, _seg_lv_checked(seg, s) ? display_lvname(seg_lv(seg, s)) : "");

PFL();
	if (!_vg_write_commit(vg))
		return 0;
PFL();
	for (s = 0; s < seg->area_count - 1; s++) {
		if ((lv1 = _seg_lv_checked(seg, s)) &&
		    !_suspend_resume_lv(lv1))
			return 0;
		if ((lv1 = _seg_metalv_checked(seg, s)) &&
		    !_suspend_resume_lv(lv1))
			return 0;
	}

	if (!backup(vg))
		log_error("Backup of VG %s failed; continuing", vg->name);

	return 1;
	// return 2; /* Fails to remove last sub LVs MDs */
}

/*
 * HM Helper:
 *
 * _raid_split_duplicate() callback to extract last metadata
 * image of @lv and remove top-level, duplicating raid1 layer
 */
static int _pre_raid_split_duplicate_remove_layer(struct logical_volume *lv, void *data)
{
	struct dm_list *removal_lvs;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO((removal_lvs = data), "remova LVs list");

	log_debug_metadata("Extracting last metadata LV of %s", display_lvname(lv));
	if (!_extract_image_component_sublist(seg, RAID_META, 0, 1, removal_lvs, 1))
		return 0;

	log_debug_metadata("Removing top-level raid1 LV %s", display_lvname(lv));
	return _remove_duplicating_layer(lv, removal_lvs);
}

/*
 * HM Helper:
 *
 * _raid_split_duplicate() callback when removing duplicating layer
 * to rename all sub_lvs of @lv (if raid) to flat namespace
 */
static int _post_raid_split_duplicate_rename_sub_lvs(struct logical_volume *lv, void *data)
{
	struct logical_volume *split_lv;

	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));
	RETURN_IF_ZERO((split_lv = data), "split LV");

	if (!_suspend_resume_lv(split_lv))
		return 0;

	log_debug_metadata("Renaming sub LVs of %s", display_lvname(lv));

	return _rename_sub_lvs(lv, to_flat);
}

/*
 * HM Helper:
 *
 * split off a sub LV of a duplicatting top-level raid1 @lv
 *
 * HM FIXME: allow for splitting off duplicated lv with "lvconvert --splitmirrors N # (N > 1)"?
 *           need ***sub_lv_names for this?
 */
static int _raid_split_duplicate(struct logical_volume *lv, int yes,
				 const char *split_name, uint32_t new_image_count)
{
	uint32_t s;
	void *fn_pre_data;
	const char *lv_name;
	struct dm_list removal_lvs;
	struct lv_segment *seg;
	struct logical_volume *split_lv = NULL;
	fn_on_lv_t fn_pre_on_lv, fn_post_on_lv;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
	RETURN_IF_ZERO(_lv_is_duplicating(lv), "Called with non-duplicating LV");
	RETURN_IF_ZERO(split_name, "split name argument");
	RETURN_IF_ZERO(seg->meta_areas, "metadata segment areas");

	if (!_lv_is_active((lv)))
		return 0;

	dm_list_init(&removal_lvs);

	/* If user passed in the sub LV name to split off and no --name option, use it */
	if (!_valid_name_requested(&lv, &split_name, 0 /* properties */, "split"))
		return 0;

	/* Try to find @split_name amongst sub LVs */
	if (!(split_lv = _find_lv_in_sub_lvs(lv, split_name, &s))) {
		log_error("No sub LV %s to split off duplicating LV %s", split_name, display_lvname(lv));
		return 0;
	}

	/* HM FIXME: limited to one sub LV to split off */
	if (seg->area_count - new_image_count != 1) {
		log_error("Only one duplicated sub LV can be split off duplicating LV %s at once",
			  display_lvname(lv));
		return 0;
	}

	/* Create unique split LV name to use (previous splits may exist) */
	RETURN_IF_ZERO((lv_name = _generate_unique_raid_name(lv, "split_")), "unique LV name created");

	/*
	 * Allow for intentionally splitting off unsynchronized LV in case user e.g. created
	 * a duplicated LV in error, aims to delete it and it is still syning too long
	 */
	if (!_dev_in_sync(lv, s)) {
		log_warn("Splitting off unsynchronized sub LV %s!", 
			 display_lvname(split_lv));
		if (yes_no_prompt("Do you really want to split off out-of-sync %s sub LV %s into %s/%s [y/n]: ",
				  lvseg_name(first_seg(split_lv)), display_lvname(split_lv),
				  lv->vg->name, lv_name) == 'n')
			return 0;

	} else if (!_raid_in_sync(lv) &&
		   _devs_in_sync_count(lv) < 2) {
		log_error("Can't split off %s into %s/%s when LV %s is not in sync",
			  split_name, lv->vg->name, lv_name, display_lvname(lv));
		return 0;

	} else if (!yes && yes_no_prompt("Do you really want to split off %s sub LV %s into %s/%s [y/n]: ",
					 lvseg_name(first_seg(split_lv)), display_lvname(split_lv),
					 lv->vg->name, lv_name) == 'n')
		return 0;

	if (sigint_caught())
		return_0;

	log_debug_metadata("Extract metadata image for split LV %s", split_name);
	if (!_extract_image_component_sublist(seg, RAID_META, s, s + 1, &removal_lvs, 1))
		return 0;

	/* Remove reference from @seg to @split_lv */
	RETURN_IF_ZERO(remove_seg_from_segs_using_this_lv(split_lv, seg), "removal of split LV reference")

	/* We keep @split_lv, so set its area n the duplicating top-level LV to unassigned */
	seg_type(seg, s) = AREA_UNASSIGNED;

	log_debug_metadata("Rename duplicated LV %s and and any of its sub LVs before splitting them off",
			   display_lvname(split_lv));
	if (!_rename_lv_and_sub_lvs(split_lv, split_lv->name, lv_name))
		return 0;

	seg->area_count--;
	seg->data_copies--;
	RETURN_IF_ZERO(seg->area_count == seg->data_copies, "valid data copies");

	split_lv->status &= ~(LV_NOTSYNCED | LV_DUPLICATED);
	lv_set_visible(split_lv);

	/* Have to shift areas down if split LV wasn't the last one to avoid areas inconsistency */
	for ( ; s < seg->area_count; s++) {
		seg->areas[s] = seg->areas[s + 1];
		seg->meta_areas[s] = seg->meta_areas[s + 1];
	}

	/* We have more than one sub LVs -> set up pre/post fns to rename and shift sub LVs down */
	if (seg->area_count > 1) {
		fn_pre_data = NULL;
		fn_pre_on_lv = _pre_raid_split_duplicate_rename_lv_and_sub_lvs;
		fn_post_on_lv = _post_raid_split_duplicate_rename_lv_and_sub_lvs;

	/* We are down to the last sub LV -> set up pre/post fns to remove the top-level raid1 mapping */
	} else  {
		log_print_unless_silent("The duplicating layer will be removed keeping %s sub LV %s for %s",
					lvseg_name(first_seg(seg_lv(seg, 0))),
					display_lvname(seg_lv(seg, 0)), display_lvname(lv));
		fn_pre_data = &removal_lvs;
		fn_pre_on_lv  = _pre_raid_split_duplicate_remove_layer;
		fn_post_on_lv = _post_raid_split_duplicate_rename_sub_lvs;
	}

	log_debug_metadata("Updating VG metadata; reloading %s and activating split LV %s",
			   display_lvname(lv), display_lvname(split_lv));
	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs,
							 fn_post_on_lv, split_lv,
							 fn_pre_on_lv, fn_pre_data);
}

/*
 * HM Helper:
 *
 * extract all rmeta images of the top-level @lv and all but @keep_idx
 * data image and put the extracted ones on @removal_lvs.
 */
static int _lv_extract_all_images_but_one(struct logical_volume *lv,
					  uint32_t keep_idx,
					  struct dm_list *removal_lvs)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(removal_lvs, "removal LVs list pointer argument");

	if (!_extract_image_component_list(seg, RAID_META, 0, removal_lvs) ||
	    (keep_idx &&
	     !_extract_image_component_sublist(seg, RAID_IMAGE, 0, keep_idx, removal_lvs, 0)) ||
	    (keep_idx < seg->area_count - 1 &&
	     !_extract_image_component_sublist(seg, RAID_IMAGE, keep_idx + 1, seg->area_count, removal_lvs, 0))) {
		log_error(INTERNAL_ERROR "Failed to extract top-level LVs %s images", display_lvname(lv));
		return 0;
	}

	/*
	 * If we don't keep the first sub LV, move sub LV at @keep_idx areas
	 * across to the first slot and adjust area_count to the remaining one
	 */
	if (keep_idx)
		seg->areas[0] = seg->areas[keep_idx];

	seg->area_count = 1;

	return 1;
}

/* HM Helper: callback to rename any sub LVs of @lv to flat namespace */
static int _post_raid_unduplicate_rename_sub_lvs(struct logical_volume *lv, void *data)
{
	if (!lv_is_raid(lv))
		return 1;

	return _rename_sub_lvs(lv, to_flat) &&
	       lv_update_and_reload(lv);
}

/*
 * HM Helper:
 *
 * remove top-level raid1 @lv and replace with requested @sub_lv_name
 */
static int _raid_unduplicate(struct logical_volume *lv,	
			     int yes, const char *sub_lv_name)
{
	uint32_t keep_idx;
	struct logical_volume *keep_lv;
	struct lv_segment *seg, *seg1;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
	RETURN_IF_ZERO(sub_lv_name, "sub LV name");

	if (!_lv_is_duplicating(lv)) {
		log_error(INTERNAL_ERROR "Called with non-duplicating LV %s",
			  display_lvname(lv));
		return 0;
	}

	/* Find the requested sub LV by name */
	if (!(keep_lv = _find_lv_in_sub_lvs(lv, sub_lv_name, &keep_idx))) {
		log_error("Duplicated sub LV name %s does not exist in duplicating LV %s",
			  sub_lv_name, display_lvname(lv));
		return 0;
	}

	/* Keeping a leg other than the master requires it to be fully in sync! */
	if (keep_idx && !_raid_in_sync(lv)) {
		log_error("Can't convert to duplicated sub LV %s when LV %s is not in sync",
			  display_lvname(keep_lv), display_lvname(lv));
		return 0;
	}

	if (!yes)
		log_warn("Really unduplicate LV %s keeping %s LV %s? Check resilience!",
			 display_lvname(lv), lvseg_name(first_seg(keep_lv)), display_lvname(keep_lv));
	if (lv->le_count != keep_lv->le_count)
		log_warn("You may want to resize your LV content (e.g. filesystem) after unduplicating it");
	if (!yes) {
		RETURN_IF_ZERO((seg1 = first_seg(keep_lv)), "segment in induplicate LV");

		if (yes_no_prompt("Do you want to convert %s to type %s thus "
				  "unduplicating it and removing %u duplicated LV(s)? [y/n]: ",
				  display_lvname(lv),
				  _get_segtype_name(seg1->segtype, seg1->area_count),
				  seg->area_count - 1) == 'n')
			return 0;

		if (sigint_caught())
			return_0;
	}

	dm_list_init(&removal_lvs);

	/*
	 * Extract all rmeta images of the raid1 top-level LV
	 * and all but the @keep_idx indexed data image.
	 */
	if (!_lv_extract_all_images_but_one(lv, keep_idx, &removal_lvs))
		return 0;

	/* Remove top-level raid1 layer keeping first sub LV and update+reload LV */
	return _remove_duplicating_layer(lv, &removal_lvs) &&
	       _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs,
							 _post_raid_unduplicate_rename_sub_lvs, lv, NULL);
}

/*
 * HM Helper:
 *
 * create a new duplicating LV for @lv based on parameters
 * @new_segtype, ...* and utilize PVs on @allocate_pvs list
 * for allocation
 *
 * If creation succeeds but any other step fails, try removing
 * it so that the user only has to remove any created *_dup_* lv
 * manually in case of a crash.
 */
static struct logical_volume *_dup_lv_create(struct logical_volume *lv,
					     const struct segment_type *new_segtype,
					     const char *lv_name,
					     const uint32_t new_data_copies, const uint32_t region_size,
					     const uint32_t new_stripes, const uint32_t new_stripe_size,
					     const uint32_t extents, const char *pool_lv_name,
					     struct dm_list *allocate_pvs)
{
	uint32_t stripe_size = new_stripe_size;
	struct logical_volume *r;

	RETURN_IF_LV_SEGTYPE_ZERO(lv, new_segtype);

	log_debug_metadata("Creating unique LV name for destination sub LV");
	RETURN_IF_ZERO(lv_name, "lv_name argument");

	if (!stripe_size && new_stripes > 1)
		stripe_size = 64;

	/* Create the destination LV deactivated, then change names and activate to avoid unsafe table loads */
	log_debug_metadata("Creating destination sub LV");
	if (!(r = _lv_create(lv->vg, lv_name, new_segtype, new_data_copies, region_size,
			     new_stripes, stripe_size, extents, CHANGE_ALN, 0 /* zero */,
			     pool_lv_name, allocate_pvs))) {
		log_error("Failed to create destination LV %s/%s", lv->vg->name, lv_name);
		return 0;
	}

	if (extents != r->le_count)
		log_warn("Duplicating sub LV %s with %u extents is larger than "
			  "%u due to stripe boundary rounding",
			  display_lvname(r), r->le_count, extents);

	r->status |= (RAID_IMAGE | LV_DUPLICATED);
	lv_set_hidden(r);

	return r;
}

/*
 * Helper:
 *
 * callback function to rename metadata sub LVs of top-level duplicating @lv
 *
 * Return 2 on success to inform caller, that metadata got already committed.
 */
static int _pre_raid_duplicate_rename_metadata_sub_lvs(struct logical_volume *lv, void *data)
{
	uint32_t s;
	struct logical_volume *dup_lv, *lv1;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO((dup_lv = data), "duplicated LV argument");
	RETURN_IF_ZERO(seg->meta_areas, "metadata areas");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0); /* area_count > 0 check */

	/* Rename top-level raid1 metadata sub LVs to their temporary names */
	log_debug_metadata("Renaming sub LVs of %s to temporary names",
			   display_lvname(lv));
	for (s = 0; s < seg->area_count; s++)
		if ((lv1 = _seg_metalv_checked(seg, s)) &&
		     !_rename_lv(lv1, "_rmeta_", "__rmeta_"))
			return 0;

	/* HM FIXME: still unsafe table load messages on some duplicated LVs !? */
	if (!_vg_write_lv_suspend_vg_commit(lv))
		return 0;

	if (!_activate_sub_lvs(dup_lv, 0))
		return 0;

	for (s = 0; s < seg->area_count; s++)
		if (!_activate_sub_lvs(seg_lv(seg, s), 0))
			return 0;

	/*
	 * Optionally pass "nosync" to kernel in case of LV_NOTSYNCED flag;
	 * e.g. raid5 does not need to be synced as duplicated LV
	 * but raid6 does to initialize parity/q-syndrome properly
	 */
	init_mirror_in_sync((dup_lv->status & LV_NOTSYNCED) ? 1 : 0);

	if (!activate_lv_excl_local(dup_lv->vg->cmd, dup_lv))
		return 0;

	init_mirror_in_sync(0);

	return 2; /* 2: metadata commited */
}

/* HM Helper: callback function to rename metadata sub LVs of top-level duplicating @lv back */
static int _post_raid_duplicate_rename_metadata_sub_lvs(struct logical_volume *lv, void *data)
{
	uint32_t s;
	struct lv_segment *seg;
	struct logical_volume *dup_lv, *mlv;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO((dup_lv = data), "duplicated LV argument");
	RETURN_IF_ZERO(seg->meta_areas, "metadata areas");
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0); /* area_count > 0 check */

	/* Rename top-level raid1 metadata sub LVs to their final names */
	log_debug_metadata("Renaming sub LVs of %s to final names",
			   display_lvname(lv));
	for (s = 0; s < seg->area_count; s++)
		if ((mlv = _seg_metalv_checked(seg, s)) &&
		    !_rename_lv(mlv, "__rmeta_", "_rmeta_"))
			return 0;

	/* Reset in case it got set to avoid syncronization of the new duplicated sub LV */
	dup_lv->status &= ~LV_NOTSYNCED;

	if (!_vg_write_commit(lv->vg))
		return 0;

	for (s = 0; s < seg->area_count; s++)
		if ((mlv = _seg_metalv_checked(seg, s)) &&
		    !_suspend_resume_lv(mlv))
			return 0;

	if (!backup(lv->vg))
		log_error("Metadata backup of VG %s failed; continuing", lv->vg->name);

	return 2;
}

/*
 * HM Helper: raid to raid conversion by duplication
 *
 * Inserts a layer on top of the given @lv (if not duplicating already),
 * creates and allocates a destination LV of ~ the same size (may be rounded)
 * with the requested @new_segtype and properties (e.g. stripes).
 */
static int _raid_duplicate(struct logical_volume *lv,
			   const struct segment_type *new_segtype,
			   int yes, int force,
			   const int new_data_copies,
			   const uint32_t new_region_size,
			   const uint32_t new_stripes,
			   const uint32_t new_stripe_size,
			   const char *pool_lv_name,
			   struct dm_list *allocate_pvs)
{
	int duplicating;
	uint32_t data_copies = new_data_copies, extents, new_area_idx, raid1_image_count, s;
	char *lv_name, *p, *suffix;
	struct logical_volume *dup_lv;
	struct lv_segment *seg;

	/* new_segtype is allowed to be naught */
	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_NONZERO(!seg_is_thin(seg) && !seg->area_count, "lv segment areas");
	RETURN_IF_ZERO(data_copies, "data copies argument");
	RETURN_IF_ZERO(allocate_pvs, "allocate pvs");

	raid1_image_count = seg->area_count + 1;

	if (data_copies < 2 &&
	    (segtype_is_mirror(new_segtype) ||
	     segtype_is_raid1(new_segtype) ||
	     segtype_is_any_raid10(new_segtype))) {
		data_copies = seg->data_copies;
		log_warn("Adjusting data copies to %u", data_copies);
	}

	if (!(duplicating = _lv_is_duplicating(lv)) &&
	    lv_is_duplicated(lv)) {
		log_error("Can't duplicate duplicated sub LV %s", display_lvname(lv));
		if ((p = strchr(lv->name, '_'))) {
			*p = '\0';
			log_error("Use \"lvconvert --duplicate --type ...\" on top-level duplicating LV %s!",
				  display_lvname(lv));
			*p = '_';
		}

		return 0;
	}

	/* Kernel md can only manage one resync at a time */
	if (duplicating && !_raid_in_sync(lv)) {
		log_error("Duplicating LV %s must be in-sync before adding another duplicated sub LV",
			  display_lvname(lv));
		return 0;
	}

	log_warn("A new duplicated %s LV will be allocated and LV %s will be synced to it.",
		 _get_segtype_name(new_segtype, new_stripes), display_lvname(lv));
	log_warn("When unduplicating LV %s or splitting off a sub LV from %s, you can select any sub LV providing its name via:",
		 display_lvname(lv), display_lvname(lv));
	log_warn("'lvconvert --unduplicate --name sub-lv-name %s' or 'lvconvert --splitmirror 1 --name sub-lv-name %s'",
		 display_lvname(lv), display_lvname(lv));
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 1, raid1_image_count, data_copies, new_stripes, new_stripe_size))
		return 0;

	/*
	 * Creation of destination LV with intended layout and insertion of raid1 top-layer from here on
	 */
	if (segtype_is_raid1(new_segtype) &&
	    new_data_copies < 2)
		new_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED);

	/* Requested size has to be netto, i.e. w/o reshape space */
	extents = lv->le_count - _reshape_len_per_lv(lv);

	/*
	 * By default, prevent any PVs holding image components from
	 * being used for allocation unless --force provided or
	 * unless new segment type is thin.
	 */
	if (!force && !segtype_is_thin(new_segtype)) {
		log_debug_metadata("Avoiding coallocation on PVs holding other LVs of %s",
				   display_lvname(lv));
		if (!_avoid_pvs_with_other_images_of_lv(lv, allocate_pvs))
			return 0;
	}

	/*
	 * Get name for new duplicated LV
	 *
	 * Pick a unique sub LV name when already duplicating
	 * The initial duplicated LV shall be suffixed sith '1',
	 * because the master leg shall get '0'
	 */
	if (duplicating) {
		RETURN_IF_ZERO((lv_name = _generate_unique_raid_name(lv, "dup_")), "unique LV name created");
	} else {
		RETURN_IF_ZERO((lv_name = _generate_raid_name(lv, "dup_", 1)), "lv_name created");
	}

	if (!(dup_lv = _dup_lv_create(lv, new_segtype, lv_name,
				      new_data_copies, new_region_size,
				      new_stripes, new_stripe_size,
				      extents, pool_lv_name, allocate_pvs)))
		return 0;

	/* HM FIXME: remove dup_lv in case of any following failure */

	/* If not yet duplicating -> add the top-level raid1 mapping with given LV as master leg */
	if (!duplicating) {
		char *first_name;

		log_debug_metadata("Creating unique LV name for source sub LV");
		ERR_IF_ZERO((first_name = _generate_raid_name(lv, "dup_", 0)), "first sub LV name created");
		ERR_IF_ZERO((suffix = strstr(first_name, "_dup_")), "source prefix found");
		log_debug_metadata("Inserting layer LV on top of source LV %s", display_lvname(lv));
		ERR_IF_ZERO((seg = _convert_lv_to_raid1(lv, suffix)), "conversion to raid1 possible");
		RETURN_IF_ZERO(_seg_lv_checked(seg, 0), "sub LV");
		seg_lv(seg, 0)->status |= LV_DUPLICATED; /* set duplicated flag on sub LV */
		seg->meta_areas = NULL;
	}

	/* Grow areas arrays for data and metadata devs to add new duplicated LV */
	log_debug_metadata("Reallocating areas array of %s", display_lvname(lv));
	ERR_IF_ZERO(_realloc_meta_and_data_seg_areas(lv, seg->area_count + 1),
		    "reallocation of areas array possible");

	new_area_idx = seg->area_count;
	seg->data_copies = ++seg->area_count; /* Must update area count after resizing it */

	/* Set @layer_lv as the LV of @area of @lv */
	log_debug_metadata("Add duplicated LV %s to top-level LV %s as raid1 leg %u",
			   display_lvname(dup_lv), display_lvname(lv), new_area_idx);
	ERR_IF_ZERO(set_lv_segment_area_lv(seg, new_area_idx, dup_lv, dup_lv->le_count, dup_lv->status),
		    "addition of duplicated sub LV possible");
	_allow_pvs(allocate_pvs);

PFLA("seg->area_count=%u", seg->area_count);
	/* If not duplicating yet, allocate first top-level raid1 metadata LV */
	if (!duplicating) {
		ERR_IF_ZERO(_alloc_rmeta_for_lv_add_set_hidden(lv, 0, allocate_pvs),
			    "allocation of metadata image possible");
		ERR_IF_ZERO(_avoid_pvs_with_other_images_of_lv(seg_metalv(seg, 0), allocate_pvs),
			    "avoidance of PV allocation")
	}

	/* Allocate new top-level raid1 metadata LV for duplicated sub LV */
	if (!_alloc_rmeta_for_lv_add_set_hidden(lv, new_area_idx, allocate_pvs))
		return 0;
	_allow_pvs(allocate_pvs);

	for (s = 0; s < seg->area_count; s++)
		seg_lv(seg, s)->status &= ~LV_REBUILD;

	if (seg->area_count > 2)
		dup_lv->status |= LV_REBUILD;

	dup_lv->status &= ~LV_NOTSYNCED;

	if (segtype_is_any_raid6(new_segtype))
		dup_lv->status |= LV_NOTSYNCED;

	lv_set_visible(lv);

PFLA("lv0->le_count=%u lv1->le_count=%u", seg_lv(seg, 0)->le_count, seg_lv(seg, 1)->le_count);
	return _lv_update_reload_fns_reset_eliminate_lvs(lv, NULL,
							 _post_raid_duplicate_rename_metadata_sub_lvs, dup_lv,
							 _pre_raid_duplicate_rename_metadata_sub_lvs, dup_lv);
err:
	return lv_remove(dup_lv);
}

/*
 * Begin takeover helper funtions
 */
/* Helper: linear -> raid0* */
TAKEOVER_HELPER_FN(_linear_raid0)
{
	struct lv_segment *seg;
	struct dm_list meta_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);

	dm_list_init(&meta_lvs);

	if ((!seg_is_linear(seg) && !seg_is_any_raid0(seg)) ||
	    seg->area_count != 1 ||
	    new_image_count != 1) {
		log_error(INTERNAL_ERROR "Can't convert non-(linear|raid0) LV or from/to image count != 1");
		return 0;
	}

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* Convert any linear segment to raid1 by inserting a layer and presetting segtype as raid1 */
	if (seg_is_linear(seg)) {
		log_debug_metadata("Converting logical volume %s to raid",
				   display_lvname(lv));
		if (!(seg = _convert_lv_to_raid1(lv, "_rimage_0")))
			return 0;
	}

	/* raid0_meta: need to add an rmeta device to pair it with the given linear device as well */
	if (segtype_is_raid0_meta(new_segtype)) {
		log_debug_metadata("Adding raid metadata device to %s",
				   display_lvname(lv));
		if (!_alloc_and_add_rmeta_devs_for_lv(lv, allocate_pvs))
			return 0;
	}

	/* HM FIXME: overloading force argument here! */
	/* We may be called to convert to !raid0*, i.e. an interim conversion on the way to radi4/5/6 */
	if (force)
		return 1;

	seg->segtype = new_segtype;
	seg->region_size = 0;
	seg->stripe_size = new_stripe_size;

	log_debug_metadata("Updating metadata and reloading mappings for %s",
			   display_lvname(lv));

	return lv_update_and_reload_origin(lv);
}

/* Helper: linear/raid0 with 1 image <-> raid1/4/5 takeover handler for @lv */
TAKEOVER_HELPER_FN(_linear_raid14510)
{
	struct lv_segment *seg;
	struct dm_list data_lvs, meta_lvs;
	struct segment_type *segtype;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);

	dm_list_init(&data_lvs);
	dm_list_init(&meta_lvs);

	if ((segtype_is_raid4(new_segtype) || segtype_is_any_raid5(new_segtype)) &&
	    (seg->area_count != 1 || new_image_count != 2)) {
		log_error("Can't convert %s from %s to %s != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_LINEAR, new_segtype->name);
		return 0;
	}
#if 1
	/* HM FIXME: elaborate this raid4 restriction more! */
	if (segtype_is_raid4(new_segtype)) {
		log_error("Can't convert %s from %s to %s, please use %s",
			  display_lvname(lv), SEG_TYPE_NAME_LINEAR,
			  SEG_TYPE_NAME_RAID4, SEG_TYPE_NAME_RAID5);
		return 0;
	}
#endif
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, new_data_copies, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	new_image_count = new_image_count > 1 ? new_image_count : 2;

	/* HM FIXME: overloading force argument to avoid metadata update in _linear_raid0() */
	/* Use helper _linear_raid0() to create the initial raid0_meta with one image pair up */
	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)) ||
	    !_linear_raid0(lv, segtype, 0, 1 /* force */, 1, 1 /* data_copies */, 0, 0, 0, allocate_pvs))
		return 0;

	/* Allocate the additional meta and data LVs requested */
	log_debug_metadata("Allocating %u additional data and metadata image pairs for %s",
			   new_image_count - 1, display_lvname(lv));
	if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, NULL))
		return 0;

	seg = first_seg(lv);
	seg->segtype = new_segtype;
	seg->data_copies = new_data_copies;
	seg->stripe_size = new_stripe_size;
	seg->region_size = new_region_size;

	_check_and_init_region_size(lv);

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, NULL, NULL);
}

/* Helper: striped/raid0* -> raid4/5/6/10 */
TAKEOVER_HELPER_FN(_striped_raid0_raid45610)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg->area_count, "area count");

PFLA("data_copies=%u", new_data_copies);

	if (segtype_is_raid10_offset(new_segtype)) {
		log_error("Can't convert LV %s to %s",
			  display_lvname(lv), new_segtype->name);
		return 0;
	}

	RETURN_IF_NONZERO(segtype_is_any_raid10(new_segtype) && new_data_copies < 2, "#data_copies > 1");

	if (new_data_copies > (segtype_is_raid10_far(new_segtype) ? seg->area_count : new_image_count)) {
		log_error("N number of data_copies \"--mirrors N-1\" may not be larger than number of stripes");
		return 0;
	}

	if (new_stripes && new_stripes != seg->area_count) {
		log_error("Can't restripe LV %s during conversion", display_lvname(lv));
		return 0;
	}

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, new_data_copies, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* This helper can be used to convert from striped/raid0* -> raid10 too */
	if (seg_is_striped(seg)) {
		log_debug_metadata("Converting LV %s from %s to %s",
				   display_lvname(lv), SEG_TYPE_NAME_STRIPED, SEG_TYPE_NAME_RAID0);
		if (!(seg = _convert_striped_to_raid0(lv, 1 /* alloc_metadata_devs */, 0 /* update_and_reload */, allocate_pvs)))
			return 0;
	}
PFL();
	/* Add metadata LVs */
	if (seg_is_raid0(seg)) {
		log_debug_metadata("Adding metadata LVs to %s", display_lvname(lv));
		if (!_raid0_add_or_remove_metadata_lvs(lv, 1 /* update_and_reload */, allocate_pvs, NULL))
			return 0;
	}
PFL();
	/* For raid10_far, we don't need additional image component pairs, just a size extension */
	if (!segtype_is_raid10_far(new_segtype)) {
		/* Add the additional component LV pairs */
		log_debug_metadata("Adding component LV pairs to %s", display_lvname(lv));
		if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, NULL))
			return 0;
	}
PFL();
	/* If this is any raid5 conversion request -> enforce raid5_n, because we convert from striped/raid0* */
	if (segtype_is_any_raid5(new_segtype)) {
		if (!segtype_is_raid5_n(new_segtype)) {
			log_warn("Overwriting requested raid type %s with %s to allow for conversion",
				 new_segtype->name, SEG_TYPE_NAME_RAID5_N);
	    		if (!(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID5_N)))
				return 0;
		}
PFL();

	/* If this is any raid6 conversion request -> enforce raid6_n_6, because we convert from striped/raid0* */
	} else if (segtype_is_any_raid6(new_segtype)) {
		if (!segtype_is_raid6_n_6(new_segtype)) {
			log_warn("Overwriting requested raid type %s with %s to allow for conversion",
				 new_segtype->name, SEG_TYPE_NAME_RAID6_N_6);
			if (!(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID6_N_6)))
				return 0;
		}
PFL();

	/* If this is a raid10 conversion request -> reorder the areas to suit it */
	/* If user wants raid10_offset, reshape afterwards */
	} else if (segtype_is_raid10_near(new_segtype)) {
		seg->data_copies = new_data_copies;

		log_debug_metadata("Reordering areas for raid0 -> raid10 takeover");
		if (!_reorder_raid10_near_seg_areas(seg, reorder_to_raid10_near))
			return 0;
PFL();

	} else if (segtype_is_raid10_far(new_segtype)) {
		seg->data_copies = 1;
		if (!_lv_raid10_resize_data_copies(lv, new_segtype, new_data_copies, allocate_pvs))
			return 0;
PFL();
	} else if (!segtype_is_raid4(new_segtype)) {
		/* Can't convert striped/raid0* to e.g. raid10_offset */
		log_error("Can't convert %s to %s", display_lvname(lv), new_segtype->name);
		return 0;
	}

PFL();
	seg = first_seg(lv);
	seg->segtype = new_segtype;
	seg->data_copies = new_data_copies;
	seg->region_size = new_region_size;

	if (!_check_and_init_region_size(lv))
		return 0;

	log_debug_metadata("Updating VG metadata and reloading %s LV %s",
			   lvseg_name(seg), display_lvname(lv));
	if (!_lv_update_reload_fns_reset_eliminate_lvs(lv, NULL, NULL))
		return 0;
PFL();

	/* If conversion to raid10, there are no rebuild images/slices -> trigger repair */
	if ((seg_is_raid10_near(seg) || seg_is_raid10_far(seg)) &&
	    !_lv_cond_repair(lv))
		return 0;
PFL();
	return 1;
}

/* raid0 -> linear */
TAKEOVER_HELPER_FN(_raid0_linear)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg->area_count == 1, "area count == 1");

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Converting %s from %s to %s",
			   display_lvname(lv),
			   SEG_TYPE_NAME_RAID0, SEG_TYPE_NAME_LINEAR);
	if (!_convert_raid_to_linear(lv, &removal_lvs))
		return_0;

	if (!(first_seg(lv)->segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	/* HM FIXME: overloading force argument here! */
	if (force)
		return 1;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* Helper: raid0* with one image -> mirror */
TAKEOVER_HELPER_FN(_raid0_mirror)
{
	struct lv_segment *seg;
	struct segment_type *segtype;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (seg->area_count != 1)
		return _error(lv, new_segtype, yes, force, 0, 1 /* data_copies */, 0, 0, 0, NULL);

	new_image_count = new_image_count > 1 ? new_image_count : 2;

	if (!_check_max_mirror_devices(new_image_count))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (seg_is_raid0(first_seg(lv))) {
		log_debug_metadata("Adding raid metadata device to %s",
				   display_lvname(lv));
		if (!_alloc_and_add_rmeta_devs_for_lv(lv, allocate_pvs))
			return 0;
	}

	/* First convert to raid1... */
	if (!(segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
		return 0;

	log_debug_metadata("Converting %s from %s to %s adding %u image component pairs",
			   display_lvname(lv),
			   lvseg_name(seg), new_segtype->name,
			   new_image_count - seg->area_count);
	if (!_linear_raid14510(lv, segtype, 0, 0, new_image_count, new_image_count, 0 /* new_stripes */,
			       new_stripe_size, new_region_size, allocate_pvs))
		return 0;

	seg = first_seg(lv);
	seg->region_size = new_region_size;

	/* ...second convert to mirror */
	log_debug_metadata("Converting %s from %s to %s",
			   display_lvname(lv),
			   segtype->name, new_segtype->name);
	return _convert_raid1_to_mirror(lv, new_segtype, new_image_count, new_region_size,
					allocate_pvs, 1 /* !update_and_reload */, NULL);
}

/* raid0 with one image -> raid1 */
TAKEOVER_HELPER_FN(_raid0_raid1)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg->area_count == 1, "single area");
	RETURN_IF_ZERO(seg_is_any_raid0(seg) && seg->area_count == 1, 
		   "converson of non-raid0 LV or with area count != 1");

	new_image_count = new_image_count > 1 ? new_image_count : 2;

	if (!_check_max_raid_devices(new_image_count))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (seg_is_raid0(seg)) {
		log_debug_metadata("Adding raid metadata device to %s",
				   display_lvname(lv));
		if (!_alloc_and_add_rmeta_devs_for_lv(lv, allocate_pvs))
			return 0;
	}

	log_debug_metadata("Converting %s from %s to %s adding %u image component pairs",
			   display_lvname(lv),
			   lvseg_name(seg), new_segtype->name,
			   new_image_count - seg->area_count);
	seg->segtype = new_segtype;
	seg->region_size = new_region_size;
	if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, NULL))
		return 0;

	/* Master leg is the first sub LV */
	seg_lv(seg, 0)->status &= ~LV_REBUILD;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, NULL, NULL);
}

/* Helper: mirror -> raid0* */
TAKEOVER_HELPER_FN(_mirror_raid0)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	dm_list_init(&removal_lvs);

	if (!seg_is_mirrored(seg)) {
		log_error(INTERNAL_ERROR "Can't convert non-mirrored segment of LV %s",
			  display_lvname(lv));
		return 0;
	}

	if (!_lv_is_synced(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Converting mirror LV %s to raid", display_lvname(lv));
	if (!_convert_mirror_to_raid(lv, new_segtype, 1, new_region_size, allocate_pvs,
				     0 /* update_and_reaload */, &removal_lvs))
		return 0;

	if (segtype_is_raid0(new_segtype)) {
		/* Remove rmeta LVs */
		log_debug_metadata("Extracting and renaming metadata LVs from LV %s",
				   display_lvname(lv));
		if (!_extract_image_component_list(seg, RAID_META, 0, &removal_lvs))
			return 0;
	}

	seg = first_seg(lv);
	seg->segtype = new_segtype;
	seg->region_size = 0;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* Helper: convert mirror with 2 images <-> raid4/5 */
TAKEOVER_HELPER_FN(_mirror_r45)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	dm_list_init(&removal_lvs);

	if (seg->area_count != 2 ||
	    !(seg_is_mirror(seg) || seg_is_raid4(seg) || seg_is_any_raid5(seg))) {
		log_error("Can't convert %s between %s and %s/%s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR,
			  SEG_TYPE_NAME_RAID4, SEG_TYPE_NAME_RAID5);
		return 0;
	}

	if (!_lv_is_synced(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 2, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (segtype_is_mirror(new_segtype)) {
		if (!_lv_free_reshape_space(lv)) {
			log_error(INTERNAL_ERROR "Failed to free reshape space of %s",
				  display_lvname(lv));
			return 0;
		}

		if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)) ||
		    !_convert_raid1_to_mirror(lv, new_segtype, 2, new_region_size, allocate_pvs,
					      0 /* !update_and_reload */, &removal_lvs))
			return 0;

	} else if (!_convert_mirror_to_raid(lv, new_segtype, 0, new_region_size, allocate_pvs,
					    0 /* update_and_reaload */, &removal_lvs))
		return 0;

	seg = first_seg(lv);
	seg->region_size = new_region_size;
	if (segtype_is_raid4(new_segtype) || segtype_is_any_raid5(new_segtype))
		seg->stripe_size = new_stripe_size ?: DEFAULT_STRIPESIZE;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* Helper: raid1 -> raid0* */
TAKEOVER_HELPER_FN(_raid1_raid0)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	dm_list_init(&removal_lvs);

	if (!seg_is_raid1(seg)) {
		log_error(INTERNAL_ERROR "Can't convert non-raid1 LV %s",
			  display_lvname(lv));
		return 0;
	}

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	seg->segtype = new_segtype;
	if (!_lv_change_image_count(lv, 1, allocate_pvs, &removal_lvs))
		return 0;

	/* Remove rmeta last LV if raid0  */
	if (segtype_is_raid0(new_segtype)) {
		log_debug_metadata("Extracting and renaming metadata LVs frim LV %s",
				   display_lvname(lv));
		if (!_extract_image_component_list(seg, RAID_META, 0, &removal_lvs))
			return 0;
	}

	seg = first_seg(lv);
	seg->region_size = 0;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* raid45 -> raid0* / striped */
TAKEOVER_HELPER_FN(_r456_r0_striped)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

PFLA("new_stripes=%u new_image_count=%u", new_stripes, new_image_count);
	if (!seg_is_raid4(seg) && !seg_is_raid5_n(seg) && !seg_is_raid6_n_6(seg)) {
		log_error("LV %s has to be of type raid4/raid5_n/raid6_n_6 to allow for this conversion",
			  display_lvname(lv));
		return 0;
	}

	/* Necessary when convering to raid0/striped w/o redundancy? */
	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	dm_list_init(&removal_lvs);

	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space of %s",
			  display_lvname(lv));
		return 0;
	}

	/* Remove meta and data LVs requested */
	if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, &removal_lvs))
		return 0;

	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)))
		return_0;

	if (segtype_is_striped(new_segtype)) {
PFLA("seg->area_count=%u seg->len=%u seg->area_len=%u", seg->area_count, seg->len, seg->area_len);
		if (!_convert_raid0_to_striped(lv, 0, &removal_lvs))
			return_0;

	} else if (segtype_is_raid0(new_segtype) &&
		   !_raid0_add_or_remove_metadata_lvs(lv, 0 /* update_and_reload */, allocate_pvs, &removal_lvs))
		return_0;

	seg = first_seg(lv);
	seg->data_copies = 1;
	seg->region_size = 0;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* helper raid1 with N images or raid4/5* with 2 images <-> linear */
TAKEOVER_HELPER_FN(_raid14510_linear)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	dm_list_init(&removal_lvs);
PFL();
	/* Only raid1 may have != 2 images when converting to linear */
	if (!seg_is_raid1(seg) && seg->area_count > 2) {
		log_error("Can't convert type %s LV  %s with %u images",
			  lvseg_name(seg), display_lvname(lv), seg->area_count);
		return 0;
	}
PFL();
	if (!_raid_in_sync(lv))
		return 0;
PFL();
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;
PFL();
	/*
	 * Have to remove any reshape space which my be a the beginning of
	 * the component data images or linear ain't happy about data content
	 */
	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space of %s",
			  display_lvname(lv));
		return 0;
	}

	/* Reduce image count to one */
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)) ||
	    !_lv_change_image_count(lv, 1, allocate_pvs, &removal_lvs))
		return 0;

	if (!_convert_raid_to_linear(lv, &removal_lvs))
		return_0;

	seg = first_seg(lv);
	seg->region_size = 0;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* Helper: raid1 with N images to M images (N != M) and raid4/5 to raid6* */
TAKEOVER_HELPER_FN(_raid145_raid1_raid6) 
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_NONZERO(!seg_is_raid1(seg) && !seg_is_raid4(seg) && !seg_is_any_raid5(seg),
			  "valid segment type");

	dm_list_init(&removal_lvs);

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, new_data_copies, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, &removal_lvs))
		return 0;

	seg->segtype = new_segtype;
	if (segtype_is_raid1(new_segtype)) {
		seg->stripe_size = 0;
		seg->data_copies = new_image_count;
	} else
		seg->data_copies = new_data_copies;

	seg = first_seg(lv);
	seg->region_size = new_region_size;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* Helper: raid1/5 with 2 images <-> raid4/5/10 or raid4 <-> raid5_n with any image count (no change to count!) */
TAKEOVER_HELPER_FN(_raid145_raid4510)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (!seg_is_raid1(seg) &&
	    !seg_is_raid4(seg) &&
	    !seg_is_any_raid5(seg)) {
		log_error(INTERNAL_ERROR "Called on LV %s with wrong segment type %s",
			  display_lvname(lv), lvseg_name(seg));
		return 0;
	}

	if (segtype_is_any_raid10(new_segtype)) {
		if (!segtype_is_raid10_near(new_segtype)) {
			log_error("Conversion of LV %s to raid10 has to be to raid10_near",
				  display_lvname(lv));
			return 0;
		}

		seg->data_copies = seg->area_count;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (new_image_count)
		log_error("Ignoring new image count for %s", display_lvname(lv));
	
	/* Overwrite image count */
	new_image_count = seg->area_count;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 2, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/*
	 * In case I convert to a non-reshapable mapping, I have to remove
	 * any reshape space which may be at the beginning of the component
	 * data images or the data content will be mapped to an offset
	 */
	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space of %s", display_lvname(lv));
		return 0;
	}

	if (seg_is_raid4(seg) && segtype_is_any_raid5(new_segtype)) {
		if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID5_N)))
			return_0;
	} else
		seg->segtype = new_segtype;

	seg = first_seg(lv);
	seg->stripe_size = new_stripe_size ?: DEFAULT_STRIPESIZE;
	seg->region_size = new_region_size;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, NULL, NULL);
}

/* Helper: raid10 -> striped/raid0/raid01 */
TAKEOVER_HELPER_FN_REMOVAL_LVS(_raid10_striped_r0)
{
	int raid10_far;
	struct lv_segment *seg;
	uint32_t data_copies;
#if 0
	/* Save data_copies and le_count for raid10_far conversion */
	uint32_t le_count = lv->le_count;
#endif

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	raid10_far = seg_is_raid10_far(seg);
	data_copies = seg->data_copies;
	RETURN_IF_ZERO(data_copies, "data copies > 0");

	if (!segtype_is_striped(new_segtype) &&
	    !segtype_is_any_raid0(new_segtype)) {
		log_error(INTERNAL_ERROR "Called for %s", new_segtype->name);
		return 0;
	}

	if (seg_is_raid10_offset(seg)) {
		log_error("Can't convert %s LV %s to %s",
			  lvseg_name(seg), display_lvname(lv), new_segtype->name);
		log_error("Please use \"lvcovert --duplicate ...\"");
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space of %s", display_lvname(lv));
		return 0;
	}

	if (seg_is_raid10_near(seg)) {
		/* Don't reduce seg->data_copies before reordering! */
		log_debug_metadata("Reordering areas for %s LV %s -> %s takeover",
				    lvseg_name(seg), display_lvname(lv), new_segtype->name);
		if (!_reorder_raid10_near_seg_areas(seg, reorder_from_raid10_near))
			return 0;

		new_image_count = seg->area_count / seg->data_copies;

		/* Remove the last half of the meta and data image pairs */
		log_debug_metadata("Removing data and metadata image LV pairs from %s", display_lvname(lv));
		if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, removal_lvs))
			return 0;

		/* Adjust raid10_near size to raid0/striped */
		RETURN_IF_ZERO(_seg_lv_checked(seg, 0), "first data sub lv");
		seg->area_len = seg_lv(seg, 0)->le_count;
		seg->len = seg->area_len * seg->area_count;
		lv->le_count = seg->len;

	/* raid10_far: shrink LV size to striped/raid0* */
	} else if (raid10_far && !_lv_raid10_resize_data_copies(lv, new_segtype, 1, NULL)) {
		log_error("Failed to reduce raid10_far LV %s to %s size",
			  display_lvname(lv), new_segtype->name);
		return 0;
	}

	seg->data_copies = 1;

PFLA("seg->len=%u seg->area_len=%u seg->area_count=%u", seg->len, seg->area_len, seg->area_count);

	if (segtype_is_striped(new_segtype)) {
		/* -> striped */
		if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)))
			return_0;

		if (!_convert_raid0_to_striped(lv, 0, removal_lvs))
			return 0;

		seg = first_seg(lv);

	/* -> raid0 (no mnetadata images)  */
	} else if (segtype_is_raid0(new_segtype) &&
		   !_raid0_add_or_remove_metadata_lvs(lv, 0 /* update_and_reload */, allocate_pvs, removal_lvs))
		return 0;

PFLA("seg->stripe_size=%u", seg->stripe_size);
PFLA("seg->chunk_size=%u", seg->chunk_size);
	seg = first_seg(lv);
	seg->segtype = new_segtype;
	seg->region_size = 0;

	/* HM FIXME: overloading force argument here! */
	return force ? 1 : _lv_update_reload_fns_reset_eliminate_lvs(lv, removal_lvs, NULL);
}

/* Helper: raid10 with 2/N (if appropriate) images <-> raid1/raid4/raid5* */
TAKEOVER_HELPER_FN(_raid10_r1456)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg->data_copies, "data copies > 0");
	RETURN_IF_ZERO(new_segtype, "lv new segment type argument");

	dm_list_init(&removal_lvs);

	if (seg_is_any_raid10(seg)) {
		if (!seg_is_raid10_near(seg)) {
			log_error(INTERNAL_ERROR "Can't takeover %s LV %s",
				  lvseg_name(seg), display_lvname(lv));
			return 0;
		}

		if (seg->data_copies != seg->area_count) {
			log_error(INTERNAL_ERROR "Can't takeover %s LV %s with data copies != areas!",
				  lvseg_name(seg), display_lvname(lv));
			return 0;
		}

	} else if (seg->area_count != 2 ) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), lvseg_name(seg), new_segtype->name);
		return 0;
	}


	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 2, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* Free any reshape space */
	if (!_lv_free_reshape_space(lv)) {
		log_error(INTERNAL_ERROR "Failed to free reshape space of %s", display_lvname(lv));
		return 0;
	}

	seg = first_seg(lv);
	seg->segtype = new_segtype;
	seg->region_size = new_region_size;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}
/* End takeover helper funtions */

/*
 * Begin all takeover functions referenced via the 2-dimensional _takeover_fn[][] matrix
 */
/* Linear -> raid0 */
TAKEOVER_FN(_l_r0)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid0(lv, new_segtype, yes, force, 1, 1, 0, new_stripe_size, 0, allocate_pvs);
}

/* Linear -> raid1 */
TAKEOVER_FN(_l_r1)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid14510(lv, new_segtype, yes, force,
				 new_image_count, new_image_count,
				 0 /* new_stripes */, 0, new_region_size,
				 allocate_pvs);
}

/* Linear -> raid4/5 */
TAKEOVER_FN(_l_r45)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return (_lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid14510(lv, new_segtype, yes, force,
				 2 /* new_image_count */, 2, 0 /* new_stripes */,
				 new_stripe_size, new_region_size, allocate_pvs));
}

/* Linear -> raid10 */
TAKEOVER_FN(_l_r10)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _lv_has_segments_with_n_areas(lv, 1) &&
	       _linear_raid14510(lv, new_segtype, yes, force,
				 2 /* new_image_count */ , 2, 0 /* new_stripes */,
				 new_stripe_size, new_region_size, allocate_pvs);
}

/* HM Helper: convert @lv from striped -> raid0(_meta) */
static int _striped_raid0(struct logical_volume *lv,
			  const struct segment_type *new_segtype,
			  int yes, int force, int alloc_metadata_devs,
			  struct dm_list *allocate_pvs)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
	if (alloc_metadata_devs)
		RETURN_IF_ZERO(allocate_pvs && !dm_list_empty(allocate_pvs), "allocate pvs");

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, 0, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_striped_to_raid0(lv, alloc_metadata_devs, 1 /* update_and_reload */, allocate_pvs) ? 1 : 0;
}

/* Striped -> raid0 */
TAKEOVER_FN(_s_r0)
{
	return _striped_raid0(lv, new_segtype, yes, force, 0, allocate_pvs);
}

/* Striped -> raid0_meta */
TAKEOVER_FN(_s_r0m)
{
	return _striped_raid0(lv, new_segtype, yes, force, 1, allocate_pvs);
} 

/* Striped -> raid4/5 */
TAKEOVER_FN(_s_r45)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 1,
					2 /* data_copies*/, 0, 0, new_region_size, allocate_pvs);
}

/* Striped -> raid6 */
TAKEOVER_FN(_s_r6)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 2,
					3 /* data_copies*/, 0, 0, new_region_size, allocate_pvs);
}

TAKEOVER_FN(_s_r10)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
PFL();
	return _striped_raid0_raid45610(lv, new_segtype, yes, force,
					first_seg(lv)->area_count * new_data_copies,
					new_data_copies, 0, 0, new_region_size, allocate_pvs);
}

/* mirror -> raid0 */
TAKEOVER_FN(_m_r0)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _mirror_raid0(lv, new_segtype, yes, force, 1, 1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* mirror -> raid0_meta */
TAKEOVER_FN(_m_r0m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _mirror_raid0(lv, new_segtype, yes, force, 1, 1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* Mirror -> raid1 */
TAKEOVER_FN(_m_r1)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, new_image_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_mirror_to_raid(lv, new_segtype, new_image_count, new_region_size,
				       allocate_pvs, 1 /* update_and_reaload */, &removal_lvs);
}

/* Mirror with 2 images -> raid4/5 */
TAKEOVER_FN(_m_r45)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _mirror_r45(lv, new_segtype, yes, force, 0, 1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* Mirror with 2 images -> raid10 */
TAKEOVER_FN(_m_r10)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	seg = first_seg(lv);
	if (seg->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR, new_segtype->name);
		return 0;
	}

	if (!_lv_is_synced(lv))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!_convert_mirror_to_raid(lv, new_segtype, 0, new_region_size, NULL, 0 /* update_and_reaload */, NULL))
		return 0;

	seg->segtype = new_segtype;

	return lv_update_and_reload(lv);;
}


/* raid0 -> linear */
TAKEOVER_FN(_r0_l)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid0_linear(lv, new_segtype, yes, force, 0, 1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid0 with one image -> mirror */
TAKEOVER_FN(_r0_m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid0_mirror(lv, new_segtype, yes, force, new_image_count,
			     1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0 -> raid0_meta */
TAKEOVER_FN(_r0_r0m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
PFL();
	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _raid0_add_or_remove_metadata_lvs(lv, 1, allocate_pvs, NULL);
}

/* raid0 -> striped */
TAKEOVER_FN(_r0_s)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_raid0_to_striped(lv, 1, &removal_lvs);
}

/* raid0 with one image -> raid1 */
TAKEOVER_FN(_r0_r1)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid0_raid1(lv, new_segtype, yes, force, new_image_count,
			    1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0 -> raid4/5_n */
TAKEOVER_FN(_r0_r45)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 1,
					1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0 -> raid6_n_6 */
TAKEOVER_FN(_r0_r6)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 2,
					1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0 with N images (N > 1) -> raid10 */
TAKEOVER_FN(_r0_r10)
{
	uint32_t data_copies = new_data_copies;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	if (segtype_is_raid10_near(new_segtype) && data_copies == 1)
		data_copies++;

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count * new_data_copies,
					new_data_copies, 0, 0, new_region_size, allocate_pvs);
}

/* raid0_meta -> */
TAKEOVER_FN(_r0m_l)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid0_linear(lv, new_segtype, yes, force, 0, 1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid0_meta -> mirror */
TAKEOVER_FN(_r0m_m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid0_mirror(lv, new_segtype, yes, force, new_image_count,
			     1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0_meta -> raid0 */
TAKEOVER_FN(_r0m_r0)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
PFL(); 
	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _raid0_add_or_remove_metadata_lvs(lv, 1, allocate_pvs, &removal_lvs);
}

/* raid0_meta -> striped */
TAKEOVER_FN(_r0m_s)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_raid0_to_striped(lv, 1, &removal_lvs);
}

/* raid0_meta wih 1 image -> raid1 */
TAKEOVER_FN(_r0m_r1)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid0_raid1(lv, new_segtype, yes, force, new_image_count,
			    1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0_meta -> raid4/5_n */
TAKEOVER_FN(_r0m_r45)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 1,
					1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid0_meta -> raid6_n_6 */
TAKEOVER_FN(_r0m_r6)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count + 2,
					1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}


/* raid0_meta wih 1 image -> raid10 */
TAKEOVER_FN(_r0m_r10)
{
	uint32_t data_copies = new_data_copies;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	if (segtype_is_raid10_near(new_segtype) && data_copies == 1)
		data_copies++;

	return _striped_raid0_raid45610(lv, new_segtype, yes, force, first_seg(lv)->area_count * data_copies,
					data_copies, 0, 0, new_region_size, allocate_pvs);
}


/* raid1 with N images -> linear */
TAKEOVER_FN(_r1_l)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
PFL();
	return _raid14510_linear(lv, new_segtype, yes, force, 1, 1, 0, 0, 0, allocate_pvs);
}

/* raid1 with N images -> striped */
TAKEOVER_FN(_r1_s)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
PFL();
	return _raid14510_linear(lv, new_segtype, yes, force, 1, 1, 0, 0, 0, allocate_pvs);
}

/* raid1 -> mirror */
TAKEOVER_FN(_r1_m)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	if (!_raid_in_sync(lv))
		return 0;

	/*
	 * FIXME: support this conversion or don't invite users to switch back to "mirror"?
	 *        I find this at least valuable in case of an erroneous conversion to raid1
	 */
	if (!yes && yes_no_prompt("WARNING: Do you really want to convert %s to "
				  "non-recommended \"%s\" type? [y/n]: ",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR) == 'n') {
		log_warn("Logical volume %s NOT converted to \"%s\"",
			  display_lvname(lv), SEG_TYPE_NAME_MIRROR);
		return 0;
	}
	if (sigint_caught())
		return_0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	return _convert_raid1_to_mirror(lv, new_segtype, new_image_count, new_region_size,
					allocate_pvs, 1, &removal_lvs);
}


/* raid1 -> raid0 */
TAKEOVER_FN(_r1_r0)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid1_raid0(lv, new_segtype, yes, force, 1, 1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid1 -> raid0_meta */
TAKEOVER_FN(_r1_r0m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _raid1_raid0(lv, new_segtype, yes, force, 1, 1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

TAKEOVER_FN(_r1_r1) 
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg_is_raid1(seg), "raid1 segment");
	RETURN_IF_ZERO(segtype_is_raid1(new_segtype), "raid1 new segment type");
	RETURN_IF_NONZERO(_lv_is_duplicating(lv), "duplicating LV allowed");

	if (seg->area_count == new_data_copies) {
		log_error("No change in number of mirrors in %s", display_lvname(lv));
		return 0;
	}

	return _raid145_raid1_raid6(lv, new_segtype, yes, force, new_image_count,
				    new_image_count, 0, 0, new_region_size, allocate_pvs);
}

/* raid1 with 2 legs -> raid4/5 */
TAKEOVER_FN(_r1_r45)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv),
			  SEG_TYPE_NAME_RAID1, new_segtype->name);
		return 0;
	}

	return _raid145_raid4510(lv, new_segtype, yes, force, new_image_count,
				 1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}
/****************************************************************************/

/* raid1 with N legs or duplicating one -> raid10_near */
TAKEOVER_FN(_r1_r10)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	if (!segtype_is_raid10_near(new_segtype)) {
		log_error("Conversion of %s to %s prohibited",
			  display_lvname(lv), new_segtype->name);
		log_error("Please use \"lvconvert --duplicate ...\"");
		return 1;
	}

	return _raid145_raid4510(lv, new_segtype, yes, force, new_image_count,
				 1 /* data_copies */, 0 /* stripes */, 0, new_region_size, allocate_pvs);
}

/* raid45 with 2 images -> linear */
TAKEOVER_FN(_r45_l)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s/%s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID4,
			  SEG_TYPE_NAME_RAID5, SEG_TYPE_NAME_LINEAR);
		return 0;
	}

	return _raid14510_linear(lv, new_segtype, yes, force, 1, 1, 0, 0, 0, allocate_pvs);
}

/* raid4/5 -> striped */
TAKEOVER_FN(_r45_s)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);
PFL();
	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 1,
				1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid4/5 with 2 images -> mirror */
TAKEOVER_FN(_r45_m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _mirror_r45(lv, new_segtype, yes, force, 0, 1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid4/5 -> raid0 */
TAKEOVER_FN(_r45_r0)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 1,
				1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid4/5 -> raid0_meta */
TAKEOVER_FN(_r45_r0m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 1,
				1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid4/5 with 2 images -> raid1 */
TAKEOVER_FN(_r45_r1)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg_is_raid4(seg) || seg_is_any_raid5(seg), "raid4/5");

	if (seg->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), lvseg_name(seg), SEG_TYPE_NAME_RAID1);
		return 0;
	}

	return _raid145_raid4510(lv, new_segtype, yes, force, 2,
				 1 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid4 <-> raid5_n */
TAKEOVER_FN(_r45_r54)
{
	struct lv_segment *seg;
	const struct segment_type *segtype_sav = new_segtype;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (!((seg_is_raid4(seg) && segtype_is_any_raid5(new_segtype)) ||
	      (seg_is_raid5_n(seg) && segtype_is_raid4(new_segtype)))) {
		log_error(INTERNAL_ERROR "Called with %s -> %s on LV %s",
			  lvseg_name(seg), new_segtype->name, display_lvname(lv));
		return 0;
	}

	if (seg_is_raid4(seg) &&
	    !(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID5_N)))
		return_0;

	if (segtype_sav != new_segtype)
		log_warn("Adjust new segtype to %s to allow for takeover",
			 lvseg_name(seg));

	seg->segtype = new_segtype;

	return lv_update_and_reload(lv);
}

/* raid4/5* <-> raid6* */
TAKEOVER_FN(_r45_r6)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (seg_is_raid4(seg)) {
		const struct segment_type *segtype_sav = new_segtype;

		if (segtype_is_any_raid5(new_segtype) &&
		    !(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID5_N)))
			return_0;

		else if (segtype_is_any_raid6(new_segtype) &&
			 !(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID6_N_6)))
			return_0;

		if (segtype_sav != new_segtype)
			log_warn("Adjust new segtype to %s to allow for takeover",
				 lvseg_name(seg));
	}

	if (seg->area_count < 3) {
		log_error("Please convert %s from 1 stripe to at least 2 with \"lvconvert --stripes 2 %s\" "
			  "first for this conversion",
			  display_lvname(lv), display_lvname(lv));
		return 0;
	}

	if (seg_is_any_raid5(seg) &&
	    segtype_is_any_raid6(new_segtype) &&
	    !(new_segtype = get_segtype_from_flag(lv->vg->cmd, _raid_seg_flag_5_to_6(seg)))) {
		log_error(INTERNAL_ERROR "Failed to get raid5 -> raid6 conversion type");
		return_0;
	}

	return _raid145_raid1_raid6(lv, new_segtype, yes, force, seg->area_count + 1,
				    3 /* data_copies */, 0, 0, new_region_size, allocate_pvs);
}

/* raid6 -> striped */
TAKEOVER_FN(_r6_s)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 2,
				1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid6 -> raid0 */
TAKEOVER_FN(_r6_r0)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 2,
				1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid6 -> raid0_meta */
TAKEOVER_FN(_r6_r0m)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	return _r456_r0_striped(lv, new_segtype, yes, force, first_seg(lv)->area_count - 2,
				1 /* data_copies */, 0, 0, 0, allocate_pvs);
}

/* raid6* -> raid4/5* */
TAKEOVER_FN(_r6_r45)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	if (!_raid_in_sync(lv))
		return 0;

	if (segtype_is_raid4(new_segtype) &&
	    !seg_is_raid6_n_6(seg)) {
		log_error("LV %s has to be of type %s to allow for this conversion",
			  display_lvname(lv), SEG_TYPE_NAME_RAID6_N_6);
		return 0;
	}

	if ((seg_is_raid6_zr(seg) ||
	     seg_is_raid6_nc(seg) ||
	     seg_is_raid6_nr(seg)) &&
	    !segtype_is_raid6_n_6(new_segtype)) {
		log_error("LV %s has to be of type %s,%s,%s,%s or %s to allow for direct conversion",
			  display_lvname(lv),
			  SEG_TYPE_NAME_RAID6_LS_6, SEG_TYPE_NAME_RAID6_LA_6,
			  SEG_TYPE_NAME_RAID6_RS_6, SEG_TYPE_NAME_RAID6_RA_6,
			  SEG_TYPE_NAME_RAID6_N_6);
		return 0;
	}

	new_image_count = seg->area_count - 1;
	seg->data_copies = 2;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_image_count, 1 /* data_copies */, 0, 0))
		return 0;

	dm_list_init(&removal_lvs);

	/* Remove meta and data LVs requested */
	log_debug_metadata("Removing one data and metadata image LV pair from %s", display_lvname(lv));
	if (!_lv_change_image_count(lv, new_image_count, allocate_pvs, &removal_lvs))
		return 0;

	seg->segtype = new_segtype;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* raid10 with 2 images -> linear */
TAKEOVER_FN(_r10_l)
{
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	if (first_seg(lv)->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID10, SEG_TYPE_NAME_MIRROR);
		return 0;
	}

	return _raid14510_linear(lv, new_segtype, yes, force, 1, 1, 0, 0, 0, allocate_pvs);
}

/* raid10 -> raid0* */
TAKEOVER_FN(_r10_s)
{
	struct dm_list removal_lvs;
PFL();
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	return _raid10_striped_r0(lv, new_segtype, yes, 0, 0, 1 /* data_copies */, 0, 0, 0, allocate_pvs, &removal_lvs);
}

/* raid10 with 2 images -> mirror */
TAKEOVER_FN(_r10_m)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);

	dm_list_init(&removal_lvs);

	if (seg->area_count != 2) {
		log_error("Can't convert %s from %s to %s with != 2 images",
			  display_lvname(lv), SEG_TYPE_NAME_RAID10, SEG_TYPE_NAME_MIRROR);
		return 0;
	}

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, seg->area_count, seg->area_count, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* HM FIXME: support -mN during this conversion */
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)) ||
	    !_convert_raid1_to_mirror(lv, new_segtype, new_image_count, new_region_size,
				      allocate_pvs, 0, &removal_lvs))
		return 0;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* raid10 -> raid0 */
TAKEOVER_FN(_r10_r0)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	return _raid10_striped_r0(lv, new_segtype, yes, 0, 0, 1 /* data_copies */, 0, 0, 0, allocate_pvs, &removal_lvs);
}

/* raid10 -> raid0_meta */
TAKEOVER_FN(_r10_r0m)
{
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, first_seg(lv), new_segtype);

	dm_list_init(&removal_lvs);

	return _raid10_striped_r0(lv, new_segtype, yes, 0, 0, 1 /* data_copies */, 0, 0, 0, allocate_pvs, &removal_lvs);
}

/* raid10 with stripes == data_copies images -> raid1 */
TAKEOVER_FN(_r10_r1)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
PFL();
	if (!seg_is_raid10_near(seg)) {
		log_error("Can't convert %s LV %s to raid1",
			  lvseg_name(seg), display_lvname(lv));
		goto bad;
	}

	if (seg->data_copies != seg->area_count) {
		log_error("Can't convert %s LV %s to raid1 with data_copies != stripes",
			  lvseg_name(seg), display_lvname(lv));
		goto bad;
	}

	return _raid10_r1456(lv, new_segtype, yes, force, new_image_count, seg->data_copies,
			     seg->area_count, 0, new_region_size, allocate_pvs);

bad:
	log_error("Please use \"lvconvert --duplicate --type raid1 ... %s\"", display_lvname(lv));
	return 0;
}

/* Helper: raid10_near with N images to M images (N != M) */
TAKEOVER_HELPER_FN(_r10_r10)
{
	uint32_t data_copies;
	struct lv_segment *seg;
	const struct segment_type *raid10_segtype;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(new_data_copies > 1, "data copies argument");
	RETURN_IF_ZERO(seg_is_raid10_near(seg), "raid10 near LV");
	RETURN_IF_ZERO(segtype_is_raid10_near(new_segtype), "raid10 near requested");

	if (seg->area_count % seg->data_copies) {
		log_error("Can't change data copies on raid10_near LV %s with odd number of images",
			  display_lvname(lv));
		return 0;
	}

	if (new_data_copies == seg->data_copies) {
		log_error("No change in number of data copies on raid10_near LV %s",
			  display_lvname(lv));
		return 0;
	}

	dm_list_init(&removal_lvs);

	raid10_segtype = seg->segtype;
	data_copies = seg->data_copies;

	if (!_raid_in_sync(lv))
		return 0;

	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_data_copies, new_data_copies, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Reordering areas for %s image component LVs %s %s",
			   new_data_copies > seg->data_copies ? "adding" : "removing",
			   new_data_copies > seg->data_copies ? "to" : "from",
			   display_lvname(lv));
	if (!_reorder_raid10_near_seg_areas(seg, reorder_from_raid10_near))
		return 0;

PFLA("seg->area_count=%u, new_count=%u", seg->area_count, seg->area_count / data_copies * new_data_copies);
	if (!_lv_change_image_count(lv, seg->area_count / data_copies * new_data_copies,
				    allocate_pvs, &removal_lvs))
		return 0;
PFL();
	/* Have to memorize segtype and set to raid0_meta for reordering of areas */
	seg->data_copies = new_data_copies;
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID0_META)))
		return_0;
PFL();
	log_debug_metadata("Reordering back image component LVs of %s ",
			   display_lvname(lv));
	if (!_reorder_raid10_near_seg_areas(seg, reorder_to_raid10_near))
		return 0;

	seg->segtype = raid10_segtype;
PFL();
	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/*
 * raid01 (mirrors on top of stripes)
 *
 * Creates the image LVs with @stripes and @stripe_size
 * for @lv starting at area @start, ending at area
 * @end-1 and commits the MDAs
 */
static int _lv_create_raid01_image_lvs(struct logical_volume *lv,
				       struct lv_segment *seg,
				       uint32_t extents,
				       uint32_t stripes, uint32_t stripe_size,
				       uint32_t start, uint32_t end,
				       struct dm_list *allocate_pvs)
{
	uint32_t data_copies = end;
	struct dm_list data_lvs, meta_lvs;

	RETURN_IF_ZERO(lv, "LV argument");
	RETURN_IF_ZERO(extents, "image extents");
	RETURN_IF_ZERO(stripes, "stripes");
	RETURN_IF_ZERO(stripe_size, "stripe_size");
	RETURN_IF_NONZERO(start > end || data_copies < 2, "proper end/start/data_copies");

	dm_list_init(&meta_lvs);
	dm_list_init(&data_lvs);

	/* Create the #data_copies striped LVs to put under raid1 */
	log_debug_metadata("Allocating %u stripe%s for %s",
			   data_copies, data_copies > 1 ? "s": "", display_lvname(lv));
	if (!_alloc_striped_image_components(lv, end - start, 0, stripes, stripe_size, extents,
					     NULL, &data_lvs, allocate_pvs))
		return_0;

	log_debug_metadata("Allocating %u metadata images for %s", end - start, display_lvname(lv));
	if (!_alloc_rmeta_devs_for_rimage_devs(lv, &data_lvs, &meta_lvs, allocate_pvs))
		return_0;

	log_debug_metadata("Adding new data and metadata LVs to %s", display_lvname(lv));
	if (!_add_image_component_list(seg, 1, 0, &meta_lvs, start) ||
	    !_add_image_component_list(seg, 1, start ? LV_REBUILD : 0, &data_lvs, start)) {
		log_error("Failed to add new data LVs to %s", display_lvname(lv));
		return_0;
	} 

	log_debug_metadata("Setting LV %s to raid01", display_lvname(lv));
	return (seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID01)) ? 1 : 0;
}

/* striped with any number of images to raid01 */
TAKEOVER_FN(_s_r01)
{
	struct lv_segment *seg, *striped_seg;

PFLA("new_data_copies=%u", new_data_copies);
	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (striped_seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(new_data_copies > 1, "data copies > 1 argument");
PFL();
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, new_data_copies, new_data_copies, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	log_debug_metadata("Converting LV %s to raid1", display_lvname(lv));
	if (!(seg = _convert_lv_to_raid1(lv, "_rimage_0")))
		return 0;
PFL();
	log_debug_metadata("Setting LV %s to raid01", display_lvname(lv));
	if (!(seg->segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID01)))
		return_0;

	log_debug_metadata("Reallocating segment areas of %s", display_lvname(lv));
	if (!_realloc_meta_and_data_seg_areas(lv, new_data_copies))
		return 0;

	seg->area_count = seg->data_copies = new_data_copies;

PFLA("seg->len=%u seg->area_len=%u", seg->len, seg->area_len);
	/* Has to be set before calling _check_and_init_region_size() */
	lv->le_count = seg->len;
	lv->size = seg->len * lv->vg->extent_size;
PFL();
	if (!_check_and_init_region_size(lv))
		return 0;

	if (!_alloc_rmeta_for_lv_add_set_hidden(lv, 0, allocate_pvs))
		return 0;
PFL();
	/* Got te first striped sub LV after the raid1 conversion -> allocate the others */
	if (!_lv_create_raid01_image_lvs(lv, seg, striped_seg->len,
					 striped_seg->area_count, striped_seg->stripe_size,
					 1 /* start */, new_data_copies, allocate_pvs))
		return 0;
PFL();
	return _lv_update_reload_fns_reset_eliminate_lvs(lv, NULL, NULL);
}

/* Helper: find any one synced sub LV in @seg and return it or NULL */
static int _get_any_synced_sub_lv(struct lv_segment *seg, uint32_t *ss)
{
	uint32_t s;

	RETURN_IF_SEG_ZERO(seg);
	RETURN_IF_ZERO(ss, "segment area pointer argument");

	for (s = 0; s < seg->area_count; s++)
		if (_lv_is_synced(seg_lv(seg, s))) {
			*ss = s;
			return 1;
		}

	return 0;
}

/* raid01 with any number of data_copies to striped */
TAKEOVER_FN(_r01_s)
{
	uint32_t keep_idx, stripe_size;
	struct logical_volume *lv_tmp;
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);

	dm_list_init(&removal_lvs);
PFL();
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, first_seg(seg_lv(seg, 0))->area_count, 1, 0, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* Find any one synced mirror and return its index in @s */
	RETURN_IF_ZERO(_get_any_synced_sub_lv(seg, &keep_idx), "raid01 mirror in sync!");
PFL();
	/*
	 * Extract all rmeta images of the raid1 top-level LV
	 * and all but the @keep_idx indexed striped data image.
	 */
	if (!_lv_extract_all_images_but_one(lv, keep_idx, &removal_lvs))
		return 0;

	lv_tmp = seg_lv(seg, 0);
	stripe_size = first_seg(lv_tmp)->stripe_size;

	if (!_lv_reset_raid_add_to_list(lv_tmp, &removal_lvs))
		return 0;

	if (!remove_layer_from_lv(lv, lv_tmp))
		return_0;

	seg = first_seg(lv);
	seg->stripe_size = stripe_size;
PFL();
	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/* Remove striped legs from raid01 */
static int _raid01_remove_images(struct logical_volume *lv, uint32_t new_data_copies,
				 struct dm_list *removal_lvs)
{
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	/* Extract any metadata LVs and the empty data LVs for disposal by the caller */
	log_debug_metadata("Removing %u striped sub LVs from LV %s",
			   seg->data_copies - new_data_copies, display_lvname(lv));
	if ((seg->meta_areas && !_extract_image_component_list(seg, RAID_META, new_data_copies, removal_lvs)) ||
	    !_extract_image_component_list(seg, RAID_IMAGE, new_data_copies, removal_lvs))
		return_0;

	seg->area_count = seg->data_copies = new_data_copies;

	if (new_data_copies == 1 &&
	    !_convert_raid01_to_striped(lv, removal_lvs))
		return 0;

	return 1;
}

/* raid01 with any number of data_copies to raid10 */
TAKEOVER_FN(_r01_r10)
{
	uint32_t data_copies, stripes;
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg_is_raid01(seg), "raid01 LV");

	dm_list_init(&removal_lvs);
	data_copies = new_data_copies > 1 ? new_data_copies : seg->data_copies;

	RETURN_IF_ZERO(data_copies > 1, "data copies");

	stripes = first_seg(seg_lv(seg, 0))->area_count * data_copies;
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, stripes, data_copies, stripes, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!_raid01_remove_images(lv, 1, &removal_lvs))
		return 0;

	/* HM FIXME: this renders the LV to be striped, thus non-resilient; there should be a message itr  */
	/* Duplication avoids this interim non-resilience altogether... */
	if (!_lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL))
		return 0;

	return _striped_raid0_raid45610(lv, new_segtype, 1, force,
					stripes, data_copies, 0, 0, new_region_size, allocate_pvs);
}

/* raid10_near with any number of data_copies (stripes must be divisable by data_copies) to raid01 */
TAKEOVER_FN(_r10_r01)
{
	uint32_t stripes;
	struct lv_segment *seg;
	struct segment_type *striped_segtype;
	struct dm_list removal_lvs;

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(seg_is_raid10_near(seg), "raid10 LV");

	if (seg->area_count % seg->data_copies) {
		log_error("Can't convert raid10_near LV %s with number of stripes not divisable by number of data copies",
			  display_lvname(lv));
		return 0;
	}

	dm_list_init(&removal_lvs);

	stripes = seg->area_count;
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, stripes, new_data_copies, stripes, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	if (!(striped_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;
PFL();
	stripes /= seg->data_copies;
	if (!_raid10_striped_r0(lv, striped_segtype, 1, 0, stripes, 1 /* data_copies */, stripes,
				seg->stripe_size, 0, allocate_pvs, &removal_lvs))
		return 0;
PFL();
	if (!_lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL))
		return 0;
PFL();
	return _s_r01(lv, new_segtype, 1, force,
		      seg->area_count, new_data_copies, 0, 0, new_region_size, allocate_pvs);
}

/* Change number of data_copies on raid01 */
TAKEOVER_FN(_r01_r01)
{
	struct lv_segment *seg;
	struct dm_list removal_lvs;

	dm_list_init(&removal_lvs);

	RETURN_IF_LV_SEG_SEGTYPE_ZERO(lv, (seg = first_seg(lv)), new_segtype);
	RETURN_IF_ZERO(new_data_copies, "data copies");

	if (new_data_copies == seg->data_copies) {
		log_error("No different data copies for LV %s", display_lvname(lv));
		return 0;
	}
	
	if (!_yes_no_conversion(lv, new_segtype, yes, force, 0, seg->area_count, new_data_copies, seg->area_count, 0))
		return 0;

	/* Archive metadata */
	if (!archive(lv->vg))
		return_0;

	/* Add new striped sub LVs as mirrors aka data copies to raid01 */
	if (new_data_copies > seg->data_copies) {
		struct logical_volume *striped_lv = seg_lv(seg, 0);
		struct lv_segment *striped_seg = first_seg(striped_lv);

		if (!striped_seg ||
		    !seg_is_striped(striped_seg)) {
			log_error("Bogus segment in first sub LV of LV %s", display_lvname(lv));
			return 0;
		}

		if (!_raid_in_sync(lv))
			return 0;

		log_debug_metadata("Reallocating segment areas of %s", display_lvname(lv));
		if (!_realloc_meta_and_data_seg_areas(lv, new_data_copies))
			return 0;
PFL();
		/* Allocate the new, striped image sub LVs */
		log_debug_metadata("Adding %u striped sub LVs to %s",
				   new_data_copies - seg->data_copies, display_lvname(lv));
		if (!_lv_create_raid01_image_lvs(lv, seg, striped_lv->le_count,
						 striped_seg->area_count, striped_seg->stripe_size,
						 seg->data_copies, new_data_copies, allocate_pvs))
			return 0;

	/* Remove mirrors aka data copies from raid01 */
	} else if (!_raid01_remove_images(lv, new_data_copies, &removal_lvs))
		return 0;

	seg->area_count = seg->data_copies = new_data_copies;

	return _lv_update_reload_fns_reset_eliminate_lvs(lv, &removal_lvs, NULL);
}

/*
 * 2-dimensional takeover function matrix defining the
 * FSM of possible/impossible or noop (i.e. requested
 * conversion already given on the lv) conversions
 *
 * Rows define segtype from and columns segtype to
 */
static takeover_fn_t _takeover_fns[][10] = {
	/* from, to ->     linear   striped  mirror   raid0    raid0_meta  raid1    raid4/5    raid6    raid10  raid01 */
	/*   | */
	/*   v */
	/* linear     */ { _noop,   _error,  _error,  _l_r0,   _l_r0,      _l_r1,   _l_r45,    _error,  _l_r10  , _error   },
	/* striped    */ { _error,  _noop,   _error,  _s_r0,   _s_r0m,     _l_r1,   _s_r45,    _s_r6,   _s_r10  , _s_r01   },
	/* mirror     */ { _error,  _error,  _noop,   _m_r0,   _m_r0m,     _m_r1,   _m_r45,    _error,  _m_r10  , _error   },
	/* raid0      */ { _r0_l,   _r0_s,   _r0_m,   _noop,   _r0_r0m,    _r0_r1,  _r0_r45,   _r0_r6,  _r0_r10 , _error   },
	/* raid0_meta */ { _r0m_l,  _r0m_s,  _r0m_m,  _r0m_r0, _noop,      _r0m_r1, _r0m_r45,  _r0m_r6, _r0m_r10, _error   },
	/* raid1      */ { _r1_l,   _r1_s,   _r1_m,   _r1_r0,  _r1_r0m,    _r1_r1,  _r1_r45,   _error,  _r1_r10 , _error   },
	/* raid4/5    */ { _r45_l,  _r45_s,  _r45_m,  _r45_r0, _r45_r0m,   _r45_r1, _r45_r54,  _r45_r6, _error  , _error   },
	/* raid6      */ { _error,  _r6_s,   _error,  _r6_r0,  _r6_r0m,    _error,  _r6_r45,   _error,  _error  , _error   },
	/* raid10     */ { _r10_l,  _r10_s,  _r10_m,  _r10_r0, _r10_r0m,   _r10_r1, _error,    _error,  _r10_r10, _r10_r01 },
	/* raid01     */ { _error,  _r01_s,  _error,  _error,  _error,     _error,  _error,   _error,   _r01_r10, _r01_r01 },
};

/* End: various conversions between layers (aka MD takeover) */
/****************************************************************************/

/*
 * Return 1 if provided @data_copies, @stripes, @stripe_size are
 * possible for conversion from @seg_from to @segtype_to, else 0.
 */
static int _log_prohibited_option(const struct lv_segment *seg_from,
				  const struct segment_type *new_segtype,
				  const char *opt_str)
{
	RETURN_IF_ZERO(seg_from, "segment from argument");
	RETURN_IF_ZERO(new_segtype, "segment type argument");

	if (seg_from->segtype == new_segtype)
		log_error("Prohibited option %s provided to convert %s LV %s",
			  opt_str, lvseg_name(seg_from), display_lvname(seg_from->lv));
	else
		log_error("Prohibited option %s provided to convert LV %s from %s to %s",
			  opt_str, display_lvname(seg_from->lv), lvseg_name(seg_from), new_segtype->name);

	return 1;
}

/* Set segtype conveniently for raid4 <-> raid5 <-> raid6 takeover */
static int _set_convenient_raid456_segtype_to(const struct lv_segment *seg_from,
					      struct segment_type **segtype)
{
	uint64_t seg_flag;
	struct cmd_context *cmd;
	struct segment_type *requested_segtype;

	RETURN_IF_ZERO(seg_from, "segment from argument");
	RETURN_IF_ZERO(segtype || *segtype, "segment type argument");

	cmd = seg_from->lv->vg->cmd;
	requested_segtype = *segtype;
PFL();
	if (seg_is_striped(seg_from) ||
	    seg_is_any_raid0(seg_from) ||
	    seg_is_raid4(seg_from)) {
PFL();
		/* If this is any raid5 conversion request -> enforce raid5_n, because we convert from striped */
		if (segtype_is_any_raid5(*segtype) &&
		    !segtype_is_raid5_n(*segtype) &&
		    !(*segtype = get_segtype_from_flag(cmd, SEG_RAID5_N))) {
			log_error(INTERNAL_ERROR "Failed to get raid5_n segtype!");
			return 0;

		/* If this is any raid6 conversion request -> enforce raid6_n_6, because we convert from striped */
		} else if (segtype_is_any_raid6(*segtype) &&
			   !segtype_is_raid6_n_6(*segtype) &&
			   !(*segtype = get_segtype_from_flag(cmd, SEG_RAID6_N_6))) {
			log_error(INTERNAL_ERROR "Failed to get raid6_n_6 segtype!");
			return 0;
		}

	/* Got to do check for raid5 -> raid6 ... */
	} else if (seg_is_any_raid5(seg_from) &&
		   segtype_is_any_raid6(*segtype) &&
		   (!(seg_flag = _raid_seg_flag_5_to_6(seg_from)) ||
		    !(*segtype = get_segtype_from_flag(cmd, seg_flag)))) {
		// log_error(INTERNAL_ERROR "Failed to get raid5 -> raid6 conversion type");
		return 0;

	/* ... and raid6 -> raid5 */
	} else if (seg_is_any_raid6(seg_from) &&
		   segtype_is_any_raid5(*segtype) &&
		   (!(seg_flag = _raid_seg_flag_6_to_5(seg_from)) ||
		    !(*segtype = get_segtype_from_flag(cmd, seg_flag)))) {
		// log_error(INTERNAL_ERROR "Failed to get raid6 -> raid5 conversion type");
		return 0;
	}

	if (requested_segtype != *segtype)
		log_print_unless_silent("Replacing requested RAID type %s with %s for LV %s to allow for takeover",
					requested_segtype->name, (*segtype)->name, display_lvname(seg_from->lv));
	
	return 1;
}

/* Check allowed conversion from @seg_from to @segtype_to */
static int _conversion_options_allowed(const struct lv_segment *seg_from,
				       struct segment_type **segtype_to,
				       uint32_t new_image_count,
				       int data_copies, int region_size,
				       int stripes, int stripe_size)
{
	int r = 1;
	uint32_t opts;

	RETURN_IF_ZERO(seg_from, "segment from argument");
	RETURN_IF_ZERO(segtype_to || *segtype_to, "segment type to argument");

PFL();
	if (!new_image_count &&
	    !_set_convenient_raid456_segtype_to(seg_from, segtype_to))
		return 0;

PFLA("seg_from->segtype=%s segtype_to=%s", lvseg_name(seg_from), (*segtype_to)->name);

	if (!_get_allowed_conversion_options(seg_from, *segtype_to, new_image_count, &opts))
		return 0;
PFLA("segtype_to=%s", (*segtype_to)->name);

	if (data_copies > 1 && !(opts & ALLOW_DATA_COPIES)) {
		if (!_log_prohibited_option(seg_from, *segtype_to, "-m/--mirrors"))
			return 0;

		r = 0;
	}

	if (stripes > 1 && !(opts & ALLOW_STRIPES)) {
		if (!_log_prohibited_option(seg_from, *segtype_to, "--stripes"))
			return 0;
		r = 0;
	}

	if (stripe_size && !(opts & ALLOW_STRIPE_SIZE)) {
		if (!_log_prohibited_option(seg_from, *segtype_to, "-I/--stripesize"))
			return 0;
		r = 0;
	}

	return r;
}

/* HM Helper: initialize @*stripe_size */
static int _init_stripe_size(const struct lv_segment *seg, uint32_t *stripe_size)
{
	RETURN_IF_SEG_ZERO(seg);
	RETURN_IF_ZERO(stripe_size, "stripe_size pointer argument");

	if (*stripe_size)
		return 1;

	if (seg->stripe_size)
		*stripe_size = seg->stripe_size;
	else {
		*stripe_size = 64;
		if (!seg_is_raid01(seg))
			log_warn("Initializing stripe size on %s to %u sectors",
				 display_lvname(seg->lv), *stripe_size);
	}

	return 1;
}

/*
 * HM Helper:
 *
 * define current conversion parameters for lv_raid_convert
 * based on those of @seg if not set
 */
static int _raid_convert_define_parms(const struct lv_segment *seg,
				      struct segment_type **segtype,
				      int duplicate,
				      int *data_copies, uint32_t *region_size,
				      uint32_t *stripes, uint32_t *stripe_size)
{
	struct cmd_context *cmd;

	RETURN_IF_SEG_ZERO(seg);
	cmd = seg->lv->vg->cmd;

	*stripes = *stripes ?: ((duplicate) ? 2 : _data_rimages_count(seg, seg->area_count));
	*stripe_size = *stripe_size ?: seg->stripe_size;
	*data_copies = *data_copies > -1 ? *data_copies : (duplicate ? 1 : seg->data_copies);
	*region_size = *region_size ?: seg->region_size;
PFLA("*datacpies=%u", *data_copies);

	/* Check region size */
	if (!*region_size &&
	    (segtype_is_mirror(*segtype) ||
	     segtype_is_raid1(*segtype) ||
	     segtype_is_reshapable_raid(*segtype))) {
		*region_size = 1024;
		log_warn("Initializing region size on %s to %s",
			 display_lvname(seg->lv), display_size(cmd, *region_size));
	}

	if (segtype_is_thin(*segtype) || segtype_is_thin_pool(*segtype)) {
		RETURN_IF_ZERO((*segtype = get_segtype_from_string(cmd, "thin")), "thin segtype");
		*data_copies = 1;
		*region_size = 0;
		*stripes = 1;
		*stripe_size = 0;

	} else if (segtype_is_mirror(*segtype) ||
		   segtype_is_raid1(*segtype)) {
		*data_copies = *data_copies < 1 ? 1 : *data_copies;
		*stripes = 1;
		*stripe_size = 0;

	} else if (segtype_is_any_raid10(*segtype)) {
		*data_copies = *data_copies < 2 ? 2 : *data_copies;

		if (!segtype_is_raid10_far(*segtype) &&
		    *stripes < 3)
			*stripes = 3;

		if (!_init_stripe_size(seg, stripe_size))
			return 0;

	} else if (segtype_is_striped(*segtype) ||
		   segtype_is_striped_raid(*segtype)) {
		if (seg_is_raid10_near(seg) && seg->area_count == 2)
			if (*stripes) {
				log_warn("Ignoring stripes argument on %s", display_lvname(seg->lv));
				*stripes = 1;
			}

		if (!_init_stripe_size(seg, stripe_size))
			return 0;

		if (*stripes == 1 &&
		    *data_copies > 1) {
			if (*stripe_size) {
				log_warn("Ignoring stripe size argument on %s", display_lvname(seg->lv));
				*stripe_size = 0;
			}
		}

	}

	if (segtype_is_raid(*segtype) &&
	    !segtype_is_any_raid0(*segtype) &&
	    !segtype_is_raid1(*segtype) &&
	    !segtype_is_raid01(*segtype)) {
		if (!segtype_is_any_raid6(*segtype) && *data_copies - 1 > *stripes) {
			log_error("Number of data copies %u is larger than number of stripes %u",
				  *data_copies, *stripes);
			return 0;
		}
		if (*region_size < *stripe_size) {
			*region_size = get_default_region_size(cmd);
			log_warn("Region size too small, setting to default %s",
				 display_size(cmd, *region_size));
		}
	}

	return 1;
}

/* HM Helper: 
 *
 * Change region size on raid @lv to @region_size if
 * different from current region_size and adjusted region size
 */
static int _region_size_change_requested(struct logical_volume *lv, int yes, uint32_t region_size)
{
	uint32_t old_region_size;
	const char *seg_region_size_str;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));

	if (!_raid_in_sync(lv)) {
		log_error("Unable to change region size on %s while it is not in-sync",
			  display_lvname(lv));
		return 0;
	}

	if (!region_size ||
	    region_size == seg->region_size)
		return 1;

	if (region_size * 16 > lv->size) {
		log_error("Requested region_size too large for LV %s size!", display_lvname(lv));
		return 0;
	}

	old_region_size = seg->region_size;
	seg->region_size = region_size;
	RETURN_IF_ZERO((seg_region_size_str = display_size(lv->vg->cmd, seg->region_size)),
		       "region size string");

	if (!_check_and_init_region_size(lv))
		return 0;

	if (seg->region_size == old_region_size) {
		log_warn("Region size on %s did not change due to adjustment", display_lvname(lv));
		return 1;
	}

	if (!yes && yes_no_prompt("Do you really want to change the region_size %s of LV %s to %s? [y/n]: ",
				  display_size(lv->vg->cmd, old_region_size),
				  display_lvname(lv), seg_region_size_str) == 'n') {
		log_error("Logical volume %s NOT converted", display_lvname(lv));
		return 0;
	}

	/* Check for new region size causing bitmap to still fit metadata image LV */
	if (seg->meta_areas && seg_metatype(seg, 0) == AREA_LV && seg_metalv(seg, 0)->le_count <
	    _raid_rmeta_extents(lv->vg->cmd, lv->le_count, seg->region_size, lv->vg->extent_size)) {
		log_error("Region size %s on %s is too small for metadata LV size",
			  seg_region_size_str, display_lvname(lv));
		return 0;
	}

	if (!lv_update_and_reload_origin(lv))
		return 0;

	log_warn("Changed region size on RAID LV %s to %s",
		 display_lvname(lv), seg_region_size_str);
	return 1;
}

/*
 * API function:
 *
 * lv_raid_convert
 *
 * Convert @lv from one RAID type (or striped/mirror segtype) to @new_segtype,
 * change RAID algorithm (e.g. left symmetric to right asymmetric),
 * add/remove LVs to/from a RAID LV or change stripe sectors and
 * create/teardown duplicating LV stacks with e.g. N (raid) LVs underneath
 * a toplevel raid1 mapping.
 *
 * Non dm-raid changes are factored in e.g. "mirror" and "striped" related
 * functions called from here.
 *
 * Conversions fall into reshape or takeover classes being coped with in
 * _raid_reshape() or via _takeover_fn[] jump table.
 *
 * Reshape is a change of the number of disks (e.g. change a raid5 set from
 * 3-way striped to 7-way striped, thus adding capacity) or the data/metadata
 * allocation algorithm (e.g. raid5 left-symmetric to right-asymmetric,
 * raid10 near to offset) or the stripe size (aka MD raid chunk size)
 * of a raid4/5/6/10 RAID LV (raid0 can't be reshaped wrt stripe size directly,
 * a conversion to raid4/5/6/10 must be carried out initially to achieve such
 * layout change in a second step).
 *
 * Takeover is defined as a switch from one raid level to another, pottentially
 * involving the addition of one or more image component pairs (i.e. data and metadata LV pair);
 * for instance a switch from raid0 to raid6 will add 2 image component pairs and
 * initialize their rebuild.
 *
 * Returns: 1 on success, 0 on failure
 */
/*
 * TODO (^ done):
 *  - review size calculations in raid1 <-> raid4/5 ^
 *  - review stripe size usage on conversion from/to striped/nonstriped segment types
 *  - review reshape space alloc/free ^
 *  - conversion raid0 -> raid10 only mentions redundancy = 1 instead of 1..#stripes maximum
 *  - false --striped user entry shows wrong message
 *  - keep ti->len small on initial disk adding reshape and grow after it has finished
 *    in order to avoid bio_endio in the targets map method?
 *  - support region size changes ^
 */
int lv_raid_convert(struct logical_volume *lv,
		    struct lv_raid_convert_params rcp)
{
	int data_copies = rcp.data_copies;
	uint32_t image_count;
	uint32_t region_size = rcp.region_size;
	uint32_t stripes = rcp.stripes;
	uint32_t stripe_size = rcp.stripe_size;
	int layout_properties_requested = (data_copies > -1 ? data_copies : 0) + stripes + stripe_size;
	struct lv_segment *seg, *seg1;
	struct segment_type *new_segtype = rcp.segtype;
	struct dm_list removal_lvs;
	takeover_fn_t tfn;

PFLA("new_segtype=%s", new_segtype ? new_segtype->name : "");

	/* new_segtype may be NAUGHT */
	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO((seg = seg1 = first_seg(lv)), "lv segment");

	if (rcp.duplicate && rcp.unduplicate) {
		log_error("--duplicate and --unduplicate are mutually exclusive!");
		return 0;
	}

	dm_list_init(&removal_lvs);

	/*
	 * Define any missing raid paramaters based on @seg of first duplicating seg
	 * and check proper duplicate/unduplicate option provided
	 */
	if (_lv_is_duplicating(lv))  {
		RETURN_IF_ZERO(_seg_lv_checked(seg, 0) &&
			       (seg1 = first_seg(seg_lv(seg, 0))),
			       "sub LV #0");

		if (!rcp.duplicate && !rcp.unduplicate && !rcp.region_size) {
			log_error("No direct conversion on duplicating LV %s possible!",
				  display_lvname(lv));
			log_error("Either --duplicate or --unduplicate or --regionsize option mandatory.");
			return 0;
		}

	} else if (rcp.unduplicate && !lv_is_duplicated(lv)) {
		log_error("LV %s is not duplicating!", display_lvname(lv));
		return 0;
	}

	if (lv_is_duplicated(lv))
		seg1 = first_seg(lv);

PFLA("new_segtype=%s", new_segtype ? new_segtype->name : "");
	new_segtype = new_segtype ?: (struct segment_type *) seg1->segtype;
PFLA("new_segtype=%s", new_segtype ? new_segtype->name : "");
PFLA("seg1->segtype=%s", seg1->segtype ? seg1->segtype->name : "");

	/* Define any undefined properties from the given segment ones */
	if (!_raid_convert_define_parms(seg1, &new_segtype, rcp.duplicate, &data_copies, &region_size, &stripes, &stripe_size))
		return 0;

PFLA("new_segtype=%s data_copies=%d region_size=%u stripes=%u stripe_size=%u #allocate_pvs=%d", new_segtype ? new_segtype->name : "", data_copies, region_size, stripes, stripe_size, rcp.allocate_pvs ? dm_list_size(rcp.allocate_pvs) : 0);

	if (lv_is_duplicated(lv) && !rcp.duplicate && !rcp.unduplicate) {
		if (!seg_is_mirrored(seg1) &&
		    stripes && stripes != _data_rimages_count(seg1, seg1->area_count)) {
			log_error("Adding/removing stripes to/from duplicated LV %s "
				  "prohibited due to sub LV size change",
				  display_lvname(lv));
			return 0;
		}
	}

PFLA("new_segtype=%s data_copies=%d region_size=%u stripes=%u stripe_size=%u", new_segtype ? new_segtype->name : "", data_copies, region_size, stripes, stripe_size);
	/* Make sure we are being called for segment types we actually support */
	/* Given segtype of @lv */
	if (!seg_is_striped(seg1) && /* Catches linear = "overloaded striped with one area" as well */
	    !seg_is_mirror(seg1) &&
	    !seg_is_thin(seg1) &&
	    !seg_is_raid(seg1))
		goto err;

	/* Requested segtype */
	if (!segtype_is_linear(new_segtype) &&
	    !segtype_is_striped(new_segtype) &&
	    !segtype_is_mirror(new_segtype) &&
	    !segtype_is_thin(new_segtype) &&
	    !segtype_is_raid(new_segtype))
		goto err;

	image_count = ((int) stripes >= data_copies) ? stripes : data_copies;
	image_count += new_segtype ? new_segtype->parity_devs : 0;

PFLA("new_segtype=%s new_data_copies=%d new_stripes=%u segtype=%s, seg->area_count=%u duplicate=%d unduplicate=%d ", new_segtype ? new_segtype->name : "", rcp.data_copies, rcp.stripes, lvseg_name(seg), seg->area_count, rcp.duplicate, rcp.unduplicate);
PFLA("new_segtype=%s segtype=%s, seg->area_count=%u", new_segtype ? new_segtype->name : "", lvseg_name(seg), seg->area_count);
PFLA("new_segtype=%s image_count=%u data_copies=%d region_size=%u stripes=%u stripe_size=%u", new_segtype ? new_segtype->name : "", image_count, data_copies, region_size, stripes, stripe_size);

	if (!_check_max_raid_devices(image_count))
		return 0;

	/*
	 * If clustered VG, @lv has to be active exclusive locally, else just has to be activei
	 *
	 * HM FIXME: exclusive activation has to change once we support clustered MD raid1 
	 *
	 * Check for active lv late to be able to display rejection reasons before
	 */
	if (!_lv_is_active((lv)))
		return 0;

	/*
	 * A conversion by duplication has been requested so either:
	 * - create a new LV of the requested segtype
	 * -or-
	 * - add another LV as a sub LV to an existing duplicating one
	 */
	if (rcp.duplicate) {
		/* Check valid options mirrors, stripes and/or stripe_size have been provided suitable to the conversion */
		if (!_conversion_options_allowed(seg, &new_segtype, image_count /* duplicate check for image_count > 0 */,
						 rcp.data_copies, rcp.region_size,
						 rcp.stripes, rcp.stripe_size))
			return _log_possible_conversion_types(lv, new_segtype);

		if (!_raid_duplicate(lv, new_segtype, rcp.yes, rcp.force, data_copies, region_size,
				     stripes, stripe_size, rcp.lv_name, rcp.allocate_pvs))
			return 0;
PFLA("new_segtype=%s image_count=%u data_copies=%d stripes=%u", new_segtype ? new_segtype->name : "", image_count, data_copies, stripes);

		goto out;

	/*
	 * Remove any active duplicating conversion ->
	 * this'll remove all but 1 leg and withdraw the
	 * top-level raid1 mapping
	 */
	} else if (rcp.unduplicate) {
PFLA("lv=%s", display_lvname(lv));
		/* If user passed in the sub LV name to keep and no --name option, use it */
		if (!_valid_name_requested(&lv, &rcp.lv_name, layout_properties_requested, "unduplicate"))
			return 0;

PFLA("lv=%s lv_name=%s", display_lvname(lv), rcp.lv_name);
		if (!_raid_unduplicate(lv, rcp.yes, rcp.lv_name)) {
			if (!_lv_is_duplicating(lv))
				_log_possible_conversion_types(lv, new_segtype);

			return 0;
		}

		goto out;
	}


	/* Special raid1 handling for segtype not given */
	if (seg->segtype == new_segtype) {
		/* Converting raid1 -> linear given "lvconvert -m0 ..." w/o "--type ..." */
		if (seg_is_raid1(seg) &&
		    image_count == 1 &&
		    !(new_segtype = get_segtype_from_string(lv->vg->cmd, SEG_TYPE_NAME_STRIPED)))
			return_0;

		/* Converting linear to raid1 given "lvconvert -mN ..." (N > 0)  w/o "--type ..." */
		else if (seg_is_linear(seg) &&
			 image_count > 1 &&
			 !(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID1)))
			return_0;
	}

	/*
	 * If not duplicating/unduplicating request ->
	 *
	 * reshape of capable raid type requested
	 */
PFL();
	switch (_reshape_requested(lv, new_segtype, data_copies, region_size, stripes, stripe_size)) {
	case 0:
		break;
	case 1:
		if (!_raid_reshape(lv, new_segtype, rcp.yes, rcp.force,
				   data_copies, region_size,
				   stripes, stripe_size, rcp.allocate_pvs)) {
			log_error("Reshape failed on LV %s", display_lvname(lv));
			return 0;
		}

		goto out;
	case 2:
		/* Error if we got here with stripes and/or stripe size change requested */
		return 0;
	case 3:
		/* Got request to change region size */
		if (!_region_size_change_requested(lv, rcp.yes, region_size))
			return 0;

		goto out;
	}

PFLA("yes=%d new_segtype=%s data_copies=%u stripes=%u stripe_size=%u", rcp.yes, new_segtype->name, data_copies, stripes, stripe_size);

	/*
	 * All non-duplicating conversion requests need to be 100% in-sync,
	 * because those'll be processed using md takeover features relying
	 * on in-sync crc/q-syndroms
	 */
	if (!_raid_in_sync(lv)) {
		log_error("Unable to convert %s while it is not in-sync",
			  display_lvname(lv));
		return 0;
	}

	/* User has given -mN on a striped LV w/o asking for a segtype or providing striped -> convert to raid01 */
	if (seg_is_striped(seg) &&
	    segtype_is_striped(new_segtype) &&
	    rcp.data_copies > 1 &&
	    !(new_segtype = get_segtype_from_flag(lv->vg->cmd, SEG_RAID01)))
		return_0;

	if (seg_is_striped(seg) &&
	    segtype_is_raid01(new_segtype) &&
	    data_copies < 2)
		data_copies = 2;

	/*
	 * Check acceptible options mirrors, region_size,
	 * stripes and/or stripe_size have been provided.
	 */
	if (!_conversion_options_allowed(seg, &new_segtype, 0 /* Takeover */,
					 rcp.data_copies, rcp.region_size,
					 rcp.stripes, rcp.stripe_size))
		return _log_possible_conversion_types(lv, new_segtype);

PFLA("new_segtype=%s image_count=%u data_copies=%u stripes=%u stripe_size=%u", new_segtype->name, image_count, data_copies, stripes, stripe_size);
	/*
	 * Table driven takeover, i.e. conversions from one segment type to another
	 */
	tfn = _takeover_fns[_takeover_fn_idx(seg->segtype, seg->area_count)][_takeover_fn_idx(new_segtype, image_count)];
	if (!tfn(lv, new_segtype, rcp.yes, rcp.force, image_count,
		 data_copies, stripes, stripe_size, region_size, rcp.allocate_pvs))
		return 0;

out:
	log_print_unless_silent("Logical volume %s successfully converted.", display_lvname(lv));
	return 1;

err:
	log_error("Converting the segment type for %s (directly) from %s to %s"
		  " is not supported.", display_lvname(lv),
		  lvseg_name(seg), new_segtype->name);

	return 0;
}

/* Return extents needed to replace on missing PVs */
static uint32_t _extents_needed_to_repair(struct logical_volume *lv, struct dm_list *remove_pvs)
{
	uint32_t r = 0;
	struct lv_segment *rm_seg;

	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));
	RETURN_IF_ZERO(remove_pvs, "remove pvs list argument");

	if ((lv->status & PARTIAL_LV) && lv_is_on_pvs(lv, remove_pvs))
		dm_list_iterate_items(rm_seg, &lv->segments)
			/*
			 * Segment areas are for stripe, mirror, raid,
		 	 * etc.  We only need to check the first area
		 	 * if we are dealing with RAID image LVs.
		 	 */
			if (seg_type(rm_seg, 0) == AREA_PV &&
			    (seg_pv(rm_seg, 0)->status & MISSING_PV))
				r += rm_seg->len;

	return r;
}

/* Try to find a PV which can hold the whole @lv for replacement */
static int _try_to_replace_whole_lv(struct logical_volume *lv, struct dm_list *remove_pvs)
{
	uint32_t extents_needed;

	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));
	RETURN_IF_ZERO(remove_pvs, "remove pvs list argument");

	/* First, get the extents needed to replace @lv */
	if ((extents_needed = _extents_needed_to_repair(lv, remove_pvs))) {
		struct lv_segment *rm_seg;

		log_debug("%u extents needed to repair %s",
			  extents_needed, display_lvname(lv));

		/* Second, do the other PVs have the space */
		dm_list_iterate_items(rm_seg, &lv->segments) {
			struct physical_volume *pv = seg_pv(rm_seg, 0);

			/* HM FIXME: TEXTME: find_pv_in_pv_ist correct here? */
			if (seg_type(rm_seg, 0) == AREA_PV &&
			    !(pv->status & MISSING_PV) &&
	    		    !find_pv_in_pv_list(remove_pvs, pv)) {
				if ((pv->pe_count - pv->pe_alloc_count) > extents_needed) {
					log_debug("%s has enough space for %s",
						  pv_dev_name(pv), display_lvname(lv));
				 	/*
					 * Now we have a multi-segment, partial image that has enough
					 * space on just one of its PVs for the entire image to be
					 * replaced.  So, we replace the image's space with an error
					 * target so that the allocator can find that space (along with
					 * the remaining free space) in order to allocate the image
					 * anew.
				     	 */
					if (!_replace_lv_with_error_segment(lv))
						return_0;

					return 1;
				}

				log_debug("Not enough space on %s for %s",
					  pv_dev_name(pv), display_lvname(lv));
			}
		}
	}

	return 0;
}

/* Find space to replace partial @lv */
static int _remove_partial_multi_segment_image(struct logical_volume *lv,
					       struct dm_list *remove_pvs)
{
	uint32_t s;
	struct lv_segment *raid_seg;

	RETURN_IF_LV_SEG_ZERO(lv, (raid_seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(raid_seg, 0);
	RETURN_IF_ZERO(remove_pvs, "remove pvs list argument");

	if (!(lv->status & PARTIAL_LV)) {
		log_error(INTERNAL_ERROR "Called with non-partial LV %s.", display_lvname(lv));
		return 0;
	}

	for (s = 0; s < raid_seg->area_count; s++) {
		/* Try to replace all extents of any damaged image and meta LVs */
		int r = _try_to_replace_whole_lv(seg_lv(raid_seg, s), remove_pvs);

		if (raid_seg->meta_areas)
			r +=  _try_to_replace_whole_lv(seg_metalv(raid_seg, s), remove_pvs);

		if (r)
			return !!r;
	}

	/*
	 * This is likely to be the normal case - single
	 * segment images completely allocated on a missing PV.
	 */
	return 0;
}

/* HM Helper fn to generate LV names and set segment area LV */
static int _generate_name_and_set_segment(struct logical_volume *lv,
					  uint32_t s, uint32_t sd,
					  struct dm_list *lvs, char **tmp_names)
{
	struct lv_segment *raid_seg;
	struct lv_list *lvl;
	const char *suffix;

	RETURN_IF_LV_SEG_ZERO(lv, (raid_seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(raid_seg, 0);
	RETURN_IF_ZERO(lvs, "LVs list argument");
	RETURN_IF_NONZERO(dm_list_empty(lvs), "LVs listed");

	lvl = dm_list_item(dm_list_first(lvs), struct lv_list);
	dm_list_del(&lvl->list);

#if 1
	suffix = (s == sd) ? "rmeta_" : "rimage_";
	if (!(tmp_names[sd] = _generate_raid_name(lv, suffix, s)))
		return_0;
#endif
	if (!set_lv_segment_area_lv(raid_seg, s, lvl->lv, 0, lvl->lv->status)) {
		log_error("Failed to add %s to %s", lvl->lv->name, lv->name);
		return 0;
	}

	lv_set_hidden(lvl->lv);
	return 1;
}

/* HM Helper: return 1 in case @slv has to be replaced, because it has any allocation on list @removal_pvs */
static int __sub_lv_needs_rebuilding(struct logical_volume *slv,
				     struct dm_list *remove_pvs, uint32_t *partial_lvs)
{
	int r = 0;

	RETURN_IF_LV_SEG_ZERO(slv, first_seg(slv));
	RETURN_IF_ZERO(remove_pvs, "remove pvs list argument");
	RETURN_IF_ZERO(partial_lvs, "partial LVs argument");

PFLA("slv=%s", display_lvname(slv));
	if (lv_is_on_pvs(slv, remove_pvs) ||
	    lv_is_virtual(slv)) {
		r = 1;

		if (slv->status & PARTIAL_LV)
			(*partial_lvs)++;
	}

	return r;
}

/* HM Helper:
 *
 * return 1 in case seg_lv(@seg, @s) (and optionally seg_metalv(seg, s))
 * has to be replaced, because it has any allocation on list @removal_pvs
 */
static int _sub_lv_needs_rebuilding(struct lv_segment *seg, uint32_t s,
				    struct dm_list *remove_pvs, uint32_t *partial_lvs)
{
	int r;

	RETURN_IF_ZERO(seg, "segment argument");
	RETURN_IF_ZERO(remove_pvs, "remove pvs list argument");
	RETURN_IF_ZERO(partial_lvs, "partial LVs argument");

	r = __sub_lv_needs_rebuilding(seg_lv(seg, s), remove_pvs, partial_lvs);

	if (seg->meta_areas)
		r += __sub_lv_needs_rebuilding(seg_metalv(seg, s), remove_pvs, partial_lvs);

	return !!r;
}

/* HM Helper: return 1 in case seg_pv(@seg, @s) has to be replaced, because it has any allocation on list @removal_pvs */
static int _sub_pv_needs_rebuilding(struct lv_segment *seg, uint32_t s, struct dm_list *remove_pvs)
{
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, s);
	RETURN_IF_ZERO(seg_type(seg, s) == AREA_PV, "area PV");

	return (_pv_on_list(seg_pv(seg, s), remove_pvs) ||
		(seg_pv(seg, s)->status & MISSING_PV))
		 ? 1 : 0;
}

/* HM Helper: check for data and optional metadata area being assigned in @seg, area @s */
static int _areas_assigned(struct lv_segment *seg, uint32_t s)
{
	if ((seg_type(seg, s) == AREA_UNASSIGNED) ||
	    (seg->meta_areas && seg_metatype(seg, s) == AREA_UNASSIGNED)) {
		log_error("Unable to replace RAID images in LV %s while it "
			  "has unassigned area %u", display_lvname(seg->lv), s);
		return 0;
	}

	return 1;
}

/* HM Helper: check for segment type specific resilience and return @*match_count*/
static int _resilient_replacement(struct logical_volume *lv,
				  int yes, uint32_t *match_count, uint32_t *partial_lvs,
				  struct dm_list *remove_pvs)
{
	uint32_t lv_mc = 0, pv_mc = 0, s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(match_count, "match_count pointer argument");
	RETURN_IF_ZERO(partial_lvs, "partial_vs pointer argument");
	RETURN_IF_ZERO(remove_pvs , "remove pvs list argument");
#if 0
	RETURN_IF_NONZERO(dm_list_empty(remove_pvs), "remove pvs listed");
#endif

	*match_count = 0;

	/*
	 * How many image component pairs/areas are being removed?
	 */
	for (s = 0; s < seg->area_count; s++) {
		if (!_areas_assigned(seg, s))
			return 0;

		if (_seg_lv_checked(seg, s) &&
		    _sub_lv_needs_rebuilding(seg, s, remove_pvs, partial_lvs)) /* Checks both data and meta images */
			lv_mc++;

		if (seg_type(seg, s) == AREA_PV &&
		    _sub_pv_needs_rebuilding(seg, s, remove_pvs))
			pv_mc++;
	}

PFLA("lv=%s lv_mc=%u pv_mc=%u area_count=%u", display_lvname(lv), lv_mc, pv_mc, seg->area_count);

	if (!(lv_mc + pv_mc)) {
		log_warn("%s does not contain devices specified"
			 " for replacement", display_lvname(lv));
		return 1;

	} else if (lv_mc == seg->area_count) {
		log_error("Unable to replace PVs in all %s of %s at once.",
			  _lv_is_duplicating(lv) ? "sub LVs" : "images", display_lvname(lv));
		return 0;

	} else if (seg->segtype->parity_devs) {
		if (lv_mc > seg->segtype->parity_devs) {
			log_error("Unable to replace more than %u images from (%s) %s",
				  seg->segtype->parity_devs,
				  lvseg_name(seg), display_lvname(lv));
			return 0;

		} else if (lv_mc == seg->segtype->parity_devs &&
			   lv_mc > *partial_lvs / 2) {
			log_warn("You'll loose all resilience on %s LV %s during replacement"
				 " until resynchronization has finished!",
				  lvseg_name(seg), display_lvname(lv));
			if (!yes && yes_no_prompt("WARNING: Do you really want to replace"
						  " PVs in %s LV %s [y/n]: ",
				  		  lvseg_name(seg), display_lvname(lv)) == 'n') {
				log_warn("PVs in LV %s NOT replaced!", display_lvname(lv));
				return 0;
			}
			if (sigint_caught())
				return_0;
		}

	} else if (seg_is_any_raid10(seg)) {
		uint32_t copies = seg->data_copies, i;

		/*
		 * For raid10_{near, offset} we have data_copies distributed across stripes
		 * so we can check for less than data copies images to be replaced.
		 *
		 * For raid10_far, we have data copies stripe zones in seqeuntial order
		 * and the same constriant applies.
		 *
		 */
		if (!seg_is_raid10_far(seg) &&
		    !(seg->area_count % seg->data_copies)) {
			uint32_t rebuilds_per_group;

			for (i = 0; i < seg->area_count * copies; i++) {
				s = i % seg->area_count;
				if (!(i % copies))
					rebuilds_per_group = 0;

				if (_sub_lv_needs_rebuilding(seg, s, remove_pvs, partial_lvs))
					rebuilds_per_group++;

				if (rebuilds_per_group >= copies) {
					log_error("Unable to replace all the devices "
						  "in a RAID10 mirror group.");
					return 0;
				}
			}

		/*
		 * ... and if not so 'mirror groups', we have to have at least
		 * one mirror for the whole raid10 set available after replacement!
		 */
		} else {
			uint32_t rebuilds = 0;

			for (s = 0; s < seg->area_count; s++)
				if (_sub_lv_needs_rebuilding(seg, s, remove_pvs, partial_lvs))
					rebuilds++;

			if (rebuilds >= copies) {
				log_error("Unable to replace all data copies images in a RAID10 set.");
				return 0;
			}
		}

	} else if (lv_mc == seg->area_count - 1)
		log_warn("You'll loose all resilience during replacement of images of %s",
			  display_lvname(lv));


	*match_count = lv_mc + pv_mc;

	return 1;
}

/* HM Helper: avoid @remove_pvs allocation */
static void _avoid_pvs(struct dm_list *pvs)
{
	struct pv_list *pvl;

	if (pvs)
		dm_list_iterate_items(pvl, pvs)
			pvl->pv->status |= PV_ALLOCATION_PROHIBITED;
}

/* HM Helper: for coallocation of metadata LVs, use PVs of @data_lv */
static int _use_pvs_of_data_lv(struct logical_volume *data_lv,
			       uint32_t areas,
			       struct dm_list *allocate_pvs)
{
	uint32_t s;
	struct lv_segment *seg;
	struct pv_list *pvl;

	RETURN_IF_LV_SEG_ZERO(data_lv, (seg = first_seg(data_lv)));
	RETURN_IF_ZERO(allocate_pvs, "allocate pvs argument");
	RETURN_IF_NONZERO(dm_list_empty(allocate_pvs), "pvs to allocate on listed");

	dm_list_iterate_items(pvl, allocate_pvs)
		pvl->pv->status |= PV_ALLOCATION_PROHIBITED;

	for (s = 0; s < min(areas, seg->area_count); s++) {
		RETURN_IF_ZERO(seg_type(seg, s) == AREA_PV, "area PV");
		seg_pv(seg, s)->status &= ~PV_ALLOCATION_PROHIBITED;
	}

	return 1;
}

/* HM Helper:
 *
 * replace area @s of @seg by allocating a new one on different PVs
 * from @top_lv, releasing the old area and moving the new one across.
 */
static int _replace_segment_area(struct logical_volume *top_lv,
				 struct logical_volume *data_lv,
				 struct lv_segment *seg, uint32_t s,
				 struct dm_list *remove_pvs,
				 struct dm_list *allocate_pvs)
{
	int alloc_tries, area_len, virtual = 0;
	uint32_t areas, end, le_count;
	uint64_t lv_size;
	struct alloc_handle *ah;
	struct logical_volume *lv;
	struct lv_segment_area *meta_areas;
	struct segment_type *segtype;

	RETURN_IF_LV_SEG_ZERO(top_lv, seg);
	RETURN_IF_ZERO((lv = seg->lv), "valid segment lv");
PFLA("s=%u seg->area_count=%u segtype_is_virtual(seg->segtype)=%u", s, seg->area_count, segtype_is_virtual(seg->segtype));
	RETURN_IF_NONZERO(!segtype_is_virtual(seg->segtype) && s >= seg->area_count, "valid area");

	le_count = lv->le_count;
	lv_size = lv->size;
	area_len = seg->area_len;
	end = 1 + (data_lv ? first_seg(data_lv)->area_count : 0);

PFLA("seg->lv=%s", display_lvname(lv));

	if (segtype_is_virtual(seg->segtype)) {
		virtual = 1;
		meta_areas = seg->meta_areas;
		lv->status |= PARTIAL_LV;
		dm_list_init(&lv->segments);
	}

	for (alloc_tries = areas = 0; alloc_tries < end; alloc_tries++) {
PFL();
		/* If @data_lv, try coallocating area for metadata LV on its PVs */
		if (data_lv) {
			if (!_use_pvs_of_data_lv(data_lv, ++areas, allocate_pvs))
				return 0;

		/* else, prevent any PVs holding any image components from being used for allocation */
		} else if (!_avoid_pvs_with_other_images_of_lv(top_lv, allocate_pvs))
			return 0;
PFL();
		/* Prevent remove_pvs from being used for allocation */
		_avoid_pvs(remove_pvs);
PFL();
		if ((ah = _allocate_extents(lv, 1, 0, 0 /* region_size */,
					    1 /* stripes */, 0 /* stripe_size */,
					    area_len /* extents */, allocate_pvs)))
			break;
PFL();
		if (!data_lv)
			return 0;
PFL();
		if (data_lv &&
		    areas == first_seg(data_lv)->area_count)
			data_lv = NULL;
PFL();
	}
PFL();
	_allow_pvs(allocate_pvs);
	_allow_pvs(remove_pvs);
PFLA("Replacing area %u in seg->lv %s", s, display_lvname(lv));

	if (!(segtype = get_segtype_from_string(top_lv->vg->cmd, "striped")))
		return_0;

	/* Add a new segment with the just allocated area to the LV */
	if (!lv_add_segment(ah, 0, 1 /* areas */, 1 /* data_copies */,
			    lv, segtype, 0, lv->status, 0)) {
		log_error("Failed to add segment to LV %s", display_lvname(seg->lv));
		return 0;
	}
PFL();
	if (virtual) {
		seg = first_seg(lv);
		seg->meta_areas = meta_areas;

	} else {
		/* Release the to be replaced area of the LVs segment */
		if (!release_and_discard_lv_segment_area(seg, s, seg->area_len))
			return_0;
PFL();
		/* Move the newly allocated area across to our just released one */
		if (!move_lv_segment_area(seg, s, last_seg(lv), 0))
			return_0;
PFL();
		/* Remove the new, now empty last segment and reset the grown size caused by adding it */
		dm_list_del(lv->segments.p);

		lv->le_count = le_count;
		lv->size = lv_size;
	}

	lv->status &= ~PARTIAL_LV;

	alloc_destroy(ah);
PFL();
	return 1;
}

/* HM Helper: replace any failed areas in @lv */
static int _replace_failed_areas(struct logical_volume *top_lv,
				 struct logical_volume *replace_lv,
				 struct logical_volume *data_lv,
				 uint32_t match_count,
				 struct dm_list *remove_pvs, struct dm_list *allocate_pvs)
{
	uint32_t alloc, areas_replaced, end, mc = 0, nr_segs = 0, s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(replace_lv, (seg = first_seg(replace_lv)));

PFLA("top_lv=%s replace_lv=%s area_count=%u data_lv=%s", display_lvname(top_lv), display_lvname(replace_lv), seg->area_count, data_lv ? display_lvname(data_lv) : "");
	if (!match_count)
		return 0;
#if 0
	replace_lv->status &= ~LV_REBUILD;
	if (data_lv)
		data_lv->status &= ~LV_REBUILD;
#endif

	/* Work list of segments; multiple segments possible at lowest level where we map AREA_PVs */
	dm_list_iterate_items(seg, &replace_lv->segments) {
		nr_segs++;

		if (segtype_is_virtual(seg->segtype))
			alloc = end = 1;
		else {
			alloc = 0;
			end = seg->area_count;
		}

		for (areas_replaced = s = 0; s < end; s++) {
			if (!_areas_assigned(seg, s))
				return 0;

			/* Lower recursion level: replace virtual segment/AREA_PV in case it's allocated on @remove_pvs */
			if (alloc || seg_type(seg, s) == AREA_PV) {
				if (alloc || _sub_pv_needs_rebuilding(seg, s, remove_pvs)) {
					if (!_replace_segment_area(top_lv, data_lv, seg, s, remove_pvs, allocate_pvs))
						return_0;

					if (data_lv) {
						data_lv->status |= LV_REBUILD;

						if (nr_segs == 1 && !_clear_lv(seg->lv))
							return 0;

					} else
						replace_lv->status |= LV_REBUILD;
PFLA("replace_lv=%s", display_lvname(replace_lv));
	
					areas_replaced++;
				}

			} else if (_seg_lv_checked(seg, s)) {
PFLA("seg_lv(seg, %u)=%s", s, display_lvname(seg_lv(seg, s)));
				/* Recurse into any component component data image LVs */
				if (!_replace_failed_areas(top_lv, seg_lv(seg, s), NULL,
							   match_count, remove_pvs, allocate_pvs))
					return_0;

				/* Recurse into any component component metadata image LVs */
				if (_seg_metalv_checked(seg, s) &&
				    !_replace_failed_areas(top_lv, seg_metalv(seg, s), seg_lv(seg, s),
							   match_count, remove_pvs, allocate_pvs))
					return_0;
			}
		}

		mc += areas_replaced / seg->area_count;
	}

	mc /= nr_segs;
	replace_lv->status &= ~VIRTUAL;

	return mc > match_count ? 0 : 1;
}

/* Helper: recusion function to suspend+resume all image component pairs @lv */
static int _pre_raid_replace_activate_recurse(struct logical_volume *lv)
{
	uint32_t s;
	struct logical_volume *slv;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0); /* area_count > 0 check */

	log_debug_metadata("Activating %u image component%s of LV %s",
			   seg->area_count, seg->meta_areas ? "pairs" : "s",
			   display_lvname(lv));
	for (s = 0; s < seg->area_count; s++) {
		if ((slv = _seg_lv_checked(seg, s))) {
			/* Recurse into any sub LVs */
			if (!_pre_raid_replace_activate_recurse(slv))
				return 0;

			if (slv->status & LV_REBUILD) {
				if (!_suspend_resume_lv(slv))
					return 0;
				if (_seg_metalv_checked(seg, s) &&
				    !_suspend_resume_lv(seg_metalv(seg, s)))
					return 0;
			}
		}
	}

	return 1;
}

/* Helper: callback function to suspend+resume all image component pairs @lv */
static int _pre_raid_replace_activate(struct logical_volume *lv, ...)
{
	RETURN_IF_ZERO(lv, "lv argument");

	return (_vg_write_lv_suspend_vg_commit(lv) &&
		_pre_raid_replace_activate_recurse(lv)) ? 2 : 0; /* 2 = metadata got updated */
}

/*
 * API function:
 *
 * lv_raid_replace
 * @top_lv
 * @replace_lv
 * @remove_pvs
 * @allocate_pvs
 *
 * Replace any PV areas on the specified PVs on list @remove_pvs mapped by any LVs in @replace_lv.
 */
static int _lv_raid_replace(struct logical_volume *top_lv,
			    struct logical_volume *replace_lv,
			    int yes,
			    struct dm_list *remove_pvs,
			    struct dm_list *allocate_pvs,
			    int recursive)
{
	uint32_t match_count, partial_lvs = 0, s, zero = 0;
	struct lv_segment *seg;
	struct volume_group *vg;

	RETURN_IF_LV_SEG_ZERO(replace_lv, (seg = first_seg(replace_lv)));
	RETURN_IF_NONZERO(!recursive && !seg_is_raid(seg), "raid segment to replace images in");
	RETURN_IF_ZERO(remove_pvs, "remove pvs list argument");

	vg = replace_lv->vg;

PFLA("top_lv=%s replace_lv=%s", display_lvname(top_lv), display_lvname(replace_lv));

	if (lv_is_duplicated(top_lv)) {
		log_error("Run on top-level duplicating LV");
		return 0;
	}

	if (!_lv_is_active(replace_lv))
		return 0;

	/* If we aren't working on a sub-lv, apply sync and striped restrictions */
	if (!recursive) {
		if (!_raid_in_sync(replace_lv)) {
			log_error("Unable to replace devices in %s while it is"
				  " not in-sync.", display_lvname(replace_lv));
			return 0;
		}
		/* Replacement of top-level striped/raid0 would cause data loss */
		if (seg_is_striped(seg) || seg_is_any_raid0(seg)) {
			log_error("Replacement of devices in %s %s LV prohibited.",
				  lvseg_name(seg), display_lvname(replace_lv));
			return 0;
		}

		if (!archive(vg))
			return_0;

		if (replace_lv->status & PARTIAL_LV)
			vg->cmd->partial_activation = 1;
	}

	/* How many image component pairs/areas are being removed (none ok) ? */
	if (!_resilient_replacement(replace_lv, yes, &match_count, &partial_lvs, remove_pvs))
		return 0;

	if (!match_count)
		return 1;

	/* Recurse into sub LVs in case of a duplicating one */
	if (_lv_is_duplicating(replace_lv)) {
		/* Recurse into sub LVs */
		for (s = 0; s < seg->area_count; s++)
			if (!_lv_raid_replace(top_lv, seg_lv(seg, s), yes, remove_pvs, allocate_pvs, 1))
				return 0;

	/* Replace any failed areas of @replace_lv */
	} else if (!_replace_failed_areas(top_lv, replace_lv, NULL, match_count, remove_pvs, allocate_pvs))
		return 0;

	if (recursive)
		return 1;

	return _lv_update_reload_fns_reset_eliminate_lvs(top_lv, NULL,
							 _post_dummy, NULL,
							 _pre_raid_replace_activate, &zero);
}

/* API */
int lv_raid_replace(struct logical_volume *lv,
		    int yes,
		    struct dm_list *remove_pvs,
		    struct dm_list *allocate_pvs)
{
#if 0
log_error("1 lv_raid_healthy=%d", lv_raid_healthy(lv)); // Yes
usleep(500000);
#endif
	return _lv_raid_replace(lv, lv, yes, remove_pvs, allocate_pvs, 0);
}

/*
 * HM Helper
 *
 * Add @pv to list of @failed_pvs if not yet on
 *
 * Returns:
 *  0       -> already on
 *  1       -> put on anew
 *  -ENOMEM -> failed to allocate "struct pv_list *" var
 *
 */
static int _add_pv_to_failed_pvs(struct physical_volume *pv, struct dm_list *failed_pvs)
{
	struct pv_list *pvl;

	RETURN_IF_ZERO(pv, "pv argument");
	RETURN_IF_ZERO(failed_pvs, "failed pvs list argument");
	/* failed_ps may be empty initially */

	if (_pv_on_list(pv, failed_pvs))
		return 0;

	if (!(pvl = dm_pool_alloc(pv->vg->vgmem, sizeof(*pvl))))
		return -ENOMEM;

	pvl->pv = pv;
	dm_list_add(failed_pvs, &pvl->list);

	return 1;
}

/* Iterate the segments of a sublv and check their allocations vs. missing pvs populating @failed_pvs list */
static int _find_sub_lv_failed_pvs(struct logical_volume *sublv, uint32_t *failed, struct dm_list *failed_pvs)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_ZERO(sublv, "sublv argument");
	RETURN_IF_ZERO(failed, "failed argument");
	RETURN_IF_ZERO(failed_pvs, "failed pvs list argument");

	*failed = 0;

	dm_list_iterate_items(seg, &sublv->segments)
		for (s = 0; s < seg->area_count; s++)
			if (seg_type(seg, s) == AREA_PV &&
		    	    is_missing_pv(seg_pv(seg, s))) {
				if (_add_pv_to_failed_pvs(seg_pv(seg, s), failed_pvs) < 0)
					return 0;

				(*failed)++;
			}

	return 1;
}

/* HM Helper: find number of @failed_rimage and @failed_rmeta sub LVs and populate @failed_pvs list */
static int _find_failed_pvs_of_lv(struct logical_volume *lv,
				  struct dm_list *failed_pvs,
				  uint32_t *failed_rimage, uint32_t *failed_rmeta)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
	RETURN_IF_ZERO(failed_pvs, "failed pvs list argument");
	RETURN_IF_ZERO(failed_rimage, "failed rimage argument");
	RETURN_IF_ZERO(failed_rmeta, "failed rmeta argument");

	if (_lv_is_duplicating(lv)) {
		for (s = 0; s < seg->area_count; s++)
			if (!_find_failed_pvs_of_lv(seg_lv(seg, s), failed_pvs, failed_rimage, failed_rmeta))
				return 0;

		for (s = 0; s < seg->area_count; s++) {
			if (seg->meta_areas &&
			    !_find_sub_lv_failed_pvs(seg_metalv(seg, s), failed_rmeta, failed_pvs))
				return 0;
		}

		return 1;
	}
		
	for (s = 0; s < seg->area_count; s++) {
		if (!_find_sub_lv_failed_pvs(seg_lv(seg, s), failed_rimage, failed_pvs))
			return 0;

		if (seg->meta_areas &&
		    !_find_sub_lv_failed_pvs(seg_metalv(seg, s), failed_rmeta, failed_pvs))
			return 0;
	}

	return 1;
}

/*
 * HM Helper
 *
 * Replace @lv with error segment, setting @lv @status,
 * puting failed PVs on list @failed_pvs and
 * reporting number of uniquely failed LVs on @*replaced_lvs
 */
static int _replace_raid_lv_with_error_segment(struct logical_volume *lv,
					       uint64_t status,
					       struct dm_list *failed_pvs,
					       uint32_t *replaced_lvs)
{
	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));
	RETURN_IF_ZERO(failed_pvs, "failed pvs list argument");
	RETURN_IF_ZERO(replaced_lvs, "replaced LVs argument");

	if (lv_is_on_pvs(lv, failed_pvs)) {
		log_debug("Replacing %s segments with error target",
			  display_lvname(lv));
		lv->status |= PARTIAL_LV;

		if (!_replace_lv_with_error_segment(lv))
			return 0;

		lv->status &= ~PARTIAL_LV;
		lv->status |= status;
		(*replaced_lvs)++;
	}

	return 1;
}

/*
 * Replace any image or metadata LVs of @lv with allocation on @failed_pvs
 * with error segments and return their number in @replaced_lvs
 */
static int _replace_lvs_on_failed_pvs_with_error_segments(struct logical_volume *lv,
							  struct dm_list *failed_pvs,
							  uint32_t *replaced_lvs)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(failed_pvs, "failed pvs list argument");
	RETURN_IF_ZERO(replaced_lvs, "replaced LVs argument");

	/* Recurse to allow for duplicating LV to work */
	if (_lv_is_duplicating(lv)) {
		for (s = 0; s < seg->area_count; s++)
			if (_replace_lvs_on_failed_pvs_with_error_segments(seg_lv(seg, s), failed_pvs, replaced_lvs))
				return 0;
		return 1;
	}

	for (s = 0; s < seg->area_count; s++) {
		if (!_replace_raid_lv_with_error_segment(seg_lv(seg, s), RAID_IMAGE, failed_pvs, replaced_lvs))
			return 0;

		if (seg->meta_areas &&
		    !_replace_raid_lv_with_error_segment(seg_metalv(seg, s), RAID_META, failed_pvs, replaced_lvs))
			return 0;
	}

	return 1;
} 

/*
 * API function:
 *
 * replace any partial data and metadata LVs with error segments
 */
int lv_raid_remove_missing(struct logical_volume *lv)
{
	uint32_t replaced_lvs = 0, failed_rimage = 0, failed_rmeta = 0, max_failed;
	struct lv_segment *seg;
	struct dm_list failed_pvs;

PFL();
	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_ZERO(seg_is_raid(seg), "raid segment to remove images from");
	RETURN_IF_ZERO(lv->status & PARTIAL_LV, "partial LV");

	dm_list_init(&failed_pvs);

	log_debug("Attempting to remove missing devices from %s LV, %s",
		  lvseg_name(seg), lv->name);

	/*
	 * Find the amount of rimage and rmeta devices on failed PVs of @lv
	 * and put the failed pvs on failed_pvs list
	 */
	log_debug_metadata("Scanning all rimage and rmeta sub LVs and all their segments of %s for any failed pvs",
			   display_lvname(lv));
	if (!_find_failed_pvs_of_lv(lv, &failed_pvs, &failed_rimage, &failed_rmeta))
		return 0;

PFL();
	/* Exit in case LV has no allocations on any failed pvs */
	if (dm_list_empty(&failed_pvs))
		return 1;

	log_debug_metadata("lv %s is mapped to %u failed pvs", display_lvname(lv), dm_list_size(&failed_pvs));

	/* Define maximum sub LVs which are allowed to fail */
	max_failed = (seg_is_striped_raid(seg) && !seg_is_any_raid10(seg)) ?
		     seg->segtype->parity_devs : seg->data_copies - 1;
	if (failed_rimage > max_failed ||
	    failed_rmeta  > seg->area_count - 1)
		log_error("RAID LV %s is not operational with %u pvs missing!",
			  display_lvname(lv), dm_list_size(&failed_pvs));

	if (!archive(lv->vg))
		return_0;

	/*
	 * Only error those rimage/rmeta devices which have allocations
	 * on @failed_pvs and only their failed segments in multi-segmented
	 * rimage/rmeta sub LVs rather than the whole sublv!
	 */
	log_debug_metadata("Replacing all failed segments in LV %s with error types",
			   display_lvname(lv));

	if (!_replace_lvs_on_failed_pvs_with_error_segments(lv, &failed_pvs, &replaced_lvs))
		return 0;

	if (replaced_lvs &&
	    !lv_update_and_reload(lv))
		return_0;

	return 1;
}

/* Return 1 if @lv has failed */
static int _lv_has_failed(struct logical_volume *lv)
{
	RETURN_IF_LV_SEG_ZERO(lv, first_seg(lv));

	return (lv->status & PARTIAL_LV) ||
		lv_is_virtual(lv);
}

/* Return 1 if a partial raid LV can be activated redundantly */
static int _partial_raid_lv_is_redundant(const struct logical_volume *lv)
{
	struct lv_segment *raid_seg;
	uint32_t failed_rimage = 0, failed_rmeta = 0, min_devs, s;

	RETURN_IF_LV_SEG_ZERO(lv, (raid_seg = first_seg(lv)));

	min_devs = raid_seg->segtype->parity_devs ?: 1;

	/*
	 * Count number of failed rimage and rmeta components seperately
	 * so that we can activate an raid set with at least one metadata
	 * dev (mandatory unless raid0) and quorum number of data devs
	 */
	for (s = 0; s < raid_seg->area_count; s++) {
		RETURN_IF_ZERO(_seg_lv_checked(raid_seg, s), "data sub lv");

		if (_lv_has_failed(seg_lv(raid_seg, s)))
			failed_rimage++;

		if (raid_seg->meta_areas) {
			RETURN_IF_ZERO(seg_metatype(raid_seg, s) == AREA_LV, "meta sub lv");

			if (_lv_has_failed(seg_lv(raid_seg, s)))
				failed_rmeta++;
		}
	}

	/* No devices failed -> fully redundant */
	if (failed_rimage + failed_rmeta == 0)
		return 1;

	/* All data devices have failed */
	if (failed_rimage == raid_seg->area_count) {
		log_verbose("All data components of raid LV %s have failed.",
			    display_lvname(lv));
		return 0; /* Insufficient redundancy to activate */
	}

	/* We require at least one metadata component to retrieve raid set state */
	if (failed_rmeta == raid_seg->area_count) {
		log_error("All metadata devices of %s have failed! Can't retrive raid set state!",
			  display_lvname(lv));
		return 0;
	}

	/*
	 * raid10:
	 *
	 lv->* - if #devices is divisable by number of data copies,
	 *   the data copies form 'mirror groups' like 'AAABBB' for 3 data copies and 6 stripes ->
	 *   check that each of the mirror groups has at least 2 data copies available
	 *
	 * - of not, we have an odd number of devices causing just _one_ mirror group ->
	 *   check that at least one data copy is available
	 *
	 */
	if (seg_is_any_raid10(raid_seg)) {
		uint32_t i;
		uint32_t mirror_groups = (raid_seg->area_count % raid_seg->data_copies) ?
					 1 : raid_seg->data_copies;
		uint32_t rebuilds_per_group = 0;

		for (i = 0; i < raid_seg->area_count * mirror_groups; i++) {
			s = i % raid_seg->area_count;

			if (!(i % mirror_groups))
				rebuilds_per_group = 0;

			if (_lv_has_failed(seg_lv(raid_seg, s)))
				rebuilds_per_group++;

			if (rebuilds_per_group >= raid_seg->data_copies) {
				log_verbose(mirror_groups == 1 ? "Tue many data copies have failed in %s." :
								 "An entire mirror group has failed in %s.",
					    display_lvname(lv));
				return 0; /* Insufficient redundancy to activate */
			}
		}

	} else if (failed_rimage) {
		/* Check raid0* */
		if  (seg_is_any_raid0(raid_seg)) {
			log_verbose("No data components of %s LV %s may fail",
				    lvseg_name(raid_seg), display_lvname(lv));
			return 0; /* Insufficient redundancy to activate */
		}

		/* Check for mirrored/parity raid being redundant */
		if (failed_rimage > min_devs) {
			log_verbose("More than %u components from %s %s have failed.",
				    min_devs, lvseg_name(raid_seg), display_lvname(lv));
			return 0; /* Insufficient redundancy to activate */
		}
	}

	return 1; /* @lv is redundant -> user data intact */
}

/* Sets *@data to 1 if @lv cannot be activated without data loss */
static int _lv_may_be_activated_in_degraded_mode(struct logical_volume *lv, void *data)
{
	int *not_capable;
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_ZERO(lv, "lv argument");
	RETURN_IF_ZERO(data, "data argument");

	not_capable = (int*) data;

	if (*not_capable ||
	    !(lv->status & PARTIAL_LV))
		return 1; /* No further checks needed */

	if (lv_is_raid(lv)) {
		*not_capable = !_partial_raid_lv_is_redundant(lv);
		return 1;
	}

	/* Ignore RAID sub-LVs. */
	if (lv_is_raid_type(lv))
		return 1;

	dm_list_iterate_items(seg, &lv->segments)
		for (s = 0; s < seg->area_count; s++)
			if (!_seg_lv_checked(seg, s)) {
				log_verbose("%s contains a segment incapable of degraded activation",
					    display_lvname(lv));
				*not_capable = 1;
			}

	return 1;
}

/*
 * API function:
 *
 * check if @clv supported degraded activation
 */
int partial_raid_lv_supports_degraded_activation(const struct logical_volume *clv)
{
	int not_capable = 0;
	struct logical_volume *lv;

	RETURN_IF_LV_SEG_ZERO(clv, first_seg(clv));
	RETURN_IF_ZERO(seg_is_raid(first_seg(clv)), "raid segment passed in");

	lv = (struct logical_volume*) clv; /* drop const */

	if (!_lv_may_be_activated_in_degraded_mode(lv, &not_capable) || not_capable)
		return_0;

	if (!for_each_sub_lv(lv, _lv_may_be_activated_in_degraded_mode, &not_capable)) {
		log_error(INTERNAL_ERROR "for_each_sub_lv failure.");
		return 0;
	}

	return !not_capable;
}

/* HM raid10_far helper: ensure consistent image LVs have been passed in for @seg */
static int _raid10_seg_images_sane(struct lv_segment *seg)
{
	uint32_t len = 0, s;
	struct logical_volume *slv;

	RETURN_IF_SEG_ZERO(seg);
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);

	for (s = 0; s < seg->area_count; s++) {
		RETURN_IF_ZERO((slv = _seg_lv_checked(seg, s)), "raid10_far image LV");

		if (len) {
			RETURN_IF_ZERO((slv->le_count == len), "consistent raid10_far image LV length");
		} else {
			RETURN_IF_ZERO((len = slv->le_count), "raid10_far image LV length");
		}

		RETURN_IF_NONZERO((slv->le_count % seg->data_copies),
				  "raid10_far image LV length divisibility by #data_copies");
	}

	return 1;
}

/* HM raid10_far helper: split up all data image sub LVs of @lv from @start LE to @end LE in @split_len increments */
static int _split_lv_data_images(struct logical_volume *lv,
				 uint32_t start, uint32_t end,
				 uint32_t split_len)
{
	uint32_t s;
	struct lv_segment *seg;

	RETURN_IF_LV_SEG_ZERO(lv, (seg = first_seg(lv)));
	RETURN_IF_SEG_AREA_INDEX_FALSE(seg, 0);
	RETURN_IF_ZERO(split_len < seg->len, "suitable agument split_len");

	for (s = 0; s < seg->area_count; s++) {
		uint32_t le;
		struct logical_volume *slv = seg_lv(seg, s);

		/* Split the image up */
		for (le = start; le < end; le += split_len)
			if (!lv_split_segment(slv, le))
				return_0;
	}

	return 1;
}

/*
 * HM API function:
 *
 * Reorder segments for @extents length in @lv;
 * @extend flag indicates extension/reduction request.
 *
 * raid10_far arranges stripe zones with data block rotation
 * one after the other and does data_copies across them.
 *
 * In order to resize those, we have to split them up by # data copies
 * and reorder the split segments.
 *
 * Called via the lv_extend()/lv_reduce() API
 *
 */
int lv_raid10_far_reorder_segments(struct logical_volume *lv, uint32_t extents, int extend)
{
	uint32_t le, s;
	struct logical_volume *slv;
	struct lv_segment *seg, *raid_seg;

	RETURN_IF_LV_SEG_ZERO(lv, (raid_seg = first_seg(lv)));
	RETURN_IF_ZERO(extents, "extents to reorder");
	/* We may only reorder in case of raid10 far */
	RETURN_IF_ZERO(seg_is_raid10_far(raid_seg), "raid10_far segment");

PFLA("extents=%u lv->le_count=%u raid_seg->area_len=%u", extents, lv->le_count, raid_seg->area_len);
	/* If this is a new LV -> no need to reorder */
	if (!lv->le_count)
		return 1;

	/* Check properties of raid10_far segment for compaitbility */
	if (!_raid10_seg_images_sane(raid_seg))
		return 0;
PFL();
	if (extend) {
		uint32_t new_split_len, prev_le_count, prev_split_len;

		/*
		 * We've got new extents added to the image LVs which
		 * are in the wrong place; got to split them up to insert
		 * the split ones into the previous raid10_far ones.
		 */
		/* Ensure proper segment boundaries so that we can move segments */

		/* Calculate previous length, because the LV is already grwon when we get here */
		prev_le_count = raid_rimage_extents(raid_seg->segtype, lv->le_count - extents,
						    raid_seg->area_count, raid_seg->data_copies);
		prev_split_len = prev_le_count / raid_seg->data_copies;

		/* Split segments of all image LVs for reordering */
		if (!_split_lv_data_images(lv, prev_split_len /* start */, prev_le_count, prev_split_len))
			return 0;

		/* Split the newly allocated part of the images up */
		slv = seg_lv(raid_seg, 0);
		new_split_len = (slv->le_count - prev_le_count) / raid_seg->data_copies;
		if (!_split_lv_data_images(lv, prev_le_count /* start */, slv->le_count, new_split_len))
			return 0;
PFL();
		/*
		 * Reorder segments of the image LVs so that the split off #data_copies
		 * segments of the new allocation get moved to the ends of the split off
		 * previous ones.
		 *
		 * E.g. with 3 data copies before/after reordering an image LV:
		 *
		 * P1, P2, P3, N1, N2, N3 -> P1, N1, P2, N2, P3, N3
		 */
		for (s = 0; s < raid_seg->area_count; s++) {
			uint32_t le2;
			struct lv_segment *seg2;

			slv = seg_lv(raid_seg, s);
			for (le = prev_split_len, le2 = prev_le_count + new_split_len;
			     le2 < slv->le_count;
			     le += prev_split_len, le2 += new_split_len) {
				seg  = find_seg_by_le(slv, le);
				seg2 = find_seg_by_le(slv, le2);
				dm_list_move(seg->list.n, &seg2->list);
			}
		}

	/*
	 * Reduce...
	 */
	} else {
		uint32_t reduction, split_len;

		/* Only reorder in case of partial reduction; deletion does not require it */
		if (extents >= raid_seg->len)
			return 1;

		/* Ensure proper segment boundaries so that we can move segments */
		slv = seg_lv(raid_seg, 0);
		reduction = extents / raid_seg->area_count;
		split_len = slv->le_count / raid_seg->data_copies;

		/* Split segments of all image LVs for reordering */
		if (!_split_lv_data_images(lv, split_len - reduction, slv->le_count, split_len) ||
		    !_split_lv_data_images(lv, split_len, slv->le_count, split_len))
			return 0;

		/* Reorder split segments of all image LVs to have those to reduce at the end */
		for (s = 0; s < raid_seg->area_count; s++) {
			slv = seg_lv(raid_seg, s);
			for (le = split_len - reduction; le < slv->le_count; le += split_len) {
				seg = find_seg_by_le(slv, le);
				dm_list_move(&slv->segments, &seg->list);
			}
		}
PFL();
	}

	/* Correct segments start logical extents in all sub LVs of @lv */
	return _lv_set_image_lvs_start_les(lv);
}

/*
 * HM API function
 *
 * Create a raid01 (mirrors on top of stripes) LV
 *
 * I.e. allocate a "raid01" top-level segment and @data_copies striped LVs
 * with @stripes each to insert as the top-level segments area LVs.
 *
 * Called via the lv_extend() API
 */
int lv_create_raid01(struct logical_volume *lv, const struct segment_type *segtype,
		     unsigned data_copies, unsigned stripes,
		     unsigned stripe_size, unsigned region_size,
		     unsigned extents, struct dm_list *allocate_pvs)
{
	uint64_t status = RAID_IMAGE | LVM_READ | LVM_WRITE;
	struct lv_segment *raid01_seg;

	RETURN_IF_LV_SEGTYPE_ZERO(lv, segtype);
	RETURN_IF_ZERO(extents, "extents");
	RETURN_IF_ZERO(allocate_pvs, "allocate pvs argument");
	RETURN_IF_NONZERO(dm_list_empty(allocate_pvs), "pvs to allocate on listed");
	RETURN_IF_NONZERO(stripes < 2, "proper number of stripes");

	data_copies = data_copies < 2 ? 2 : data_copies;

	/* Create the raid01 top-level segment */
	if (!(raid01_seg = alloc_lv_segment(segtype, lv, 0 /* le */, extents /* len */,
					    0 /* reshape_len */, status | RAID,
					    0 /* stripe_size */, NULL,
					    data_copies, extents /* area_len */,
					    data_copies, 0, region_size, 0, NULL))) {
		log_error("Failed to create %s top-level segment for LV %s",
			  segtype->name, display_lvname(lv));
		return_0;
	}

	/* Put new raid01 segment on LV's segment list */
	dm_list_init(&lv->segments);
	dm_list_add(&lv->segments, &raid01_seg->list);

	/* Has to be set before calling _check_and_init_region_size() */
	lv->le_count = raid01_seg->len;
	lv->size = raid01_seg->len * lv->vg->extent_size;

	log_debug_metadata("Allocating segment metadata areas of %s", display_lvname(lv));
	raid01_seg->meta_areas = NULL;
	if (!_realloc_seg_areas(lv, data_copies, RAID_META))
		return 0;

	/* Create the #data_copies striped sub LVs */
	return _lv_create_raid01_image_lvs(lv, raid01_seg, extents,
					   stripes, stripe_size,
					   0 /* first area */, data_copies,
					   allocate_pvs);
}
