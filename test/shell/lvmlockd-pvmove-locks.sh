#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='pvmove cluster lock protection (lvmlockd --test mode)'

. lib/inittest

# Requires lvmlockd running with --test (daemon_test mode).
[[ "${LVM_TEST_LVMLOCKD_TEST:-0}" = 0 ]] && skip

which md5sum || skip

aux prepare_vg 5

# ===================================================================
# Test 1: lvmlockctl --set-remote-lv-lock injects and clears remote
#         lock state
#
# Verify round-trip: lvmlockctl sends set_remote_lv_lock, the
# lockspace thread creates a resource with test_remote_ex/sh set,
# and lm_lock returns EAGAIN for incompatible lock requests.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"

lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Inject remote EX lock.  This sets test_remote_ex on the resource,
# simulating another cluster node holding an exclusive lock.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv1_uuid" --lock-mode ex

# Activation must fail: lm_lock returns EAGAIN because test_remote_ex is set,
# simulating another cluster node holding the EX lock.
not lvchange -ay $vg/$lv1
check inactive $vg $lv1

# Clear the remote state.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un

# Activation must succeed now.
lvchange -aey $vg/$lv1
check active $vg $lv1
lvchange -an $vg/$lv1

lvremove -ff $vg

# ===================================================================
# Test 2: activation of a pvmove-LOCKED LV is refused when the LV's
#         own cluster lock is held by the pvmove node (another node)
#
# pvmove now holds the named LV's EX lock throughout the operation.
# lockd_lv(lv, "ex") in lv_active_change() fails on a remote node
# because the pvmove node holds EX.
#
# We simulate the "another node" condition by starting pvmove in the
# background (which locks both pvmove0 and lv1), then injecting
# remote EX on lv1 to simulate the pvmove node holding it remotely.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
check inactive $vg $lv1

lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove with long poll delay so the daemon is idle.
# pvmove creates pvmove0 (activated in DM) and sets LOCKED on lv1.
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# pvmove holds persistent EX locks on BOTH pvmove0 AND lv1.
# Persistent locks survive the pvmove foreground process exiting (-b mode).
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# Confirm pvmove0 is active in DM.
dmsetup info "${vg}-pvmove0"

# Kill the poll daemon -- persistent locks must survive daemon death.
aux kill_tagged_processes
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# Remove the pvmove0 DM device to simulate pvmove running on another node.
dmsetup remove "${vg}-pvmove0"
not dmsetup info "${vg}-pvmove0"

# Inject remote EX on lv1 -- simulates another node holding it.
# This also drops the local persistent lock so that subsequent lock
# requests go through lm_lock where test_remote_ex is checked.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv1_uuid" --lock-mode ex

# Local persistent lock dropped; remote EX blocks lockd_lv(lv1, "ex").
lvmlockctl --info | tee out
not grep "LK LV ex $lv1_uuid" out

# lockd_lv(lv1, "ex") fails (remote EX) -> activation refused.
not lvchange -ay $vg/$lv1

# Clean up: clear remote lock on lv1, then abort the in-progress pvmove.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un
pvmove --abort

# pvmove0 must be gone after abort.
not dmsetup info "${vg}-pvmove0"
check inactive $vg $lv1

lvremove -ff $vg

# ===================================================================
# Test 3: pvmove --abort releases pvmove0 lock space in shared VG
#
# pvmove holds persistent EX locks on pvmove0 AND the named LV.
# After --abort, pvmove_finish() calls lv_remove(pvmove0) which
# triggers lockd_lvremove_done() to release and queue the lock, then
# lockd_free_removed_lvs() to free the lock space from lvmlockd.
# The named LV lock remains held for normal operation.
#
# Verification: pvmove0 UUID must be absent from lvmlockctl --info
# after abort, and lv1 must be usable.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove with long poll delay
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# pvmove0 and lv1 must have persistent EX locks
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# Abort pvmove -- must release and free pvmove0 lock space
pvmove --abort

# pvmove0 lock space must be gone completely from lvmlockd
lvmlockctl --info | tee out
not grep "$pvmove0_uuid" out

# No pvmove LV left in metadata
get lv_field $vg name -a | not grep "pvmove"

# lv1 must be activatable -- lv1 lock remains held (pvmove kept it)
lvchange -ay $vg/$lv1
check active $vg $lv1

aux kill_tagged_processes
lvchange -an $vg/$lv1
lvremove -ff $vg

