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

#ifndef _LVM_ALLOC_BRIDGE_H
#define _LVM_ALLOC_BRIDGE_H

#include "liballoc/alloc.h"

/*
 * Bridge functions between LVM2 metadata and liballoc
 */

/*
 * Build allocation sources from PV list
 *
 * Converts PV structures to alloc_source structures for liballoc.
 * PVs are stored as opaque handles in the sources.
 */
struct dm_list *build_alloc_sources_from_pvs(struct dm_pool *mem,
					     struct volume_group *vg,
					     struct dm_list *allocatable_pvs);

/*
 * Apply allocation result to LV
 *
 * Converts alloc_result back to LV segment structure.
 * Handles are cast back to PV pointers.
 */
int apply_alloc_result_to_lv(struct logical_volume *lv,
			     const struct segment_type *segtype,
			     struct alloc_result *result,
			     uint64_t status,
			     uint32_t stripe_size,
			     uint32_t region_size);

/*
 * Allocate extents using liballoc
 *
 * Drop-in replacement for allocate_extents() that uses the new liballoc library.
 * This is the main migration path - allows gradual transition to liballoc.
 *
 * Returns: alloc_handle containing alloc_result on success, NULL on failure
 */
struct alloc_handle *allocate_extents_liballoc(struct volume_group *vg,
					       struct logical_volume *lv,
					       const struct segment_type *segtype,
					       uint32_t stripes,
					       uint32_t mirrors, uint32_t log_count,
					       uint32_t region_size, uint32_t extents,
					       struct dm_list *allocatable_pvs,
					       alloc_policy_t alloc,
					       unsigned approx_alloc,
					       struct dm_list *parallel_areas);

#endif /* _LVM_ALLOC_BRIDGE_H */
