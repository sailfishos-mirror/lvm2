# liballoc Usage Example

This document shows how `lib/metadata` will use liballoc for allocation.

## Key Principle

**liballoc types are the canonical definitions**. Both the library and LVM2 use them directly - no translation needed.

## Current State (Before Migration)

```c
/* In lib/metadata/lv_manip.c - CURRENT CODE */

struct pv_map {
    struct physical_volume *pv;  /* Direct PV reference */
    struct dm_list areas;
    uint32_t pe_count;
};

struct pv_area {
    struct pv_map *map;
    uint32_t start;
    uint32_t count;
    uint32_t unreserved;
    struct dm_list list;
};

/* Build pv_maps from PVs */
struct dm_list *pvms = create_pv_maps(mem, vg, allocatable_pvs);

/* Allocate using internal algorithm */
struct alloc_handle *ah = _alloc_init(...);
int success = _allocate(ah, ...);
```

## Future State (After Migration)

```c
/* In lib/metadata/lv_manip.c - FUTURE CODE */

#include "liballoc/alloc.h"  /* Uses liballoc types directly */

/* Build allocation sources from PVs */
static struct dm_list *_build_alloc_sources(struct dm_pool *mem,
                                             struct volume_group *vg,
                                             struct dm_list *pvs)
{
    struct dm_list *sources;
    struct pv_list *pvl;

    if (!(sources = dm_pool_zalloc(mem, sizeof(*sources))))
        return NULL;

    dm_list_init(sources);

    dm_list_iterate_items(pvl, pvs) {
        struct physical_volume *pv = pvl->pv;
        struct alloc_source *src;
        struct pv_segment *pvseg;

        /* Skip non-allocatable PVs */
        if (!(pv->status & ALLOCATABLE_PV))
            continue;

        /* Create source using liballoc type */
        if (!(src = dm_pool_zalloc(mem, sizeof(*src))))
            return NULL;

        src->handle = pv;  /* Store PV as opaque handle */
        dm_list_init(&src->areas);
        dm_list_init(&src->tags);
        src->pe_count = pv->pe_count;

        /* Convert PV segments to alloc_areas */
        dm_list_iterate_items(pvseg, &pv->segments) {
            struct alloc_area *area;

            /* Skip allocated segments */
            if (pvseg->lvseg)
                continue;

            /* Create area using liballoc type */
            if (!(area = dm_pool_zalloc(mem, sizeof(*area))))
                return NULL;

            area->start = pvseg->pe;
            area->count = pvseg->len;
            area->unreserved = pvseg->len;
            area->source_handle = pv;
            area->map = src;

            dm_list_add(&src->areas, &area->list);
        }

        /* Copy PV tags */
        if (!str_list_dup(mem, &src->tags, &pv->tags))
            return NULL;

        dm_list_add(sources, &src->list);
    }

    return sources;
}

/* New allocation interface */
int lv_extend_new(struct logical_volume *lv,
                  const struct segment_type *segtype,
                  uint32_t stripes,
                  uint32_t stripe_size,
                  uint32_t extents,
                  struct dm_list *allocatable_pvs,
                  alloc_policy_t alloc)
{
    struct dm_pool *mem = lv->vg->vgmem;
    struct alloc_handle *ah;
    struct alloc_request req = {0};
    struct alloc_result *result;
    struct lv_segment *seg;

    /* Build sources from PVs - uses liballoc types */
    req.sources = _build_alloc_sources(mem, lv->vg, allocatable_pvs);
    if (!req.sources)
        return_0;

    /* Set up request - uses liballoc types */
    req.new_extents = extents;
    req.area_count = stripes;
    req.parity_count = 0;
    req.alloc = alloc;

    /* Call library */
    ah = alloc_create(mem);
    if (!ah)
        return_0;

    if (!alloc_allocate(ah, &req, &result)) {
        alloc_destroy(ah);
        return_0;
    }

    /* Apply allocation result to LV */
    if (!_apply_alloc_result(lv, segtype, result)) {
        alloc_destroy(ah);
        return_0;
    }

    alloc_destroy(ah);
    return 1;
}

/* Apply allocation result to LV segments */
static int _apply_alloc_result(struct logical_volume *lv,
                                const struct segment_type *segtype,
                                struct alloc_result *result)
{
    struct lv_segment *seg;
    uint32_t s;

    /* Create LV segment */
    seg = alloc_lv_segment(segtype, lv, lv->le_count,
                           result->total_area_len, ...);
    if (!seg)
        return_0;

    /* Map each parallel area to segment */
    for (s = 0; s < result->area_count; s++) {
        struct alloc_segment *aseg;

        /* Result uses liballoc types */
        dm_list_iterate_items(aseg, &result->allocated[s]) {
            struct physical_volume *pv = aseg->source_handle;

            /* This creates the LV segment area */
            if (!set_lv_segment_area_pv(seg, s, pv, aseg->start_extent))
                return_0;
        }
    }

    dm_list_add(&lv->segments, &seg->list);
    lv->le_count += result->total_area_len;

    return 1;
}
```

