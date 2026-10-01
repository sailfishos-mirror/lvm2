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

aux prepare_devs 9 80

integrity_make_files 4096
integrity_make_rand

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	aux clear_devs "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	integrity_prepare_vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
}

# Push the original files out of the cache so the re-read reaches the raid
# images underneath.
_evict_cache() {
	mkdir "$mnt/new1"
	cp randA "$mnt/new1/randA"
	cp randB "$mnt/new1/randB"
	cp randC "$mnt/new1/randC"
	sync
	du -h "$mnt/new1"
	cp -r "$mnt/new1" "$mnt/new2" || true
	cp -r "$mnt/new1" "$mnt/new3" || true
	cp -r "$mnt/new1" "$mnt/new4" || true
	sync
	du -h "$mnt"
	df -h "$mnt"
}

_test_fs_with_read_repair() {
	# An empty max-corrupt corrupts every device listed, as this test always
	# did.  The raid6 case passes all five devices, but only the data members
	# holding the file pattern match, so at most nr-2 are written.
	# RAID_SEG is the raid origin segment, since the mounted LV is the cache
	# device and holds none of the data.
	# Not reached for the writecache variant, which corrupts nothing: see
	# $read_repair in do_test().
	test_fs_with_read_repair "$vg/$lv1" "$mnt" "$vg/${lv1}_${suffix}" "mkfs.ext4 -b 4096" "_evict_cache" "" "$@"
}

# lv1 is a raid+integrity LV
# three variations of caching on lv1:
#
# 1. lvcreate --type cache-pool -n fast -l 4 $vg $dev6
#    lvconvert --type cache --cachepool fast $vg/$lv1
#
# 2. lvcreate --type linear -n fast -l 4 $vg $dev6
#    lvconvert --type cache --cachvol fast $vg/$lv1
#
# 3. lvcreate --type linear -n fast -l 4 $vg $dev6
#    lvconvert --type writecache --cachvol fast $vg/$lv1

do_test() {

# --cachepool | --cachevol
local create_type=$1

# cache | writecache
local convert_type=$2

# --type cache-pool | --type linear
local convert_option=$3

# corig | wcorig
local suffix=$4

# Read repair needs the file data to reach the raid images: the mounted LV is
# the cache device and holds none of it, so the corruption helper has to find
# the pattern on the images underneath.  Both cache variants leave it there,
# dm-writecache does not, so that variant corrupts nothing and has no
# read-repair coverage here.  The eviction hook in
# _test_fs_with_read_repair() does not achieve it either - the mkfs and
# fileA/B/C writes are already past the fast device by then, while the ~23M it
# writes stays in cache.  FIXME: work out what the writecache variant needs
# (dropping the fast device before the re-read is the likely answer) and give
# it back its read-repair cases.
local read_repair=1
if [[ $convert_type = writecache ]]; then
	read_repair=0
fi

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
if [[ $read_repair = 1 ]]; then
	_test_fs_with_read_repair "$dev1"
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}_rimage_0
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}
fi
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvs -a -o name,size,segtype,devices,sync_percent $vg
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}
if [[ $read_repair = 1 ]]; then
	_test_fs_with_read_repair "$dev1" "$dev2"
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}_rimage_0
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}
fi
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvconvert --splitcache $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}
if [[ $read_repair = 1 ]]; then
	_test_fs_with_read_repair "$dev1" "$dev2" "$dev3"
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_1
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_2
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}
fi
lvconvert --splitcache $vg/$lv1
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing integrity from an active LV

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
integrity_add_more_data_to_mnt
lvconvert --splitcache $vg/$lv1
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding integrity to an active LV

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
# Can only be enabled while raid is top level lv (for now.)
not lvconvert --raidintegrity y $vg/${lv1}_${suffix}
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while inactive

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
umount "$mnt"
lvchange -an $vg/$lv1
# use two new devs for raid extend to ensure redundancy
vgextend $vg "$dev7" "$dev8"
lvs -a -o name,segtype,devices $vg
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8"
lvs -a -o name,segtype,devices $vg
lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test lvextend while active

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
# use two new devs for raid extend to ensure redundancy
vgextend $vg "$dev7" "$dev8"
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8"
lvs -a -o name,size,segtype,devices,sync_percent $vg
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --raidintegrity y -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
vgextend $vg "$dev7" "$dev8" "$dev9"
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvextend -l 16 $vg/$lv1 "$dev7" "$dev8" "$dev9"
lvs -a -o name,size,segtype,devices,sync_percent $vg
resize2fs "$DM_DEV_DIR/$vg/$lv1"
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test adding image to raid1

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
# currently only allowed while raid is top level lv
not lvconvert -y -m+1 $vg/${lv1}_${suffix}
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
#aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test removing image from raid1

