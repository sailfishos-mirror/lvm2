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

/*
 * alloc_bridge - Bridge between LVM2 metadata and liballoc
 *
 * This file translates between PV/VG/LV structures and liballoc types.
 */

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/metadata/alloc_bridge.h"
#include "lib/metadata/lv_alloc.h"
#include "lib/metadata/segtype.h"
#include "lib/datastruct/str_list.h"
#include "liballoc/alloc.h"
#include <string.h>

/*
 * allocated_area structure from lv_manip.c
 * Needed for binary compatibility with old allocation code
 */
struct allocated_area {
	struct dm_list list;
	struct physical_volume *pv;
	uint32_t pe;
	uint32_t len;
};

/*
 * Build alloc_sources from list of PVs
 *
 * Translates PV structures to generic alloc_source structures
 * that liballoc can work with.
 */
struct dm_list *build_alloc_sources_from_pvs(struct dm_pool *mem,
					     struct volume_group *vg,
					     struct dm_list *allocatable_pvs)
{
	struct dm_list *sources;
	struct pv_list *pvl;
	struct physical_volume *pv;
	struct alloc_source *src;
	struct pv_segment *pvseg;

	if (!(sources = dm_pool_zalloc(mem, sizeof(*sources)))) {
		log_error("Failed to allocate sources list.");
		return NULL;
	}

	dm_list_init(sources);

	/* Iterate each PV in the list */
	dm_list_iterate_items(pvl, allocatable_pvs) {
		pv = pvl->pv;

		/* Skip non-allocatable PVs */
		if (!(pv->status & ALLOCATABLE_PV))
			continue;

		if (pv->status & PV_ALLOCATION_PROHIBITED) {
			pv->status &= ~PV_ALLOCATION_PROHIBITED;
			continue;
		}

		if (is_missing_pv(pv))
			continue;

		/* Create source for this PV */
		if (!(src = dm_pool_zalloc(mem, sizeof(*src)))) {
			log_error("Failed to allocate source.");
			return NULL;
		}

		src->handle = pv;  /* Store PV as opaque handle */
		dm_list_init(&src->areas);
		dm_list_init(&src->tags);
		dm_list_init(&src->list);
		src->pe_count = pv->pe_count;

		/* Convert PV segments to alloc_areas (sorted by size) */
		dm_list_iterate_items(pvseg, &pv->segments) {
			/* Skip allocated segments */
			if (pvseg->lvseg)
				continue;

			/* Add free area to source (automatically sorted) */
			if (!alloc_source_add_area(mem, src, pvseg->pe, pvseg->len, pv)) {
				log_error("Failed to add area to source.");
				return NULL;
			}

			log_debug_alloc("Source %s: area at PE %u length %u.",
			                pv_dev_name(pv), pvseg->pe, pvseg->len);
		}

		/* Copy PV tags if any */
		if (!str_list_dup(mem, &src->tags, &pv->tags)) {
			log_error("Failed to copy tags.");
			return NULL;
		}

		dm_list_add(sources, &src->list);
	}

	return sources;
}

/*
 * Apply allocation result to LV segment
 *
 * Translates alloc_result back to LV segment structure.
 *
 * TODO:
 */
int apply_alloc_result_to_lv(struct logical_volume *lv,
			     const struct segment_type *segtype,
			     struct alloc_result *result,
			     uint64_t status,
			     uint32_t stripe_size,
			     uint32_t region_size)
{
	struct lv_segment *seg;
	struct alloc_segment *aseg;
	struct physical_volume *pv;
	uint64_t le_offset;
	uint32_t s;

	if (!result || !result->total_extents)
		return_0;

	/* Create LV segment */
	seg = alloc_lv_segment(segtype, lv, lv->le_count,
	                       result->total_area_len, 0, status,
	                       stripe_size, NULL,
	                       result->area_count, result->total_area_len,
	                       0, 0, region_size, 0, NULL);
	if (!seg) {
		log_error("Failed to create LV segment.");
		return 0;
	}

	/* Map allocation results to segment areas */
	for (s = 0; s < result->area_count; s++) {
		le_offset = 0;

		/* Process each segment in this parallel area */
		dm_list_iterate_items(aseg, &result->allocated[s]) {
			pv = aseg->source_handle;

			/* Set segment area to point to PV */
			if (!set_lv_segment_area_pv(seg, s, pv, aseg->start_extent)) {
				log_error("Failed to set segment area.");
				return 0;
			}

			log_debug_alloc("LV %s: stripe %u uses %s PE %" PRIu64 "-%" PRIu64 ".",
			                lv->name, s, pv_dev_name(pv),
			                aseg->start_extent,
			                aseg->start_extent + aseg->extent_count - 1);

			le_offset += aseg->extent_count;

			/* For fragmented allocations, we'd need multiple segments */
			/* For now, assume contiguous */
			break;
		}
	}

	/* Add segment to LV */
	dm_list_add(&lv->segments, &seg->list);
	lv->le_count += result->total_area_len;
	lv->size += (uint64_t) result->total_area_len * lv->vg->extent_size;

	return 1;
}