## Data Flow

```
1. LVM2 Tool (lvcreate)
   ?
2. lib/metadata/lv_manip.c::lv_extend_new()
   ?
   Build alloc_source from PV:
   - Iterate PV segments
   - Create alloc_area for each free segment
   - Store PV* as opaque handle
   ?
3. liballoc/alloc.c::alloc_allocate()
   ?
   Pure algorithm:
   - Works with alloc_area, alloc_source
   - Doesn't know what "handle" points to
   - Returns alloc_result with alloc_segment list
   ?
4. lib/metadata/lv_manip.c::_apply_alloc_result()
   ?
   Create LV segments:
   - Cast handle back to PV*
   - Create lv_segment_area
   - Link to LV
```

## Type Evolution

### From pv_map -> alloc_source

```c
/* OLD (lib/metadata/pv_map.h) */
struct pv_map {
    struct physical_volume *pv;  /* <- Breaks abstraction */
    struct dm_list areas;
    uint32_t pe_count;
};

/* NEW (liballoc/alloc_types.h) */
struct alloc_source {
    void *handle;                /* <- Generic! */
    struct dm_list areas;
    uint64_t pe_count;
    struct dm_list tags;         /* <- Added for policies */
    struct dm_list list;
};
```

### From pv_area -> alloc_area

```c
/* OLD */
struct pv_area {
    struct pv_map *map;
    uint32_t start;
    uint32_t count;
    uint32_t unreserved;
    struct dm_list list;
};

/* NEW */
struct alloc_area {
    uint64_t start;             /* <- Wider type */
    uint64_t count;
    uint64_t unreserved;
    void *source_handle;        /* <- Back-reference */
    struct alloc_source *map;   /* <- Generic type */
    struct dm_list list;
};
```

## Migration Strategy

### Phase 1: Side-by-side (SAFE)

Keep both old and new:
- pv_map.c stays in lib/metadata
- New code uses liballoc types
- Old code gradually migrated

```c
/* Existing function keeps working */
struct alloc_handle *allocate_extents(...) {
    /* OLD: uses pv_map */
    struct dm_list *pvms = create_pv_maps(...);
    return _alloc_init(...);
}

/* New function added */
struct alloc_handle *allocate_extents_v2(...) {
    /* NEW: uses alloc_source */
    struct dm_list *sources = _build_alloc_sources(...);
    return alloc_create(...);
}
```

### Phase 2: Gradual Replacement

Update one call site at a time:
- lvcreate -> use new API
- lvextend -> use new API
- lvconvert -> use new API

### Phase 3: Remove Old Code

Once all callers migrated:
- Remove allocate_extents() (old function)
- Remove pv_map.c
- Rename allocate_extents_v2() -> allocate_extents()

## Benefits

1. **LVM2 owns the mapping**: PV -> alloc_source conversion in lib/metadata
2. **Library is pure**: No PV/VG/LV knowledge
3. **Same types**: No duplication, single source of truth
4. **Unit testable**: Can test alloc.c without PV structures
5. **Incremental**: Can migrate one function at a time

## Unit Test Example

```c
/* test/unit/alloc_test.c - No PV structures needed! */

void test_simple_allocation(void)
{
    struct dm_pool *mem = dm_pool_create("test", 1024);
    struct alloc_handle *ah;
    struct alloc_source *src;
    struct alloc_area *area;
    struct alloc_request req = {0};
    struct alloc_result *result;

    /* Create mock source - no PV needed! */
    src = dm_pool_zalloc(mem, sizeof(*src));
    src->handle = (void*)0xDEADBEEF;  /* Mock handle */
    dm_list_init(&src->areas);
    dm_list_init(&src->tags);
    src->pe_count = 100;

    /* Add free area */
    area = dm_pool_zalloc(mem, sizeof(*area));
    area->start = 0;
    area->count = 100;
    area->unreserved = 100;
    area->source_handle = src->handle;
    area->map = src;
    dm_list_add(&src->areas, &area->list);

    /* Create source list */
    struct dm_list *sources = dm_pool_zalloc(mem, sizeof(*sources));
    dm_list_init(sources);
    dm_list_add(sources, &src->list);

    /* Request allocation */
    req.sources = sources;
    req.new_extents = 50;
    req.area_count = 1;
    req.alloc = ALLOC_ANYWHERE;

    /* Allocate */
    ah = alloc_create(mem);
    assert(ah != NULL);

    /* NOTE: This will fail with stub implementation */
    /* But shows how testing will work */
    int success = alloc_allocate(ah, &req, &result);

    alloc_destroy(ah);
    dm_pool_destroy(mem);

    /* With real implementation, would verify:
     * - result->total_extents == 50
     * - result->allocated[0] contains one segment
     * - segment start == 0, length == 50
     */
}
```

This test has **zero** dependencies on LVM2 metadata!

---

**Summary**: liballoc defines the types, LVM2 uses them. Clean, simple, testable.
