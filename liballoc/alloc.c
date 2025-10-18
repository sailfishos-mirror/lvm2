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

/* liballoc - Standalone allocation library implementation */

#include "liballoc/alloc.h"
#include <assert.h>

/*
 * Internal allocation handle structure
 */
struct alloc_handle {
	struct dm_pool *mem;
	alloc_policy_t policy;

	/* Could add statistics, logging callbacks, etc. here */
};

/*
 * Create allocation handle
 */
struct alloc_handle *liballoc_create(struct dm_pool *mem)
{
	struct alloc_handle *ah;

	if (!mem)
		return NULL;

	if (!(ah = dm_pool_zalloc(mem, sizeof(*ah))))
		return NULL;

	ah->mem = mem;

	return ah;
}

/*
 * Destroy allocation handle
 */
void liballoc_destroy(struct alloc_handle *ah)
{
	/* Memory pool cleanup is caller's responsibility */
	(void)ah;
}

/*
 * Insert area into source's area list, maintaining size order (largest first)
 *
 * This is critical for ALLOC_NORMAL efficiency.
 */
static void _insert_area_sorted(struct alloc_source *src, struct alloc_area *area)
{
	struct alloc_area *a;

	/* Find insertion point - areas sorted by count, largest first */
	dm_list_iterate_items(a, &src->areas) {
		if (area->count > a->count) {
			dm_list_add(&a->list, &area->list);
			return;
		}
	}

	/* Smallest, add at end */
	dm_list_add(&src->areas, &area->list);
}

/*
 * Find suitable area - ALLOC_ANYWHERE
 *
 * Just find first area that has any free space.
 * Splitting is always allowed for ALLOC_ANYWHERE.
 */
static struct alloc_area *_find_area_anywhere(struct dm_list *sources,
					      uint64_t needed)
{
	struct alloc_source *src;
	struct alloc_area *area;

	dm_list_iterate_items(src, sources) {
		dm_list_iterate_items(area, &src->areas) {
			if (area->unreserved > 0)
				return area;
		}
	}

	return NULL;
}

/*
 * Find suitable area - ALLOC_NORMAL
 *
 * Prefer larger areas to reduce fragmentation.
 * We iterate all sources to find the globally largest area.
 */
static struct alloc_area *_find_area_normal(struct dm_list *sources,
					    uint64_t needed,
					    unsigned can_split)
{
	struct alloc_source *src;
	struct alloc_area *area;
	struct alloc_area *best_area = NULL;
	uint64_t best_size = 0;

	dm_list_iterate_items(src, sources) {
		dm_list_iterate_items(area, &src->areas) {
			/* Skip areas with no free space */
			if (area->unreserved == 0)
				continue;

			/* If no splitting, need full contiguous space */
			if (!can_split) {
				if (area->unreserved >= needed && area->count >= needed)
					return area;  /* First fit is fine for contiguous */
				continue;
			}

			/* If splitting allowed, prefer largest area */
			if (area->unreserved > best_size) {
				best_area = area;
				best_size = area->unreserved;
			}
		}
	}

	return best_area;
}

/*
 * Find suitable area - ALLOC_CONTIGUOUS
 *
 * Requires exactly contiguous space - no splitting allowed.
 * All requested extents must fit in a single area.
 */
static struct alloc_area *_find_area_contiguous(struct dm_list *sources,
						uint64_t needed)
{
	struct alloc_source *src;
	struct alloc_area *area;

	dm_list_iterate_items(src, sources) {
		dm_list_iterate_items(area, &src->areas) {
			/* Must have full space available, no splitting */
			if (area->unreserved >= needed && area->count >= needed)
				return area;
		}
	}

	return NULL;
}

/*
 * Find suitable area - ALLOC_CLING
 *
 * Prefer allocating from same sources as existing parallel areas.
 * This helps keep related stripes/mirrors on the same PVs.
 *
 * If no parallel_areas provided, falls back to ALLOC_NORMAL behavior.
 */