/*
 * Wrapper alloc_handle that bridges old and new allocation systems
 *
 * IMPORTANT: This must match the layout of struct alloc_handle in lv_manip.c
 * up to and including the allocated_areas field for binary compatibility.
 *
 * This allows allocate_extents_liballoc() to return a handle that's
 * compatible with existing code while using liballoc internally.
 */
struct alloc_handle_liballoc {
	/* Fields matching old alloc_handle (lv_manip.c:1882) */
	struct cmd_context *cmd;
	struct dm_pool *mem;

	alloc_policy_t alloc;
	int approx_alloc;
	uint32_t new_extents;
	uint32_t area_count;
	uint32_t parity_count;
	uint32_t area_multiple;
	uint32_t log_area_count;
	uint32_t metadata_area_count;
	uint32_t log_len;
	uint32_t region_size;
	uint32_t total_area_len;

	unsigned maximise_cling;
	unsigned mirror_logs_separate;
	unsigned alloc_and_split_meta;
	unsigned split_metadata_is_allocated;

	const struct dm_config_node *cling_tag_list_cn;
	struct dm_list *parallel_areas;

	/*
	 * Variable-length array - MUST be last field
	 * Contains area_count + log_area_count lists of allocated_area structures
	 *
	 * NOTE: We do NOT store liballoc_ah or result pointers here because they
	 * would break binary compatibility. The old alloc_handle has allocated_areas
	 * immediately after parallel_areas. Any extra fields would shift allocated_areas
	 * and cause pointer arithmetic errors in old code.
	 */
	struct dm_list allocated_areas[];
};

