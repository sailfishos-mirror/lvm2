# liballoc Design - Standalone Allocation Library

## Core Principle

**liballoc has ZERO knowledge of PV/VG/LV structures**

The library works purely with:
- **Input**: Lists of free space areas (abstract extents)
- **Algorithm**: Allocation policies and strategies
- **Output**: Which areas were allocated and how

## Dependency Architecture

```
Tools (lvcreate, lvextend, etc.)
    ?
lib/metadata (PV/VG/LV structures)
    ?
    Translate: PV -> free_area_list
    ?
liballoc (pure allocation algorithm)
    ? depends only on
libdm + device-mapper (dm_list, dm_pool, etc.)
```

## Clean API Boundary

### What liballoc Knows:
```c
/* Generic free space area - no PV knowledge */
struct alloc_area {
    uint64_t start;           /* Starting extent number */
    uint64_t length;          /* Number of contiguous extents */
    void *handle;             /* Opaque - caller's reference */
    struct dm_list tags;      /* String tags for policy decisions */
};

/* Collection of areas from one "source" (caller interprets as PV) */
struct alloc_source {
    void *handle;             /* Opaque - caller's reference */
    struct dm_list areas;     /* List of alloc_area */
    uint64_t total_extents;
    struct dm_list tags;      /* Tags for this source */
};

/* What caller wants allocated */
struct alloc_request {
    uint32_t extents_needed;  /* Total extents to allocate */
    uint32_t parallel_areas;  /* For striping/RAID (e.g., 3 for 3-way stripe) */
    uint32_t parity_areas;    /* For RAID parity */
    alloc_policy_t policy;    /* CONTIGUOUS, CLING, NORMAL, etc. */
    struct dm_list *sources;  /* List of alloc_source */

    /* Policy-specific hints */
    struct dm_list *cling_sources;    /* For CLING: prefer these sources */
    const char *cling_tag;            /* For CLING_BY_TAGS */
    int partition_by_tags;            /* For tag partitioning */
};

/* What caller gets back */
struct alloc_result {
    uint32_t extents_allocated;
    uint32_t parallel_count;

    /* Array of parallel area allocations */
    /* For 3-way stripe: allocated[0], allocated[1], allocated[2] */
    /* Each contains list of segments that make up that stripe */
    struct dm_list *allocated[]; /* Array of dm_list, each containing alloc_segment */
};

/* One allocated segment */
struct alloc_segment {
    void *source_handle;      /* Which source (PV) this came from */
    uint64_t start;           /* Starting extent */
    uint64_t length;          /* Number of extents */
    struct dm_list list;      /* For linking in allocated[] list */
};
```

### What lib/metadata Does:

```c
/* In lib/metadata/lv_manip.c - TRANSLATION LAYER */

static struct dm_list *_pvs_to_alloc_sources(struct dm_pool *mem,
                                             struct dm_list *pvs)
{
    struct dm_list *sources = dm_list_create(mem);
    struct pv_list *pvl;

    dm_list_iterate_items(pvl, pvs) {
        struct alloc_source *src = dm_pool_zalloc(mem, sizeof(*src));

        src->handle = pvl->pv;  /* PV* stored as opaque handle */
        dm_list_init(&src->areas);
        dm_list_init(&src->tags);

        /* Convert PV segments to generic areas */
        struct pv_segment *pvseg;
        dm_list_iterate_items(pvseg, &pvl->pv->segments) {
            if (pvseg->lvseg)
                continue;  /* Not free */

            struct alloc_area *area = dm_pool_zalloc(mem, sizeof(*area));
            area->start = pvseg->pe;
            area->length = pvseg->len;
            area->handle = pvl->pv;  /* Back-reference */
            dm_list_add(&src->areas, &area->list);
        }

        /* Copy tags */
        _copy_tags(&src->tags, &pvl->pv->tags);

        dm_list_add(sources, &src->list);
    }

    return sources;
}

static int _apply_allocation(struct volume_group *vg,
                             struct lv_segment *seg,
                             struct alloc_result *result)
{
    /* Translate alloc_result back to LV segment areas */
    for (uint32_t s = 0; s < result->parallel_count; s++) {
        struct alloc_segment *aseg;
        dm_list_iterate_items(aseg, result->allocated[s]) {
            struct physical_volume *pv = aseg->source_handle;

            /* Create lv_segment_area from alloc_segment */
            set_lv_segment_area_pv(seg, s, pv, aseg->start);
        }
    }

    return 1;
}
```

## What Moved vs What Stays

### Move to liballoc:

**Core Algorithm** (from lv_manip.c):
- `_find_some_parallel_space()` -> Pure allocation logic
- `_allocate()` -> Main allocation loop
- `_alloc_parallel_area()` -> Parallel area selection
- Policy checkers: `_check_contiguous()`, `_check_cling()`, etc.

**NOT** moving to liballoc:
- `allocate_extents()` - This stays in lib/metadata as translation layer
- Anything that touches `struct physical_volume`
- Anything that touches `struct volume_group`
- Anything that touches `struct logical_volume`

### pv_map.c - Wrong First Step!

**Problem**: pv_map.c still uses `struct physical_volume *` directly!

```c
/* Current pv_map.h - WRONG for liballoc */
struct pv_map {
    struct physical_volume *pv;  /* <- This breaks abstraction! */
    struct dm_list areas;
    uint32_t pe_count;
};
```

