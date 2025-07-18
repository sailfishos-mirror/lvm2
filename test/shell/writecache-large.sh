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


. lib/inittest --skip-with-lvmpolld

aux have_writecache 1 0 0 || skip
which mkfs.xfs || skip

# scsi_debug devices with 512 LBS 512 PBS
aux prepare_scsi_debug_dev 1200
check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "512"
check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "512"

aux prepare_devs 2 600
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

mnt="mnt"
mkdir -p $mnt

awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_add_new_data_to_mnt() {
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
	sync
}

_add_more_data_to_mnt() {
	mkdir $mnt/more
	cp fileA $mnt/more
	cp fileB $mnt/more
	cp fileC $mnt/more
	cp randA $mnt/more
	cp randB $mnt/more
	cp randC $mnt/more
	sync
}

_verify_data_on_mnt() {
	diff randA $mnt/randA
	diff randB $mnt/randB
	diff randC $mnt/randC
	diff fileA $mnt/1/fileA
	diff fileB $mnt/1/fileB
	diff fileC $mnt/1/fileC
	diff fileA $mnt/2/fileA
	diff fileB $mnt/2/fileB
	diff fileC $mnt/2/fileC
}

_verify_more_data_on_mnt() {
	diff randA $mnt/more/randA
	diff randB $mnt/more/randB
	diff randC $mnt/more/randC
	diff fileA $mnt/more/fileA
	diff fileB $mnt/more/fileB
	diff fileC $mnt/more/fileC
}

_verify_data_on_lv() {
	lvchange -ay $vg/$lv1
	mount "$DM_DEV_DIR/$vg/$lv1" $mnt
	_verify_data_on_mnt
	rm $mnt/randA
	rm $mnt/randB
	rm $mnt/randC
	rm -rf $mnt/1
	rm -rf $mnt/2
	umount $mnt
	lvchange -an $vg/$lv1
}

vgcreate $SHARED $vg "$dev1"
vgextend $vg "$dev2"

# Use a large enough size so that the cleaner will not
# finish immediately when detaching, and will require
# a secondary check from command top level.

lvcreate -n $lv1 -L 560M -an $vg "$dev1"
lvcreate -n $lv2 -L 500M -an $vg "$dev2"

lvchange -ay $vg/$lv1
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"

lvconvert --yes --type writecache --cachevol $lv2 $vg/$lv1
dmsetup table $vg-$lv1
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"

_add_new_data_to_mnt
_add_more_data_to_mnt
_verify_data_on_mnt

dd if=/dev/zero of=$mnt/big1 bs=1M count=100 conv=fdatasync
dd if=/dev/zero of=$mnt/big2 bs=1M count=100 conv=fdatasync
dd if=/dev/zero of=$mnt/big3 bs=1M count=100 conv=fdatasync
dd if=/dev/zero of=$mnt/big4 bs=1M count=100 conv=fdatasync

lvconvert --splitcache $vg/$lv1
check lv_field $vg/$lv1 segtype linear
check lv_field $vg/$lv2 segtype linear
dmsetup table $vg-$lv1
_verify_data_on_mnt
_verify_more_data_on_mnt
dd if=$mnt/big4 of=/dev/null bs=1M count=100
umount $mnt
lvchange -an $vg/$lv1
_verify_data_on_lv
lvchange -an $vg/$lv2
lvremove $vg/$lv1
lvremove $vg/$lv2

# Repeat similar using uncache

lvcreate -n $lv1 -L 560M -an $vg "$dev1"
lvcreate -n $lv2 -L 500M -an $vg "$dev2"

lvchange -ay $vg/$lv1
lvconvert --yes --type writecache --cachevol $lv2 $vg/$lv1

_add_new_data_to_mnt
_add_more_data_to_mnt
dd if=/dev/zero of=$mnt/big1 bs=1M count=100 conv=fdatasync

umount $mnt
lvchange -an $vg/$lv1

lvconvert --uncache $vg/$lv1

check lv_field $vg/$lv1 segtype linear
not lvs $vg/$lv2

lvchange -ay $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" $mnt

_verify_data_on_mnt
_verify_more_data_on_mnt

umount $mnt
lvchange -an $vg/$lv1

vgremove -ff $vg
