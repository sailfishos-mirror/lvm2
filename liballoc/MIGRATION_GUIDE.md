# liballoc Migration Guide

This guide explains how to migrate LVM2 code from the old allocation system to the new liballoc library.

## Overview

The liballoc library is a standalone, testable allocation library that replaces LVM2's embedded allocation code. It has been successfully integrated into the LVM2 build system and is ready for gradual migration.

## Architecture

```
OLD System:                          NEW System:
+---------------------+             +---------------------+
|  allocate_extents() |             |allocate_extents_    |
|                     |             |  liballoc()         |
|  [in lv_manip.c]    |             |  [in alloc_bridge.c]|
+----------+----------+             +----------+----------+
           |                                   |
           ?                                   ?
    +--------------+                  +------------------+
    | create_pv_   |                  |build_alloc_      |
    |   maps()     |                  | sources_from_pvs |
    |              |                  +--------+---------+
    | pv_map.c     |                           |
    +------+-------+                           ?
           |                          +-----------------+
           ?                          |  liballoc_      |
    +--------------+                  |   allocate()    |
    |_allocate()   |                  |                 |
    | [complex     |                  | [liballoc/      |
    |  allocation  |                  |  alloc.c]       |
    |  algorithm]  |                  +--------+--------+
    +--------------+                           |
                                               ?
                                      +--------------------+
                                      | Policy-specific    |
                                      | allocation funcs:  |
                                      | - _find_area_      |
                                      |     anywhere()     |
                                      | - _find_area_      |
                                      |     normal()       |
                                      | - _find_area_      |
                                      |     contiguous()   |
                                      | - _find_area_      |
                                      |     cling()        |
                                      +--------------------+
```

## Key Components

### 1. liballoc Library (liballoc/)

Standalone allocation library with:
- **alloc_types.h**: Canonical type definitions used by both library and LVM2
- **alloc.h/alloc.c**: Main allocation algorithms (NORMAL, CONTIGUOUS, CLING, ANYWHERE)
- **No LVM2 dependencies**: Only depends on libdm (dm_list, dm_pool)

### 2. Bridge Layer (lib/metadata/alloc_bridge.c)

Translation layer between LVM2 and liballoc:
- `build_alloc_sources_from_pvs()`: Converts PV list -> alloc_source list
- `apply_alloc_result_to_lv()`: Converts alloc_result -> LV segments
- `allocate_extents_liballoc()`: **Main migration function** - drop-in replacement for `allocate_extents()`

### 3. Old Allocation System (lib/metadata/lv_manip.c)

Legacy code that will be gradually replaced:
- `allocate_extents()`: Old allocation entry point
- `_allocate()`: Complex allocation algorithm
- `create_pv_maps()`: Creates pv_map/pv_area structures

## Migration Path

### Step 1: Drop-in Replacement (CURRENT STATUS)

You can now replace `allocate_extents()` calls with `allocate_extents_liballoc()`:

**Before:**
```c
#include "lib/metadata/lv_alloc.h"

struct alloc_handle *ah;
ah = allocate_extents(vg, lv, segtype, stripes, mirrors, log_count,
                      region_size, extents, allocatable_pvs,
                      alloc, approx_alloc, parallel_areas);
if (!ah) {
    log_error("Allocation failed");
    return 0;
}

/* Use ah... */
alloc_destroy(ah);
```

**After:**
```c
#include "lib/metadata/alloc_bridge.h"  /* NEW */

struct alloc_handle *ah;
ah = allocate_extents_liballoc(vg, lv, segtype, stripes, mirrors, log_count,
                                region_size, extents, allocatable_pvs,
                                alloc, approx_alloc, parallel_areas);
if (!ah) {
    log_error("Allocation failed");
    return 0;
}

/* Use ah exactly the same way... */
alloc_destroy(ah);  /* Still works! */
```

**Key Points:**
- Identical function signature
- Same return type (struct alloc_handle *)
- Compatible with existing code
- No changes needed to callers

### Step 2: Gradual Callsite Migration (IN PROGRESS)

Identify callsites of `allocate_extents()`:
```bash
grep -r "allocate_extents(" lib/ tools/ --include="*.c"
```

Migrate them one by one, testing each change:
1. Change include from `lv_alloc.h` to `alloc_bridge.h`
2. Change function name from `allocate_extents` to `allocate_extents_liballoc`
3. Test thoroughly
4. Commit

### Step 3: Retire Old Code (FUTURE)