static struct alloc_area *_find_area_cling(struct dm_list *sources,
					   uint64_t needed,
					   struct dm_list *parallel_areas,
					   unsigned can_split,
					   unsigned maximise_cling)
{
	struct alloc_source *src;
	struct alloc_area *area;

	/* If no parallel areas provided, fall back to ALLOC_NORMAL */
	if (!parallel_areas || dm_list_empty(parallel_areas))
		return _find_area_normal(sources, needed, can_split);

	/*
	 * First pass: Try to allocate from sources already used
	 * in parallel_areas (cling to existing allocations)
	 *
	 * Note: parallel_areas is expected to be populated with lists of
	 * alloc_segment from a previous allocation result.
	 */
	dm_list_iterate_items(src, sources) {
		int found_in_parallel = 0;

		/* Check if this source is used in any parallel area */
		if (parallel_areas) {
			struct alloc_segment *seg;
			/* Iterate through all segments in all parallel areas */
			dm_list_iterate_items(seg, parallel_areas) {
				if (seg->source_handle == src->handle) {
					found_in_parallel = 1;
					break;
				}
			}
		}

		if (!found_in_parallel)
			continue;

		/* This source is used in parallel - prefer it */
		dm_list_iterate_items(area, &src->areas) {
			if (area->unreserved < needed)
				continue;

			if (!can_split && area->count < needed)
				continue;

			return area;
		}
	}

	/*
	 * Second pass: If maximise_cling not set, allow non-cling allocation
	 */
	if (!maximise_cling)
		return _find_area_normal(sources, needed, can_split);

	/* maximise_cling set - require cling or fail */
	return NULL;
}

/*
 * Simple string list matching - internal implementation
 * Does NOT depend on lib/datastruct/str_list.h
 *
 * This is a minimal implementation for tag matching within liballoc.
 * Uses dm_str_list structure from base/data-struct/list.h.
 */

/*
 * Check if a tag string exists in a tag list
 */
static int _tag_list_has_item(const struct dm_list *tag_list, const char *tag)
{
	struct dm_str_list *sl;

	if (!tag_list || !tag)
		return 0;

	dm_list_iterate_items(sl, tag_list) {
		if (sl->str && !strcmp(sl->str, tag))
			return 1;
	}

	return 0;
}

/*
 * Check if any tag from list1 matches any tag from list2
 */
static int _tag_lists_have_common(const struct dm_list *list1,
				   const struct dm_list *list2)
{
	struct dm_str_list *sl;

	if (!list1 || !list2)
		return 0;

	dm_list_iterate_items(sl, list1) {
		if (sl->str && _tag_list_has_item(list2, sl->str))
			return 1;
	}

	return 0;
}

/*
 * Check if two sources have matching tags based on configuration
 *
 * Returns 1 if tags match, 0 otherwise.
 */
static int _sources_have_matching_tags(const struct dm_config_node *cling_tag_list_cn,
					struct alloc_source *src1,
					struct alloc_source *src2)
{
	const struct dm_config_value *cv;
	const char *str;

	if (!cling_tag_list_cn || !src1 || !src2)
		return 0;

	/* Iterate through tag configuration */
	for (cv = cling_tag_list_cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING)
			continue;

		str = cv->v.str;
		if (!*str || *str != '@')
			continue;

		str++;  /* Skip '@' prefix */
		if (!*str)
			continue;

		/* Wildcard matches any tag against any tag */
		if (!strcmp(str, "*")) {
			if (_tag_lists_have_common(&src1->tags, &src2->tags))
				return 1;
			continue;
		}

		/* Check if both sources have this specific tag */
		if (_tag_list_has_item(&src1->tags, str) &&
		    _tag_list_has_item(&src2->tags, str))
			return 1;
	}

	return 0;
}

/*
 * Find suitable area - ALLOC_CLING_BY_TAGS
 *
 * Prefer allocating from sources with matching tags to existing parallel areas.
 * Tags to match are specified in cling_tag_list_cn configuration.
 *
 * If no parallel_areas provided, falls back to ALLOC_NORMAL behavior.
 */
