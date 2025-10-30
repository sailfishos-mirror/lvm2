# liballoc Migration Status

## Summary

The new `allocate_extents_liballoc()` function is ready to use as a drop-in replacement for `allocate_extents()`. This document tracks migration progress.

## Available for Migration

### Callsites Found

Total callsites of `allocate_extents()`: **8**

| File | Line | Function | Status |
|------|------|----------|--------|
| lib/metadata/lv_manip.c | 3776 | allocate_extents (definition) | ? Keep as fallback |
| lib/metadata/lv_manip.c | 4553 | lv_extend | ? Ready |
| lib/metadata/mirror.c | 1543 | _alloc_and_add_rmeta_devs_for_lv | ? Ready |
| lib/metadata/mirror.c | 1793 | _alloc_rmeta_for_lv | ? Ready |
| lib/metadata/mirror.c | 1867 | add_mirrors_to_segments | ? Ready |
| lib/metadata/mirror.c | 1931 | lv_add_mirrors | ? Ready |
| lib/metadata/raid_manip.c | 1115 | _alloc_rmeta_devs_for_lv | ? Ready |
| lib/metadata/raid_manip.c | 2610 | lv_raid_rebuild | ? Ready |

## Migration Strategy

### Phase 1: Low-Risk Callsites (RECOMMENDED START)

Start with simple, well-tested paths:

1. **lib/metadata/lv_manip.c:4553** - `lv_extend()`
   - Common operation, well tested
   - Used by `lvextend` command
   - Good first candidate

2. **lib/metadata/mirror.c:1867** - `add_mirrors_to_segments()`
   - Mirror operations are well isolated
   - Good test coverage

### Phase 2: Mirror Operations

3. **lib/metadata/mirror.c:1543** - `_alloc_and_add_rmeta_devs_for_lv()`
4. **lib/metadata/mirror.c:1793** - `_alloc_rmeta_for_lv()`
5. **lib/metadata/mirror.c:1931** - `lv_add_mirrors()`

### Phase 3: RAID Operations

6. **lib/metadata/raid_manip.c:1115** - `_alloc_rmeta_devs_for_lv()`
7. **lib/metadata/raid_manip.c:2610** - `lv_raid_rebuild()`

### Phase 4: Keep Original as Fallback

The original `allocate_extents()` at lib/metadata/lv_manip.c:3776 should be kept until all callsites are migrated and tested. It can serve as a fallback or comparison reference.

## How to Migrate a Callsite

### Example: Migrating lv_extend()

**File:** lib/metadata/lv_manip.c:4553

**Before:**
```c
if (!(ah = allocate_extents(lv->vg, lv, segtype, stripes, mirrors,
                             log_count, region_size, extents,
                             allocatable_pvs, alloc, 0, parallel_areas)))
    return_0;
```

**After:**
```c
#include "lib/metadata/alloc_bridge.h"  /* Add this include at top of file */

if (!(ah = allocate_extents_liballoc(lv->vg, lv, segtype, stripes, mirrors,
                                      log_count, region_size, extents,
                                      allocatable_pvs, alloc, 0, parallel_areas)))
    return_0;
```

**Changes Required:**
1. Add `#include "lib/metadata/alloc_bridge.h"` to includes
2. Change `allocate_extents` -> `allocate_extents_liballoc`
3. No other changes needed!

**Testing:**
```bash
# After making the change
make
make check_local  # Run local tests
make check        # Full test suite

# Manual testing
lvcreate -L 100M -n test vg
lvextend -L +50M vg/test  # Uses lv_extend internally
```

## Current Status

- ? liballoc library: Complete
- ? Build system integration: Complete
- ? Bridge layer: Complete
- ? allocate_extents_liballoc(): Complete
- ? Documentation: Complete
- ? Callsite migration: **Ready to start**
- ? Unit tests: Not yet started
- ? Retire old code: Future

## Next Steps

1. **Choose first callsite**: Recommend lib/metadata/lv_manip.c:4553 (lv_extend)
2. **Make the change**: Add include, change function name
3. **Build and test**: Verify no regressions
4. **Commit**: Small, focused commit
5. **Repeat**: Move to next callsite

## Testing Checklist

For each migrated callsite:

- [ ] Build succeeds (`make`)
- [ ] Local tests pass (`make check_local`)
- [ ] Full test suite passes (`make check`)
- [ ] Manual testing of affected commands
- [ ] No memory leaks (valgrind if available)
- [ ] Allocation results match old behavior

## Benefits Realized After Migration

After migrating all callsites:

1. **Code Reduction**: Can remove ~2000 lines from lv_manip.c
2. **Testability**: liballoc can be unit-tested separately
3. **Clarity**: Cleaner separation of concerns
4. **Reusability**: Other projects can use liballoc
5. **Maintainability**: Easier to understand and modify

## Risk Assessment

**Low Risk:**
- Simple allocations (linear LVs, basic extends)
- Well-tested code paths
- Drop-in replacement with identical API

**Medium Risk:**
- Mirror operations (complex but well-tested)
- RAID metadata allocation

**High Risk:**
- None identified - API is designed for compatibility

## Rollback Plan

If issues are found:
1. Revert the specific callsite change
2. Original `allocate_extents()` remains available
3. No impact on other migrated callsites
4. Each migration is independent

---

**Status:** Ready for migration
**Recommended First Target:** lib/metadata/lv_manip.c:4553 (lv_extend)
**Last Updated:** 2025-10-19
