# liballoc - LVM2 Allocation Library

## Overview

This library provides standalone extent allocation algorithms that work with generic free space areas. **liballoc has zero knowledge of PV/VG/LV structures** - it operates purely on abstract allocation concepts.

## Key Design Principle

**liballoc types are canonical** - Both the library and LVM2 use the same structures defined in `alloc_types.h`. There is no translation layer - LVM2 directly populates liballoc structures from its metadata and uses the results directly.

## Migration Status

### Phase 1: Clean API Design (COMPLETED)

**Goal**: Define standalone allocation types that both library and LVM2 will use.

**What was done**:
1. Created `liballoc/` directory at top level
2. Defined core types in `alloc_types.h`:
   - `struct alloc_area` - Generic free space area
   - `struct alloc_source` - Collection of areas (abstraction of PV)
   - `struct alloc_request` - What to allocate
   - `struct alloc_result` - What was allocated
   - `alloc_policy_t` - Allocation policies
3. Created public API in `alloc.h`:
   - `alloc_create()` - Create allocation handle
   - `alloc_allocate()` - Perform allocation
   - `alloc_destroy()` - Cleanup
4. Implemented stub in `alloc.c`:
   - Compiles successfully
   - No dependencies on lib/metadata
   - Returns "not implemented" for now
5. Updated build system:
   - `Makefile.in`: Added `liballoc` to SUBDIRS
   - `liballoc/Makefile.in`: Builds alloc.c

**Key Achievement**: **liballoc compiles without any lib/metadata dependencies**

### Testing Phase 1

To verify the migration worked:

```bash
# From LVM2 root directory
./configure  # or your usual configure command
make clean
make

# The build should complete successfully
# pv_map functionality should work identically to before
```

### Phase 2: Implement ALLOC_ANYWHERE (NEXT)

**Goal**: Implement simplest allocation policy to prove the design works.

**What to do**:
1. Implement `alloc_allocate()` for ALLOC_ANYWHERE policy:
   - Find any free area that fits
   - No contiguity requirements
   - Pure algorithmic logic

2. Create helper in `lib/metadata`:
   ```c
   struct dm_list *build_alloc_sources_from_pvs(
       struct dm_pool *mem,
       struct dm_list *pvs);
   ```
   - Iterates PV segments
   - Creates `alloc_source` with `alloc_area` entries
   - PV stored as opaque `handle`

3. Test with simple lvcreate case

**Success Criteria**:
- Can allocate linear LV using liballoc
- No changes to existing allocation code (runs in parallel)
- Unit test works without PV structures

### Phase 3: Remaining Policies (PLANNED)

**Goal**: Implement all allocation policies.

**Order** (simplest -> most complex):
1. ? ALLOC_ANYWHERE (Phase 2)
2. ALLOC_NORMAL - Basic space-efficient allocation
3. ALLOC_CONTIGUOUS - Must be sequential on one source
4. ALLOC_CLING - Prefer same sources as hint
5. ALLOC_CLING_BY_TAGS - Tag-based affinity

**For each policy**:
- Extract logic from lv_manip.c
- Remove PV/VG/LV dependencies
- Use generic `alloc_area`/`alloc_source`
- Add unit tests

### Phase 4: Core Algorithm Extraction (PLANNED)

**Main allocation functions to migrate**:
- `allocate_extents()` (lv_manip.c:3776) - Public entry point
- `_allocate()` (lv_manip.c:3365) - Core allocation loop
- `_find_some_parallel_space()` (lv_manip.c:3005) - Parallel area finding
- `_alloc_parallel_area()` (lv_manip.c:2111) - Single parallel area allocation

**Strategy**:
1. Keep original functions as wrappers
2. Create new `liballoc` functions with cleaner interfaces
3. Gradually migrate internal callers
4. Eventually deprecate old functions

## Current File Structure

```
liballoc/
+-- Makefile.in          # Build configuration
+-- README.md            # This document
+-- DESIGN.md            # Architecture and principles
+-- USAGE_EXAMPLE.md     # How LVM2 will use the library
+-- alloc_types.h        # Core type definitions (canonical!)
+-- alloc.h              # Public API
+-- alloc.c              # Implementation (currently stub)
+-- pv_map.c             # Old file (to be removed)
+-- pv_map.h             # Old file (to be removed)

lib/metadata/
+-- pv_map.c             # Stays here (builds alloc_source from PV)
+-- pv_map.h             # Stays here
+-- lv_manip.c           # Will use liballoc types & API
```

## Design Principles

1. **Backward Compatibility**: Every change maintains existing API
2. **Incremental Migration**: One component at a time
3. **Test at Each Step**: Build must succeed after each phase
4. **Clear Separation**: Library has no knowledge of VG/LV structures (eventual goal)
5. **Pluggable Policies**: Allocation strategies as strategy pattern

## Target API (Long-term Vision)

```c
/* Allocation request */
struct alloc_request {
    uint32_t extents_needed;
    uint32_t area_count;        /* For striping/RAID */
    uint32_t parity_count;      /* For RAID parity */
    struct dm_list *free_areas; /* List of available space */
    struct alloc_policy *policy; /* Allocation strategy */
};

/* Allocation result */
struct alloc_result {
    struct dm_list *allocated_areas[]; /* Array of area lists */
    uint32_t extents_allocated;
};

/* Main API */
struct alloc_handle *alloc_create(struct dm_pool *mem,
                                  struct alloc_request *req);
int alloc_allocate(struct alloc_handle *ah,
                   struct alloc_result **result);
void alloc_destroy(struct alloc_handle *ah);
```

## Dependencies to Remove (Eventually)

Currently, liballoc still depends on:
- `lib/misc/lib.h` (for logging, memory pools)
- `lib/metadata/metadata.h` (for PV/VG structures)

Long-term goal:
- Abstract logging via callbacks
- Use opaque handles instead of PV/VG pointers
- Depend only on libdm and basic infrastructure

## Testing Strategy

### Current (Phase 1)
- Existing LVM2 test suite validates functionality
- No unit tests yet (pv_map is tightly coupled to metadata)

### Future Phases
- Create `liballoc/test/` directory
- Mock free area lists for unit testing
- Test each allocation policy in isolation
- Performance regression tests

## Key Constraints

1. **No Breaking Changes**: Existing code must continue to work
2. **Build Order**: liballoc must build before lib (done in Makefile.in)
3. **Include Paths**: Use `liballoc/` prefix for new includes
4. **Memory Management**: Use dm_pool throughout (compatible with existing code)

## Questions for Discussion

1. Should we eventually remove the deprecated wrappers in lib/metadata/?
2. When to start writing unit tests (now or after more extraction)?
3. Should policy plugins be dynamically loadable or compile-time only?
4. How to handle the cmd_context dependency (needed for logging)?

## References

- Original allocation code: `lib/metadata/lv_manip.c` (~10,153 lines)
- PV mapping code: now in `liballoc/pv_map.c` (~228 lines)
- Existing library example: `libdaemon/` (client/server pattern)
- Allocation policy enums: `lib/display/display.c:33-37`

## Change Log

### 2025-10-19 - Phase 1 Complete
- Created liballoc directory structure
- Moved pv_map.c/h with backward-compatible wrappers
- Updated build system with proper dependencies
- Documented migration plan

---

**Status**: Phase 1 complete, ready for Phase 2
**Next Step**: Extract alloc_internal.h with core structures
**Estimated Timeline**: 4-6 months for full migration
