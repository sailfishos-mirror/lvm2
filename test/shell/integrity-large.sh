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

# Test writecache usage

SKIP_WITH_LOW_SPACE=1100

. lib/inittest --skip-with-lvmpolld

aux have_integrity 1 5 0 || skip
aux have_fs xfs || skip

mnt="mnt"
mkdir -p "$mnt"

# raid1 LV needs to be extended to 512MB to test imeta being extended
aux prepare_devs 4 632

# this test may consume lot of disk space - so make sure cleaning works
# also in failure case
cleanup_mounted_and_teardown()
{
	umount "$mnt" 2>/dev/null || true
	# Comment out this 'vgremove' when there is any need to analyze
	# content of the failed test dir, otherwise all is deleted.
	vgremove -ff $vg || true
	aux teardown
}

trap 'cleanup_mounted_and_teardown' EXIT

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_prepare_vg() {
	vgcreate $SHARED $vg "$dev1" "$dev2"
	pvs
}

_add_data_to_lv() {
	mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# add original data
	cp randA $mnt
	cp randB $mnt
	cp randC $mnt
	mkdir $mnt/1
	cp fileA $mnt/1
	cp fileB $mnt/1
	cp fileC $mnt/1
	mkdir $mnt/2
	cp fileA $mnt/2
	cp fileB $mnt/2
	cp fileC $mnt/2

	umount $mnt
}

_verify_data_on_lv() {
	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	diff randA $mnt/randA
	diff randB $mnt/randB
	diff randC $mnt/randC
	diff fileA $mnt/1/fileA
	diff fileB $mnt/1/fileB
	diff fileC $mnt/1/fileC
	diff fileA $mnt/2/fileA
	diff fileB $mnt/2/fileB
	diff fileC $mnt/2/fileC

	umount $mnt
}

# lvextend to 512MB is needed for the imeta LV to
# be extended from 4MB to 8MB.

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -L 300 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
_add_data_to_lv
# lv needs to be inactive when adding integrity to increase LBS from 512 and get a ribs of 4k
lvchange -an $vg/$lv1
lvconvert --raidintegrity y $vg/$lv1
lvchange -ay $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
_verify_data_on_lv
lvchange -an $vg/$lv1
lvextend -L 512M $vg/$lv1
lvs -a -o+devices $vg
lvchange -ay $vg/$lv1
_verify_data_on_lv
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
check lv_field $vg/${lv1}_rimage_0_imeta size "12.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "12.00m"

# provide space to extend the images onto new devs
vgextend $vg "$dev3" "$dev4"

# extending the images is possible using dev3,dev4
# but extending imeta on the existing dev1,dev2 fails
not lvextend -L +512M $vg/$lv1

# removing integrity will permit extending the images
# using dev3,dev4 since imeta limitation is gone
lvconvert --raidintegrity n $vg/$lv1
lvextend -L +512M $vg/$lv1
lvs -a -o+devices $vg

# adding integrity again will allocate new 12MB imeta LVs
# on dev3,dev4
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg
check lv_field $vg/${lv1}_rimage_0_imeta size "20.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "20.00m"

lvchange -an $vg/$lv1
lvremove $vg/$lv1

# lvextend must size the integrity metadata with the journal given at creation
# time.  With 4MiB extents, a 400MiB image with a 32MiB journal (65536 sectors)
# needs (400 / 500 + 1) * 4MiB = 4MiB for the tags plus the 32MiB journal, i.e.
# 36MiB.  Extended to 504MiB it needs (504 / 500 + 1) * 4MiB = 8MiB plus the
# same 32MiB journal, i.e. 40MiB.  Sizing it with the default 4MiB journal of
# images up to 512MiB gives only 12MiB, so the 36MiB metadata LV looked large
# enough and was left too small for the journal and the larger tag area.
check vg_field $vg vg_extent_size 4.00m
lvcreate --type raid1 -m1 --raidintegrity y -an -L 400m -n $lv1 \
	--integritysettings journal_sectors=65536 $vg "$dev1" "$dev2"
check lv_field $vg/${lv1}_rimage_0_imeta size "36.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "36.00m"

lvextend -L 504m $vg/$lv1
check lv_field $vg/${lv1}_rimage_0_imeta size "40.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "40.00m"

# the kernel rejects metadata that cannot hold the journal and the tags
lvchange -ay $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
lvs -a -o+devices $vg

lvchange -an $vg/$lv1
lvremove $vg/$lv1

# delay legs so not all data need to be written, the lvremove below
# then runs while the resync is still in progress
aux delay_dev "$dev1" 400 0 "$(( $(get first_extent_sector "$dev1") + 16000 )):1200000"
aux delay_dev "$dev2" 0 10 "$(( $(get first_extent_sector "$dev2") + 16000 )):1200000"

# this succeeds because dev1,dev2 can hold rmeta+rimage
lvcreate --type raid1 -n $lv1 -L 592M -an $vg "$dev1" "$dev2"
lvs -a -o+devices $vg
lvremove $vg/$lv1
aux enable_dev "$dev1" "$dev2"

# this fails because dev1,dev2 can hold rmeta+rimage, but not imeta
# and we require imeta to be on same devs as rmeta/rimeta
not lvcreate --type raid1 --raidintegrity y -n $lv1 -L 624M -an $vg "$dev1" "$dev2"
lvs -a -o+devices $vg

# this can allocate from more devs so there's enough space for imeta to
# be allocated in the vg, but lvcreate fails because rmeta+rimage are
# allocated from dev1,dev2, we restrict imeta to being allocated on the
# same devs as rmeta/rimage, and dev1,dev2 can't fit imeta.
not lvcreate --type raid1 --raidintegrity y -n $lv1 -L 624M -an $vg
lvs -a -o+devices $vg

# counterintuitively, increasing the size will allow lvcreate to succeed
# because rmeta+rimage are pushed to being allocated on dev1,dev2,dev3,dev4
# which means imeta is now free to be allocated from dev3,dev4 which have
# plenty of space
lvcreate --type raid1 --raidintegrity y -n $lv1 -L 640M -an $vg
lvs -a -o+devices $vg

vgremove -ff $vg

# lvextend must round the integrity metadata up to whole VG extents.  With 3MiB
# extents a 498MiB image (166 extents) needs (498 / 500 + 1) * 4MiB = 4MiB of
# metadata plus the 4MiB initial journal, i.e. 8MiB = 3 extents = 9MiB.  After
# extending to 600MiB (200 extents) it needs (600 / 500 + 1) * 4MiB = 8MiB plus
# the 8MiB journal bucket of images over 512MiB, i.e. 16MiB, which is 6 whole
# extents = 18MiB.  Rounding down would leave 5 extents = 15MiB, short of the
# 16MiB the journal needs.
vgcreate $SHARED -s 3m $vg "$dev1" "$dev2"
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 166 $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
check lv_field $vg/${lv1}_rimage_0_imeta size "9.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "9.00m"

lvextend -l 200 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
check lv_field $vg/${lv1}_rimage_0_imeta size "18.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "18.00m"

vgremove -ff $vg
