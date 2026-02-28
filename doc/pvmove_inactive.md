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

All `lockd_lv()` calls are consolidated in `_pvmove_setup_single()` with
goto-based cleanup releasing both `lv_locked` and `lv_mirr_locked` on
error.

```
SETUP  (pvmove <pv> [-n <lv>])
  process_each_pv(..., READ_FOR_UPDATE)
    VG EX lock acquired (lvmlockd for shared VGs, file lock otherwise)
    _pvmove_setup_single()
      [named LV (-n)]
        _pvmove_lv_check_moveable(lv)        -- reject converting/merging/etc.
        lv_is_locked(lv) -> error            -- reject already-pvmoved LV
      [shared VG, new pvmove]
        lockd_lv(lv, "ex", PERSISTENT)       -- EX lock on named LV
        lv_create_empty("pvmove%d")           -- create pvmove0 LV
        lockd_init_lv_args(lv_mirr)           -- allocate lock space
        lockd_lv(lv_mirr, "ex", PERSISTENT)   -- EX lock on pvmove0
      _set_up_pvmove_lv()                     -- populate mirror segments
        [per bottom-level LV on source PV]
          lv_is_locked: named -> error, unnamed -> skip
          _pvmove_lv_check_holder_lock()      -- try EX lock on holder
            named + locked remotely -> error (-1)
            unnamed + locked remotely -> skip (0)
            success -> add holder to locked_lvs
          _insert_pvmove_mirrors()            -- insert mirror segs
      _update_metadata()
        lv_update_and_reload(active_lv)       -- suspend/resume if active
        OR vg_write/vg_commit                 -- metadata only if inactive
        activate_lv(lv_mirr)                  -- start data copy
      [shared VG]
        lockd_lv(lv, "un")                    -- release named LV lock;
                                              -- LOCKED flag protects it
                                              -- log_warn on failure
        for each LV in locked_lvs:
          lockd_lv(lv, "un")                  -- release holder locks
                                              -- log_warn on failure
      [error cleanup: goto out]
        lockd_lv(lv, "un") if lv_locked       -- log_warn on failure
        lockd_lv(lv_mirr, "un") if lv_mirr_locked
        for each LV in locked_lvs:
          lockd_lv(lv, "un")                  -- log_warn on failure
    VG lock released on return

POLLING  (lvmpolld, one call per completed section)
  pvmove_update_metadata()
    lv_update_and_reload(lv_mirr)             -- advance to next section

FINISH  (last section complete or --abort)
  pvmove_finish()
    _detach_pvmove_mirror()                   -- remove mirror leg
    lv_update_and_reload(lv_mirr)             -- replace with error target
    refresh_pvmoved_lvs()                     -- suspend+resume moved LVs
    [shared VG]
      for each active holder in lvs_changed (deduplicated):
        lockd_lv(holder, "ex", PERSISTENT)    -- re-acquire LV lock
        on failure: deactivate_lv(holder)     -- cannot lock -> deactivate
                                              -- log_warn on deactivate failure
    _remove_pvmove_lv()
      lv_remove(lv_mirr)
        lockd_lvremove_done()                 -- unlock pvmove0
      vg_write/vg_commit
      lockd_free_removed_lvs()                -- free lock space

RESUME  (pvmove <pv> when pvmove already in progress)
  _pvmove_setup_single() detects existing lv_mirr via find_pvmove_lv()
    _pvmove_validate_resume()                 -- find lvs_changed, check
                                              -- --name match, warn ignored args
    lockd_lv(lv_mirr, "ex", PERSISTENT)       -- re-acquire EX on pvmove0
    activate_lv(lv_mirr)                      -- LOCKED flag protects named LVs
    -> continues into POLLING

ABORT  (pvmove --abort, including initial activation failure)
  pvmove_abort_initial()                      -- called from _update_metadata
    _detach_pvmove_mirror(keep_source=1)      -- keep original PV data
    _remove_pvmove_lv()                       -- remove pvmove0 + commit
    refresh_pvmoved_lvs()                     -- reload original mappings
```

### Helper Functions in pvmove.c

| Function | Purpose |
|----------|---------|
| `_pvmove_setup_single()` | Main entry: all lock calls, resume/new branching |
| `_pvmove_validate_resume()` | Resume-path validation: find lvs_changed, name match check |
| `_set_up_pvmove_lv()` | Mirror population: iterate LVs, check holder locks, insert mirrors, add mirror legs |
| `_update_metadata()` | Write metadata, reload active LVs, activate pvmove LV |
| `_pvmove_lv_check_moveable()` | Check if LV can be pvmoved (converting, merging, writecache, integrity) |
| `_pvmove_lv_check_holder_lock()` | Check holder lock state: 1=moveable, 0=skip, -1=error |

