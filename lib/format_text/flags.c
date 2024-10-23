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

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "import-export.h"
#include "lib/misc/lvm-string.h"
#include "lib/metadata/segtype.h"

/*
 * Bitsets held in the 'status' flags get
 * converted into arrays of strings.
 */
struct flag {
	const char description[32];
	const uint64_t mask;
	int kind;
};

static const struct flag _vg_flags[] = {
	/* Alphabetically sorted by description! */
	{"CLUSTERED", CLUSTERED, STATUS_FLAG},
	{"EXPORTED", EXPORTED_VG, STATUS_FLAG},
	{"NOAUTOACTIVATE", NOAUTOACTIVATE, COMPATIBLE_FLAG},
	{"PVMOVE", PVMOVE, STATUS_FLAG},
	{"READ", LVM_READ, STATUS_FLAG},
	{"RESIZEABLE", RESIZEABLE_VG, STATUS_FLAG},
	{"SHARED", SHARED, STATUS_FLAG},
	{"WRITE", LVM_WRITE, STATUS_FLAG},
	{"WRITE_LOCKED", LVM_WRITE_LOCKED, COMPATIBLE_FLAG},
	{"", (PARTIAL_VG |
	      PRECOMMITTED |
	      ARCHIVED_VG), 0},
};

static const struct flag _pv_flags[] = {
	/* Alphabetically sorted by description! */
	{"ALLOCATABLE", ALLOCATABLE_PV, STATUS_FLAG},
	{"EXPORTED", EXPORTED_VG, STATUS_FLAG},
	{"MISSING", MISSING_PV, COMPATIBLE_FLAG | STATUS_FLAG}, /* 1st. */
	{"", (PV_MOVED_VG |
	      UNLABELLED_PV), 0},
};

static const struct flag _lv_flags[] = {
	/* Alphabetically sorted by description! */
	{"ACTIVATION_SKIP", LV_ACTIVATION_SKIP, COMPATIBLE_FLAG},
	{"CACHE_USES_CACHEVOL", LV_CACHE_USES_CACHEVOL, SEGTYPE_FLAG},
	{"CACHE_VOL", LV_CACHE_VOL, COMPATIBLE_FLAG},
	{"CROP_METADATA", LV_CROP_METADATA, SEGTYPE_FLAG},
	{"ERROR_WHEN_FULL", LV_ERROR_WHEN_FULL, COMPATIBLE_FLAG},
	{"FIXED_MINOR", FIXED_MINOR, STATUS_FLAG},
	{"LOCKED", LOCKED, STATUS_FLAG},
	{"METADATA_FORMAT", LV_METADATA_FORMAT, SEGTYPE_FLAG},
	{"NOAUTOACTIVATE", LV_NOAUTOACTIVATE, COMPATIBLE_FLAG},
	{"NOTSYNCED", LV_NOTSYNCED, STATUS_FLAG},
	{"PVMOVE", PVMOVE, STATUS_FLAG},
	{"READ", LVM_READ, STATUS_FLAG},
	{"REBUILD", LV_REBUILD, STATUS_FLAG},
	{"REMOVE_AFTER_RESHAPE", LV_REMOVE_AFTER_RESHAPE, SEGTYPE_FLAG},
	{"RESHAPE", LV_RESHAPE, SEGTYPE_FLAG},
	{"RESHAPE_DATA_OFFSET", LV_RESHAPE_DATA_OFFSET, SEGTYPE_FLAG},
	{"RESHAPE_DELTA_DISKS_MINUS", LV_RESHAPE_DELTA_DISKS_MINUS, SEGTYPE_FLAG},
	{"RESHAPE_DELTA_DISKS_PLUS", LV_RESHAPE_DELTA_DISKS_PLUS, SEGTYPE_FLAG},
	{"VISIBLE", VISIBLE_LV, STATUS_FLAG},
	{"WRITE", LVM_WRITE, STATUS_FLAG},
	{"WRITEMOSTLY", LV_WRITEMOSTLY, STATUS_FLAG},
	{"WRITE_LOCKED", LVM_WRITE_LOCKED, COMPATIBLE_FLAG},
	{"", (LV_NOSCAN |
	      LV_TEMPORARY |
	      POOL_METADATA_SPARE |
	      LOCKD_SANLOCK_LV |
	      RAID |
	      RAID_META |
	      RAID_IMAGE |
	      MIRROR |
	      MIRROR_IMAGE |
	      MIRROR_LOG |
	      MIRRORED |
	      VIRTUAL |
	      SNAPSHOT |
	      MERGING |
	      CONVERTING |
	      PARTIAL_LV |
	      POSTORDER_FLAG |
	      VIRTUAL_ORIGIN |
	      THIN_VOLUME |
	      THIN_POOL |
	      THIN_POOL_DATA |
	      THIN_POOL_METADATA |
	      CACHE |
	      CACHE_POOL |
	      CACHE_POOL_DATA |
	      CACHE_POOL_METADATA |
	      LV_VDO |
	      LV_VDO_POOL |
	      LV_VDO_POOL_DATA |
	      WRITECACHE |
	      INTEGRITY |
	      INTEGRITY_METADATA |
	      LV_PENDING_DELETE |  /* FIXME Display like COMPATIBLE_FLAG */
	      LV_REMOVED), 0},
};

