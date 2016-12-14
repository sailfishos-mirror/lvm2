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

typedef enum {
	/* Split:
	 *   For a mirrored or raid LV, split mirror into two mirrors, optionally tracking
	 *     future changes to the main mirror to allow future recombination.
	 */
	CONV_SPLIT = 1,
	CONV_SPLIT_MIRRORS = 2,

	/* Every other segment type or mirror log conversion we haven't separated out */
	CONV_OTHER = 3,
} conversion_type_t;

struct lvconvert_params {
	/* Exactly one of these command categories is determined */
	int split;		/* 1 */
	int keep_mimages;	/* 2 */	/* --splitmirrors */
	/* other */		/* 3 */

	/* FIXME Eliminate all cases where more than one of the above are set then use conv_type instead */
	conversion_type_t	conv_type;

	int track_changes;	/* CONV_SPLIT_MIRRORS is set */

	int corelog;		/* Equivalent to --mirrorlog core */
	int mirrorlog;		/* Only one of corelog and mirrorlog may be set */

	int mirrors_supplied;	/* When type_str is not set, this may be set with keep_mimages for --splitmirrors */
	const char *type_str;	/* When this is set, mirrors_supplied may optionally also be set */
				/* Holds what you asked for based on --type or other arguments, else "" */

	const struct segment_type *segtype;	/* Holds what segment type you will get */

	int force;
	int yes;
	int zero;

	const char *lv_split_name;

	uint32_t region_size;

	uint32_t mirrors;
	sign_t mirrors_sign;
	uint32_t stripes;
	uint32_t stripe_size;
	unsigned stripes_supplied;
	unsigned stripe_size_supplied;
	uint32_t read_ahead;

	unsigned target_attr;

	alloc_policy_t alloc;

	int pv_count;
	struct dm_list *pvh;

	int wait_completion;
	int need_polling;
	struct logical_volume *lv_to_poll;
	struct dm_list idls;
};


/* FIXME Temporary function until the enum replaces the separate variables */
static void _set_conv_type(struct lvconvert_params *lp, int conv_type)
{
	if (lp->conv_type != CONV_OTHER)
		log_error(INTERNAL_ERROR "Changing conv_type from %d to %d.", lp->conv_type, conv_type);

	lp->conv_type = conv_type;
}

static int _raid0_type_requested(const char *type_str)
{
	return (!strcmp(type_str, SEG_TYPE_NAME_RAID0) || !strcmp(type_str, SEG_TYPE_NAME_RAID0_META));
}

/* mirror/raid* (1,10,4,5,6 and their variants) reshape */
static int _mirror_or_raid_type_requested(struct cmd_context *cmd, const char *type_str)
{
	return (arg_is_set(cmd, mirrors_ARG) || !strcmp(type_str, SEG_TYPE_NAME_MIRROR) ||
		(!strncmp(type_str, SEG_TYPE_NAME_RAID, 4) && !_raid0_type_requested(type_str)));
}

static int _linear_type_requested(const char *type_str)
{
	return (!strcmp(type_str, SEG_TYPE_NAME_LINEAR));
}

static int _striped_type_requested(const char *type_str)
{
	return (!strcmp(type_str, SEG_TYPE_NAME_STRIPED) || _linear_type_requested(type_str));
}

static int _read_conversion_type(struct cmd_context *cmd,
				 struct lvconvert_params *lp)
{

	const char *type_str = arg_str_value(cmd, type_ARG, "");

	lp->type_str =  type_str;
	if (!lp->type_str[0])
		return 1;

	/* FIXME: Check thin-pool and thin more thoroughly! */
	if (!strcmp(type_str, SEG_TYPE_NAME_SNAPSHOT) || _striped_type_requested(type_str) ||
	    !strncmp(type_str, SEG_TYPE_NAME_RAID, 4) || !strcmp(type_str, SEG_TYPE_NAME_MIRROR) ||
	    !strcmp(type_str, SEG_TYPE_NAME_CACHE_POOL) || !strcmp(type_str, SEG_TYPE_NAME_CACHE) ||
	    !strcmp(type_str, SEG_TYPE_NAME_THIN_POOL) || !strcmp(type_str, SEG_TYPE_NAME_THIN))
		return 1;

	log_error("Conversion using --type %s is not supported.", type_str);

	return 0;
}

static int _read_params(struct cmd_context *cmd, struct lvconvert_params *lp)
{
	const char *vg_name = NULL;
	int region_size;
	int pagesize = lvm_getpagesize();

	if (!_read_conversion_type(cmd, lp))
		return_0;

	if (!arg_is_set(cmd, background_ARG))
		lp->wait_completion = 1;

	if (arg_is_set(cmd, corelog_ARG))
		lp->corelog = 1;

	if (arg_is_set(cmd, mirrorlog_ARG)) {
		if (lp->corelog) {
			log_error("--mirrorlog and --corelog are incompatible.");
			return 0;
		}
		lp->mirrorlog = 1;
	}

	if (arg_is_set(cmd, split_ARG)) {
		if (arg_outside_list_is_set(cmd, "cannot be used with --split",
					    split_ARG,
					    name_ARG,
					    force_ARG, noudevsync_ARG, test_ARG,
					    -1))
			return_0;
		lp->split = 1;
		_set_conv_type(lp, CONV_SPLIT);
	}
	
	if (arg_is_set(cmd, trackchanges_ARG))
		lp->track_changes = 1;

	if (lp->split) {
		if ((lp->lv_split_name = arg_str_value(cmd, name_ARG, NULL))) {
			if (!validate_restricted_lvname_param(cmd, &vg_name, &lp->lv_split_name))
				return_0;
		}


	/*
	 * The '--splitmirrors n' argument is equivalent to '--mirrors -n'
	 * (note the minus sign), except that it signifies the additional
	 * intent to keep the mimage that is detached, rather than
	 * discarding it.
	 */
	} else if (arg_is_set(cmd, splitmirrors_ARG)) {
		if (_mirror_or_raid_type_requested(cmd, lp->type_str)) {
			log_error("--mirrors/--type mirror/--type raid* and --splitmirrors are "
				  "mutually exclusive.");
			return 0;
		}

		if (!arg_is_set(cmd, name_ARG) && !lp->track_changes) {
			log_error("Please name the new logical volume using '--name'");
			return 0;
		}

		if ((lp->lv_split_name = arg_str_value(cmd, name_ARG, NULL))) {
			if (!validate_restricted_lvname_param(cmd, &vg_name, &lp->lv_split_name))
				return_0;
		}

		lp->keep_mimages = 1;
		_set_conv_type(lp, CONV_SPLIT_MIRRORS);
		lp->mirrors = arg_uint_value(cmd, splitmirrors_ARG, 0);
		lp->mirrors_sign = SIGN_MINUS;
	} else {
		if (lp->track_changes) {
			log_error("--trackchanges is only valid with --splitmirrors.");
			return 0;
		}
		if (arg_is_set(cmd, name_ARG)) {
			log_error("The 'name' argument is only valid with --splitmirrors");
			return 0;
		}
	}

	/* If no other case was identified, then use of --stripes means --type striped */
	if (!arg_is_set(cmd, type_ARG) && !*lp->type_str &&
	    !lp->split && !lp->mirrorlog && !lp->corelog &&
	    (arg_is_set(cmd, stripes_long_ARG) || arg_is_set(cmd, stripesize_ARG)))
		lp->type_str = SEG_TYPE_NAME_STRIPED;

	if ((arg_is_set(cmd, stripes_long_ARG) || arg_is_set(cmd, stripesize_ARG)) &&
	    !(_mirror_or_raid_type_requested(cmd, lp->type_str) || _striped_type_requested(lp->type_str) ||
	      _raid0_type_requested(lp->type_str) || arg_is_set(cmd, thinpool_ARG))) {
		log_error("--stripes or --stripesize argument is only valid "
			  "with --mirrors/--type mirror/--type raid*/--type striped/--type linear, --repair and --thinpool");
		return 0;
	}

	if (arg_is_set(cmd, mirrors_ARG)) {
		/* --splitmirrors is the mechanism for detaching and keeping a mimage */
		lp->mirrors_supplied = 1;
		lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0);
		lp->mirrors_sign = arg_sign_value(cmd, mirrors_ARG, SIGN_NONE);
	}

	lp->alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);

	/* We should have caught all these cases already. */
	if (lp->split + lp->keep_mimages > 1) {
		log_error(INTERNAL_ERROR "Unexpected combination of incompatible options selected.");
		return 0;
	}

	/*
	 * Final checking of each case:
	 *   lp->split
	 *   lp->keep_mimages
	 *   --type mirror|raid  lp->mirrorlog lp->corelog
	 *   --type raid0|striped
	 */
	switch(lp->conv_type) {
	case CONV_SPLIT:
	case CONV_SPLIT_MIRRORS:
                break;

	case CONV_OTHER:
		if (_mirror_or_raid_type_requested(cmd, lp->type_str) ||
			   lp->mirrorlog || lp->corelog) { /* Mirrors (and some RAID functions) */
			if (arg_is_set(cmd, chunksize_ARG)) {
				log_error("--chunksize is only available with snapshots or pools.");
				return 0;
			}

			if (arg_is_set(cmd, zero_ARG)) {
				log_error("--zero is only available with snapshots or thin pools.");
				return 0;
			}

			/*
			 * --regionsize is only valid if converting an LV into a mirror.
			 * Checked when we know the state of the LV being converted.
			 */
			if (arg_is_set(cmd, regionsize_ARG)) {
				if (arg_sign_value(cmd, regionsize_ARG, SIGN_NONE) ==
					    SIGN_MINUS) {
					log_error("Negative regionsize is invalid.");
					return 0;
				}
				lp->region_size = arg_uint_value(cmd, regionsize_ARG, 0);
			} else {
				region_size = get_default_region_size(cmd);
				if (region_size < 0) {
					log_error("Negative regionsize in "
						  "configuration file is invalid.");
					return 0;
				}
				lp->region_size = region_size;
			}

			if (lp->region_size % (pagesize >> SECTOR_SHIFT)) {
				log_error("Region size (%" PRIu32 ") must be "
					  "a multiple of machine memory "
					  "page size (%d).",
					  lp->region_size, pagesize >> SECTOR_SHIFT);
				return 0;
			}

			if (!is_power_of_2(lp->region_size)) {
				log_error("Region size (%" PRIu32
					  ") must be a power of 2.", lp->region_size);
				return 0;
			}

			if (!lp->region_size) {
				log_error("Non-zero region size must be supplied.");
				return 0;
			}

			/* FIXME man page says in one place that --type and --mirrors can't be mixed */
			if (lp->mirrors_supplied && !lp->mirrors)
				/* down-converting to linear/stripe? */
				lp->type_str = SEG_TYPE_NAME_STRIPED;

		} else if (_raid0_type_requested(lp->type_str) || _striped_type_requested(lp->type_str)) { /* striped or linear or raid0 */
			if (arg_from_list_is_set(cmd, "cannot be used with --type raid0 or --type striped or --type linear",
						 chunksize_ARG, corelog_ARG, mirrors_ARG, mirrorlog_ARG, regionsize_ARG, zero_ARG,
						 -1))
				return_0;
		} /* else segtype will default to current type */
	}

	lp->force = arg_count(cmd, force_ARG);
	lp->yes = arg_count(cmd, yes_ARG);

	return 1;
}