static struct alloc_area *_find_area_cling_by_tags(struct dm_list *sources,
						    uint64_t needed,
						    struct dm_list *parallel_areas,
						    const struct dm_config_node *cling_tag_list_cn,
						    unsigned can_split,
						    unsigned maximise_cling)
{
	struct alloc_source *src;
	struct alloc_source *parallel_src;
	struct alloc_area *area;
	struct alloc_segment *seg;
	int has_matching_tag;

	/* If no tag configuration, fall back to regular CLING */
	if (!cling_tag_list_cn)
		return _find_area_cling(sources, needed, parallel_areas,
		                        can_split, maximise_cling);

	/* If no parallel areas provided, fall back to ALLOC_NORMAL */
	if (!parallel_areas || dm_list_empty(parallel_areas))
		return _find_area_normal(sources, needed, can_split);

	/*
	 * First pass: Try to allocate from sources with matching tags
	 * to sources used in parallel_areas
	 */
	dm_list_iterate_items(src, sources) {
		has_matching_tag = 0;

		/* Check if this source has matching tags with any parallel area source */
		dm_list_iterate_items(seg, parallel_areas) {
			/* Find the source for this segment */
			dm_list_iterate_items(parallel_src, sources) {
				if (parallel_src->handle == seg->source_handle) {
					if (_sources_have_matching_tags(cling_tag_list_cn,
									src, parallel_src)) {
						has_matching_tag = 1;
						break;
					}
				}
			}
			if (has_matching_tag)
				break;
		}

		if (!has_matching_tag)
			continue;

		/* This source has matching tags - prefer it */
		dm_list_iterate_items(area, &src->areas) {
			if (area->unreserved < needed)
				continue;

			if (!can_split && area->count < needed)
				continue;

			return area;
		}
	}

	/*
	 * Second pass: If maximise_cling not set, allow non-cling allocation
	 */
	if (!maximise_cling)
		return _find_area_normal(sources, needed, can_split);

	/* maximise_cling set - require tag match or fail */
	return NULL;
}

/*
 * Find suitable area - dispatcher
 *
 * Dispatches to the appropriate policy-specific allocation function.
 */
static struct alloc_area *_find_area(struct dm_list *sources,
				     uint64_t needed,
				     const struct alloc_request *request)
{
	switch (request->alloc) {
	case ALLOC_ANYWHERE:
		return _find_area_anywhere(sources, needed);
	case ALLOC_NORMAL:
		return _find_area_normal(sources, needed, request->can_split);
	case ALLOC_CONTIGUOUS:
		/* CONTIGUOUS never splits - ignore can_split parameter */
		return _find_area_contiguous(sources, needed);
	case ALLOC_CLING:
		return _find_area_cling(sources, needed,
		                        request->parallel_areas,
		                        request->can_split,
		                        request->maximise_cling);
	case ALLOC_CLING_BY_TAGS:
		return _find_area_cling_by_tags(sources, needed,
		                                request->parallel_areas,
		                                request->cling_tag_list_cn,
		                                request->can_split,
		                                request->maximise_cling);
	default:
		return NULL;
	}
}

/*
 * Allocate from area
 */
static struct alloc_segment *_allocate_segment(struct dm_pool *mem,
					       struct alloc_area *area,
					       uint64_t count)
{
	struct alloc_segment *seg;

	if (!(seg = dm_pool_zalloc(mem, sizeof(*seg))))
		return NULL;

	seg->source_handle = area->source_handle;
	seg->start_extent = area->start;
	seg->extent_count = count;

	/* Update area */
	area->start += count;
	area->count -= count;
	area->unreserved -= count;

	return seg;
}

/*
 * Multi-area synchronized allocation (striped/RAID)
 *
 * Allocates multiple parallel areas with identical segment layouts.
 * All areas must get the same extent count per round.
 */