# ===================================================================
# Test 4: Remotely-locked LV is skipped during unnamed pvmove in shared VG
#
# Unnamed pvmove (no -n) in a shared VG is not yet supported
# (tools/pvmove.c rejects it immediately).  When that restriction is
# lifted, this test should verify that lockd_lv("ex") failing on a
# remotely-locked LV causes pvmove to skip it while moving others.
# See the FIXME in tools/pvmove.c for the missing per-LV locking in the
# unnamed shared-VG path.
#
# NOTE: The holder lock probe in _pvmove_lv_check_holder_lock() is
# also unreachable for named pvmove because _set_up_pvmove_lv()
# filters to LVs matching lv_name, so holder always equals the
# named LV.  These paths are for future unnamed pvmove support.
# ===================================================================

# ===================================================================
# Test 5: Normal pvmove completion releases pvmove0 lock space
#
# Counterpart to Test 3 (abort path): verify that a clean pvmove
# completion also frees the pvmove0 lock space.  pvmove_finish()
# calls lv_remove(pvmove0) -> lockd_lvremove_done() (unlock) ->
# lockd_free_removed_lvs() (free).
# The named LV lock remains held for normal operation.
#
# Verification: pvmove0 UUID absent from --info after completion,
# no pvmove LV in metadata, lv1 on destination PV.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove with a short initial delay so pvmove0 is
# visible in metadata before the poll daemon cleans it up.
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +2 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# Wait for pvmove to complete normally (poll daemon handles it).
# Use get lv_field $vg/pvmove0 lv_name -a (correct syntax for hidden LV).
i=60; while get lv_field $vg/pvmove0 lv_name -a 2>/dev/null && [ "$i" -gt 0 ] ; do sleep .5; i=$((i-1)); done
test "$i" -ne 0 || die "pvmove did not complete in time"

# pvmove0 lock space must be freed after clean completion
lvmlockctl --info | tee out
not grep "$pvmove0_uuid" out

# lv1 lock remains held (pvmove kept it throughout)
grep "LK LV ex $lv1_uuid" out

# No pvmove LV in metadata
get lv_field $vg name -a | not grep "pvmove"

# lv1 must be on dev5 (moved successfully)
check lv_on $vg $lv1 "$dev5"

aux kill_tagged_processes
lvremove -ff $vg

# ===================================================================
# Test 6: Named pvmove is refused when another node holds EX on
#         the named LV
#
# pvmove -n acquires EX on the named LV before inserting mirror
# segments.  If another cluster node already holds EX (--remote
# simulates this), lockd_lv() returns EAGAIN and pvmove must fail
# gracefully without modifying metadata.
#
# After clearing the foreign lock, the LV must still be on dev1
# and pvmove must succeed normally.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Inject remote EX: simulates another cluster node holding the lock
# while lv1 is not active locally.  pvmove's lockd_lv("ex") call
# gets EAGAIN (test_remote_ex is set on the resource).
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv1_uuid" --lock-mode ex

# pvmove must fail: cannot acquire EX on the named LV
not pvmove -n $vg/$lv1 "$dev1" "$dev5"

# lv1 must still be on dev1, untouched
check lv_on $vg $lv1 "$dev1"
check inactive $vg $lv1

# No pvmove LV created in metadata
get lv_field $vg name -a | not grep "pvmove"

# Clear the remote lock state, lv1 accessible again.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un

# pvmove must now succeed with the inactive LV
pvmove -i0 -n $vg/$lv1 "$dev1" "$dev5"
check lv_on $vg $lv1 "$dev5"
check inactive $vg $lv1

lvremove -ff $vg

# ===================================================================
# Test 7: LOCKED LV keeps its own cluster lock during pvmove
#
# Lock lifecycle: pvmove acquires EX on the named LV and keeps it
# held throughout the operation.  Both the named LV and pvmove0
# must have EX locks in lvmlockctl --info during pvmove.
#
# Activation of LOCKED LV on pvmove node succeeds because lockd_lv()
# sees the lock is already held.  Deactivation skips unlock so
# pvmove's lock is not released.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# pvmove0 must hold EX lock
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out

# Named LV lock MUST be present -- pvmove keeps it held.
grep "LK LV ex $lv1_uuid" out

# Activate lv1 while LOCKED -- lockd_lv() succeeds (already held)
lvchange -ay $vg/$lv1

# After activation, LV lock still present (not double-counted)
lvmlockctl --info | tee out
grep "LK LV ex $lv1_uuid" out

# Deactivate -- lock must NOT be released (pvmove still needs it)
lvchange -an $vg/$lv1
lvmlockctl --info | tee out
grep "LK LV ex $lv1_uuid" out

# Clean up
aux kill_tagged_processes
pvmove --abort
lvremove -ff $vg

