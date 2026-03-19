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

# Test that a raid+integrity LV can be reactivated after one of the
# underlying PVs has been corrupted while the LV was inactive.
#
# When the integrity superblock on a raid image is corrupted, the kernel
# md-raid sees "invalid superblocks" and treats it as a new device
# injected without 'rebuild' parameter, causing activation to fail with:
#   "device-mapper: raid: New device injected into existing raid set
#    without 'delta_disks' or 'rebuild' parameter specified"


. lib/inittest --skip-with-lvmpolld

which mkfs.ext4 || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 5 64

dd if=/dev/urandom of=randA bs=512K count=2 2>/dev/null
dd if=/dev/urandom of=randB bs=512K count=3 2>/dev/null
dd if=/dev/urandom of=randC bs=512K count=4 2>/dev/null

_prepare_vg() {
	vgremove -ff $vg
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
}

_check_lv() {
	mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"
	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
	cp randA "$mnt"
	cp randB "$mnt"
	cp randC "$mnt"
	umount "$mnt"

	lvchange -an $vg/$lv1

	dd if=/dev/urandom of="$dev2" oflag=direct bs=1M seek=2 count=16

	if lvchange -ay $vg/$lv1 ; then

		mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
		cmp -b randA "$mnt/randA"
		cmp -b randB "$mnt/randB"
		cmp -b randC "$mnt/randC"
		umount "$mnt"
	else
		# Warn if get here, activation is not handling invalid integrity superblock
		should :
	fi
}

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"

#
# Test 1: raid1 -m1 (2 legs) with integrity journal, corrupt one PV, reactivate
#
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode journal -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1

_check_lv

_prepare_vg

#
# Test 2: raid1 -m3 (4 legs) with integrity journal, corrupt one PV, reactivate
#
lvcreate --type raid1 -m3 --raidintegrity y --raidintegritymode journal -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/$lv1

_check_lv

_prepare_vg

#
# Test 3: raid1 -m1 with integrity bitmap, corrupt one PV, reactivate
#
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1

_check_lv

_prepare_vg

#
# Test 4: raid1 -m1, add integrity via lvconvert, corrupt one PV, reactivate
#
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/$lv1
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1

_check_lv

_prepare_vg

#
# Test 5: raid5 with integrity, corrupt one PV, reactivate
#
lvcreate --type raid5 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1

_check_lv

_prepare_vg

#
# Test 6: raid6 with integrity, corrupt one PV, reactivate
#
lvcreate --type raid6 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1

_check_lv

vgremove -ff $vg
