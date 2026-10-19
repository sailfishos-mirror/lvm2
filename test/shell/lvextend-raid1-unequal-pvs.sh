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

# RAID1 lvextend with unequal PV sizes must not report a cryptic error.

. lib/inittest --skip-with-lvmpolld

aux have_raid 1 3 0 || skip

# An extend that does not fit the smaller PVs explains the imbalance.
# dev3 and dev4 stay unused until the metadata collision case at the end.
aux prepare_devs 4 64
pvcreate --yes --setphysicalvolumesize 64M "$dev1"
pvcreate --yes --setphysicalvolumesize 32M "$dev2"
vgcreate -s 512K $vg "$dev1" "$dev2"

# --nosync skips the initial resync, which would only burn CPU on data
# nobody reads.  Keep the LV active (the lvcreate default): extending an
# inactive not-synced RAID LV prompts and fails non-interactively.
lvcreate --type raid1 -m 1 --nosync -n $lv -L 8M $vg

not lvextend -l+50 $vg/$lv 2>&1 | tee out
grep -F "Insufficient suitable allocatable extents" out
grep -F "Unequal physical volume sizes or free space in volume group" out

# The hint explains an extend failure only, not an lvcreate failure.
not lvcreate --type raid1 -m 1 -n ${lv}2 -L 8M $vg "$dev2" 2>&1 | tee out2
grep -F "Insufficient suitable allocatable extents" out2
not grep -F "Unequal physical volume" out2

# An extend that fits reports no imbalance.
lvextend -l+1 $vg/$lv 2>&1 | tee out3
not grep -F "Unequal physical volume" out3

vgremove -ff $vg
pvremove "$dev1" "$dev2"

# A failed extend in a VG with equal PVs must not blame PV sizes.
vgcreate $vg "$dev1" "$dev2"

lvcreate --type raid1 -m 1 --nosync -n $lv -L 8M $vg

not lvextend -l+200 $vg/$lv 2>&1 | tee out4
grep -F "Insufficient free space" out4
not grep -F "Unequal physical volume" out4

# A failed extend that would place both images on one PV names the images.
lvcreate --type raid1 -m 1 --nosync --alloc anywhere -n ${lv}2 -L 8M $vg "$dev1"

not lvextend --alloc normal -l+1 $vg/${lv}2 2>&1 | tee out5
grep -F "RAID data images" out5
grep -F "both use physical volume" out5
grep -F "RAID data images must use different physical volumes" out5

vgremove -ff $vg

# Equal PV sizes but unequal free space is the same imbalance seen from the
# allocator's side: filling one PV makes the extend fail even though every
# PV reports the same pe_count, so the hint must be armed by pv_free too.
vgcreate $vg "$dev1" "$dev2"

lvcreate --type raid1 -m 1 --nosync -n $lv -L 8M $vg
# 100%PVS fills dev1 exactly, so the two images cannot both find room there
# and the extend fails although every PV reports the same pe_count.
lvcreate -l 100%PVS -n filler $vg "$dev1"

not lvextend -l+64 $vg/$lv 2>&1 | tee out6
grep -F "Unequal physical volume sizes or free space in volume group" out6

vgremove -ff $vg

# A metadata collision alone is reported as metadata: pvmove moves only the
# rimage, so the images end up on different PVs while both metas stay on dev1.
# The filler LVs leave dev1 and dev2 without free extents, so the new image
# extents must go to dev3 and dev4 and the data check passes; dev1 is still
# scanned by the check because it is an allocatable PV, and that is where both
# metadata LVs live.
vgcreate -s 512K $vg "$dev1" "$dev2" "$dev3" "$dev4"

lvcreate --type raid1 -m 1 --alloc anywhere -n $lv -L 8M $vg "$dev1"
check lv_on $vg ${lv}_rimage_0 "$dev1"
check lv_on $vg ${lv}_rimage_1 "$dev1"

pvmove -i0 -n ${lv}_rimage_1 "$dev1" "$dev2"
check lv_on $vg ${lv}_rimage_1 "$dev2"
check lv_on $vg ${lv}_rmeta_1 "$dev1"

lvcreate -l 100%PVS -n filler1 $vg "$dev1"
lvcreate -l 100%PVS -n filler2 $vg "$dev2"

not lvextend --alloc normal -l+1 $vg/$lv 2>&1 | tee out7
grep -F "RAID metadata images" out7
grep -F "must use different physical volumes" out7

vgremove -ff $vg
