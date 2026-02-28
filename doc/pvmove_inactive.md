# PVMOVE Design Document

## How PVMOVE Works

PVMOVE relocates physical extents from one PV to another while LVs remain
accessible.  The mechanism uses a dm-mirror layer inserted under the LV:

```
Before:
  vg-lv
    `-- PV1(PE0..9) | PV2(PE0..4) | PV3(PE0..2)

During pvmove of PV2 -> PV4:
  vg-lv
    |-- identity(PV1)
    |-- mirror(PV2 -> PV4)   <- dm-mirror copies + intercepts writes
    `-- identity(PV3)

After:
  vg-lv
    `-- PV1 | PV4 | PV3
```

Writes to the moving region are intercepted by dm-mirror which ensures
both source and destination are updated.  When copy completes, the mirror
is collapsed and metadata updated to point to the new PV.

### Key Source Files

| File | Role |
|------|------|
| `tools/pvmove.c` | Command entry, LV selection, mirror setup |
| `tools/pvmove_poll.c` | Polling completion, metadata updates, cleanup |
| `tools/polldaemon.c` | Generic polling loop (shared with lvconvert) |
| `lib/metadata/lv_manip.c` | Segment insertion, layer creation, chunking |
| `lib/metadata/mirror.c` | Mirror manipulation, `refresh_pvmoved_lvs()` |
| `lib/activate/activate.c` | Activation with `track_pvmove_deps` control |
| `lib/activate/dev_manager.c` | Device tree building, pvmove dependency tracking |
| `lib/metadata/lv.c` | `lv_lock_holder()` - lock holder resolution |

## Current Implementation

### Operation and Lock Flow

```
SETUP  (pvmove <pv> [-n <lv>])
  process_each_pv(..., READ_FOR_UPDATE)
    VG EX lock acquired (lvmlockd for shared VGs, file lock otherwise)
    _pvmove_setup_single()
      [shared VG, new pvmove]
        lockd_lv(lv, "ex", PERSISTENT)       -- EX persistent lock on named LV
        lockd_init_lv_args(lv_mirr)           -- allocate lock space for pvmove0
        lockd_lv(lv_mirr, "ex", PERSISTENT)  -- EX persistent lock on pvmove0
      [per bottom-level LV on source PV]
        lockd_query_lv(holder, &ex, &sh)     -- skip sh-locked (multi-node) LVs
        _insert_pvmove_mirrors()              -- insert mirror segs in metadata
      _update_metadata()
        lv_update_and_reload(active_lv)      -- suspend/resume if any LV active
        OR vg_write/vg_commit                -- metadata only if all LVs inactive
        activate_lv(lv_mirr)                 -- start data copy
      [shared VG, new pvmove]
        lockd_lv(lv, "un")                   -- release named LV lock;
                                             -- LOCKED flag in metadata protects it
    VG lock released on return

POLLING  (lvmpolld, one call per completed section)
  pvmove_update_metadata()
    lv_update_and_reload(lv_mirr)            -- advance mirror to next section

FINISH  (last section complete or --abort)
  pvmove_finish()
    _detach_pvmove_mirror()                  -- remove mirror leg
    lv_update_and_reload(lv_mirr)            -- replace with error target
    refresh_pvmoved_lvs()                   -- suspend+resume all moved LVs
    deactivate_lv(lv_mirr)
    lv_remove(lv_mirr)
      lockd_lvremove_done()                  -- unlock pvmove0, queue lock free
    vg_write/vg_commit                       -- write final LV locations
    lockd_free_removed_lvs()                 -- free pvmove0 lock space in lvmlockd

RESUME  (pvmove <pv> when pvmove already in progress, e.g. after crash)
  _pvmove_setup_single() detects existing lv_mirr via find_pvmove_lv()
    lockd_lv(lv_mirr, "ex", PERSISTENT)     -- re-acquire EX lock on pvmove0
    activate_lv(lv_mirr)                     -- LOCKED flag protects named LVs
    -> continues into POLLING
