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
# Test 1: lvmlockctl --set-lock injects and clears lock modes
#
# Verify LD_OP_SET_LOCK round-trip: lvmlockctl sends set_lock_lv,
# the lockspace thread sets r->mode and all lk->mode entries, and
# "lvmlockctl --info" reports the injected mode for the LV resource.
# ===================================================================

lvcreate -aey -l4 -n $lv1 $vg "$dev1"

lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# lv1 is active: lvmlockd holds EX on it.
lvmlockctl --info | tee out
grep "LK LV ex $lv1_uuid" out

# Inject SH -- info must now show sh for this LV.
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode sh
lvmlockctl --info | tee out
grep "LK LV sh $lv1_uuid" out

# Inject EX -- info must now show ex.
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode ex
lvmlockctl --info | tee out
grep "LK LV ex $lv1_uuid" out

# Reset to un -- lock is released; resource is freed and no longer in --info.
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un

# Deactivate lv1 -- its EX lock is released in lvmlockd.
lvchange -an $vg/$lv1
check inactive $vg $lv1

# Inject remote EX lock.  This sets test_remote_ex on the resource,
# simulating another cluster node holding an exclusive lock.
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode ex --remote

# Activation must fail: lm_lock returns EAGAIN because test_remote_ex is set,
# simulating another cluster node holding the EX lock.
not lvchange -ay $vg/$lv1
check inactive $vg $lv1

# Clear the remote state.
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un

lvremove -ff $vg

# ===================================================================
# Test 2: activation of a pvmove-LOCKED LV is refused when pvmove0
#         is not active locally (pvmove running on another node)
#
# lv_active_change() contains a guard:
#   if lv is LOCKED (pvmove participant) AND
#      find_pvmove_lv_in_lv(lv) returns pvmove0 AND
#      lockd_query_lv(pvmove0) returns ex=1 (mirror type always does) AND
#      !lv_is_active(pvmove0)           <- pvmove0 DM device absent
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

# pvmove holds persistent EX locks on both pvmove0 and the LOCKED lv1.
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

# Remove the pvmove0 DM device to simulate pvmove running on another node.
dmsetup remove "${vg}-pvmove0"
not dmsetup info "${vg}-pvmove0"

# EX lock still present in lvmlockd with no local DM device.
lvmlockctl --info | tee out
grep "LK LV ex $pvmove0_uuid" out

# pvmove0 not active locally -> activation of the LOCKED lv1 must be refused.
not lvchange -ay $vg/$lv1

# Simulate the remote node releasing locks (crash or completion elsewhere).
# In a real cluster DLM would drop the locks automatically on node death.
lvmlockctl --set-lock $vg --lv-uuid "$pvmove0_uuid" --lock-mode un
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un

# Restart pvmove locally: finds the existing pvmove LV, re-acquires EX
# locks, and completes the data copy via the now-active pvmove0 device.
# Shared VGs require -n <lv>; destination is already encoded in metadata.
pvmove -i1 -n $vg/$lv1 "$dev1"

# pvmove0 is gone after successful completion.
not dmsetup info "${vg}-pvmove0"
check inactive $vg $lv1

lvremove -ff $vg

# ===================================================================
# Test 3: pvmove --abort releases pvmove0 lock space in shared VG
#
# pvmove holds persistent EX locks on both pvmove0 and the named LV.
# After --abort, pvmove_finish() calls lv_remove(pvmove0) which
# triggers lockd_lvremove_done() to release and queue the lock, then
# lockd_free_removed_lvs() to free the lock space from lvmlockd.
# The named LV's EX lock persists (it was held before pvmove started
# and is released normally on lvchange -an).
#
# Verification: pvmove0 UUID must be absent from lvmlockctl --info
# after abort, and lv1 must be usable.
# ===================================================================

lvcreate -an -l4 -n $lv1 $vg "$dev1"
lv1_uuid=$(get lv_field $vg/$lv1 lv_uuid)

# Start background pvmove with long poll delay
LVM_TEST_TAG="kill_me_$PREFIX" pvmove -n $vg/$lv1 -i +3 -b "$dev1" "$dev5"

pvmove0_uuid=$(get lv_field $vg/pvmove0 lv_uuid)

# Both pvmove0 and lv1 must have persistent EX locks
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

# lv1 must be activatable -- no stale lock blocks it
lvchange -ay $vg/$lv1
check active $vg $lv1

aux kill_tagged_processes
lvchange -an $vg/$lv1
lvremove -ff $vg

# ===================================================================
# Test 4: SH-locked LV is skipped during unnamed pvmove in shared VG
#
# Unnamed pvmove (no -n) in a shared VG is not yet supported
# (tools/pvmove.c rejects it immediately).  When that restriction is
# lifted, this test should verify that lockd_query_lv() returning sh=1
# causes pvmove to skip the SH-locked LV while moving the EX-locked one.
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
grep "LK LV ex $lv1_uuid" out

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
# while lv1 is not active locally.  Our lockd_lv("ex") call in pvmove
# setup gets EAGAIN (test_remote_ex is set on the resource).
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode ex --remote

# pvmove must fail: cannot acquire EX on the named LV
not pvmove -n $vg/$lv1 "$dev1" "$dev5"

# lv1 must still be on dev1, untouched
check lv_on $vg $lv1 "$dev1"
check inactive $vg $lv1

# No pvmove LV created in metadata
get lv_field $vg name -a | not grep "pvmove"

# Clear the remote lock state, lv1 accessible again.
lvmlockctl --set-lock $vg --lv-uuid "$lv1_uuid" --lock-mode un

# pvmove must now succeed with the inactive LV
pvmove -i0 -n $vg/$lv1 "$dev1" "$dev5"
check lv_on $vg $lv1 "$dev5"
check inactive $vg $lv1

lvremove -ff $vg

vgremove -ff $vg
