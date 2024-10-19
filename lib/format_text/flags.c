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
	{ "CLUSTERED", CLUSTERED, STATUS_FLAG },
	{ "EXPORTED", EXPORTED_VG, STATUS_FLAG },
	{ "NOAUTOACTIVATE", NOAUTOACTIVATE, COMPATIBLE_FLAG },
	{ "PVMOVE", PVMOVE, STATUS_FLAG },
	{ "READ", LVM_READ, STATUS_FLAG },
	{ "RESIZEABLE", RESIZEABLE_VG, STATUS_FLAG },
	{ "SHARED", SHARED, STATUS_FLAG },
	{ "WRITE", LVM_WRITE, STATUS_FLAG },
	{ "WRITE_LOCKED", LVM_WRITE_LOCKED, COMPATIBLE_FLAG },
	{ "", (PARTIAL_VG |
	      PRECOMMITTED |
	      ARCHIVED_VG), 0 },
};

static const struct flag _pv_flags[] = {
	/* Alphabetically sorted by description! */
	{ "ALLOCATABLE", ALLOCATABLE_PV, STATUS_FLAG },
	{ "EXPORTED", EXPORTED_VG, STATUS_FLAG },
	{ "MISSING", MISSING_PV, COMPATIBLE_FLAG | STATUS_FLAG }, /* 1st. */
	{ "", (PV_MOVED_VG |
	      UNLABELLED_PV), 0 },
};

static const struct flag _lv_flags[] = {
	/* Alphabetically sorted by description! */
	{ "ACTIVATION_SKIP", LV_ACTIVATION_SKIP, COMPATIBLE_FLAG },
	{ "CACHE_USES_CACHEVOL", LV_CACHE_USES_CACHEVOL, SEGTYPE_FLAG },
	{ "CACHE_VOL", LV_CACHE_VOL, COMPATIBLE_FLAG },
	{ "CROP_METADATA", LV_CROP_METADATA, SEGTYPE_FLAG },
	{ "ERROR_WHEN_FULL", LV_ERROR_WHEN_FULL, COMPATIBLE_FLAG },
	{ "FIXED_MINOR", FIXED_MINOR, STATUS_FLAG },
	{ "LOCKED", LOCKED, STATUS_FLAG },
	{ "METADATA_FORMAT", LV_METADATA_FORMAT, SEGTYPE_FLAG },
	{ "NOAUTOACTIVATE", LV_NOAUTOACTIVATE, COMPATIBLE_FLAG },
	{ "NOTSYNCED", LV_NOTSYNCED, STATUS_FLAG },
	{ "PVMOVE", PVMOVE, STATUS_FLAG },
	{ "READ", LVM_READ, STATUS_FLAG },
	{ "REBUILD", LV_REBUILD, STATUS_FLAG },
	{ "REMOVE_AFTER_RESHAPE", LV_REMOVE_AFTER_RESHAPE, SEGTYPE_FLAG },
	{ "RESHAPE", LV_RESHAPE, SEGTYPE_FLAG },
	{ "RESHAPE_DATA_OFFSET", LV_RESHAPE_DATA_OFFSET, SEGTYPE_FLAG },
	{ "RESHAPE_DELTA_DISKS_MINUS", LV_RESHAPE_DELTA_DISKS_MINUS, SEGTYPE_FLAG },
	{ "RESHAPE_DELTA_DISKS_PLUS", LV_RESHAPE_DELTA_DISKS_PLUS, SEGTYPE_FLAG },
	{ "VISIBLE", VISIBLE_LV, STATUS_FLAG },
	{ "WRITE", LVM_WRITE, STATUS_FLAG },
	{ "WRITEMOSTLY", LV_WRITEMOSTLY, STATUS_FLAG },
	{ "WRITE_LOCKED", LVM_WRITE_LOCKED, COMPATIBLE_FLAG },
	{ "", (LV_NOSCAN |
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
	      LV_REMOVED), 0 },
};

static const struct flag *_get_flags(enum pv_vg_lv_e type, size_t *flags_count)
{
	switch (type) {
	case VG_FLAGS:
		*flags_count = DM_ARRAY_SIZE(_vg_flags);
		return _vg_flags;

	case PV_FLAGS:
		*flags_count = DM_ARRAY_SIZE(_pv_flags);
		return _pv_flags;

	case LV_FLAGS:
		*flags_count = DM_ARRAY_SIZE(_lv_flags);
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
	int first = 1;
	const struct flag *flags;
	size_t flags_count;

	if (!(flags = _get_flags(type, &flags_count)))
		return_0;

	if (!size)
		return 0;

	buffer[0] = 0;

	for (; status; ++flags) {
		if (status & flags->mask) {
			status &= ~flags->mask;

			if (!(mask & flags->kind)) {
				if (!flags->kind)
					break; /* Internal-only flag? */
				continue;
			}

			if (!emit_to_buffer(&buffer, &size, "%s\"%s\"",
					    (!first) ? ", " : "",
					    flags->description))
				return_0;
			first = 0;
		} else if (!flags->kind)
			break;
	}

	if (status)
		log_warn(INTERNAL_ERROR "Metadata inconsistency: "
			 "Not all flags successfully exported.");

	return 1;
}

int read_flags(uint64_t *status, enum pv_vg_lv_e type, int mask, const struct dm_config_value *cv)
{
	uint64_t s = UINT64_C(0);
	const struct flag *flags;
	struct flag *found;
	size_t flags_count;
	typedef int (*compare_fn_t) (const void *, const void *);

	if (!(flags = _get_flags(type, &flags_count)))
		return_0;

	if (cv->type == DM_CFG_EMPTY_ARRAY)
		goto out;

	do {
		if (cv->type != DM_CFG_STRING) {
			log_error("Status value is not a string.");
			return 0;
		}

		/*
		 * v.str is a string as well as 'struct flag' starts with string.
		 * So compare directly 'strcmp()'.
		 * Since the last flags entry is always empty "", reduce count by 1
		 */
		if ((found = bsearch((struct flag*)cv->v.str, flags, flags_count - 1,
				     sizeof(struct flag), (compare_fn_t) strcmp))) {
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
	const struct flag *flags;
	const char *delim;
	size_t len;

	do {
		if ((delim = strchr(flags_str, '+')))
			len = delim - flags_str;
		else
			len = strlen(flags_str);

		/* Scan all lv_flags
		 * Not using bsearch() ATM, as the string may end with '+'
		 * and these segtypes are rare in metadata set */
		for (flags = _lv_flags; flags->kind; ++flags)
			if ((flags->kind & SEGTYPE_FLAG) &&
			    !strncmp(flags_str, flags->description, len) &&
			    !flags->description[len]) {
				*status |= flags->mask;
				break; /* Found matching flag */
			}

		if (!flags->kind) {
			log_warn("WARNING: Unrecognised flag(s) %s.", flags_str);
			return 0;  /* Unknown flag is incompatible */
		}

		flags_str = delim + 1;
	}  while (delim);	/* Till no more flags in type appear */

	return 1;
}

int print_segtype_lvflags(char *buffer, size_t size, uint64_t status)
{
	const struct flag *flags;

	if (!size)
		return 0;

	buffer[0] = 0;

	for (flags = _lv_flags; status && flags->kind; ++flags) {
		if ((flags->kind & SEGTYPE_FLAG) &&
		    (status & flags->mask) &&
		    !emit_to_buffer(&buffer, &size, "+%s",
				    flags->description))
			return 0;
		status &= ~flags->mask;
	}

	return 1;
}