```

### Segment Chunking

Large segments are split before mirroring via `pvmove_max_segment_size_mb`
configuration (`lib/metadata/lv_manip.c`).  This prevents mirroring
excessively large amounts of data in a single operation and provides
better progress granularity.

```
activation {
    pvmove_max_segment_size_mb = 10240  # 10 GiB chunks
}
```

### Lock Holder Resolution

`lv_lock_holder()` (`lib/metadata/lv.c`) recursively finds the LV
that holds the lock.  Special cases:
- COW snapshots -> follow to origin
- Thin pools -> pool is the lock holder.  In clustered VGs, only
  the pool holds a cluster lock; thin volumes share it.
- pvmove0 -> follows `pvmove_source_seg` back to the participating LV

### DM Suspend Ordering

`lv_update_and_reload()` suspends the LV's full dependency tree before
loading new tables, then resumes.  This is required for atomic table
transitions so in-flight I/O sees consistent targets.

### track_pvmove_deps

`track_pvmove_deps` in `struct dev_manager` controls whether the
device-tree builder walks `segs_using_this_lv` upward from pvmove0 to
discover all participating LVs.

- **Activation/suspend**: `track_pvmove_deps = 1` -- full walk needed
  to load mirror tables for all participants atomically.
- **Deactivation**: `track_pvmove_deps = 0` -- upward walk suppressed.
  Kernel deps walk provides the correct dependency hierarchy at level 1,
  where open_count > 0 safely prevents premature deactivation of pvmove0.

### Inactive LV Handling (branch `dev-zkabelac-pvmove-inactive`)

pvmove inserts mirror segments in metadata for ALL affected LVs
regardless of their activation state.  Inactive LVs are NOT activated
to install mirrors -- the mirror takes effect when the LV is next
activated.  The pvmove mirror LV (pvmove0) is always activated to
perform the actual data copy.

Key implementation points in `tools/pvmove.c`:

- `_create_pvmove_lv()` and `_remove_pvmove_lv()` handle the
  pvmove0 mirror LV exclusively -- participating LVs are not touched.
- `_update_metadata()` iterates `lvs_changed` to find the first
  active LV for `lv_update_and_reload()`.  If no LV is active, does
  `vg_write()` + `vg_commit()` without suspend/resume (no kernel
  tables to reload).

### Completion: suspend+resume for Table Reload

`refresh_pvmoved_lvs()` (`lib/metadata/mirror.c`) uses
`lv_refresh_suspend_resume()` rather than `activate_lv()`.  After
the pvmove layer is removed, component LVs (e.g. `_tmeta`) still
have kernel tables pointing to the old pvmove device.  suspend+resume
forces DM table reload from committed metadata.  Lock holders are
deduplicated so multiple component LVs sharing the same holder do not
trigger redundant refreshes.

### Deactivation During Active pvmove

`lvchange -an LV` during active pvmove correctly deactivates only
the requested LV.  pvmove0 and other participating LVs remain active
and pvmove continues.

Two fixes in `lib/activate/activate.c` and `lib/activate/dev_manager.c`:

1. `_lv_deactivate()` sets `track_pvmove_deps = 0` -- prevents the
   upward walk from pvmove0 to all participating LVs via
   `segs_using_this_lv`.

2. In `_add_lv_to_dtree()` segment area loop, pvmove area LVs are
   skipped during DEACTIVATE/CLEAN actions (pvmove LV treated as a
   PV boundary).  The kernel deps walk already places pvmove0 at
   level 1 (child of the deactivating LV), where open_count > 0
   safely prevents deactivation.  ACTIVATE/PRELOAD/SUSPEND still
   do the full segment walk for atomic table reload.

### vgchange -an With Active pvmove

`vgchange -an` checks for active pvmove LVs before starting
deactivation (`tools/vgchange.c`, `vgchange_activate()`):

- **No force flag**: report error, direct user to `pvmove --abort`.
- **-f**: prompt before interrupting pvmove.
- **-ff or -f -y**: warn and interrupt without prompting.

The check runs before the deactivation loop.  If placed after,
deactivating the last visible LV drops pvmove's open count to zero,
allowing `dm_tree_deactivate_children` to silently remove pvmove0.

Interrupted pvmove leaves mirror metadata intact; pvmove can be
restarted from the last completed segment.

### Force-Removal of pvmove-Locked LVs

After `vgchange -ff -an` interrupts a pvmove, metadata contains
LOCKED LVs with mirror segments but no active DM devices.
`lvremove -f` (DONT_PROMPT) removes them with a warning.

`lv_remove_single()` (`lib/metadata/lv_manip.c`):

1. Detects the pvmove LV the LOCKED participant references
   (AREA_LV with PVMOVE flag) before removing the participant.
2. After `lv_remove()` frees the participant's segments (removing
   its reference from `pvmove_lv->segs_using_this_lv`), checks
   whether pvmove_lv is now unreferenced.
3. If so, calls `lv_remove()` on pvmove_lv to free PV extents.
   Calls `lockd_free_lv_queue()` first for shared VGs.

Works for both MIRROR_BY_SEG (AREA_PV in pvmove segments) and
MIRROR_BY_SEGMENTED_LV (mimage sub-LVs cascade via lv_is_mirror_image).

### Cluster Lock for pvmove LV in Shared VGs

pvmove0 actively modifies data but being a hidden LV it would not
normally acquire a cluster lock.  If all visible LVs are deactivated,
no lock holder would remain and other cluster nodes would be unaware
pvmove is running.

- `lockd_lv_uses_lock()` returns 1 for pvmove LVs (before the
  visibility check) -- pvmove LVs acquire cluster locks.
- `lockd_init_lv_args()` is called after pvmove0 creation in
  `_set_up_pvmove_lv()` for shared VGs.
- Lock space is freed via `lockd_free_lv_queue()` in both the normal
  completion path (`pvmove_finish()` in `pvmove_poll.c`) and the
  force-removal path (`lv_remove_single()`).

### Named LV Lock Release After LOCKED

In shared VGs, the named LV lock is released immediately after
`_update_metadata()` commits the LOCKED flag to metadata.  The
LOCKED flag in VG metadata provides the protection -- other nodes
see LOCKED and refuse activation when pvmove0 is not active locally.
This avoids holding a persistent EX lock on the named LV for the
entire duration of the data copy.

On resume (crash recovery), only the pvmove0 lock is re-acquired.
The named LV's LOCKED flag is already in metadata from the original
setup.

### Test Lock Injection (lvmlockd --test mode)

`lvmlockctl --set-lock` injects lock state into lvmlockd's daemon_test
mode for testing without real DLM/sanlock infrastructure.

Two modes of injection:

- **Local** (`--set-lock --lock-mode {sh|ex|un}`): sets `r->mode` and
  all `lk->mode` entries on the resource.  UN frees all lock entries.
  Visible in `lvmlockctl --info` output.

- **Remote** (`--set-lock --lock-mode {sh|ex|un} --remote`): sets
  `r->test_remote_ex` or `r->test_remote_sh` flags on the resource
  without creating any `struct lock` entries.  Simulates another
  cluster node holding a lock.  The `lm_lock_*()` functions check
  these flags when `daemon_test` is set:
  - No remote flags: lock succeeds (return 0)
  - SH request + remote SH only: compatible (return 0)
  - All other combinations: return -EAGAIN

  Local UN (`--lock-mode un` without `--remote`) also clears
  `test_remote_ex` and `test_remote_sh`, matching the behavior of
  real lock managers where UN releases all lock state.

## Implementation Plan

### Phase 1: Skip Forced Activation -- DONE

1. Insert mirror segments in metadata for all affected LVs -- unchanged.
2. Remove force-activation of inactive LVs -- done: `d7ec8e56a`,
   `0aacdc94b`.
3. Remove separate `activate_lvs()` call -- done: `d7ec8e56a`.
4. Fix `_update_metadata()` for first-LV-inactive case -- done:
   `0aacdc94b` iterates `lvs_changed` for first active LV.
5. Remove exclusive handling -- done: `d7ec8e56a`.
6. Poll pvmove LV; track LVs that become active during poll -- open.
7. Decide completion strategy for LVs that stayed inactive -- open
   (see Open Questions).

### Phase 2: Deactivation During pvmove -- DONE

1. Fix `_lv_deactivate()` to not track pvmove deps -- done: `639888fcb`.
2. Fix deactivation tree: treat pvmove LV as PV boundary in segment
   walk -- done: `639888fcb`.
3. Fix `refresh_pvmoved_lvs()`: replace `activate_lv()` with
   `lv_refresh_suspend_resume()` -- done: `b23821f49`.
4. vgchange -an with active pvmove: error + force flags -- done:
   `ef7be5fa2`, `219548a77`.
5. Force-removal of pvmove-locked LVs after interruption -- done:
   `d90948536`, `d4c1b3959`.

### Phase 3: Cluster Lock for pvmove LV -- DONE

1. `lockd_lv_uses_lock()` returns 1 for pvmove LVs -- done: `d7ead8483`.
2. `lockd_init_lv_args()` after pvmove0 creation in shared VGs -- done:
   `d7ead8483`.
3. Lock space freed on pvmove LV removal -- done: `c879f0011`.
4. Named LV lock released after LOCKED committed to metadata -- done.
5. Resume path only locks pvmove0 (LOCKED flag protects named LVs) -- done.
6. lvmlockd test lock injection: --set-lock --remote with
   test_remote_ex/test_remote_sh flags -- done.
7. Verification with lvmlockd --test environment -- done
   (`test/shell/lvmlockd-pvmove-locks.sh`).

### Phase 4: Internalize track_pvmove_deps -- open

Remove `track_pvmove_deps` parameter from `dev_manager_create()`.
Tree builder determines pvmove tracking internally based on LV role.

### Phase 5: Polling Lock Optimization -- open

SH->EX lock upgrade in polling loop.

### Phase 6: Clustered VG Support -- open

Add pre-check: abort if any participating LV is active on another
node but not locally.  Uses `lockd_lv()` / lock state to detect
remote activation.  pvmove must run on the node where LVs are active.

## Test Coverage

`test/shell/pvmove-inactive.sh`:

Tests 1-4 use `-n` and run in both clustered and non-clustered configs.
Tests 5-8 move all LVs from a PV without naming them; non-clustered only.

- Test 1: pvmove -n moves only named inactive LV; other LVs not touched
- Test 2: pvmove abort with background daemon (-i +100 to delay poll)
- Test 3: Deactivate unrelated LV while pvmove of named LV is in progress
- Test 4: vgchange -an fails with pvmove; vgchange -ff -an
          interrupts and deactivates
- Test 5: Mix of active and inactive LVs on source PV
- Test 6: All participating LVs inactive
- Test 7: Inactive LV first in segment order
- Test 8: Inactive thin-pool sub-LV on source PV

`test/shell/lvmlockd-pvmove-locks.sh` (requires LVM_TEST_LVMLOCKD_TEST):

- Test 1: lvmlockctl --set-lock round-trip: inject SH/EX/UN modes,
          verify --info output; --remote EX blocks activation
- Test 2: Activation of LOCKED LV refused when pvmove0 absent locally;
          persistent locks survive daemon death; pvmove restartable after
          remote lock release
- Test 3: pvmove --abort releases pvmove0 lock space (lockd_lvremove_done
          + lockd_free_removed_lvs path)
- Test 4: (pending unnamed pvmove support in shared VG -- FIXME in pvmove.c)
- Test 5: Normal pvmove completion releases pvmove0 lock space
- Test 6: Named pvmove refused when remote EX held on named LV; clears
          after lock release and pvmove succeeds

## Open Questions

1. **Completion for LVs that stayed inactive throughout pvmove**:
   - Option A: activate mirror layers at end to force sync, then collapse
   - Option B: leave mirror segments; sync happens on next activation
   - Option C: report incomplete, require admin to activate or re-run
   Current code does Option B implicitly.

2. **LV activated during pvmove poll loop**:
   - LV activates through mirror layer (metadata already has mirrors)
   - Poll loop must detect new active mirrors and start tracking them
   - May need to re-scan active state each poll iteration

3. **Progress reporting when some LVs are inactive**:
   - Report active mirror progress as today
   - Additionally report count of inactive LVs with pending mirrors

4. **Interaction with dmeventd monitoring**:
   - dmeventd monitors active mirrors; need to ensure it picks up
     mirrors for LVs that become active mid-pvmove

5. **Phase 4 (track_pvmove_deps internalization)**: requires careful
   audit to ensure ACTIVATE/SUSPEND paths are not broken by removing
   the external parameter.