static int _allocate_multi_area(struct alloc_handle *ah,
				const struct alloc_request *request,
				struct alloc_result *res,
				struct alloc_source **used_sources,
				uint32_t *used_source_count,
				uint32_t areas,
				uint64_t per_area,
				uint64_t *allocated)
{
	struct alloc_source *src;
	struct alloc_source *conflicting_source;
	struct alloc_area *area;
	struct alloc_area *a;
	struct alloc_segment *seg;
	uint64_t *area_needed;
	uint64_t area_size;
	uint64_t round_size;
	uint32_t s;
	uint32_t i;
	uint32_t restore_s, all_s, prev_s;
	int source_already_used;
	int area_already_selected;
	int all_done;
	int incomplete_round;

	/* Synchronized allocation in rounds */
	area_needed = dm_pool_zalloc(ah->mem, areas * sizeof(uint64_t));
	if (!area_needed)
		return 0;

	/* Initialize needed count for each area */
	for (s = 0; s < areas; s++)
		area_needed[s] = per_area;

	/* Allocate in rounds until all areas satisfied or no more space */
	while (1) {
		struct alloc_area *selected_areas[areas];
		uint64_t saved_unreserved[areas];

		round_size = 0;
		incomplete_round = 0;  /* Flag: couldn't find areas for all stripes */
		all_done = 1;

		/* Check if all areas are satisfied */
		for (s = 0; s < areas; s++) {
			if (area_needed[s] > 0) {
				all_done = 0;
				break;
			}
		}

		if (all_done)
			break;

		/* Initialize saved_unreserved */
		for (s = 0; s < areas; s++)
			saved_unreserved[s] = 0;

		/* Find areas for this round */
		for (s = 0; s < areas; s++) {
			if (area_needed[s] == 0) {
				selected_areas[s] = NULL;
				continue;
			}

			/* Find suitable area, retrying until we find one that doesn't conflict */
			area = _find_area(request->sources, area_needed[s], request);
			if (!area) {
				/* No area found - insufficient space (not a hard error).
				 * Restore any temporarily marked areas and stop allocation. */
				for (restore_s = 0; restore_s < s; restore_s++) {
					if (saved_unreserved[restore_s] > 0 && selected_areas[restore_s]) {
						selected_areas[restore_s]->unreserved = saved_unreserved[restore_s];
					}
				}
				/* Mark ALL areas as done to exit allocation loop.
				 * Also clear selected_areas[] to ensure round_size stays 0. */
				incomplete_round = 1;
				for (all_s = 0; all_s < areas; all_s++) {
					area_needed[all_s] = 0;
					selected_areas[all_s] = NULL;  /* Clear selection */
				}
				break;  /* Exit the stripe selection loop - we're done */
			}
			/* Check redundancy constraint (parallel_areas_separate)
			 * Must check sources selected IN THIS ROUND, not just previous rounds
			 * Loop until we find a non-conflicting source or exhaust all options */
			if (request->parallel_areas_separate && used_sources) {
				while (area) {
					source_already_used = 0;

					/* Check sources used in previous rounds */
					for (i = 0; i < *used_source_count; i++) {
						if (used_sources[i] == area->map) {
							source_already_used = 1;
							break;
						}
					}

					/* Also check sources selected earlier in THIS round */
					if (!source_already_used) {
						for (prev_s = 0; prev_s < s; prev_s++) {
							if (selected_areas[prev_s] && selected_areas[prev_s]->map == area->map) {
								source_already_used = 1;
								break;
							}
						}
					}

					if (!source_already_used) {
						/* Found a good area! */
						break;
					}

					/* Source conflicts - mark ALL areas from this source as excluded and retry */
					conflicting_source = area->map;

					/* Mark all areas from this source as unavailable */
					dm_list_iterate_items(a, &conflicting_source->areas) {
						if (a->unreserved > 0) {
							a->unreserved = 0;
						}
					}

					area = _find_area(request->sources, area_needed[s], request);
				}

				/* Restore all excluded sources */
				dm_list_iterate_items(src, request->sources) {
					dm_list_iterate_items(a, &src->areas) {
						if (a->count > 0 && a->unreserved == 0) {
							a->unreserved = a->count;
						}
					}
				}

				if (!area) {
					/* Cannot satisfy redundancy constraint - insufficient PVs.
					 * Restore any temporarily marked areas and stop allocation. */
					for (restore_s = 0; restore_s < s; restore_s++) {
						if (saved_unreserved[restore_s] > 0 && selected_areas[restore_s]) {
							selected_areas[restore_s]->unreserved = saved_unreserved[restore_s];
						}
					}
					/* Exit allocation loop with whatever was allocated */
					incomplete_round = 1;
					for (all_s = 0; all_s < areas; all_s++) {
						area_needed[all_s] = 0;
						selected_areas[all_s] = NULL;
					}
					break;
				}
			}

			/* Check if this area was already selected in this round (for stripes on same PV) */
			area_already_selected = 0;
			for (prev_s = 0; prev_s < s; prev_s++) {
				if (selected_areas[prev_s] == area) {
					area_already_selected = 1;
					break;
				}
			}

			if (area_already_selected) {
				/* This area was already selected by a previous stripe in this round.
				 * Temporarily mark all already-selected areas as unavailable and retry. */
				for (prev_s = 0; prev_s < s; prev_s++) {
					if (selected_areas[prev_s] && selected_areas[prev_s]->unreserved > 0) {
						saved_unreserved[prev_s] = selected_areas[prev_s]->unreserved;
						selected_areas[prev_s]->unreserved = 0;
					}
				}

				area = _find_area(request->sources, area_needed[s], request);

				/* Restore all temporarily marked areas */
				for (prev_s = 0; prev_s < s; prev_s++) {
					if (saved_unreserved[prev_s] > 0 && selected_areas[prev_s]) {
						selected_areas[prev_s]->unreserved = saved_unreserved[prev_s];
						saved_unreserved[prev_s] = 0;
					}
				}

				if (!area) {
					/* No other areas available - insufficient space.
					 * Stop allocation with whatever was allocated. */
					incomplete_round = 1;
					for (all_s = 0; all_s < areas; all_s++) {
						area_needed[all_s] = 0;
						selected_areas[all_s] = NULL;
					}
					break;
				}
			}

			selected_areas[s] = area;
		}

		/* Determine round size (minimum of all selected areas) */
		for (s = 0; s < areas; s++) {
			if (selected_areas[s]) {
				area_size = (selected_areas[s]->count < area_needed[s]) ?
				            selected_areas[s]->count : area_needed[s];
				if (round_size == 0 || area_size < round_size)
					round_size = area_size;
			}
		}

		/* Restore any temporarily marked areas before allocating */
		for (s = 0; s < areas; s++) {
			if (saved_unreserved[s] > 0 && selected_areas[s]) {
				selected_areas[s]->unreserved = saved_unreserved[s];
			}
		}

		if (round_size == 0 || incomplete_round)
			break;  /* No more space or couldn't satisfy all stripes */

		/* Allocate round_size from all selected areas */
		for (s = 0; s < areas; s++) {
			if (selected_areas[s]) {
				if (!(seg = _allocate_segment(ah->mem, selected_areas[s], round_size)))
					return 0;

				dm_list_add(&res->allocated[s], &seg->list);
				area_needed[s] -= round_size;
				*allocated += round_size;

				/* Track source for redundancy */
				if (request->parallel_areas_separate && used_sources && !used_sources[s])
					used_sources[(*used_source_count)++] = selected_areas[s]->map;
			}
		}
	}

	return 1;
}