### Segment Chunking

Large segments are split before mirroring via `pvmove_max_segment_size_mb`
configuration (`lib/metadata/lv_manip.c`).  This prevents mirroring
excessively large amounts of data in a single operation and provides
better progress granularity.

```
allocation {
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

### Inactive LV Handling

pvmove inserts mirror segments in metadata for ALL affected LVs
regardless of their activation state.  Inactive LVs are NOT activated
to install mirrors -- the mirror takes effect when the LV is next
activated.  The pvmove mirror LV (pvmove0) is always activated to
perform the actual data copy.

Key implementation points in `tools/pvmove.c`:

- pvmove0 creation (`lv_create_empty`) is inlined in
  `_pvmove_setup_single()`; `_remove_pvmove_lv()` in `pvmove_poll.c`
  handles removal on finish/abort.
- `_update_metadata()` iterates `lvs_changed` to find the first
  active LV for `lv_update_and_reload()`.  If no LV is active, does
  `vg_write()` + `vg_commit()` without suspend/resume (no kernel
  tables to reload).
- On initial activation failure of pvmove0, `pvmove_abort_initial()`
  reverts: detach mirror (keep source data), remove pvmove LV, refresh
  participant LVs back to original mappings.

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
- `lockd_init_lv_args()` is called inline in `_pvmove_setup_single()`
  after pvmove0 creation for shared VGs.
- All `lockd_lv()` calls consolidated in `_pvmove_setup_single()`:
  named LV lock, pvmove0 lock, unlock on error via goto cleanup.
- Lock space is freed via `lockd_lvremove_done()` in both the normal
  completion path (`_remove_pvmove_lv()` in `pvmove_poll.c`) and the
  force-removal path (`lv_remove_single()`).

### LV Lock Lifecycle During pvmove

In shared VGs, participating LVs do not hold their own cluster locks
while pvmove is running.  pvmove0's EX lock is the sole guard:

1. **Setup**: the named LV lock is acquired to verify no other host
   holds it, then released after `_update_metadata()` commits the
   LOCKED flag.  Holder locks from `_pvmove_lv_check_holder_lock()`
   are likewise released after LOCKED is committed.
2. **During pvmove**: LOCKED LVs skip `lockd_lv()` on activation and
   deactivation (`lv_active_change()`).  The pvmove0 probe guards
   against remote activation.  No per-LV lock is held.
3. **Finish**: before releasing pvmove0's lock, `pvmove_finish()`
   re-acquires EX locks on all active participating LVs.  If a lock
   cannot be acquired, the LV is deactivated.  The VG EX lock
   (READ_FOR_UPDATE) prevents races during this transition.
4. **Resume** (crash recovery): only the pvmove0 lock is re-acquired.
   The named LV's LOCKED flag is already in metadata from the original
   setup.

### LOCKED LV Activation Guard

`lv_active_change()` (`lib/metadata/lv.c`) handles LOCKED LVs in shared
VGs specially:

- **Activation**: probes the pvmove0 cluster lock via
  `lockd_lv(pvmove0, "ex")`.  If the lock attempt fails (another node
  holds it) and pvmove0 is not active locally, activation is refused.
  If the probe succeeds, the lock is immediately released.  The normal
  `lockd_lv(lv)` call is skipped -- pvmove0's lock is the guard.
- **Deactivation**: the `lockd_lv(lv, "un")` call is skipped since
  no per-LV lock is held during pvmove.

### Why lockd_query_lv Is Not Used in the pvmove Path

An alternative approach would be `lockd_query_lv()` to check whether a
remote host held a lock on an LV before deciding to skip it during
pvmove.  Direct `lockd_lv(EX)` lock attempts are used instead for two
reasons:

1. **Lock managers do not reliably support state queries.**
   DLM in particular has no "query who holds this lock" operation.
   `lockd_query_lv()` worked in lvmlockd's test mode and with sanlock
   (which stores lock state on disk), but is not a portable primitive
   across lock backends.  The correct pattern is to *try to acquire*
   the lock -- failure means another host holds it.

2. **Try-lock is race-free.**
   A query returns a snapshot that can go stale before the caller acts
   on it (TOCTOU).  A lock attempt either succeeds (caller now holds
   it) or fails (someone else does), with no window in between.  The
   VG EX lock (READ_FOR_UPDATE) already serializes metadata changes,
   so the lock attempt under VG EX is definitive.

`lockd_query_lv()` is still used by `lib/report/report.c` for
displaying lock status (best-effort, no correctness requirement).

### Test Lock Injection (lvmlockd --test mode)

`lvmlockctl --set-remote-lv-lock <vg> --lv-uuid <uuid> --lock-mode <mode>`
injects remote lock state into lvmlockd's daemon_test mode for testing
without real DLM/sanlock infrastructure.

Sets `r->test_remote_ex` or `r->test_remote_sh` flags on the resource
without creating any `struct lock` entries.  Simulates another cluster
node holding a lock.  The `lm_lock_*()` functions check these flags
when `daemon_test` is set:
- No remote flags: lock succeeds (return 0)
- SH request + remote SH only: compatible (return 0)
- All other combinations: return -EAGAIN

The resource is kept alive across client disconnect so the injected
state persists until explicitly cleared with `--lock-mode un`.

The operation is rejected unless lvmlockd runs in `--test` mode.

## Implementation Plan

### Phase 1: Skip Forced Activation -- DONE

1. Insert mirror segments in metadata for all affected LVs -- unchanged.
2. Remove force-activation of inactive LVs -- done.
3. Remove separate `activate_lvs()` call -- done.
4. Fix `_update_metadata()` for first-LV-inactive case -- done:
   iterates `lvs_changed` for first active LV.
5. Remove exclusive handling -- done.
6. Poll pvmove LV; track LVs that become active during poll -- open.
7. Decide completion strategy for LVs that stayed inactive -- open
   (see Open Questions).

### Phase 2: Deactivation During pvmove -- DONE

1. Fix `_lv_deactivate()` to not track pvmove deps -- done.
2. Fix deactivation tree: treat pvmove LV as PV boundary in segment
   walk -- done.
3. Fix `refresh_pvmoved_lvs()`: replace `activate_lv()` with
   `lv_refresh_suspend_resume()` -- done.
4. vgchange -an with active pvmove: error + force flags -- done.
5. Force-removal of pvmove-locked LVs after interruption -- done.

### Phase 3: Cluster Lock for pvmove LV -- DONE

1. `lockd_lv_uses_lock()` returns 1 for pvmove LVs -- done.
2. `lockd_init_lv_args()` after pvmove0 creation in shared VGs -- done.
3. Lock space freed on pvmove LV removal -- done.
4. Named LV lock released after LOCKED committed to metadata -- done.
5. Resume path only locks pvmove0 (LOCKED flag protects named LVs) -- done.
6. All lockd_lv calls consolidated in `_pvmove_setup_single()` with
   goto-based cleanup -- done.
7. Refactored helpers: `_pvmove_validate_resume()`,
   `_set_up_pvmove_lv` refactored as mirror population function
   with holder lock checks via `_pvmove_lv_check_holder_lock()` -- done.
8. LOCKED LV activation guard (`lv_active_change`) -- done.
9. lvmlockd test lock injection: --set-remote-lv-lock -- done.
10. Verification with lvmlockd --test environment -- done
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

Tests 1-4 use `-n` and run in both shared (lvmlockd) and non-shared configs.
Tests 5-8 move all LVs from a PV without naming them; non-shared only.

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

- Test 1: lvmlockctl --set-remote-lv-lock round-trip: inject EX/UN modes,
          verify remote lock blocks activation
- Test 2: Activation of LOCKED LV refused when pvmove0 absent locally;
          persistent locks survive daemon death; pvmove restartable after
          remote lock release
- Test 3: pvmove --abort releases pvmove0 lock space (lockd_lvremove_done
          + lockd_free_removed_lvs path)
- Test 4: (pending unnamed pvmove support in shared VG -- FIXME in pvmove.c)
- Test 5: Normal pvmove completion releases pvmove0 lock space
- Test 6: Named pvmove refused when remote EX held on named LV; clears
          after lock release and pvmove succeeds
- Test 7: LOCKED LV activation skips per-LV lockd_lv; pvmove0 lock is
          the sole guard
- Test 8: Lock re-acquisition at pvmove finish: active participant gets
          its own EX lock back after pvmove0 is released
- Test 9: Named pvmove of already-LOCKED LV is refused

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