static int _insert_lvconvert_layer(struct cmd_context *cmd,
				   struct logical_volume *lv)
{
	char format[NAME_LEN], layer_name[NAME_LEN];
	int i;

	/*
	 * We would like to give the same number for this layer
	 * and the newly added mimage.
	 * However, LV name of newly added mimage is determined *after*
	 * the LV name of this layer is determined.
	 *
	 * So, use generate_lv_name() to generate mimage name first
	 * and take the number from it.
	 */

	if (dm_snprintf(format, sizeof(format), "%s_mimage_%%d", lv->name) < 0) {
		log_error("lvconvert: layer name creation failed.");
		return 0;
	}

	if (!generate_lv_name(lv->vg, format, layer_name, sizeof(layer_name)) ||
	    sscanf(layer_name, format, &i) != 1) {
		log_error("lvconvert: layer name generation failed.");
		return 0;
	}

	if (dm_snprintf(layer_name, sizeof(layer_name), MIRROR_SYNC_LAYER "_%d", i) < 0) {
		log_error("layer name creation failed.");
		return 0;
	}

	if (!insert_layer_for_lv(cmd, lv, 0, layer_name)) {
		log_error("Failed to insert resync layer");
		return 0;
	}

	return 1;
}

static int _failed_mirrors_count(struct logical_volume *lv)
{
	struct lv_segment *lvseg;
	int ret = 0;
	unsigned s;

	dm_list_iterate_items(lvseg, &lv->segments) {
		if (!seg_is_mirrored(lvseg))
			return -1;
		for (s = 0; s < lvseg->area_count; s++) {
			if (seg_type(lvseg, s) == AREA_LV) {
				if (is_temporary_mirror_layer(seg_lv(lvseg, s)))
					ret += _failed_mirrors_count(seg_lv(lvseg, s));
				else if (lv_is_partial(seg_lv(lvseg, s)))
					++ ret;
			}
			else if (seg_type(lvseg, s) == AREA_PV &&
				 is_missing_pv(seg_pv(lvseg, s)))
				++ret;
		}
	}

	return ret;
}

static int _failed_logs_count(struct logical_volume *lv)
{
	int ret = 0;
	unsigned s;
	struct logical_volume *log_lv = first_seg(lv)->log_lv;
	if (log_lv && lv_is_partial(log_lv)) {
		if (lv_is_mirrored(log_lv))
			ret += _failed_mirrors_count(log_lv);
		else
			ret += 1;
	}
	for (s = 0; s < first_seg(lv)->area_count; s++) {
		if (seg_type(first_seg(lv), s) == AREA_LV &&
		    is_temporary_mirror_layer(seg_lv(first_seg(lv), s)))
			ret += _failed_logs_count(seg_lv(first_seg(lv), s));
	}
	return ret;
}

static int _is_partial_lv(struct logical_volume *lv,
			  void *baton __attribute__((unused)))
{
	return lv_is_partial(lv);
}

/*
 * Walk down the stacked mirror LV to the original mirror LV.
 */
static struct logical_volume *_original_lv(struct logical_volume *lv)
{
	struct logical_volume *next_lv = lv, *tmp_lv;

	while ((tmp_lv = find_temporary_mirror(next_lv)))
		next_lv = tmp_lv;

	return next_lv;
}

static void _lvconvert_mirrors_repair_ask(struct cmd_context *cmd,
					  int failed_log, int failed_mirrors,
					  int *replace_log, int *replace_mirrors)
{
	const char *leg_policy, *log_policy;
	int force = arg_count(cmd, force_ARG);
	int yes = arg_count(cmd, yes_ARG);

	if (arg_is_set(cmd, usepolicies_ARG)) {
		leg_policy = find_config_tree_str(cmd, activation_mirror_image_fault_policy_CFG, NULL);
		log_policy = find_config_tree_str(cmd, activation_mirror_log_fault_policy_CFG, NULL);
		*replace_mirrors = strcmp(leg_policy, "remove");
		*replace_log = strcmp(log_policy, "remove");
		return;
	}

	if (force != PROMPT) {
		*replace_log = *replace_mirrors = 0;
		return;
	}

	*replace_log = *replace_mirrors = 1;

	if (yes)
		return;

	if (failed_log &&
	    yes_no_prompt("Attempt to replace failed mirror log? [y/n]: ") == 'n')
		*replace_log = 0;

	if (failed_mirrors &&
	    yes_no_prompt("Attempt to replace failed mirror images "
			  "(requires full device resync)? [y/n]: ") == 'n')
		*replace_mirrors = 0;
}

/*
 * _get_log_count
 * @lv: the mirror LV
 *
 * Get the number of on-disk copies of the log.
 *  0  = 'core'
 *  1  = 'disk'
 *  2+ = 'mirrored'
 */
static uint32_t _get_log_count(struct logical_volume *lv)
{
	struct logical_volume *log_lv;

	log_lv = first_seg(_original_lv(lv))->log_lv;
	if (log_lv)
		return lv_mirror_count(log_lv);

	return 0;
}