/*
 * Simple allocation - each area independent
 *
 * Allocates areas independently, allowing each to fragment differently.
 */
static int _allocate_simple(struct alloc_handle *ah,
			    const struct alloc_request *request,
			    struct alloc_result *res,
			    struct alloc_source **used_sources,
			    uint32_t *used_source_count,
			    uint32_t areas,
			    uint64_t per_area,
			    uint64_t *allocated)
{
	struct alloc_source *area_source;
	struct alloc_source *src;
	struct alloc_area *area;
	struct alloc_area *restore_area;
	struct alloc_segment *seg;
	uint64_t needed;
	uint64_t area_allocated;
	uint64_t to_alloc;
	uint64_t saved_unreserved_single;
	uint32_t s;
	uint32_t i;
	int source_already_used;
	int found;

	/* Simple allocation - each area independent */
	for (s = 0; s < areas; s++) {
		needed = per_area;
		area_allocated = 0;
		area_source = NULL;

		while (needed > 0) {

			if (!(area = _find_area(request->sources, needed, request)))
				/* No area found - insufficient space (not a hard error).
				 * Stop allocating this area with whatever was allocated. */
				break;

			/* Check redundancy constraint */
			if (request->parallel_areas_separate && used_sources) {
				source_already_used = 0;

				for (i = 0; i < *used_source_count; i++) {
					if (used_sources[i] == area->map) {
						source_already_used = 1;
						break;
					}
				}

				if (source_already_used) {
					saved_unreserved_single = area->unreserved;
					area->unreserved = 0;

					if (!(area = _find_area(request->sources, needed, request)))
						/* Cannot satisfy redundancy constraint - insufficient PVs.
						 * Stop allocating this area with whatever was allocated. */
						break;

					if (saved_unreserved_single > 0) {
						found = 0;
						dm_list_iterate_items(src, request->sources) {
							dm_list_iterate_items(restore_area, &src->areas) {
								if (restore_area == area) {
									restore_area->unreserved = saved_unreserved_single;
									found = 1;
									break;
								}
							}
							if (found)
								break;
						}
					}
				}
			}

			if (!area_source && area->map)
				area_source = area->map;

			to_alloc = (area->count < needed) ? area->count : needed;
			if (!(seg = _allocate_segment(ah->mem, area, to_alloc)))
				return 0;

			dm_list_add(&res->allocated[s], &seg->list);
			needed -= to_alloc;
			area_allocated += to_alloc;
		}

		if (area_source && used_sources)
			used_sources[(*used_source_count)++] = area_source;

		*allocated += area_allocated;
	}

	return 1;
}