static const struct flag *_get_flags(enum pv_vg_lv_e type, size_t *flags_count)
{
	switch (type) {
	case VG_FLAGS:
                *flags_count = DM_ARRAY_SIZE(_vg_flags) - 1;
		return _vg_flags;

	case PV_FLAGS:
                *flags_count = DM_ARRAY_SIZE(_pv_flags) - 1;
		return _pv_flags;

	case LV_FLAGS:
                *flags_count = DM_ARRAY_SIZE(_lv_flags) - 1;
		return _lv_flags;
	}

	log_error(INTERNAL_ERROR "Unknown flag set requested.");
	return NULL;
}

/*
 * Converts a bitset to an array of string values,
 * using one of the tables defined at the top of
 * the file.
 */
int print_flags(char *buffer, size_t size, enum pv_vg_lv_e type, int mask, uint64_t status)
{
	int f, first = 1;
	const struct flag *flags;
	size_t flags_count;

	if (!(flags = _get_flags(type, &flags_count)))
		return_0;

	if (size)
		buffer[0] = 0;

	for (f = 0; status && f <= flags_count; f++) {
		if (status & flags[f].mask) {
			status &= ~flags[f].mask;

			if (!(mask & flags[f].kind))
				continue;

			/* Internal-only flag? */
			if (!flags[f].kind)
				continue;

			if (!emit_to_buffer(&buffer, &size, "%s\"%s\"",
					    (!first) ? ", " : "",
					    flags[f].description))
				return_0;
			first = 0;
		}
	}

	if (status)
		log_warn(INTERNAL_ERROR "Metadata inconsistency: "
			 "Not all flags successfully exported.");

	return 1;
}

static int _compare_flags_s(const void *a, const void *b)
{
	return strcmp(((const struct flag*)a)->description,
		      ((const struct flag*)b)->description);
}