static int _lv_update_mirrored_log(struct logical_volume *lv,
				   struct dm_list *operable_pvs,
				   int log_count)
{
	int old_log_count;
	struct logical_volume *log_lv;

	/*
	 * When log_count is 0, mirrored log doesn't need to be
	 * updated here but it will be removed later.
	 */
	if (!log_count)
		return 1;

	log_lv = first_seg(_original_lv(lv))->log_lv;
	if (!log_lv || !lv_is_mirrored(log_lv))
		return 1;

	old_log_count = _get_log_count(lv);
	if (old_log_count == log_count)
		return 1;

	/* Reducing redundancy of the log */
	return remove_mirror_images(log_lv, log_count,
				    is_mirror_image_removable,
				    operable_pvs, 0U);
}

static int _lv_update_log_type(struct cmd_context *cmd,
			       struct lvconvert_params *lp,
			       struct logical_volume *lv,
			       struct dm_list *operable_pvs,
			       int log_count)
{
	int old_log_count;
	uint32_t region_size = (lp) ? lp->region_size :
		first_seg(lv)->region_size;
	alloc_policy_t alloc = (lp) ? lp->alloc : lv->alloc;
	struct logical_volume *original_lv;
	struct logical_volume *log_lv;

	old_log_count = _get_log_count(lv);
	if (old_log_count == log_count)
		return 1;

	original_lv = _original_lv(lv);
	/* Remove an existing log completely */
	if (!log_count) {
		if (!remove_mirror_log(cmd, original_lv, operable_pvs,
				       arg_count(cmd, yes_ARG) ||
				       arg_count(cmd, force_ARG)))
			return_0;
		return 1;
	}

	log_lv = first_seg(original_lv)->log_lv;

	/* Adding redundancy to the log */
	if (old_log_count < log_count) {
		region_size = adjusted_mirror_region_size(lv->vg->extent_size,
							  lv->le_count,
							  region_size, 0,
							  vg_is_clustered(lv->vg));

		if (!add_mirror_log(cmd, original_lv, log_count,
				    region_size, operable_pvs, alloc))
			return_0;
		/*
		 * FIXME: This simple approach won't work in cluster mirrors,
		 *	  but it doesn't matter because we don't support
		 *	  mirrored logs in cluster mirrors.
		 */
		if (old_log_count &&
		    !lv_update_and_reload(log_lv))
			return_0;

		return 1;
	}

	/* Reducing redundancy of the log */
	return remove_mirror_images(log_lv, log_count,
				    is_mirror_image_removable, operable_pvs, 1U);
}

/*
 * _lvconvert_mirrors_parse_params
 *
 * This function performs the following:
 *  1) Gets the old values of mimage and log counts
 *  2) Parses the CLI args to find the new desired values
 *  3) Adjusts 'lp->mirrors' to the appropriate absolute value.
 *     (Remember, 'lp->mirrors' is specified in terms of the number of "copies"
 *     vs. the number of mimages.  It can also be a relative value.)
 *  4) Sets 'lp->need_polling' if collapsing
 *  5) Validates other mirror params
 *
 * Returns: 1 on success, 0 on error
 */
static int _lvconvert_mirrors_parse_params(struct cmd_context *cmd,
					   struct logical_volume *lv,
					   struct lvconvert_params *lp,
					   uint32_t *old_mimage_count,
					   uint32_t *old_log_count,
					   uint32_t *new_mimage_count,
					   uint32_t *new_log_count)
{
	*old_mimage_count = lv_mirror_count(lv);
	*old_log_count = _get_log_count(lv);

	if (is_lockd_type(lv->vg->lock_type) && lp->keep_mimages) {
		/* FIXME: we need to create a lock for the new LV. */
		log_error("Unable to split mirrors in VG with lock_type %s", lv->vg->lock_type);
		return 0;
	}

	/*
	 * Adjusting mimage count?
	 */
	if (!lp->mirrors_supplied && !lp->keep_mimages)
		lp->mirrors = *old_mimage_count;
	else if (lp->mirrors_sign == SIGN_PLUS)
		lp->mirrors = *old_mimage_count + lp->mirrors;
	else if (lp->mirrors_sign == SIGN_MINUS)
		lp->mirrors = (*old_mimage_count > lp->mirrors) ?
			*old_mimage_count - lp->mirrors: 0;
	else
		lp->mirrors += 1;

	*new_mimage_count = lp->mirrors;

	/* Too many mimages? */
	if (lp->mirrors > DEFAULT_MIRROR_MAX_IMAGES) {
		log_error("Only up to %d images in mirror supported currently.",
			  DEFAULT_MIRROR_MAX_IMAGES);
		return 0;
	}

	/* Did the user try to subtract more legs than available? */
	if (lp->mirrors < 1) {
		log_error("Unable to reduce images by specified amount - only %d in %s",
			  *old_mimage_count, lv->name);
		return 0;
	}

	/*
	 * FIXME: It would be nice to say what we are adjusting to, but
	 * I really don't know whether to specify the # of copies or mimages.
	 */
	if (*old_mimage_count != *new_mimage_count)
		log_verbose("Adjusting mirror image count of %s", lv->name);

	/*
	 * Adjust log type
	 *
	 * If we are converting from a mirror to another mirror or simply
	 * changing the log type, we start by assuming they want the log
	 * type the same and then parse the given args.  OTOH, If we are
	 * converting from linear to mirror, then we start from the default
	 * position that the user would like a 'disk' log.
	 */
	*new_log_count = (*old_mimage_count > 1) ? *old_log_count : 1;
	if (!lp->corelog && !lp->mirrorlog)
		return 1;

	*new_log_count = arg_int_value(cmd, mirrorlog_ARG, lp->corelog ? MIRROR_LOG_CORE : DEFAULT_MIRRORLOG);

	/*
	 * No mirrored logs for cluster mirrors until
	 * log daemon is multi-threaded.
	 */
	if ((*new_log_count == MIRROR_LOG_MIRRORED) && vg_is_clustered(lv->vg)) {
		log_error("Log type, \"mirrored\", is unavailable to cluster mirrors.");
		return 0;
	}

	log_verbose("Setting logging type to %s.", get_mirror_log_name(*new_log_count));

	/*
	 * Region size must not change on existing mirrors
	 */
	if (arg_is_set(cmd, regionsize_ARG) && lv_is_mirrored(lv) &&
	    (lp->region_size != first_seg(lv)->region_size)) {
		log_error("Mirror log region size cannot be changed on "
			  "an existing mirror.");
		return 0;
	}

	/*
	 * For the most part, we cannot handle multi-segment mirrors. Bail out
	 * early if we have encountered one.
	 */
	if (lv_is_mirrored(lv) && dm_list_size(&lv->segments) != 1) {
		log_error("Logical volume %s has multiple "
			  "mirror segments.", display_lvname(lv));
		return 0;
	}

	return 1;
}

/*
 * _lvconvert_mirrors_aux
 *
 * Add/remove mirror images and adjust log type.  'operable_pvs'
 * are the set of PVs open to removal or allocation - depending
 * on the operation being performed.
 */
static int _lvconvert_mirrors_aux(struct cmd_context *cmd,
				  struct logical_volume *lv,
				  struct lvconvert_params *lp,
				  struct dm_list *operable_pvs,
				  uint32_t new_mimage_count,
				  uint32_t new_log_count,
				  struct dm_list *pvh)
{
	uint32_t region_size;
	struct lv_segment *seg = first_seg(lv);
	struct logical_volume *layer_lv;
	uint32_t old_mimage_count = lv_mirror_count(lv);
	uint32_t old_log_count = _get_log_count(lv);

	if ((lp->mirrors == 1) && !lv_is_mirrored(lv)) {
		log_warn("Logical volume %s is already not mirrored.",
			 display_lvname(lv));
		return 1;
	}

	region_size = adjusted_mirror_region_size(lv->vg->extent_size,
						  lv->le_count,
						  lp->region_size ? : seg->region_size, 0,
						  vg_is_clustered(lv->vg));

	if (!operable_pvs)
		operable_pvs = pvh;

	/*
	 * Up-convert from linear to mirror
	 */
	if (!lv_is_mirrored(lv)) {
		/* FIXME Share code with lvcreate */

		/*
		 * FIXME should we give not only pvh, but also all PVs
		 * currently taken by the mirror? Would make more sense from
		 * user perspective.
		 */
		if (!lv_add_mirrors(cmd, lv, new_mimage_count - 1, lp->stripes,
				    lp->stripe_size, region_size, new_log_count, operable_pvs,
				    lp->alloc, MIRROR_BY_LV))
			return_0;

		if (!arg_is_set(cmd, background_ARG))
			lp->need_polling = 1;

		goto out;
	}