/*
 * Perform allocation
 *
 * Implements various allocation policies.
 */
int liballoc_allocate(struct alloc_handle *ah,
		      const struct alloc_request *request,
		      struct alloc_result **result)
{
	struct alloc_result *res;
	struct alloc_source **used_sources = NULL;
	uint64_t allocated = 0;
	uint64_t per_area;
	uint32_t areas;
	uint32_t s;
	uint32_t used_source_count = 0;

	if (!ah || !request || !result)
		return 0;

	if (!request->sources || dm_list_empty(request->sources))
		return 0;

	/* Validate policy */
	if (request->alloc != ALLOC_ANYWHERE &&
	    request->alloc != ALLOC_NORMAL &&
	    request->alloc != ALLOC_CONTIGUOUS &&
	    request->alloc != ALLOC_CLING &&
	    request->alloc != ALLOC_CLING_BY_TAGS)
		return 0;

	/* Calculate extents per parallel area */
	areas = request->area_count + request->parity_count + request->log_area_count;
	if (!areas)
		areas = 1;

	per_area = request->new_extents;
	if (request->area_multiple)
		per_area = request->new_extents / request->area_multiple;

	/* Allocate result structure */
	res = dm_pool_zalloc(ah->mem, sizeof(*res) + areas * sizeof(struct dm_list));
	if (!res)
		return 0;

	res->area_count = request->area_count ? request->area_count : 1;
	res->parity_count = request->parity_count;
	res->total_area_len = per_area;

	/* Initialize area lists */
	for (s = 0; s < areas; s++)
		dm_list_init(&res->allocated[s]);

	/* Track sources used for parallel areas (for redundancy) */
	if (request->parallel_areas_separate && areas > 1) {
		/* Allocate array to track which sources have been used */
		used_sources = dm_pool_zalloc(ah->mem, areas * sizeof(struct alloc_source *));
		if (!used_sources)
			return 0;
	}

	/*
	 * Multi-area allocation with splitting requires synchronized rounds
	 * to ensure all parallel areas have identical segment layouts.
	 */
	if (areas > 1 && request->can_split) {
		if (!_allocate_multi_area(ah, request, res, used_sources, &used_source_count,
					  areas, per_area, &allocated))
			return 0;
	} else {
		if (!_allocate_simple(ah, request, res, used_sources, &used_source_count,
				      areas, per_area, &allocated))
			return 0;
	}

	res->total_extents = allocated;
	/* Update total_area_len to reflect actual allocation per area */
	if (areas > 1 && request->area_multiple)
		res->total_area_len = allocated / (request->area_count + request->parity_count);
	else if (areas > 1)
		res->total_area_len = allocated / areas;
	/* else: total_area_len already set correctly for single area */

	*result = res;

	return 1;
}

/*
 * Helper: Create allocation source
 */
struct alloc_source *alloc_source_create(struct dm_pool *mem, void *handle)
{
	struct alloc_source *src;

	if (!mem)
		return NULL;

	if (!(src = dm_pool_zalloc(mem, sizeof(*src))))
		return NULL;

	src->handle = handle;
	dm_list_init(&src->areas);
	dm_list_init(&src->tags);
	dm_list_init(&src->list);

	return src;
}

/*
 * Helper: Add area to source (in sorted order)
 */
int alloc_source_add_area(struct dm_pool *mem,
			  struct alloc_source *src,
			  uint64_t start,
			  uint64_t count,
			  void *source_handle)
{
	struct alloc_area *area;

	if (!mem || !src || !count)
		return 0;

	/* Allocate area structure */
	if (!(area = dm_pool_zalloc(mem, sizeof(*area))))
		return 0;

	area->start = start;
	area->count = count;
	area->unreserved = count;
	area->source_handle = source_handle;
	area->map = src;

	/* Insert in sorted order (largest first) */
	_insert_area_sorted(src, area);

	return 1;
}

/*
 * Helper: Create source list
 */
struct dm_list *alloc_source_list_create(struct dm_pool *mem)
{
	struct dm_list *list;

	if (!mem)
		return NULL;

	if (!(list = dm_pool_zalloc(mem, sizeof(*list))))
		return NULL;

	dm_list_init(list);

	return list;
}
