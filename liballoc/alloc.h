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

#ifndef _LIBALLOC_H
#define _LIBALLOC_H

#include "liballoc/alloc_types.h"

/*
 * liballoc - Public API
 *
 * Main entry points for extent allocation.
 */

/*
 * Create an allocation handle
 *
 * @mem: Memory pool for allocations
 * @returns: Allocation handle or NULL on error
 *
 * The handle maintains state for one allocation operation.
 * Destroy with liballoc_destroy() when done.
 */
struct alloc_handle *liballoc_create(struct dm_pool *mem);

/*
 * Perform allocation
 *
 * @ah: Allocation handle from liballoc_create()
 * @request: Allocation parameters
 * @result: Pointer to receive result (allocated by library)
 * @returns: 1 on success, 0 on failure
 *
 * Allocates extents according to the request parameters.
 * On success, *result contains the allocated segments.
 * The result is allocated from the handle's memory pool.
 *
 * Caller is responsible for applying the allocation to their
 * data structures (e.g., LV segments).
 */
int liballoc_allocate(struct alloc_handle *ah,
		      const struct alloc_request *request,
		      struct alloc_result **result);

/*
 * Destroy allocation handle
 *
 * @ah: Allocation handle to destroy
 *
 * Frees all resources associated with the handle.
 * The memory pool passed to liballoc_create() is not destroyed.
 */
void liballoc_destroy(struct alloc_handle *ah);

/*
 * Helper: Create allocation source from areas
 *
 * @mem: Memory pool
 * @handle: Opaque handle (e.g., PV pointer)
 * @returns: New allocation source
 *
 * Creates an empty source that can have areas added to it.
 */
struct alloc_source *alloc_source_create(struct dm_pool *mem, void *handle);

/*
 * Helper: Add area to source (sorted)
 *
 * @mem: Memory pool
 * @src: Allocation source
 * @start: Starting extent
 * @count: Number of extents
 * @source_handle: Opaque handle for this area (e.g., PV pointer)
 * @returns: 1 on success, 0 on error
 *
 * Adds a free area to the source. Areas are automatically sorted by size
 * (largest first) for efficient ALLOC_NORMAL allocation.
 */
int alloc_source_add_area(struct dm_pool *mem,
			  struct alloc_source *src,
			  uint64_t start,
			  uint64_t count,
			  void *source_handle);

/*
 * Helper: Create list of sources
 *
 * @mem: Memory pool
 * @returns: Empty dm_list for sources
 *
 * Convenience function to create a source list.
 */
struct dm_list *alloc_source_list_create(struct dm_pool *mem);

#endif /* _LIBALLOC_H */