**Should be** (in liballoc):
```c
/* Generic source map - no PV knowledge */
struct alloc_source_map {
    void *source_handle;         /* <- Opaque! */
    struct dm_list areas;        /* List of alloc_area */
    uint64_t total_extents;
    struct dm_list tags;
};
```

## Corrected Migration Plan

### Phase 1: Define Clean API (START HERE)

1. **Create liballoc/alloc_types.h**:
   ```c
   /* Pure data structures, no PV/VG/LV dependencies */
   struct alloc_area { ... };
   struct alloc_source { ... };
   struct alloc_request { ... };
   struct alloc_result { ... };
   struct alloc_segment { ... };
   ```

2. **Create liballoc/alloc.h** (public API):
   ```c
   struct alloc_handle *alloc_create(struct dm_pool *mem);
   int alloc_allocate(struct alloc_handle *ah,
                      struct alloc_request *req,
                      struct alloc_result **result);
   void alloc_destroy(struct alloc_handle *ah);
   ```

3. **Build with minimal implementation**:
   - Just the data structures
   - Stub allocation function that returns "not implemented"
   - Verify it compiles without lib/metadata dependencies

### Phase 2: Extract One Policy

1. **Implement ALLOC_ANYWHERE** first (simplest):
   - Take any free area that fits
   - No contiguity requirements
   - Pure algorithmic logic

2. **Create test in lib/metadata**:
   ```c
   /* Proof that translation layer works */
   sources = _pvs_to_alloc_sources(mem, pvs);
   result = alloc_allocate(ah, &req, sources);
   _apply_allocation(vg, seg, result);
   ```

### Phase 3: Extract Remaining Policies

- ALLOC_NORMAL
- ALLOC_CONTIGUOUS
- ALLOC_CLING
- ALLOC_CLING_BY_TAGS

Each with unit tests that don't need PV structures!

### Phase 4: Migrate Complex Algorithms

- RAID parallel allocation
- Striping logic
- Mirror allocation

## Dependencies

### liballoc can depend on:
- ? `device_mapper/libdm.h` (dm_list, dm_pool)
- ? Standard C library
- ? Basic LVM2 utilities (logging callbacks)

### liballoc CANNOT depend on:
- ? `lib/metadata/metadata.h`
- ? `lib/metadata/pv.h`
- ? `lib/metadata/vg.h`
- ? `lib/metadata/lv.h`
- ? Anything that includes the above

## Testing Strategy

### Unit Tests (NEW - liballoc can have these!)

```c
/* test/unit/alloc_test.c */
void test_alloc_anywhere(void)
{
    struct dm_pool *mem = dm_pool_create("test", 1024);
    struct alloc_handle *ah = alloc_create(mem);

    /* Create mock free areas - NO PV STRUCTURES! */
    struct alloc_source *src = create_source(mem, NULL);
    add_area(src, 0, 100);    /* 100 free extents at offset 0 */
    add_area(src, 200, 50);   /* 50 free extents at offset 200 */

    struct alloc_request req = {
        .extents_needed = 30,
        .parallel_areas = 1,
        .policy = ALLOC_ANYWHERE,
        .sources = list_of(src)
    };

    struct alloc_result *result;
    assert(alloc_allocate(ah, &req, &result) == 1);
    assert(result->extents_allocated == 30);

    /* Verify allocation */
    struct alloc_segment *seg = first_segment(result->allocated[0]);
    assert(seg->start == 0 || seg->start == 200);
    assert(seg->length == 30);

    alloc_destroy(ah);
    dm_pool_destroy(mem);
}
```

This can run WITHOUT initializing any LVM2 metadata!

## Example Usage Flow

```c
/* In tools/lvcreate.c or lib/metadata/lv_manip.c */

/* 1. Build allocation request from LVM2 structures */
struct dm_list *sources = _pvs_to_alloc_sources(cmd->mem, allocatable_pvs);

struct alloc_request req = {
    .extents_needed = extents,
    .parallel_areas = stripes,
    .parity_areas = parity,
    .policy = vg->alloc,
    .sources = sources
};

/* If CLING policy, add hints */
if (prev_lvseg) {
    req.cling_sources = _get_prev_pvs(prev_lvseg);
}

/* 2. Call pure allocation library */
struct alloc_handle *ah = alloc_create(cmd->mem);
struct alloc_result *result;

if (!alloc_allocate(ah, &req, &result)) {
    log_error("Allocation failed");
    return 0;
}

/* 3. Apply result back to LVM2 structures */
if (!_apply_allocation_to_lv(lv, result)) {
    log_error("Failed to apply allocation");
    alloc_destroy(ah);
    return 0;
}

alloc_destroy(ah);
return 1;
```

## Benefits of This Approach

1. **Unit Testable**: Can test allocation algorithms without VG/PV setup
2. **Reusable**: Could be used by other storage systems
3. **Clear Boundaries**: Translation layer is explicit
4. **Maintainable**: Allocation logic isolated from metadata
5. **Faster Development**: Mock tests run in milliseconds

## What About pv_map.c?

**Option 1**: Don't move it yet
- Keep in lib/metadata as part of translation layer
- It's the bridge between PV structures and generic areas

**Option 2**: Rewrite as translation helper
- Move to lib/metadata/alloc_translate.c
- Create generic version in liballoc without PV dependency

I recommend **Option 1** for now - pv_map.c is part of the translation layer, not the pure allocation algorithm.

---

**Conclusion**: We need to restart with clean API design, not just moving existing coupled code.