	/*
	 * Up-convert m-way mirror to n-way mirror
	 */
	if (new_mimage_count > old_mimage_count) {
		if (lv_is_not_synced(lv)) {
			log_error("Can't add mirror to out-of-sync mirrored "
				  "LV: use lvchange --resync first.");
			return 0;
		}

		/*
		 * We allow snapshots of mirrors, but for now, we
		 * do not allow up converting mirrors that are under
		 * snapshots.  The layering logic is somewhat complex,
		 * and preliminary test show that the conversion can't
		 * seem to get the correct %'age of completion.
		 */
		if (lv_is_origin(lv)) {
			log_error("Can't add additional mirror images to "
				  "mirror %s which is under snapshots.",
				  display_lvname(lv));
			return 0;
		}

		/*
		 * Is there already a convert in progress?  We do not
		 * currently allow more than one.
		 */
		if (find_temporary_mirror(lv) || lv_is_converting(lv)) {
			log_error("%s is already being converted.  Unable to start another conversion.",
				  display_lvname(lv));
			return 0;
		}

		/*
		 * Log addition/removal should be done before the layer
		 * insertion to make the end result consistent with
		 * linear-to-mirror conversion.
		 */
		if (!_lv_update_log_type(cmd, lp, lv,
					 operable_pvs, new_log_count))
			return_0;

		/* Insert a temporary layer for syncing,
		 * only if the original lv is using disk log. */
		if (seg->log_lv && !_insert_lvconvert_layer(cmd, lv)) {
			log_error("Failed to insert resync layer.");
			return 0;
		}

		/* FIXME: can't have multiple mlogs. force corelog. */
		if (!lv_add_mirrors(cmd, lv,
				    new_mimage_count - old_mimage_count,
				    lp->stripes, lp->stripe_size,
				    region_size, 0U, operable_pvs, lp->alloc,
				    MIRROR_BY_LV)) {
			layer_lv = seg_lv(first_seg(lv), 0);
			if (!remove_layer_from_lv(lv, layer_lv) ||
			    !deactivate_lv(cmd, layer_lv) ||
			    !lv_remove(layer_lv) ||
			    !vg_write(lv->vg) || !vg_commit(lv->vg)) {
				log_error("ABORTING: Failed to remove "
					  "temporary mirror layer %s.",
					  display_lvname(layer_lv));
				log_error("Manual cleanup with vgcfgrestore "
					  "and dmsetup may be required.");
				return 0;
			}

			return_0;
		}
		if (seg->log_lv)
			lv->status |= CONVERTING;
		lp->need_polling = 1;

		goto out_skip_log_convert;
	}

	/*
	 * Down-convert (reduce # of mimages).
	 */
	if (new_mimage_count < old_mimage_count) {
		uint32_t nmc = old_mimage_count - new_mimage_count;
		uint32_t nlc = (!new_log_count || lp->mirrors == 1) ? 1U : 0U;

		/* FIXME: Why did nlc used to be calculated that way? */

		/* Reduce number of mirrors */
		if (lp->keep_mimages) {
			if (lp->track_changes) {
				log_error("--trackchanges is not available "
					  "to 'mirror' segment type.");
				return 0;
			}
			if (!lv_split_mirror_images(lv, lp->lv_split_name,
						    nmc, operable_pvs))
				return_0;
		} else if (!lv_remove_mirrors(cmd, lv, nmc, nlc,
					      is_mirror_image_removable, operable_pvs, 0))
			return_0;

		goto out; /* Just in case someone puts code between */
	}

out:
	/*
	 * Converting the log type
	 */
	if (lv_is_mirrored(lv) && (old_log_count != new_log_count)) {
		if (!_lv_update_log_type(cmd, lp, lv,
					 operable_pvs, new_log_count))
			return_0;
	}

out_skip_log_convert:

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

int mirror_remove_missing(struct cmd_context *cmd,
			  struct logical_volume *lv, int force)
{
	struct dm_list *failed_pvs;
	int log_count = _get_log_count(lv) - _failed_logs_count(lv);

	if (!(failed_pvs = failed_pv_list(lv->vg)))
		return_0;

	if (force && _failed_mirrors_count(lv) == (int)lv_mirror_count(lv)) {
		log_error("No usable images left in %s.", display_lvname(lv));
		return lv_remove_with_dependencies(cmd, lv, DONT_PROMPT, 0);
	}

	/*
	 * We must adjust the log first, or the entire mirror
	 * will get stuck during a suspend.
	 */
	if (!_lv_update_mirrored_log(lv, failed_pvs, log_count))
		return_0;

	if (_failed_mirrors_count(lv) > 0 &&
	    !lv_remove_mirrors(cmd, lv, _failed_mirrors_count(lv),
			       log_count ? 0U : 1U,
			       _is_partial_lv, NULL, 0))
		return_0;

	if (lv_is_mirrored(lv) &&
	    !_lv_update_log_type(cmd, NULL, lv, failed_pvs, log_count))
		return_0;

	if (!lv_update_and_reload(lv))
		return_0;

	return 1;
}

/*
 * _lvconvert_mirrors_repair
 *
 * This function operates in two phases.  First, all of the bad
 * devices are removed from the mirror.  Then, if desired by the
 * user, the devices are replaced.
 *
 * 'old_mimage_count' and 'old_log_count' are there so we know
 * what to convert to after the removal of devices.
 */
static int _lvconvert_mirrors_repair(struct cmd_context *cmd,
				     struct logical_volume *lv,
				     struct lvconvert_params *lp,
				     struct dm_list *pvh)
{
	int failed_logs;
	int failed_mimages;
	int replace_logs = 0;
	int replace_mimages = 0;
	uint32_t log_count;

	uint32_t original_mimages = lv_mirror_count(lv);
	uint32_t original_logs = _get_log_count(lv);

	cmd->partial_activation = 1;
	lp->need_polling = 0;

	lv_check_transient(lv); /* TODO check this in lib for all commands? */

	if (!lv_is_partial(lv)) {
		log_print_unless_silent("Volume %s is consistent. Nothing to repair.",
					display_lvname(lv));
		return 1;
	}

	failed_mimages = _failed_mirrors_count(lv);
	failed_logs = _failed_logs_count(lv);

	/* Retain existing region size in case we need it later */
	if (!lp->region_size)
		lp->region_size = first_seg(lv)->region_size;

	if (!mirror_remove_missing(cmd, lv, 0))
		return_0;

	if (failed_mimages)
		log_print_unless_silent("Mirror status: %d of %d images failed.",
					failed_mimages, original_mimages);

	/*
	 * Count the failed log devices
	 */
	if (failed_logs)
		log_print_unless_silent("Mirror log status: %d of %d images failed.",
					failed_logs, original_logs);

	/*
	 * Find out our policies
	 */
	_lvconvert_mirrors_repair_ask(cmd, failed_logs, failed_mimages,
				      &replace_logs, &replace_mimages);

	/*
	 * Second phase - replace faulty devices
	 */
	lp->mirrors = replace_mimages ? original_mimages : (original_mimages - failed_mimages);

	/*
	 * It does not make sense to replace the log if the volume is no longer
	 * a mirror.
	 */
	if (lp->mirrors == 1)
		replace_logs = 0;

	log_count = replace_logs ? original_logs : (original_logs - failed_logs);

	while (replace_mimages || replace_logs) {
		log_warn("Trying to up-convert to %d images, %d logs.", lp->mirrors, log_count);
		if (_lvconvert_mirrors_aux(cmd, lv, lp, NULL,
					   lp->mirrors, log_count, pvh))
			break;
		if (lp->mirrors > 2)
			--lp->mirrors;
		else if (log_count > 0)
			--log_count;
		else
			break; /* nowhere to go, anymore... */
	}

	if (replace_mimages && lv_mirror_count(lv) != original_mimages)
		log_warn("WARNING: Failed to replace %d of %d images in volume %s.",
			 original_mimages - lv_mirror_count(lv), original_mimages,
			 display_lvname(lv));
	if (replace_logs && _get_log_count(lv) != original_logs)
		log_warn("WARNING: Failed to replace %d of %d logs in volume %s.",
			 original_logs - _get_log_count(lv), original_logs,
			 display_lvname(lv));

	/* if (!arg_is_set(cmd, use_policies_ARG) && (lp->mirrors != old_mimage_count
						  || log_count != old_log_count))
						  return 0; */

