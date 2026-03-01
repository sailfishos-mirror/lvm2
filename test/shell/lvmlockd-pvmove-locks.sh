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
# Test 2: activation of a pvmove-LOCKED LV is refused when pvmove0
#         is not active locally (pvmove running on another node)
#
# lv_active_change() contains a guard:
#   if lv is LOCKED (pvmove participant) AND
#      find_pvmove_lv_in_lv(lv) returns pvmove0 AND
#      !lv_is_active(pvmove0) AND
#      lockd_lv(pvmove0, "ex") fails    <- EX lock held by pvmove
#   then refuse with "pvmove is running on another node".
#
# We simulate the "another node" condition by starting pvmove in the
# background (which creates and activates pvmove0), then removing the
# pvmove0 DM device so lv_is_active() returns false on this node.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
check inactive $vg $lv1

lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove with long poll delay so the daemon is idle.
# pvmove creates pvmove0 (activated in DM) and sets LOCKED on lv1.
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# pvmove holds persistent EX lock on pvmove0.
# Named LV lock was released after LOCKED committed to metadata.
# Persistent locks survive the pvmove foreground process exiting (-b mode).
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out

# Confirm pvmove0 is active in DM.
dmsetup info "${vg}-pvmove0"

# Kill the poll daemon -- persistent locks must survive daemon death.
aux kill_tagged_processes
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out

# Remove the pvmove0 DM device to simulate pvmove running on another node.
dmsetup remove "${vg}-pvmove0"
not dmsetup info "${vg}-pvmove0"

# Inject remote EX on pvmove0 -- simulates another node holding it.
# This also drops the local persistent lock so that subsequent lock
# requests go through lm_lock where test_remote_ex is checked.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$pvmove0_uuid" --lock-mode ex

# Local persistent lock dropped; remote EX blocks lockd_lv(pvmove0, "ex").
lvmlockctl --info | tee out
not grep "LK LV ex $pvmove0_uuid" out

# pvmove0 not active locally AND lockd_lv(pvmove0) fails (remote EX)
# -> activation of the LOCKED lv1 must be refused.
not lvchange -ay $vg/$lv1

# Clean up: clear remote lock, then abort the in-progress pvmove.
lvmlockctl --set-remote-lv-lock $vg --lv-uuid "$pvmove0_uuid" --lock-mode un
pvmove --abort

# pvmove0 must be gone after abort.
not dmsetup info "${vg}-pvmove0"
check inactive $vg $lv1

lvremove -ff $vg

# ===================================================================
# Test 3: pvmove --abort releases pvmove0 lock space in shared VG
#
# pvmove holds persistent EX lock on pvmove0.  The named LV lock
# was released after LOCKED was committed to metadata.
# After --abort, pvmove_finish() calls lv_remove(pvmove0) which
# triggers lockd_lvremove_done() to release and queue the lock, then
# lockd_free_removed_lvs() to free the lock space from lvmlockd.
#
# Verification: pvmove0 UUID must be absent from lvmlockctl --info
# after abort, and lv1 must be usable.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove with long poll delay
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# pvmove0 must have persistent EX lock; named LV lock already released
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out

# Abort pvmove -- must release and free pvmove0 lock space
pvmove --abort

# pvmove0 lock space must be gone completely from lvmlockd
lvmlockctl --info | tee out
not grep "$pvmove0_uuid" out

# No pvmove LV left in metadata
get lv_field $vg name -a | not grep "pvmove"

# lv1 must be activatable -- no stale lock blocks it
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
# ===================================================================

# ===================================================================
# Test 5: Normal pvmove completion releases pvmove0 lock space
#
# Counterpart to Test 3 (abort path): verify that a clean pvmove
# completion also frees the pvmove0 lock space.  pvmove_finish()
# calls lv_remove(pvmove0) -> lockd_lvremove_done() (unlock) ->
# lockd_free_removed_lvs() (free).
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

# Wait for pvmove to complete normally (poll daemon handles it).
# Use get lv_field $vg/pvmove0 lv_name -a (correct syntax for hidden LV).
i=60; while get lv_field $vg/pvmove0 lv_name -a 2>/dev/null && [ "$i" -gt 0 ] ; do sleep .5; i=$((i-1)); done
test "$i" -ne 0 || die "pvmove did not complete in time"

# pvmove0 lock space must be freed after clean completion
lvmlockctl --info | tee out
not grep "$pvmove0_uuid" out

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
# Test 7: LOCKED LV activation does NOT acquire its own cluster lock
#
# Lock lifecycle: after pvmove commits LOCKED to metadata, the named
# LV lock is released.  pvmove0's EX lock guards all participants.
# When a LOCKED LV is activated (e.g. for table reload), the
# lv_active_change() guard skips lockd_lv() for LOCKED LVs in shared
# VGs -- no per-LV EX lock should appear in lvmlockctl --info.
#
# Verification: after pvmove starts and LOCKED is committed,
# the named LV lock UUID must NOT appear in lvmlockctl --info,
# only pvmove0's lock does.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# pvmove0 must hold EX lock
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out

# Named LV lock must NOT be present -- it was released after LOCKED
# was committed to metadata.  pvmove0's lock is the sole guard.
not grep "LK LV ex $lv1_uuid" out
not grep "LK LV sh $lv1_uuid" out

# Activate lv1 while LOCKED -- lv_active_change() skips lockd_lv()
lvchange -ay $vg/$lv1

# Even after activation, no per-LV lock should exist
lvmlockctl --info | tee out
not grep "LK LV ex $lv1_uuid" out

lvchange -an $vg/$lv1

# Clean up
aux kill_tagged_processes
pvmove --abort
lvremove -ff $vg

# ===================================================================
# Test 8: Lock re-acquisition at pvmove finish for active participants
#
# During pvmove, LOCKED LVs have no own cluster lock.  When pvmove
# finishes (pvmove_finish()), it must re-acquire EX locks on all
# active participant LVs before releasing pvmove0's lock, so they
# are protected again for normal operation.
#
# Verification: after pvmove completes, the participant LV must have
# its own EX lock back in lvmlockctl --info.
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

# After pvmove_finish() re-acquired locks: lv1 must have its own
# EX lock back (pvmove0's lock is already freed)
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

vgremove -ff $vg
