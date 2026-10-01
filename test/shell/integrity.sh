#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


. shell/integrity-basic.sh
. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip # too slow test for valgrind
aux have_fs ext4 || skip
which resize2fs || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 5 64

# Cover two 4K chunks so RAID6 can corrupt two data members, excluding parity.
integrity_make_files 8192
integrity_make_rand 64K

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	integrity_prepare_vg --clear "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
}

_test_fs_with_read_repair() {
	# $1 is the max-corrupt count, which test_fs_with_read_repair() takes
	# just before the device list; the rest is forwarded unchanged.
	local max_corrupt=$1; shift
	test_fs_with_read_repair "$vg/$lv1" "$mnt" "" "mkfs.ext4 -b 4096" "" "$max_corrupt" "$@"
}

# Test corrupting data on an image and verifying that
# it is detected by integrity and corrected by raid.

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair 1 "$dev1"
integrity_not_zero_mismatches $vg/${lv1}_rimage_0
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvchange $vg/$lv1 --writemostly "$dev2"
lvchange $vg/$lv1 --writemostly "$dev3"
_test_fs_with_read_repair 2 "$dev1" "$dev2"
integrity_not_zero_mismatches $vg/${lv1}_rimage_0
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid4 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair 1 "$dev1" "$dev2" "$dev3"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair 1 "$dev1" "$dev2" "$dev3"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair 2 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/${lv1}_rimage_3
lvs -o integritymismatches $vg/${lv1}_rimage_4
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid10 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair 2 "$dev1" "$dev3"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/${lv1}_rimage_3
# raid may read from a non-corrupted image, so we can't
# be sure that mismatches were detected.
# lvs -o integritymismatches $vg/$lv1 |tee mismatch
# not grep 0 mismatch
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing integrity from an active LV

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid4 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid5 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid6 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid10 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test adding integrity to an active LV

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid4 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid5 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid6 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid10 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test lvextend while inactive

lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
umount "$mnt"
lvchange -an $vg/$lv1
lvextend -l 16 $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o name,segtype,devices,sync_percent $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid6 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,sync_percent,devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
umount "$mnt"
lvchange -an $vg/$lv1
lvextend -l 16 $vg/$lv1
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o name,segtype,devices,sync_percent $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test lvextend while active

lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid5 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

lvcreate --type raid10 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test adding image to raid1

lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -a -o+devices $vg
integrity_add_new_data_to_mnt
lvconvert -y -m+1 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
lvs -a -o+devices $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test removing image from raid1

lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
lvconvert -y -m-1 $vg/$lv1
lvs -a -o+devices $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test disallowed operations on raid+integrity

lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
integrity_add_new_data_to_mnt
not lvconvert -y -m-1 $vg/$lv1
not lvconvert --splitmirrors 1 -n tmp -y $vg/$lv1
not lvconvert --splitmirrors 1 --trackchanges -y $vg/$lv1
not lvreduce -L4M $vg/$lv1
not pvmove -n $vg/$lv1 "$dev1"
not pvmove "$dev1"
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Repeat many of the tests above using bitmap mode

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair 1 "$dev1"
integrity_not_zero_mismatches $vg/${lv1}_rimage_0
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y --raidintegritymode bitmap -n $lv1 -I 4K -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_recalc $vg/$lv1
_test_fs_with_read_repair 2 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvs -o integritymismatches $vg/${lv1}_rimage_0
lvs -o integritymismatches $vg/${lv1}_rimage_1
lvs -o integritymismatches $vg/${lv1}_rimage_2
lvs -o integritymismatches $vg/${lv1}_rimage_3
lvs -o integritymismatches $vg/${lv1}_rimage_4
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1

# remove from active lv
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/$lv1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# add to active lv
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y --raidintegritymode bitmap $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# lvextend active
lvcreate --type raid1 --raidintegrity y --raidintegritymode bitmap -m1 -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
resize2fs "$DM_DEV_DIR/$vg/$lv1"
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# add image to raid1
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg
lvs -a -o name,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
integrity_add_new_data_to_mnt
lvconvert -y -m+1 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1

# Test that raid+integrity cannot be a sublv
# part1: cannot add integrity to a raid LV that is already a sublv

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvconvert -y --type thin-pool $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_tdata
not lvconvert --raidintegrity y $vg/${lv1}_tmeta
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvconvert -y --type cache-pool $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_cdata
not lvconvert --raidintegrity y $vg/${lv1}_cmeta
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type cache-pool -n cpool -l 8 $vg
lvconvert -y --type cache --cachepool cpool $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type raid1 -m1 -n cvol -l 8 $vg
lvconvert -y --type cache --cachevol cvol $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
not lvconvert --raidintegrity y $vg/cvol
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate -n cvol -l 8 $vg
lvchange -an $vg
lvconvert -y --type writecache --cachevol cvol $vg/$lv1
not lvconvert --raidintegrity y $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_wcorig
lvremove -y $vg/$lv1

# Test that raid+integrity cannot be a sublv
# part2: cannot convert an existing raid+integrity LV into a sublv

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvconvert -y --type thin-pool $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_tdata
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type raid1 -m1 -n $lv2 -l 8 $vg
lvconvert -y --type cache --cachevol $lv2 $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
not lvconvert --raidintegrity y $vg/${lv2}_cvol
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvcreate --type raid1 -m1 -n $lv2 -l 8 $vg
lvconvert -y --type cache --cachepool $lv2 $vg/$lv1
not lvconvert --raidintegrity y $vg/${lv1}_corig
not lvconvert --raidintegrity y $vg/${lv2}_cpool_cdata
not lvconvert --raidintegrity y $vg/${lv2}_cpool_cmeta
lvremove -y $vg/$lv1

lvcreate --type raid1 -m1 -n $lv1 -l 8 --raidintegrity y --integritysettings journal_watermark=10 $vg
lvs -a -o integritysettings $vg/${lv1}_rimage_0 | grep journal_watermark=10
not lvchange --integritysettings journal_watermark=20 $vg/$lv1
lvchange -an $vg/$lv1
lvchange --integritysettings journal_watermark=80 $vg/$lv1
lvchange -ay $vg/$lv1
lvs -a -o integritysettings $vg/${lv1}_rimage_0 | grep journal_watermark=80
# The journal_watermark value reported in the table may be
# slightly different from the value set due to the kernel
# reporting it as a computed value that may include rounding.
dmsetup table $vg-${lv1}_rimage_0 | tee out
grep journal_watermark out
lvchange -an $vg/$lv1
lvremove -y $vg/$lv1

vgremove -ff $vg