	return 1;
}

static int _lvconvert_validate_thin(struct logical_volume *lv,
				    struct lvconvert_params *lp)
{
	if (!lv_is_thin_pool(lv) && !lv_is_thin_volume(lv))
		return 1;

	log_error("Converting thin%s segment type for %s to %s is not supported.",
		  lv_is_thin_pool(lv) ? " pool" : "",
		  display_lvname(lv), lp->segtype->name);

	if (lv_is_thin_volume(lv))
		return 0;

	/* Give advice for thin pool conversion */
	log_error("For pool data volume conversion use %s.",
		  display_lvname(seg_lv(first_seg(lv), 0)));
	log_error("For pool metadata volume conversion use %s.",
		  display_lvname(first_seg(lv)->metadata_lv));

	return 0;
}

/*
 * _lvconvert_mirrors
 *
 * Determine what is being done.  Are we doing a conversion, repair, or
 * collapsing a stack?  Once determined, call helper functions.
 */
static int _lvconvert_mirrors(struct cmd_context *cmd,
			      struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	uint32_t old_mimage_count;
	uint32_t old_log_count;
	uint32_t new_mimage_count;
	uint32_t new_log_count;

	if ((lp->corelog || lp->mirrorlog) && *lp->type_str && strcmp(lp->type_str, SEG_TYPE_NAME_MIRROR)) {
		log_error("--corelog and --mirrorlog are only compatible with mirror devices.");
		return 0;
	}

	if (!_lvconvert_validate_thin(lv, lp))
		return_0;

	if (lv_is_thin_type(lv)) {
		log_error("Mirror segment type cannot be used for thinpool%s.\n"
			  "Try \"%s\" segment type instead.",
			  lv_is_thin_pool_data(lv) ? "s" : " metadata",
			  SEG_TYPE_NAME_RAID1);
		return 0;
	}

	if (lv_is_cache_type(lv)) {
		log_error("Mirrors are not yet supported on cache LVs %s.",
			  display_lvname(lv));
		return 0;
	}

	if (_linear_type_requested(lp->type_str)) {
		if (arg_is_set(cmd, mirrors_ARG) && (arg_uint_value(cmd, mirrors_ARG, 0) != 0)) {
			log_error("Cannot specify mirrors with linear type.");
			return 0;
		}
		lp->mirrors_supplied = 1;
		lp->mirrors = 0;
	}

	/* Adjust mimage and/or log count */
	if (!_lvconvert_mirrors_parse_params(cmd, lv, lp,
					     &old_mimage_count, &old_log_count,
					     &new_mimage_count, &new_log_count))
		return_0;

	if (((old_mimage_count < new_mimage_count && old_log_count > new_log_count) ||
	     (old_mimage_count > new_mimage_count && old_log_count < new_log_count)) &&
	    lp->pv_count) {
		log_error("Cannot both allocate and free extents when "
			  "specifying physical volumes to use.");
		log_error("Please specify the operation in two steps.");
		return 0;
	}

	/* Nothing to do?  (Probably finishing collapse.) */
	if ((old_mimage_count == new_mimage_count) &&
	    (old_log_count == new_log_count))
		return 1;

	if (!_lvconvert_mirrors_aux(cmd, lv, lp, NULL,
				    new_mimage_count, new_log_count, lp->pvh))
		return_0;

	backup(lv->vg);

	if (!lp->need_polling)
		log_print_unless_silent("Logical volume %s converted.",
					display_lvname(lv));
	else
		log_print_unless_silent("Logical volume %s being converted.",
					display_lvname(lv));

	return 1;
}

static int _is_valid_raid_conversion(const struct segment_type *from_segtype,
				    const struct segment_type *to_segtype)
{
	if (from_segtype == to_segtype)
		return 1;

	/* Support raid0 <-> striped conversions */
	if (segtype_is_striped(from_segtype) && segtype_is_striped(to_segtype))
		return 1;

	if (!segtype_is_raid(from_segtype) && !segtype_is_raid(to_segtype))
		return_0;  /* Not converting to or from RAID? */

	return 1;
}

/* Check for dm-raid target supporting raid4 conversion properly. */
static int _raid4_conversion_supported(struct logical_volume *lv, struct lvconvert_params *lp)
{
	int ret = 1;
	struct lv_segment *seg = first_seg(lv);

	if (seg_is_raid4(seg))
		ret = raid4_is_supported(lv->vg->cmd, seg->segtype);
	else if (segtype_is_raid4(lp->segtype))
		ret = raid4_is_supported(lv->vg->cmd, lp->segtype);

	if (ret)
		return 1;

	log_error("Cannot convert %s LV %s to %s.",
		  lvseg_name(seg), display_lvname(lv), lp->segtype->name);
	return 0;
}

static int _lvconvert_raid(struct logical_volume *lv, struct lvconvert_params *lp)
{
	int image_count = 0;
	struct cmd_context *cmd = lv->vg->cmd;
	struct lv_segment *seg = first_seg(lv);

	if (_linear_type_requested(lp->type_str)) {
		if (arg_is_set(cmd, mirrors_ARG) && (arg_uint_value(cmd, mirrors_ARG, 0) != 0)) {
			log_error("Cannot specify mirrors with linear type.");
			return 0;
		}
		lp->mirrors_supplied = 1;
		lp->mirrors = 0;
	}

	/* Can only change image count for raid1 and linear */
	if (lp->mirrors_supplied) {
		if (_raid0_type_requested(lp->type_str)) {
			log_error("--mirrors/-m is not compatible with conversion to %s.",
				  lp->type_str);
			return 0;
		}
		if (!seg_is_mirrored(seg) && !seg_is_linear(seg)) {
			log_error("--mirrors/-m is not compatible with %s.",
				  lvseg_name(seg));
			return 0;
		}
		if (seg_is_raid10(seg)) {
			log_error("--mirrors/-m cannot be changed with %s.",
				  lvseg_name(seg));
			return 0;
		}
	}

	if (!_lvconvert_validate_thin(lv, lp))
		return_0;

	if (!_is_valid_raid_conversion(seg->segtype, lp->segtype))
		goto try_new_takeover_or_reshape;

	if (seg_is_linear(seg) && !lp->mirrors_supplied) {
		if (_raid0_type_requested(lp->type_str)) {
			log_error("Linear LV %s cannot be converted to %s.",
				  display_lvname(lv), lp->type_str);
			return 0;
		} else if (!strcmp(lp->type_str, SEG_TYPE_NAME_RAID1)) {
			log_error("Raid conversions of LV %s require -m/--mirrors.",
				  display_lvname(lv));
			return 0;
		}
		goto try_new_takeover_or_reshape;
	}

	/* Change number of RAID1 images */
	if (lp->mirrors_supplied || lp->keep_mimages) {
		image_count = lv_raid_image_count(lv);
		if (lp->mirrors_sign == SIGN_PLUS)
			image_count += lp->mirrors;
		else if (lp->mirrors_sign == SIGN_MINUS)
			image_count -= lp->mirrors;
		else
			image_count = lp->mirrors + 1;

		if (image_count < 1) {
			log_error("Unable to %s images by specified amount.",
				  lp->keep_mimages ? "split" : "reduce");
			return 0;
		}

		/* --trackchanges requires --splitmirrors which always has SIGN_MINUS */
		if (lp->track_changes && lp->mirrors != 1) {
			log_error("Exactly one image must be split off from %s when tracking changes.",
				  display_lvname(lv));
			return 0;
		}
	}

	if ((lp->corelog || lp->mirrorlog) && strcmp(lp->type_str, SEG_TYPE_NAME_MIRROR)) {
		log_error("--corelog and --mirrorlog are only compatible with mirror devices");
		return 0;
	}

	if (lp->track_changes)
		return lv_raid_split_and_track(lv, lp->pvh);

	if (lp->keep_mimages)
		return lv_raid_split(lv, lp->lv_split_name, image_count, lp->pvh);

	if (lp->mirrors_supplied) {
		if (!*lp->type_str || !strcmp(lp->type_str, SEG_TYPE_NAME_RAID1) || !strcmp(lp->type_str, SEG_TYPE_NAME_LINEAR) ||
		    (!strcmp(lp->type_str, SEG_TYPE_NAME_STRIPED) && image_count == 1)) {
			if (image_count > DEFAULT_RAID1_MAX_IMAGES) {
				log_error("Only up to %u mirrors in %s LV %s supported currently.",
					  DEFAULT_RAID1_MAX_IMAGES, lp->segtype->name, display_lvname(lv));
				return 0;
			}
			if (!lv_raid_change_image_count(lv, image_count, lp->pvh))
				return_0;

			log_print_unless_silent("Logical volume %s successfully converted.",
						display_lvname(lv));

			return 1;
		}
		goto try_new_takeover_or_reshape;
	} else if ((!*lp->type_str || seg->segtype == lp->segtype)) {
		log_error("Conversion operation not yet supported.");
		return 0;
	}

	if ((seg_is_linear(seg) || seg_is_striped(seg) || seg_is_mirrored(seg) || lv_is_raid(lv)) &&
	    (lp->type_str && lp->type_str[0])) {
		/* Activation is required later which precludes existing supported raid0 segment */
		if ((seg_is_any_raid0(seg) || segtype_is_any_raid0(lp->segtype)) &&
		    !(lp->target_attr & RAID_FEATURE_RAID0)) {
			log_error("RAID module does not support RAID0.");
			return 0;
		}

		/* Activation is required later which precludes existing supported raid4 segment */
		if (!_raid4_conversion_supported(lv, lp))
			return_0;

		/* Activation is required later which precludes existing supported raid10 segment */
		if ((seg_is_raid10(seg) || segtype_is_raid10(lp->segtype)) &&
		    !(lp->target_attr & RAID_FEATURE_RAID10)) {
			log_error("RAID module does not support RAID10.");
			return 0;
		}

		if (!arg_is_set(cmd, stripes_long_ARG))
			lp->stripes = 0;

		if (!lv_raid_convert(lv, lp->segtype, lp->yes, lp->force, lp->stripes, lp->stripe_size_supplied, lp->stripe_size,
				     lp->region_size, lp->pvh))
			return_0;

		log_print_unless_silent("Logical volume %s successfully converted.",
					display_lvname(lv));
		return 1;
	}

try_new_takeover_or_reshape:

	if (!_raid4_conversion_supported(lv, lp))
		return 0;

	/* FIXME This needs changing globally. */
	if (!arg_is_set(cmd, stripes_long_ARG))
		lp->stripes = 0;

	/* Only let raid4 through for now. */
	if (lp->type_str && lp->type_str[0] && lp->segtype != seg->segtype &&
	    ((seg_is_raid4(seg) && seg_is_striped(lp) && lp->stripes > 1) ||
	     (seg_is_striped(seg) && seg->area_count > 1 && seg_is_raid4(lp)))) {
		if (!lv_raid_convert(lv, lp->segtype, lp->yes, lp->force, lp->stripes, lp->stripe_size_supplied, lp->stripe_size,
				     lp->region_size, lp->pvh))
			return_0;

		log_print_unless_silent("Logical volume %s successfully converted.",
					display_lvname(lv));
		return 1;
	}

	log_error("Conversion operation not yet supported.");
	return 0;
}

/*
 * Change the number of images in a mirror LV.
 * lvconvert --mirrors Number LV
 */
static int _convert_mirror_number(struct cmd_context *cmd, struct logical_volume *lv,
				  struct lvconvert_params *lp)
{
	return _lvconvert_mirrors(cmd, lv, lp);
}

/*
 * Split images from a mirror LV and use them to create a new LV.
 * lvconvert --splitmirrors Number LV
 *
 * Required options:
 * --name Name
 */

static int _convert_mirror_splitmirrors(struct cmd_context *cmd, struct logical_volume *lv,
					struct lvconvert_params *lp)
{
	return _lvconvert_mirrors(cmd, lv, lp);
}

/*
 * Change the type of log used by a mirror LV.
 * lvconvert --mirrorlog Type LV
 */
static int _convert_mirror_log(struct cmd_context *cmd, struct logical_volume *lv,
				  struct lvconvert_params *lp)
{
	return _lvconvert_mirrors(cmd, lv, lp);
}

/*
 * Convert mirror LV to linear LV.
 * lvconvert --type linear LV
 *
 * Alternate syntax:
 * lvconvert --mirrors 0 LV
 */
static int _convert_mirror_linear(struct cmd_context *cmd, struct logical_volume *lv,
				  struct lvconvert_params *lp)
{
	return _lvconvert_mirrors(cmd, lv, lp);
}

/*
 * Convert mirror LV to raid1 LV.
 * lvconvert --type raid1 LV
 */
static int _convert_mirror_raid(struct cmd_context *cmd, struct logical_volume *lv,
				struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Change the number of images in a raid1 LV.
 * lvconvert --mirrors Number LV
 */
static int _convert_raid_number(struct cmd_context *cmd, struct logical_volume *lv,
				struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Split images from a raid1 LV and use them to create a new LV.
 * lvconvert --splitmirrors Number LV
 *
 * Required options:
 * --trackchanges | --name Name
 */
static int _convert_raid_splitmirrors(struct cmd_context *cmd, struct logical_volume *lv,
				      struct lvconvert_params *lp)
{
	/* FIXME: split the splitmirrors section out of _lvconvert_raid and call it here. */
	return _lvconvert_raid(lv, lp);
}

/*
 * Convert a raid* LV to use a different raid level.
 * lvconvert --type raid* LV
 */
static int _convert_raid_raid(struct cmd_context *cmd, struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Convert a raid* LV to a mirror LV.
 * lvconvert --type mirror LV
 */
static int _convert_raid_mirror(struct cmd_context *cmd, struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Convert a raid* LV to a striped LV.
 * lvconvert --type striped LV
 */
static int _convert_raid_striped(struct cmd_context *cmd, struct logical_volume *lv,
				 struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Convert a raid* LV to a linear LV.
 * lvconvert --type linear LV
 */
static int _convert_raid_linear(struct cmd_context *cmd, struct logical_volume *lv,
				struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Convert a striped/linear LV to a mirror LV.
 * lvconvert --type mirror LV
 *
 * Required options:
 * --mirrors Number
 *
 * Alternate syntax:
 * This is equivalent to above when global/mirror_segtype_default="mirror".
 * lvconvert --mirrors Number LV
 */
static int _convert_striped_mirror(struct cmd_context *cmd, struct logical_volume *lv,
				   struct lvconvert_params *lp)
{
	return _lvconvert_mirrors(cmd, lv, lp);
}

/*
 * Convert a striped/linear LV to a raid* LV.
 * lvconvert --type raid* LV
 *
 * Required options:
 * --mirrors Number
 *
 * Alternate syntax:
 * This is equivalent to above when global/mirror_segtype_default="raid1".
 * lvconvert --mirrors Number LV
 */
static int _convert_striped_raid(struct cmd_context *cmd, struct logical_volume *lv,
				 struct lvconvert_params *lp)
{
	return _lvconvert_raid(lv, lp);
}

/*
 * Functions called to perform all valid operations on a given LV type.
 *
 * _convert_<lvtype>
 */

static int _convert_mirror(struct cmd_context *cmd, struct logical_volume *lv,
			   struct lvconvert_params *lp)
{
	if (arg_is_set(cmd, mirrors_ARG))
		return _convert_mirror_number(cmd, lv, lp);

	if (arg_is_set(cmd, splitmirrors_ARG))
		return _convert_mirror_splitmirrors(cmd, lv, lp);

	if (arg_is_set(cmd, mirrorlog_ARG) || arg_is_set(cmd, corelog_ARG))
		return _convert_mirror_log(cmd, lv, lp);

	if (_linear_type_requested(lp->type_str))
		return _convert_mirror_linear(cmd, lv, lp);

	if (segtype_is_raid(lp->segtype))
		return _convert_mirror_raid(cmd, lv, lp);

	log_error("Unknown operation on mirror LV %s.", display_lvname(lv));
	return 0;
}

static int _convert_raid(struct cmd_context *cmd, struct logical_volume *lv,
			 struct lvconvert_params *lp)
{
	if (arg_is_set(cmd, mirrors_ARG))
		return _convert_raid_number(cmd, lv, lp);

	if (arg_is_set(cmd, splitmirrors_ARG))
		return _convert_raid_splitmirrors(cmd, lv, lp);

	if (segtype_is_raid(lp->segtype))
		return _convert_raid_raid(cmd, lv, lp);

	if (segtype_is_mirror(lp->segtype))
		return _convert_raid_mirror(cmd, lv, lp);

	if (!strcmp(lp->type_str, SEG_TYPE_NAME_STRIPED))
		return _convert_raid_striped(cmd, lv, lp);

	if (_linear_type_requested(lp->type_str))
		return _convert_raid_linear(cmd, lv, lp);

	log_error("Unknown operation on raid LV %s.", display_lvname(lv));
	return 0;
}

static int _convert_striped(struct cmd_context *cmd, struct logical_volume *lv,
			    struct lvconvert_params *lp)
{
	const char *mirrors_type = find_config_tree_str(cmd, global_mirror_segtype_default_CFG, NULL);

	if (!strcmp(lp->type_str, SEG_TYPE_NAME_MIRROR))
		return _convert_striped_mirror(cmd, lv, lp);

	if (segtype_is_raid(lp->segtype))
		return _convert_striped_raid(cmd, lv, lp);

	/* --mirrors can mean --type mirror or --type raid1 depending on config setting. */

	if (arg_is_set(cmd, mirrors_ARG) && mirrors_type && !strcmp(mirrors_type, SEG_TYPE_NAME_MIRROR))
		return _convert_striped_mirror(cmd, lv, lp);

	if (arg_is_set(cmd, mirrors_ARG) && mirrors_type && !strcmp(mirrors_type, SEG_TYPE_NAME_RAID1))
		return _convert_striped_raid(cmd, lv, lp);

	log_error("Unknown operation on striped or linear LV %s.", display_lvname(lv));
	return 0;
}

/*
 * Main entry point.
 * lvconvert performs a specific <operation> on a specific <lv_type>.
 *
 * The <operation> is specified by command line args.
 * The <lv_type> is found using lv_is_foo(lv) functions.
 *
 * for each lvtype,
 *     _convert_lvtype();
 *	 for each arg_is_set(operation)
 *	     _convert_lvtype_operation();
 *
 * FIXME: this code (identifying/routing each unique operation through
 * _convert_lvtype_op) was designed to work based on the new type that
 * the user entered after --type, not the final segment type in lp->type_str.
 * Sometimes the two differ because tricks are played with lp->type_str.
 * So, when the use of arg_type_str(type_ARG) here was replaced with
 * lp->type_str, some commands are no longer identified/routed correctly.
 */
static int _lvconvert(struct cmd_context *cmd, struct logical_volume *lv,
		      struct lvconvert_params *lp)
{
	struct lv_segment *seg = first_seg(lv);
	int ret = 0;

	/* Set up segtype either from type_str or else to match the existing one. */
	if (!*lp->type_str)
		lp->segtype = seg->segtype;
	else if (!(lp->segtype = get_segtype_from_string(cmd, lp->type_str)))
		goto_out;

	if (!strcmp(lp->type_str, SEG_TYPE_NAME_MIRROR)) {
		if (!lp->mirrors_supplied && !seg_is_raid1(seg)) {
			log_error("Conversions to --type mirror require -m/--mirrors");
			goto out;
		}
	}

	/* lv->segtype can't be NULL */
	if (activation() && lp->segtype->ops->target_present &&
	    !lp->segtype->ops->target_present(cmd, NULL, &lp->target_attr)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel.", lp->segtype->name);
		goto out;
	}

	/* Process striping parameters */
	/* FIXME This is incomplete */
	if (_mirror_or_raid_type_requested(cmd, lp->type_str) || _raid0_type_requested(lp->type_str) ||
	    _striped_type_requested(lp->type_str) || lp->mirrorlog || lp->corelog) {
		/* FIXME Handle +/- adjustments too? */
		if (!get_stripe_params(cmd, lp->segtype, &lp->stripes, &lp->stripe_size, &lp->stripes_supplied, &lp->stripe_size_supplied))
			goto_out;

		if (_raid0_type_requested(lp->type_str) || _striped_type_requested(lp->type_str))
			/* FIXME Shouldn't need to override get_stripe_params which defaults to 1 stripe (i.e. linear)! */
			/* The default keeps existing number of stripes, handled inside the library code */
			if (!arg_is_set(cmd, stripes_long_ARG))
				lp->stripes = 0;
	}

	/* any operations on a cache LV are directed to the cache origin LV. */
	if (lv_is_cache(lv))
		lv = seg_lv(first_seg(lv), 0);

	/*
	 * Each LV type that can be converted.
	 * (The existing type of the LV, not a requested type.)
	 */
	if (lv_is_mirror(lv)) {
		ret = _convert_mirror(cmd, lv, lp);
		goto out;
	}

	if (lv_is_raid(lv)) {
		ret = _convert_raid(cmd, lv, lp);
		goto out;
	}

	/*
	 * FIXME: add lv_is_striped() and lv_is_linear()?
	 * This does not include raid0 which is caught by the test above.
	 * If operations differ between striped and linear, split this case.
	 */
	if (segtype_is_striped(seg->segtype) || segtype_is_linear(seg->segtype)) {
		ret = _convert_striped(cmd, lv, lp);
		goto out;
	}

	/*
	 * The intention is to explicitly check all cases above and never
	 * reach here, but this covers anything that was missed.
	 */
	log_error("Cannot convert LV %s.", display_lvname(lv));

out:
	return ret ? ECMD_PROCESSED : ECMD_FAILED;
}

/*
 * Change LV between raid/mirror/linear/striped
 */

static int _lvconvert_raid_types_single(struct cmd_context *cmd, struct logical_volume *lv,
			     struct processing_handle *handle)
{
	struct lvconvert_params *lp = (struct lvconvert_params *) handle->custom_handle;
	struct dm_list *use_pvh;
	struct convert_poll_id_list *idl;
	struct lvinfo info;
	int ret;

	/*
	 * lp->pvh holds the list of PVs available for allocation or removal
	 */

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
		lp->pv_count = cmd->position_argc - 1;
	} else
		use_pvh = &lv->vg->pvs;

	lp->pvh = use_pvh;

	lp->lv_to_poll = lv;

	ret = _lvconvert(cmd, lv, lp);

	if (ret != ECMD_PROCESSED)
		return_ECMD_FAILED;

	if (lp->need_polling) {
		/* _lvconvert() call may alter the reference in lp->lv_to_poll */
		if (!lv_info(cmd, lp->lv_to_poll, 0, &info, 0, 0) || !info.exists)
			log_print_unless_silent("Conversion starts after activation.");
		else {
			if (!(idl = convert_poll_id_list_create(cmd, lp->lv_to_poll)))
				return_ECMD_FAILED;
			dm_list_add(&lp->idls, &idl->list);
		}
	}

	return ECMD_PROCESSED;
}

static int _lvconvert_raid_types_check(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle,
			int lv_is_named_arg)
{
	struct lv_types *lvtype;
	int lvt_enum;

	if (!lv_is_visible(lv)) {
		if (!lv_is_cache_pool_metadata(lv) &&
		    !lv_is_cache_pool_data(lv) &&
		    !lv_is_thin_pool_metadata(lv) &&
		    !lv_is_thin_pool_data(lv) &&
		    !lv_is_used_cache_pool(lv) &&
		    !lv_is_mirrored(lv) &&
		    !lv_is_raid(lv))
			goto fail_hidden;
	}

	lvt_enum = get_lvt_enum(lv);
	if (lvt_enum)
		lvtype = get_lv_type(lvt_enum);

	/*
	 * FIXME: this validation could be done by command defs.
	 *
	 * Outside the standard linear/striped/mirror/raid LV
	 * types, cache is the only special LV type that is handled
	 * (the command is redirected to origin).
	 */
	switch (lvt_enum) {
	case thin_LVT:
	case thinpool_LVT:
	case cachepool_LVT:
	case snapshot_LVT:
		log_error("Operation not permitted (%s %d) on LV %s type %s.",
			  cmd->command->command_line_id, cmd->command->command_line_enum,
			  display_lvname(lv), lvtype ? lvtype->name : "unknown");
		return 0;
	}

	return 1;

 fail_hidden:
	log_error("Operation not permitted (%s %d) on hidden LV %s.",
		  cmd->command->command_line_id, cmd->command->command_line_enum,
		  display_lvname(lv));
	return 0;
}

int lvconvert_raid_types_cmd(struct cmd_context * cmd, int argc, char **argv)
{
	int poll_ret, ret;
	int saved_ignore_suspended_devices;
	struct processing_handle *handle;
	struct convert_poll_id_list *idl;
	struct lvconvert_params lp = {
		.conv_type = CONV_OTHER,
		.target_attr = ~0,
		.idls = DM_LIST_HEAD_INIT(lp.idls),
	};

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lp;

	if (!_read_params(cmd, &lp)) {
		ret = EINVALID_CMD_LINE;
		goto_out;
	}

	saved_ignore_suspended_devices = ignore_suspended_devices();

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, &_lvconvert_raid_types_check, &_lvconvert_raid_types_single);

	init_ignore_suspended_devices(saved_ignore_suspended_devices);

	dm_list_iterate_items(idl, &lp.idls) {
		poll_ret = lvconvert_poll_by_id(cmd, idl->id,
						 lp.wait_completion ? 0 : 1U,
						 idl->is_merging_origin,
						 idl->is_merging_origin_thin);
		if (poll_ret > ret)
			ret = poll_ret;
	}

out:
	destroy_processing_handle(cmd, handle);

	return ret;
}

/*
 * change mirror log
 */

static int _lvconvert_visible_check(struct cmd_context *cmd, struct logical_volume *lv,
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

static int _lvconvert_change_mirrorlog_single(struct cmd_context *cmd, struct logical_volume *lv,
			     struct processing_handle *handle)
{
	struct lvconvert_params *lp = (struct lvconvert_params *) handle->custom_handle;
	struct dm_list *use_pvh;

	/*
	 * lp->pvh holds the list of PVs available for allocation or removal
	 */

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
		lp->pv_count = cmd->position_argc - 1;
	} else
		use_pvh = &lv->vg->pvs;

	lp->pvh = use_pvh;

 	/* FIXME: extract the mirrorlog functionality out of _lvconvert()? */
	return _lvconvert(cmd, lv, lp);
}

int lvconvert_change_mirrorlog_cmd(struct cmd_context * cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvconvert_params lp = {
		.conv_type = CONV_OTHER,
		.target_attr = ~0,
		.idls = DM_LIST_HEAD_INIT(lp.idls),
	};
	int ret;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lp;

	/* FIXME: extract the relevant bits of read_params and put here. */
	if (!_read_params(cmd, &lp)) {
		ret = EINVALID_CMD_LINE;
		goto_out;
	}

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, &_lvconvert_visible_check, &_lvconvert_change_mirrorlog_single);

out:
	destroy_processing_handle(cmd, handle);

	return ret;
}

/*
 * split mirror images
 */

static int _lvconvert_split_mirror_images_single(struct cmd_context *cmd, struct logical_volume *lv,
			     struct processing_handle *handle)
{
	struct lvconvert_params *lp = (struct lvconvert_params *) handle->custom_handle;
	struct dm_list *use_pvh;

	/*
	 * lp->pvh holds the list of PVs available for allocation or removal
	 */

	if (cmd->position_argc > 1) {
		/* First pos arg is required LV, remaining are optional PVs. */
		if (!(use_pvh = create_pv_list(cmd->mem, lv->vg, cmd->position_argc - 1, cmd->position_argv + 1, 0)))
			return_ECMD_FAILED;
		lp->pv_count = cmd->position_argc - 1;
	} else
		use_pvh = &lv->vg->pvs;

	lp->pvh = use_pvh;

 	/* FIXME: extract the split functionality out of _lvconvert()? */
	return _lvconvert(cmd, lv, lp);
}

int lvconvert_split_mirror_images_cmd(struct cmd_context * cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct lvconvert_params lp = {
		.conv_type = CONV_OTHER,
		.target_attr = ~0,
		.idls = DM_LIST_HEAD_INIT(lp.idls),
	};
	int ret;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lp;

	/* FIXME: extract the relevant bits of read_params and put here. */
	if (!_read_params(cmd, &lp)) {
		ret = EINVALID_CMD_LINE;
		goto_out;
	}

	/* FIXME: are there any hidden LVs that should be disallowed? */

	ret = process_each_lv(cmd, 1, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			      handle, NULL, &_lvconvert_split_mirror_images_single);

out:
	destroy_processing_handle(cmd, handle);

	return ret;
}

/*
 * merge mirror images
 *
 * Called from both lvconvert --mergemirrors and lvconvert --merge.
 */

int lvconvert_merge_mirror_images_single(struct cmd_context *cmd,
                                          struct logical_volume *lv,
                                          struct processing_handle *handle)
{
	if (!lv_raid_merge(lv))
		return ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvconvert_merge_mirror_images_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	/* arg can be a VG name, which is the standard option usage */
	cmd->command->flags &= ~GET_VGNAME_FROM_OPTIONS;

	return process_each_lv(cmd, cmd->position_argc, cmd->position_argv, NULL, NULL, READ_FOR_UPDATE,
			       NULL, &_lvconvert_visible_check, &lvconvert_merge_mirror_images_single);
}

/*
 * repair/replace code, is called from lvconvert --repair/--replace
 * utilities in lvconvert_other.c
 */

int lvconvert_repair_pvs_mirror(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle,
			struct dm_list *use_pvh)
{
	struct lvconvert_result *lr = (struct lvconvert_result *) handle->custom_handle;
	struct lvconvert_params lp = { 0 };
	struct convert_poll_id_list *idl;
	struct lvinfo info;
	int ret;

	/*
	 * FIXME: temporary use of lp because _lvconvert_mirrors_repair()
	 * and _aux() still use lp fields everywhere.
	 * Migrate them away from using lp (for the most part just use
	 * local variables, and check arg_values directly).
	 */

	/*
	 * Fill in any lp fields here that this fn expects to be set before
	 * it's called.  It's hard to tell what the old code expects in lp
	 * for repair; it doesn't take the stripes option, but it seems to
	 * expect lp.stripes to be set to 1.
	 */
	lp.alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, ALLOC_INHERIT);
	lp.stripes = 1;

	ret = _lvconvert_mirrors_repair(cmd, lv, &lp, use_pvh);

	if (lp.need_polling) {
		if (!lv_info(cmd, lv, 0, &info, 0, 0) || !info.exists)
			log_print_unless_silent("Conversion starts after activation.");
		else {
			if (!(idl = convert_poll_id_list_create(cmd, lv)))
				return 0;
			dm_list_add(&lr->poll_idls, &idl->list);
		}
		lr->need_polling = 1;
	}

	return ret;
}

static void _lvconvert_repair_pvs_raid_ask(struct cmd_context *cmd, int *do_it)
{
	const char *dev_policy;

	*do_it = 1;

	if (arg_is_set(cmd, usepolicies_ARG)) {
		dev_policy = find_config_tree_str(cmd, activation_raid_fault_policy_CFG, NULL);

		if (!strcmp(dev_policy, "allocate") ||
		    !strcmp(dev_policy, "replace"))
			return;

		/* else if (!strcmp(dev_policy, "anything_else")) -- no replace */
		*do_it = 0;
		return;
	}

	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt("Attempt to replace failed RAID images "
			  "(requires full device resync)? [y/n]: ") == 'n') {
		*do_it = 0;
	}
}

int lvconvert_repair_pvs_raid(struct cmd_context *cmd, struct logical_volume *lv,
			struct processing_handle *handle,
			struct dm_list *use_pvh)
{
	struct dm_list *failed_pvs;
	int do_it;

	if (!lv_is_active_exclusive_locally(lv_lock_holder(lv))) {
		log_error("%s must be active %sto perform this operation.",
			  display_lvname(lv),
			  vg_is_clustered(lv->vg) ?
			  "exclusive locally " : "");
		return 0;
	}

	_lvconvert_repair_pvs_raid_ask(cmd, &do_it);

	if (do_it) {
		if (!(failed_pvs = failed_pv_list(lv->vg)))
			return_0;

		if (!lv_raid_replace(lv, arg_count(cmd, force_ARG), failed_pvs, use_pvh)) {
			log_error("Failed to replace faulty devices in %s.",
				  display_lvname(lv));
			return 0;
		}

		log_print_unless_silent("Faulty devices in %s successfully replaced.",
					display_lvname(lv));
		return 1;
	}

	/* "warn" if policy not set to replace */
	if (arg_is_set(cmd, usepolicies_ARG))
		log_warn("Use 'lvconvert --repair %s' to replace "
			 "failed device.", display_lvname(lv));
	return 1;
}

/*
 * All lvconvert command defs have their own function,
 * so the generic function name is unused.
 */

int lvconvert(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %s.",
		  cmd->command->command_line_id);
	return ECMD_FAILED;
}

