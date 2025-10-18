/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
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

#ifndef _LIBALLOC_TYPES_H
#define _LIBALLOC_TYPES_H

#include "device_mapper/all.h"
#include <stdint.h>

/*
 * liballoc - Standalone Allocation Library Types
 *
 * These types are the canonical definitions used by both:
 * - liballoc (the allocation algorithm library)
 * - lib/metadata (LVM2's metadata layer)
 *
 * The library knows nothing about PV/VG/LV structures.
 * LVM2 is responsible for populating these structures from its metadata.
 */

/*
 * Allocation policies
 * Defines how space should be allocated from available areas.
 */
typedef enum {
	ALLOC_INVALID = 0,
	ALLOC_CONTIGUOUS = 1,    /* Must be contiguous on same source */
	ALLOC_CLING = 2,         /* Prefer same sources as existing allocation */
	ALLOC_CLING_BY_TAGS = 3, /* Cling to sources with matching tags */
	ALLOC_NORMAL = 4,        /* Normal allocation */
	ALLOC_ANYWHERE = 5,      /* No restrictions */
	ALLOC_INHERIT = 6        /* Inherit from parent (LVM2 only) */
} alloc_policy_t;

/*
 * Single contiguous free area within a source
 *
 * Represents a range of free extents that can be allocated.
 * Multiple areas can exist on one source (e.g., fragmented free space).
 */
struct alloc_area {
	uint64_t start;          /* Starting extent number (within source) */
	uint64_t count;          /* Number of contiguous free extents */
	uint64_t unreserved;     /* Extents not yet reserved (for multi-pass alloc) */

	void *source_handle;     /* Back-pointer to source (opaque to library) */
	struct alloc_source *map; /* Containing source map */

	struct dm_list list;     /* For linking in source->areas */
};

/*
 * Tracking structure for provisional allocation
 *
 * During parallel allocation (striping/RAID), we may provisionally
 * reserve parts of areas and need to track usage.
 */
struct alloc_area_used {
	struct alloc_area *pva;
	uint32_t used;           /* Extents reserved from this area */
};

/*
 * Allocation source (abstraction of a physical volume)
 *
 * Represents one "device" with free space.
 * LVM2 creates one per PV, library doesn't know that.
 */
struct alloc_source {
	void *handle;            /* Opaque reference (e.g., PV*) */
	struct dm_list areas;    /* List of struct alloc_area, sorted by size */
	uint64_t pe_count;       /* Total extents managed (free + used) */

	/* For policy decisions */
	struct dm_list tags;     /* List of struct str_list */

	struct dm_list list;     /* For linking in request->sources */
};

/*
 * Allocation request parameters
 *
 * Describes what should be allocated and how.
 */
struct alloc_request {
	/* Available sources */
	struct dm_list *sources;     /* List of struct alloc_source */

	/* Required allocation */
	uint32_t area_count;         /* Number of parallel areas (stripes/images) */
	uint32_t area_multiple;      /* Each area is this many extents */
	uint32_t new_extents;        /* Total extents to allocate */
	uint32_t parity_count;       /* Additional parity areas for RAID */

	/* Allocation policy */
	alloc_policy_t alloc;

	/* Policy-specific parameters */
	struct dm_list *parallel_areas;       /* For CLING: existing allocations to match */
	const struct dm_config_node *cling_tag_list_cn; /* Tag configuration */

	/* Flags for allocation behavior */
	unsigned alloc_and_split_meta:1;     /* RAID: allocate data+metadata together */
	unsigned approx_alloc:1;             /* Allocate partial if full not available */
	unsigned can_split:1;                /* Allow splitting across multiple areas */
	unsigned maximise_cling:1;
	unsigned mirror_logs_separate:1;     /* Force mirror logs on separate sources */
	unsigned parallel_areas_separate:1;  /* Require each parallel area on different source (for RAID/mirror redundancy) */

	/* RAID metadata parameters */
	uint32_t log_area_count;             /* Number of log/metadata areas */
	uint32_t log_len;                    /* Length of each log/metadata area */
	uint32_t metadata_area_count;        /* Number of RAID metadata areas */
	uint32_t region_size;                /* Mirror region size */
};

/*
 * Single allocated segment
 *
 * Represents a contiguous range allocated from one source.
 */
struct alloc_segment {
	void *source_handle;     /* Which source this came from */
	uint64_t start_extent;   /* Starting extent within source */
	uint64_t extent_count;   /* Number of extents */

	struct dm_list list;     /* For linking in allocated area list */
};

/*
 * Complete allocation result
 *
 * Describes what was allocated across all parallel areas.
 *
 * Example for 3-way stripe with 100 extents each:
 *   area_count = 3
 *   allocated[0] = segments totaling 100 extents (stripe 0)
 *   allocated[1] = segments totaling 100 extents (stripe 1)
 *   allocated[2] = segments totaling 100 extents (stripe 2)
 *
 * Example for RAID5 (3 data + 1 parity):
 *   area_count = 3 (data)
 *   parity_count = 1
 *   allocated[0..2] = data stripes
 *   allocated[3] = parity stripe
 */
struct alloc_result {
	uint32_t total_extents;      /* Total extents allocated */
	uint32_t area_count;         /* Number of parallel data areas */
	uint32_t parity_count;       /* Number of parity areas */
	uint32_t total_area_len;     /* Length of each parallel area */

	/*
	 * Variable-length array of allocated areas.
	 * Size is (area_count + parity_count + log_area_count).
	 * Each element is a dm_list of struct alloc_segment.
	 */
	struct dm_list allocated[];
};

/*
 * Allocation handle (opaque)
 *
 * Context for allocation operations.
 * Created by alloc_create(), destroyed by alloc_destroy().
 */
struct alloc_handle;

#endif /* _LIBALLOC_TYPES_H */