Once all callsites migrated:
1. Remove `allocate_extents()` from lv_manip.c
2. Remove `create_pv_maps()` and pv_map.c
3. Remove old `struct alloc_handle` definition
4. Clean up old allocation code

## Testing Strategy

### Unit Tests
liballoc is designed to be unit-testable because it has no LVM2 dependencies:

```c
/* test/unit/liballoc_test.c (future) */
#include "liballoc/alloc.h"
#include <assert.h>

void test_alloc_normal(void)
{
    struct dm_pool *mem = dm_pool_create("test", 1024);
    struct alloc_handle *ah = liballoc_create(mem);

    /* Create mock sources */
    struct dm_list *sources = alloc_source_list_create(mem);
    struct alloc_source *src = alloc_source_create(mem, NULL);
    alloc_source_add_area(mem, src, 0, 100, NULL);
    dm_list_add(sources, &src->list);

    /* Build request */
    struct alloc_request req = {
        .new_extents = 50,
        .area_count = 1,
        .sources = sources,
        .alloc = ALLOC_NORMAL,
        .can_split = 1
    };

    /* Allocate */
    struct alloc_result *result;
    assert(liballoc_allocate(ah, &req, &result));
    assert(result->total_extents == 50);

    liballoc_destroy(ah);
    dm_pool_destroy(mem);
}
```

### Integration Tests
Use existing LVM2 test suite:
```bash
make check  # Run full test suite
make check_local  # Run local tests
```

### Comparison Testing
Run both old and new allocators on same input, verify identical output:
```c
/* Temporary debugging code */
struct alloc_handle *ah_old = allocate_extents(...);
struct alloc_handle *ah_new = allocate_extents_liballoc(...);
compare_allocation_results(ah_old, ah_new);  /* Should match! */
```

## Benefits of Migration

1. **Testability**: liballoc can be unit-tested without full LVM2 environment
2. **Clarity**: Clean separation between allocation algorithm and LVM2 metadata
3. **Reusability**: Other projects can use liballoc (e.g., Stratis, bcache tools)
4. **Maintainability**: Simpler code structure, easier to understand
5. **Performance**: Can optimize liballoc independently
6. **Future**: Can add new allocation policies without touching LVM2 core

## Current Status (2025)

? **Completed:**
- liballoc library implemented with all major allocation policies
- Build system integration (autoconf/automake)
- Bridge layer with PV<->source translation
- `allocate_extents_liballoc()` wrapper function
- All function names unique across codebase
- Full LVM2 builds successfully with liballoc

? **In Progress:**
- Documentation and migration guides
- Identifying callsites for migration

? **Future:**
- Gradual migration of callsites
- Unit test suite for liballoc
- RAID parity support
- CLING_BY_TAGS policy
- Performance optimization

## Getting Help

- Read `liballoc/USAGE_EXAMPLE.md` for API examples
- Check `liballoc/alloc.h` for detailed API documentation
- See `lib/metadata/alloc_bridge.c` for translation examples
- Review existing allocation code in `lib/metadata/lv_manip.c`

## Implementation Notes

### Memory Management
- liballoc uses dm_pool for all allocations
- Caller provides pool to `liballoc_create()`
- Caller destroys pool after `liballoc_destroy()`
- No malloc/free within liballoc

### Opaque Handles
- liballoc uses `void*` for PV references
- Bridge layer casts between `struct physical_volume*` and `void*`
- liballoc never dereferences these pointers

### Area Sorting
- Free areas maintained in descending size order (largest first)
- Critical for ALLOC_NORMAL efficiency
- Insertion is O(n), allocation is O(1) for first fit

### Policy Dispatch
- `_find_area()` dispatcher routes to policy-specific functions
- Easy to add new policies
- Each policy encapsulated in separate function

## Example Migration: lvcreate

**File: tools/lvcreate.c**

**Before:**
```c
if (!(ah = allocate_extents(vg, lv, segtype,
                             lp->stripes, lp->mirrors, log_count,
                             lp->region_size, extents,
                             allocatable_pvs, lp->alloc, 0, NULL)))
    return_0;
```

**After:**
```c
if (!(ah = allocate_extents_liballoc(vg, lv, segtype,
                                      lp->stripes, lp->mirrors, log_count,
                                      lp->region_size, extents,
                                      allocatable_pvs, lp->alloc, 0, NULL)))
    return_0;
```

Just change the function name and include!

---

**Last Updated:** 2025-10-19
**Status:** Ready for gradual migration
