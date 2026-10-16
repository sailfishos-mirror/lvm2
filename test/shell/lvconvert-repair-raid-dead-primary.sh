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

# A RAID1 array left idle with an out-of-sync leg ("dead primary") must make
# the status reader report the stuck idle state, stop a plain image reduction
# and make lvconvert --repair print the salvage advice reachable through the
# rebuild/replace gate, rather than stopping at a bare "array is not in sync".

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 13 0 || skip
aux target_at_least dm-delay 1 1 0 || skip "Missing dm-delay target."

# Keep the VG metadata off the two sources: breaking their writes below is
# what drives dm-raid into the stuck state, and a metadata area there would
# make the VG metadata writes fail instead of reaching the status checks.
aux prepare_devs 3
get_devs
pvcreate --metadatacopies 0 "$dev1" "$dev2"
pvcreate "$dev3"
vgcreate $SHARED -s 2m "$vg" "${DEVICES[@]}"

# Create a RAID1 array and break it into the dead-primary state: while a
# freshly added third leg is still out of sync, break the superblock updates
# on the two sources so dm-raid fails them and is left with the new leg
# still out of sync.
make_stuck_lv() {
	lvcreate -aey --type raid1 --mirrors 1 --ignoremonitoring -l 2 -n "$1" \
		$vg "$dev1" "$dev2"
	aux wait_for_sync $vg "$1"

	aux delay_dev "$dev3" 0 1000 "$(get first_extent_sector "$dev3"):"
	lvconvert --yes -m +1 "$vg/$1" "$dev3"

	aux writeerror_dev "$dev1"
	aux writeerror_dev "$dev2"
}

# The array oscillates between an interrupted recovery and idle, so wait for
# the dead-primary idle state (a failed leg 'D' and the out-of-sync leg 'a').
wait_dead_primary() {
	local i a

	for i in {1..40}; do
		a=( $(dmsetup status --noflush "$vg-$1") ) || die "Unable to get status of $vg/$1"
		[[ "${a[7]}" = "idle" && "${a[5]}" = *D* && "${a[5]}" = *a* ]] && return 0
		sleep .25
	done
	return 1
}

make_stuck_lv $lv1
wait_dead_primary $lv1 || skip "Array did not fall idle with out-of-sync legs."

# A plain image reduction must be stopped by the extraction-side status
# check, which reports the stuck idle state it read.
not lvconvert --yes -m -1 "$vg/$lv1" >out 2>&1
grep "is idle with out-of-sync devices" out
grep "Failed to extract images" out

# Reset, then build a fresh stuck array for the repair-gate assertion.
aux enable_dev "$dev1" "$dev2" "$dev3"
lvchange -an "$vg/$lv1"
lvremove -f "$vg/$lv1"

make_stuck_lv $lv1
wait_dead_primary $lv1 || skip "Array did not fall idle with out-of-sync legs."

# The repair gate must reach the dead-primary advice (RAID_READ_STUCK)
# instead of refusing with a bare "not in sync".
not lvconvert --yes --repair "$vg/$lv1" >out 2>&1
grep "Source devices failed" out
grep "You should choose one of" out
grep "is idle with out-of-sync devices" out

aux enable_dev "$dev1" "$dev2" "$dev3"
lvchange -an "$vg/$lv1"
vgremove -ff $vg
