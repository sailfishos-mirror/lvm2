#!/usr/bin/env bash
#
# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test dmeventd-driven repair on transient majority mirror leg failure.
#
# Scenario:
#  1. Create 4-way mirror with data on 4 PVs (no spare)
#  2. Fail 3 of 4 legs (majority)
#  3. dmeventd strips to linear (no spare PV for replacement)
#  4. Deactivate VG, re-enable devices, reactivate
#  5. lvcreate must not produce corrupted LV segments
#
# Uses 4 PVs only so repair has no spare for allocation.
#


. lib/inittest --skip-with-lvmpolld

aux mirror_recovery_works || skip
aux prepare_dmeventd
aux prepare_vg 4

# dmeventd-driven repair on majority failure
test_mirror_dmeventd_repair()
{
	lvcreate -aey --type mirror -m 3 --ignoremonitoring -L 1 -n $lv1 $vg \
		"$dev1" "$dev2" "$dev3" "$dev4"
	aux wait_for_sync $vg $lv1

	lvchange --monitor y $vg/$lv1

	aux disable_dev --error "$dev2" "$dev3" "$dev4"

	mkfs.ext3 "$DM_DEV_DIR/$vg/$lv1"

	aux enable_dev "$dev2" "$dev3" "$dev4"

	sleep 5

	lvs -a -o +devices $vg | tee out
	not grep unknown out

	check linear $vg $lv1

	vgremove -ff $vg
}

# Reproduce Corey's bug (RHEL-116884)
# dmeventd strips mirror to linear on majority failure,
# devices return with stale metadata (seqno mismatch),
# subsequent lvcreate must NOT produce corrupted LV segments.
test_mirror_stale_metadata_lvcreate()
{
	# Use most of space but leave some free on each PV
	# (Corey: 35GB PVs with 4MB PE, mirror used ~34.99GB leaving 4MB free per PV)
	lvcreate -aey --type mirror -m 3 --nosync --ignoremonitoring -l60 -n $lv1 $vg \
		"$dev1" "$dev2" "$dev3" "$dev4"

	lvchange --monitor y $vg/$lv1

	aux disable_dev --error "$dev2" "$dev3" "$dev4"

	mkfs.ext3 "$DM_DEV_DIR/$vg/$lv1"

	sleep 5

	lvs -a -o +devices $vg | tee out
	not grep unknown out

	check linear $vg $lv1

	# Corey's sequence - deactivate while devices still offline
	vgchange -an $vg

	# Devices come back with stale metadata (seqno mismatch)
	aux enable_dev "$dev2" "$dev3" "$dev4"

	# Reactivate - VG now sees inconsistent metadata across PVs
	vgchange -ay $vg

	# Must not produce "LV segments corrupted" / "Internal error"
	# -l100%VG auto-reduces to remaining free space (matches Corey's command)
	lvcreate -aey --type raid1 -m 3 --nosync -l100%VG -n $lv2 $vg

	lvs -a -o +devices $vg

	vgremove -ff $vg
}

#####################################################################
# Run tests
#####################################################################

#test_mirror_dmeventd_repair
test_mirror_stale_metadata_lvcreate