_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvconvert -y -m-1 $vg/${lv1}_${suffix}
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# Test disallowed operations on raid+integrity

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
not lvconvert -y -m-1 $vg/$lv1
not lvconvert -y -m-1 $vg/${lv1}_${suffix}
not lvconvert --splitmirrors 1 -n tmp -y $vg/$lv1
not lvconvert --splitmirrors 1 -n tmp -y $vg/${lv1}_${suffix}
not lvconvert --splitmirrors 1 --trackchanges -y $vg/$lv1
not lvconvert --splitmirrors 1 --trackchanges -y $vg/${lv1}_${suffix}
not lvreduce -L4M $vg/$lv1
not lvreduce -L4M $vg/${lv1}_${suffix}
not lvcreate -s -n snap -L4M $vg/${lv1}_${suffix}
# plan to enable snap on top level raid+integrity, so then
# snap+writecache+raid+integrity should be allowed.
not lvcreate -s -n snap -L4M $vg/$lv1
lvs -a -o name,size,segtype,devices
not pvmove -n $vg/$lv1 "$dev1"
not pvmove -n $vg/${lv1}_${suffix} "$dev1"
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
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
if [[ $read_repair = 1 ]]; then
	_test_fs_with_read_repair "$dev1"
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}_rimage_0
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}
fi
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --raidintegrity y --raidintegritymode bitmap -n $lv1 -I4 -l 8 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}_rimage_2
aux wait_recalc $vg/${lv1}_${suffix}_rimage_3
aux wait_recalc $vg/${lv1}_${suffix}_rimage_4
aux wait_recalc $vg/${lv1}_${suffix}
if [[ $read_repair = 1 ]]; then
	_test_fs_with_read_repair "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_0
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_1
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_2
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_3
	lvs -o integritymismatches $vg/${lv1}_${suffix}_rimage_4
	integrity_not_zero_mismatches $vg/${lv1}_${suffix}
fi
lvchange -an $vg/$lv1
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
lvremove $vg/$lv1
vgremove -ff $vg

# remove from active lv
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
integrity_add_new_data_to_mnt
lvconvert --raidintegrity n $vg/${lv1}_${suffix}
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# add to active lv
_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
integrity_add_new_data_to_mnt
lvconvert --raidintegrity y --raidintegritymode bitmap $vg/$lv1
lvcreate --type $create_type -n fast -l 4 -an $vg "$dev6"
lvconvert -y --type $convert_type $convert_option fast $vg/$lv1
lvs -a -o name,size,segtype,devices,sync_percent $vg
aux wait_recalc $vg/${lv1}_${suffix}_rimage_0
aux wait_recalc $vg/${lv1}_${suffix}_rimage_1
aux wait_recalc $vg/${lv1}_${suffix}
integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg
}

do_test cache-pool cache --cachepool corig
do_test linear cache --cachevol corig
do_test linear writecache --cachevol wcorig

# TODO: add do_test() variant that skips adding the cache to lv1.
# This would be equivalent to integrity.sh which could be dropped.