int read_flags(uint64_t *status, enum pv_vg_lv_e type, int mask, const struct dm_config_value *cv)
{
	unsigned f;
	uint64_t s = UINT64_C(0);
	const struct flag *flags;
	size_t flags_count;

	if (!(flags = _get_flags(type, &flags_count)))
		return_0;

	if (cv->type == DM_CFG_EMPTY_ARRAY)
		goto out;

	do {
		if (cv->type != DM_CFG_STRING) {
			log_error("Status value is not a string.");
			return 0;
		}
#if 0
		/*
		 * For a short time CACHE_VOL was a STATUS_FLAG, then it
		 * was changed to COMPATIBLE_FLAG, so we want to read it
		 * from either place.
		 */
		if (type == LV_FLAGS && !strcmp(cv->v.str, "CACHE_VOL"))
			mask = (STATUS_FLAG | COMPATIBLE_FLAG);
		for (f = 0; f <= flags_count; f++) {
			if ((flags[f].kind & mask) &&
			    !strcmp(flags[f].description, cv->v.str)) {
				s |= flags[f].mask;
				break;
			}
		}
		if (type == VG_FLAGS && !strcmp(cv->v.str, "PARTIAL")) {
			/*
			 * Exception: We no longer write this flag out, but it
			 * might be encountered in old backup files, so restore
			 * it in that case. It is never part of live metadata
			 * though, so only vgcfgrestore needs to be concerned
			 * by this case.
			 */
			s |= PARTIAL_VG;
		} else if (!flags[f].kind && (mask & STATUS_FLAG)) {
			log_error("Unknown status flag '%s'.", cv->v.str);
			return 0;
		}
#else
		struct flag *found;

		/* v.str is a string and 'struct flag' also starts with string */
		if ((found = bsearch((struct flag*)cv->v.str, flags, flags_count,
				     sizeof(struct flag), _compare_flags_s))) {
			if ((type == LV_FLAGS) && (found->mask & LV_CACHE_VOL))
				/*
				 * For a short time CACHE_VOL was a STATUS_FLAG, then it
				 * was changed to COMPATIBLE_FLAG, so we want to read it
				 * from either place.
				 */
				mask = (STATUS_FLAG | COMPATIBLE_FLAG);
			if (found->kind & mask)
				s |= found->mask;
		} else {
			if ((type == VG_FLAGS) && !strcmp(cv->v.str, "PARTIAL")) {
			/*
			 * Exception: We no longer write this flag out, but it
			 * might be encountered in old backup files, so restore
			 * it in that case. It is never part of live metadata
			 * though, so only vgcfgrestore needs to be concerned
			 * by this case.
			 */
				s |= PARTIAL_VG;
			} else if (mask & STATUS_FLAG) {
				log_error("Unknown status flag '%s'.", cv->v.str);
				return 0;
			}
		}
#endif
	} while ((cv = cv->next));

      out:
	*status |= s;
	return 1;
}

/*
 * Parse extra status flags from segment "type" string.
 * These flags are seen as INCOMPATIBLE by any older lvm2 code.
 * Returns 0 and prints warning for an UNKNOWN flag.
 *
 * Note: using these segtype status flags instead of actual
 * status flags ensures wanted incompatibility.
 */
int read_lvflags(uint64_t *status, const char *flags_str)
{
	const struct flag *flags = _lv_flags;
	const char *delim;
	size_t len;
	unsigned i;

	do {
		if ((delim = strchr(flags_str, '+')))
			len = delim - flags_str;
		else
			len = strlen(flags_str);

		for (i = 0; flags[i].kind; ++i)
			if ((flags[i].kind & SEGTYPE_FLAG) &&
			    !strncmp(flags_str, flags[i].description, len) &&
			    !flags[i].description[len]) {
				*status |= flags[i].mask;
				break; /* Found matching flag */
			}

		if (!flags[i].kind) {
			log_warn("WARNING: Unrecognised flag(s) %s.", flags_str);
			return 0;  /* Unknown flag is incompatible */
		}

		flags_str = delim + 1;
	}  while (delim);	/* Till no more flags in type appear */

	return 1;
}

int print_segtype_lvflags(char *buffer, size_t size, uint64_t status)
{
	unsigned i;
	const struct flag *flags = _lv_flags;

	buffer[0] = 0;
	for (i = 0; status && flags[i].kind; i++) {
		if ((flags[i].kind & SEGTYPE_FLAG) &&
		    (status & flags[i].mask) &&
		    !emit_to_buffer(&buffer, &size, "+%s",
				    flags[i].description))
			return 0;
		status &= ~flags[i].mask;
	}

	return 1;
}