/*
 * Allocate extents using liballoc
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
					       struct dm_list *parallel_areas)
{
	struct dm_pool *mem;
	struct dm_list *sources;
	struct alloc_handle_liballoc *ah_wrapper;
	struct alloc_handle *liballoc_ah;
	struct alloc_result *result;
	struct alloc_segment **area_segs;
	struct allocated_area *aa;
	uint32_t area_count;
	uint32_t area_multiple;
	uint32_t total_areas;
	uint32_t allocated_area_count;
	uint32_t expected_areas;
	uint32_t max_segments;
	uint32_t seg_count;
	uint32_t batch;
	uint32_t s;
	size_t wrapper_size;
	struct alloc_request request = {
		.new_extents = extents,
		.parallel_areas = parallel_areas,
		.approx_alloc = approx_alloc,
		.log_area_count = log_count,
		.region_size = region_size,
	};

	/* Validate parameters */
	if (segtype_is_virtual(segtype)) {
		log_error("allocate_extents_liballoc does not handle virtual segments.");
		return NULL;
	}

	if (!allocatable_pvs) {
		log_error(INTERNAL_ERROR "Missing allocatable pvs.");
		return NULL;
	}

	/*
	 * TODO: liballoc doesn't yet support variable-size parallel areas (e.g., thin pool
	 * metadata which is smaller than the data area). Fall back to old allocation code
	 * for now when log/metadata areas are needed.
	 */
	if (log_count > 0) {
		log_debug_alloc("Falling back to old allocation code for log/metadata areas.");
		/* Use old allocate_extents() - declared in lv_alloc.h */
		return allocate_extents(vg, lv, segtype, stripes, mirrors, log_count,
					region_size, extents, allocatable_pvs, alloc,
					approx_alloc, parallel_areas);
	}

	if (vg->fid->fmt->ops->segtype_supported &&
	    !vg->fid->fmt->ops->segtype_supported(vg->fid, segtype)) {
		log_error("Metadata format (%s) does not support required "
			  "LV segment type (%s).",
			  vg->fid->fmt->name, segtype->name);
		log_error("Consider changing the metadata format by running "
		          "vgconvert.");
		return NULL;
	}

	/* Create memory pool for this allocation */
	if (!(mem = dm_pool_create("liballoc_wrapper", 1024))) {
		log_error("Failed to create memory pool for allocation.");
		return NULL;
	}

	/* Build allocation sources from PVs */
	if (!(sources = build_alloc_sources_from_pvs(mem, vg, allocatable_pvs))) {
		log_error("Failed to build allocation sources.");
		dm_pool_destroy(mem);
		return NULL;
	}

	/* Create liballoc handle */
	if (!(liballoc_ah = liballoc_create(mem))) {
		log_error("Failed to create liballoc handle.");
		dm_pool_destroy(mem);
		return NULL;
	}

	/* Resolve ALLOC_INHERIT */
	if (alloc >= ALLOC_INHERIT)
		alloc = vg->alloc;

	/* Calculate area_count from stripes and mirrors */
	if (mirrors > 1)
		area_count = mirrors * stripes;
	else
		area_count = stripes;

	/*
	 * Calculate area_multiple - determines relationship between
	 * LV size and per-area allocation.
	 *
	 * For RAID10: area_multiple = stripes (number of data stripes)
	 * For striped: area_multiple = area_count
	 * For mirrored stripes: area_multiple = stripes
	 * For mirrored: area_multiple = 1
	 */
	area_multiple = 0;  /* 0 means don't divide */
	if (segtype_is_striped(segtype)) {
		area_multiple = area_count;
	} else if (segtype_is_raid10(segtype)) {
		area_multiple = stripes;  /* RAID10: divide by stripe count */
	} else if (stripes > 1) {
		/* Mirrored stripes: each mirror image is striped */
		area_multiple = stripes;
	} else if (area_count > 1) {
		/* Plain mirrored or RAID with no striping */
		area_multiple = 1;
	}

	/* Build allocation request */
	request.alloc = alloc;
	request.area_count = area_count;
	request.area_multiple = area_multiple;
	request.parity_count = segtype->parity_devs;  /* RAID parity devices */
	request.sources = sources;

	/*
	 * Determine if fragmented allocation is allowed (can_split).
	 *
	 * Background: Multi-area allocations (striping, mirroring, RAID) require
	 * synchronized rounds to ensure all parallel areas have identical segment
	 * layouts. This is critical for data integrity.
	 *
	 * Allow splitting for:
	 * 1. Single-area allocations (simple LVs, area_count=1)
	 *    - No synchronization needed, can fragment freely
	 *
	 * 2. Striped volumes without redundancy (plain striped or RAID0)
	 *    - liballoc supports synchronized multi-area allocation
	 *    - Plain striped: segtype_is_striped()
	 *    - RAID0 variants: segtype_is_any_raid0()
	 *    - Must have no parity devices (parity_devs=0)
	 *    - Must have mirrors<2 to exclude multi-way mirrors
	 *
	 * Note on mirrors parameter:
	 * - The mirrors parameter counts ADDITIONAL mirror copies beyond the original:
	 *   mirrors=0: 1 total copy (no mirroring) - plain striped, RAID0
	 *   mirrors=1: 2 total copies (original + 1 mirror) - 2-way mirror
	 *   mirrors=2: 3 total copies (original + 2 mirrors) - 3-way mirror
	 * - For RAID types, mirrors may be set even for non-mirrored configurations
	 *   (e.g., RAID0 might pass mirrors=1 for implementation reasons)
	 * - We rely on segtype check to distinguish true redundant RAID from RAID0
	 *
	 * Do NOT allow splitting for:
	 * - ALLOC_CONTIGUOUS (requires single contiguous area)
	 * - RAID with parity (parity_devs > 0) - old code handles better
	 * - Multi-way mirrors (mirrors>=2) - old code handles better
	 * - Other RAID types with redundancy - old code handles better
	 */
	log_debug_alloc("can_split logic: alloc=%u CONTIGUOUS=%u area_count=%u "
			"stripes=%u mirrors=%u parity=%u segtype=%s.",
			alloc, ALLOC_CONTIGUOUS, area_count, stripes, mirrors,
			segtype->parity_devs, segtype->name);
	request.can_split = (alloc != ALLOC_CONTIGUOUS) &&
			    ((area_count == 1) ||
			     ((segtype_is_striped(segtype) || segtype_is_any_raid0(segtype)) &&
			      mirrors < 2 && segtype->parity_devs == 0));
	log_debug_alloc("can_split result: %u.", request.can_split);

	/* For mirrors/RAID, require each parallel area on a different PV for redundancy
	 * Plain striped volumes don't need this - stripes can be on same PV at different offsets */
	request.parallel_areas_separate = (mirrors > 1) ||  /* Mirrors need redundancy */
	                                   (segtype->parity_devs > 0);  /* RAID needs redundancy */

	log_debug_alloc("allocate_extents_liballoc: extents=%u stripes=%u mirrors=%u "
			"area_count=%u area_multiple=%u alloc=%u parallel_areas=%p.",
			extents, stripes, mirrors, area_count, area_multiple,
			alloc, parallel_areas);

	/* Perform allocation */
	if (!liballoc_allocate(liballoc_ah, &request, &result)) {
		log_error("Allocation failed (memory allocation error).");
		liballoc_destroy(liballoc_ah);
		dm_pool_destroy(mem);
		return NULL;
	}

	log_debug_alloc("liballoc returned: total_extents=%u total_area_len=%u approx_alloc=%d.",
			result->total_extents, result->total_area_len, approx_alloc);

	/* Check if allocation found any space */
	if (result->total_extents == 0) {
		/* With approx_alloc, allocating 0 extents is acceptable (nothing left) */
		if (!approx_alloc) {
			log_error("Insufficient free space: %u extents requested, "
				  "0 extents available.", extents);
		}
		liballoc_destroy(liballoc_ah);
		dm_pool_destroy(mem);
		return NULL;
	}

	/* Calculate total areas needed (data + parity + log/metadata) */
	total_areas = area_count + segtype->parity_devs + log_count;
	wrapper_size = sizeof(*ah_wrapper) + total_areas * sizeof(struct dm_list);

	/* Create wrapper handle with space for allocated_areas */
	if (!(ah_wrapper = dm_pool_zalloc(mem, wrapper_size))) {
		log_error("Failed to allocate wrapper handle.");
		liballoc_destroy(liballoc_ah);
		dm_pool_destroy(mem);
		return NULL;
	}

	/* Fill in compatibility fields */
	ah_wrapper->cmd = vg->cmd;
	ah_wrapper->mem = mem;
	ah_wrapper->alloc = alloc;
	ah_wrapper->approx_alloc = approx_alloc;
	/* new_extents is total target size (existing + new), not just new extents */
	ah_wrapper->new_extents = (lv ? lv->le_count : 0) + extents;
	ah_wrapper->area_count = area_count;
	ah_wrapper->parity_count = segtype->parity_devs;
	ah_wrapper->area_multiple = area_multiple;
	ah_wrapper->log_area_count = log_count;
	ah_wrapper->metadata_area_count = 0;
	ah_wrapper->log_len = 0;
	ah_wrapper->region_size = region_size;
	ah_wrapper->total_area_len = result->total_area_len;
	ah_wrapper->maximise_cling = 0;
	ah_wrapper->mirror_logs_separate = 0;
	ah_wrapper->alloc_and_split_meta = 0;
	ah_wrapper->split_metadata_is_allocated = 0;
	ah_wrapper->cling_tag_list_cn = NULL;
	ah_wrapper->parallel_areas = parallel_areas;

	/* Initialize allocated_areas lists */
	for (s = 0; s < total_areas; s++)
		dm_list_init(&ah_wrapper->allocated_areas[s]);

	/* Convert liballoc result to old allocated_area format
	 *
	 * IMPORTANT: The old code expects allocated_areas to contain arrays of
	 * allocated_area structures, where aa[0], aa[1], ... aa[area_count-1]
	 * represent parallel allocations (stripes). Each segment in the allocation
	 * result needs to create such an array.
	 *
	 * For simplicity, liballoc currently only returns one segment per area
	 * (contiguous allocation), so we create one array per allocation.
	 */

	/* Calculate how many areas were actually allocated (including log/metadata) */
	allocated_area_count = 0;
	for (s = 0; s < total_areas; s++) {
		if (!dm_list_empty(&result->allocated[s]))
			allocated_area_count++;
	}

	if (!allocated_area_count) {
		log_error("liballoc returned no allocations.");
		dm_pool_destroy(mem);
		return NULL;
	}

	/* Expect area_count + parity_count areas (data + parity) */
	expected_areas = area_count + segtype->parity_devs;
	if (allocated_area_count != expected_areas) {
		log_error("liballoc allocated %u areas, expected %u areas (%u data + %u parity).",
			  allocated_area_count, expected_areas, area_count, segtype->parity_devs);
		dm_pool_destroy(mem);
		return NULL;
	}

	/*
	 * Convert liballoc segments to allocated_area structures
	 *
	 * IMPORTANT: The old code expects allocated_areas to contain arrays, not individual elements.
	 * For each allocation "batch" (set of parallel segments), we must:
	 * 1. Allocate ONE contiguous array of allocated_area[area_count]
	 * 2. Fill in all parallel areas in that array
	 * 3. Add only the FIRST element (aa[0]) to allocated_areas[0]
	 * 4. The code can then access aa[1], aa[2], etc. via array indexing
	 *
	 * For fragmented allocations (multiple segments per area), we create multiple
	 * such arrays, one per "batch" of parallel segments.
	 */

	/*
	 * Convert liballoc segments to allocated_area arrays
	 *
	 * For fragmented allocations, we need to handle multiple segments per area.
	 * We create one array per "batch" of parallel segments.
	 *
	 * Example: Allocating 264 extents on 4 PVs with 66 extents each (area_count=1):
	 * - Batch 0: aa[0] = {pv1, pe=0, len=66}
	 * - Batch 1: aa[0] = {pv2, pe=0, len=66}
	 * - Batch 2: aa[0] = {pv3, pe=0, len=66}
	 * - Batch 3: aa[0] = {pv4, pe=0, len=66}
	 * All four arrays have aa[0] added to allocated_areas[0]
	 */

	/* Determine maximum number of segments across all areas */
	max_segments = 0;
	for (s = 0; s < allocated_area_count; s++) {
		if (!(seg_count = dm_list_size(&result->allocated[s]))) {
			log_error("liballoc area %u has no segments.", s);
			dm_pool_destroy(mem);
			return NULL;
		}
		if (seg_count > max_segments)
			max_segments = seg_count;
	}

	log_debug_alloc("Converting %u areas with max %u segments per area.",
			allocated_area_count, max_segments);

	/* Create arrays of allocated_area pointers to track segments for each area */
	if (!(area_segs = dm_pool_alloc(mem, allocated_area_count * sizeof(*area_segs)))) {
		log_error("Failed to allocate segment tracking array.");
		dm_pool_destroy(mem);
		return NULL;
	}

	/* Initialize pointers to the first segment of each area */
	for (s = 0; s < allocated_area_count; s++) {
		if (dm_list_empty(&result->allocated[s]))
			area_segs[s] = NULL;
		else
			area_segs[s] = dm_list_item(dm_list_first(&result->allocated[s]),
						    struct alloc_segment);
	}

	/* Create one array per segment batch */
	for (batch = 0; batch < max_segments; batch++) {
		/* Allocate array for this batch */
		if (!(aa = dm_pool_zalloc(mem, allocated_area_count * sizeof(*aa)))) {
			log_error("Failed to allocate allocated_area array.");
			dm_pool_destroy(mem);
			return NULL;
		}

		/* Fill in the array from current segments */
		for (s = 0; s < allocated_area_count; s++) {
			dm_list_init(&aa[s].list);

			if (area_segs[s]) {
				aa[s].pv = area_segs[s]->source_handle;
				aa[s].pe = area_segs[s]->start_extent;
				aa[s].len = area_segs[s]->extent_count;

				log_debug_alloc("Batch %u area %u: pv=%s pe=%u len=%u.",
						batch, s, pv_dev_name(aa[s].pv),
						aa[s].pe, aa[s].len);

				/* Advance to next segment */
				if (area_segs[s]->list.n != &result->allocated[s])
					area_segs[s] = dm_list_item(area_segs[s]->list.n,
								    struct alloc_segment);
				else
					area_segs[s] = NULL;
			} else {
				/* No more segments for this area */
				aa[s].pv = NULL;
				aa[s].pe = 0;
				aa[s].len = 0;
				log_debug_alloc("Batch %u area %u: empty.", batch, s);
			}
		}

		/* Add each element of this batch's array to its corresponding list */
		for (s = 0; s < allocated_area_count; s++)
			dm_list_add(&ah_wrapper->allocated_areas[s], &aa[s].list);
	}

	log_debug_alloc("Allocated %u extents using liballoc.", result->total_extents);

	/* Return wrapper cast as old alloc_handle type */
	log_debug_alloc("Returning ah_wrapper=%p, allocated_areas[0]=%p, area_count=%u.",
			ah_wrapper, &ah_wrapper->allocated_areas[0], ah_wrapper->area_count);

	return (struct alloc_handle *)ah_wrapper;
}
