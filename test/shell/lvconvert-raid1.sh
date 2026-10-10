#!/usr/bin/env bash

# Copyright (C) 2011-2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 3 0 || skip

aux prepare_pvs 3
get_devs
vgcreate $SHARED -s 2m "$vg" "${DEVICES[@]}"

# Delay PV extents, but leave the PV/VG metadata area undelayed.
delay_pv_pe_area() {
	local dev=$1

	aux delay_dev "$dev" 0 100 "$(get first_extent_sector "$dev"):"
}

lvcreate --type raid1 -m 1 -aey -l 2 -n "$lv1" "$vg" "$dev1" "$dev2"
aux wait_for_sync "$vg" "$lv1"

# Reduce a RAID1 LV while a newly added mirror leg is still recovering.
# On dm-raid < 1.14.0, lvm2 temporarily re-marks LV_REBUILD to pass reload;
# on 1.14.0+ (rebuild-keeps-recovery) that workaround is skipped automatically.

# Delay the new image data, leaving its metadata extent available.
extent_size=$(get vg_field "$vg" vg_extent_size --units s --nosuffix)
first_data_extent=$(($(get first_extent_sector "$dev3") + extent_size))
aux delay_dev "$dev3" 0 100 "$first_data_extent:$(( 2 * extent_size ))"

lvconvert --yes -m +1 "$vg/$lv1" "$dev3"
# Legacy dm-raid restarts recovery on rebuild; newer targets retain progress.
printf 'y\n' | lvconvert -m -1 "$vg/$lv1" "$dev1" >lvconvert-raid1.out 2>&1
if aux have_raid 1 14 0; then
	! grep -Fq "will restart from 0%" lvconvert-raid1.out
else
	grep -F "will restart from 0%" lvconvert-raid1.out
fi
rm -f lvconvert-raid1.out

aux wait_for_sync "$vg" "$lv1"
aux enable_dev "$dev3"
lvremove -ff "$vg"

###########################################
# Upconverted RAID1 should not allow loss of primary
#  - don't allow removal of primary while syncing
#  - DO allow removal of secondaries while syncing
###########################################
delay_pv_pe_area "$dev2"
lvcreate -aey -l 2 -n $lv1 $vg "$dev1"
lvconvert -y -m 1 $vg/$lv1 \
	--config 'global { mirror_segtype_default = "raid1" }' "$dev2"
lvs --noheadings -o attr $vg/$lv1 | grep '^[[:space:]]*r'
not lvconvert --yes -m 0 $vg/$lv1 "$dev1"
lvconvert --yes -m 0 $vg/$lv1 "$dev2"
aux enable_dev "$dev2"
lvremove -ff $vg

###########################################
# lvcreated RAID1 should allow all down-conversion
#  - DO allow removal of primary while syncing
#  - DO allow removal of secondaries while syncing
###########################################
case "$(uname -r)" in
4.8.14*)
echo "Skipping RAID1 down-conversion checks on kernel 4.8.14"
;;
*)
delay_pv_pe_area "$dev2"
lvcreate --type raid1 -m 2 -aey -l 2 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
lvconvert --yes -m 1 $vg/$lv1 "$dev3"

# Cannot remove origin 1st. leg while synchronizing
not lvconvert --yes -m 0 $vg/$lv1 "$dev1"

aux enable_dev "$dev2"
aux wait_for_sync $vg $lv1

# When raid1 is synchronized, 1st. leg can be removed
lvconvert --yes -m 0 $vg/$lv1 "$dev1"
lvremove -ff $vg
;;
esac

###########################################
# Converting from 2-way RAID1 to 3-way
#  - DO allow removal of one of primary sources
#  - Do not allow removal of all primary sources
###########################################
lvcreate --type raid1 -m 1 -aey -l 2 -n $lv1 $vg "$dev1" "$dev2"
aux wait_for_sync $vg $lv1
delay_pv_pe_area "$dev3"
lvconvert --yes -m +1 $vg/$lv1 "$dev3"
# should allow 1st primary to be removed
lvconvert --yes -m -1 $vg/$lv1 "$dev1"
# should NOT allow last primary to be removed
test "${LVM_VALGRIND:-0}" -eq 0 && not lvconvert --yes -m -1 $vg/$lv1 "$dev2"
# should allow non-primary to be removed
lvconvert --yes -m 0 $vg/$lv1 "$dev3"
aux enable_dev "$dev3"
lvremove -ff $vg

###########################################
# Converting from 2-way RAID1 to 3-way
#  - Should allow removal of two devices,
#    as long as they aren't both primary
###########################################
lvcreate --type raid1 -m 1 -aey -l 2 -n $lv1 $vg "$dev1" "$dev2"
aux wait_for_sync $vg $lv1
delay_pv_pe_area "$dev3"
lvconvert --yes -m +1 $vg/$lv1 "$dev3"
# should NOT allow both primaries to be removed
not lvconvert -m 0 $vg/$lv1 "$dev1" "$dev2"
# should allow primary + non-primary
lvconvert --yes -m 0 $vg/$lv1 "$dev1" "$dev3"
aux enable_dev "$dev3"
lvremove -ff $vg

vgremove -ff "$vg"