# ===================================================================
# Test 8: LV lock persists through pvmove finish for active participants
#
# pvmove keeps the named LV's EX lock throughout the operation.
# When pvmove finishes, the lock is NOT released -- it remains held
# for normal LV operation after LOCKED is cleared.
#
# Verification: after pvmove completes, the participant LV must
# still have its EX lock in lvmlockctl --info.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Activate lv1 before pvmove -- it will be an active participant
lvchange -aey $vg/$lv1
check active $vg $lv1

# Start pvmove (foreground with -i0 for immediate completion)
pvmove -i0 -n $vg/$lv1 "$dev1" "$dev5"

# pvmove completed: lv1 must be on dev5 and still active
check lv_on $vg $lv1 "$dev5"
check active $vg $lv1

# LV lock persists after pvmove finish (pvmove0's lock is freed)
lvmlockctl --info | tee out
grep "LK LV ex $lv1_uuid" out

# pvmove0 must be gone
lvchange -an $vg/$lv1
lvremove -ff $vg

# ===================================================================
# Test 9: Named pvmove of already-LOCKED LV is refused
#
# If lv1 is already participating in a pvmove (LOCKED flag set),
# a second pvmove -n targeting the same LV must be refused with
# "already locked by another pvmove".  This guards against double
# pvmove insertion.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"

# Start first pvmove in background
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

# lv1 is now LOCKED in metadata (pvmove0 exists as proof)
get lv_field $vg/pvmove0 lv_uuid

# Second pvmove targeting the same LV must be refused
not pvmove -n $vg/$lv1 "$dev5" "$dev1" 2>err
grep "locked" err

# Clean up
aux kill_tagged_processes
pvmove --abort
lvremove -ff $vg

# ===================================================================
# Test 10: Persistent locks survive poll daemon death; abort cleans up
#
# Start pvmove in background, kill the poll daemon, verify
# persistent EX locks on both pvmove0 and lv1 survive.
# Then abort the pvmove -- locks must be properly released.
#
# NOTE: Resume (re-running pvmove after killing the poll daemon)
# does not work with lvmpolld because the stale "in_progress"
# registration causes infinite polling.  Use abort instead.
# ===================================================================

lvcreate -aey -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

aux mkdev_md5sum $vg $lv1

# Start pvmove with long poll delay
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +100 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# Both locks must be held
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# Kill poll daemon -- persistent locks survive in lvmlockd
aux kill_tagged_processes

# Locks must still be present after daemon death
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# Abort releases pvmove0 lock, keeps lv1 lock
pvmove --abort

lvmlockctl --info | tee out
not grep "$pvmove0_uuid" out
grep "LK LV ex $lv1_uuid" out

# lv1 must be back on dev1, data intact
check lv_on $vg $lv1 "$dev1"
check active $vg $lv1
check dev_md5sum $vg $lv1

get lv_field $vg name -a | not grep "pvmove"

vgchange -an $vg
lvremove -ff $vg

# ===================================================================
# Test 11: Resume with wrong LV name is refused
#
# Start pvmove of lv1, kill daemon, try to resume naming lv2.
# The resume path must detect that lv2 is not part of the
# in-progress pvmove and refuse.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lvcreate -an -l4 -n $lv2 $vg "$dev1"

# Start pvmove of lv1 only
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +100 -b "$dev1" "$dev5"

# pvmove0 must exist
get lv_field $vg/pvmove0 lv_uuid

aux kill_tagged_processes

# Resume naming lv2 -- must fail (lv2 not part of in-progress pvmove)
not pvmove -i0 -n $vg/$lv2 "$dev1" "$dev5" 2>err
grep "not part of" err

# Original pvmove must still be in metadata
get lv_field $vg name -a | tee out
grep "pvmove" out

# Clean up via abort
pvmove --abort

get lv_field $vg name -a | not grep "pvmove"

lvremove -ff $vg

# ===================================================================
# Test 12: Named pvmove succeeds with remotely-locked unrelated LV
#
# lv1 and lv2 both on dev1.  Inject remote EX on lv2 to simulate
# another node holding it.  Named pvmove of lv1 must succeed
# because it only locks the named LV, not other LVs on the PV.
# lv2 must remain untouched on dev1.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lvcreate -an -l4 -n $lv2 $vg "$dev1"
lv2_uuid=$(get lv_field $vg/$lv2 lv_uuid)

# Inject remote EX on lv2
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv2_uuid" --lock-mode ex

# Named pvmove of lv1 must succeed -- lv2 is not involved
pvmove -i0 -n $vg/$lv1 "$dev1" "$dev5"

check lv_on $vg $lv1 "$dev5"

# lv2 must still be on dev1 (not touched by named pvmove)
check lv_on $vg $lv2 "$dev1"

# Clear remote lock on lv2
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$lv2_uuid" --lock-mode un

get lv_field $vg name -a | not grep "pvmove"

lvremove -ff $vg

vgremove -ff $vg
