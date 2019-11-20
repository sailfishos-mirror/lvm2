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

SKIP_WITH_LVMPOLLD=1

. lib/inittest

# aux have_integrity 1 0 0 || skip
which mkfs.xfs || skip

mnt="mnt"
mkdir -p $mnt
aux prepare_devs 5 64

for i in `seq 1 16384`; do echo -n "A" >> fileA; done
for i in `seq 1 16384`; do echo -n "B" >> fileB; done
for i in `seq 1 16384`; do echo -n "C" >> fileC; done

_prepare_vg() {
	# zero devs so we are sure to find the correct file data
	# on the underlying devs when corrupting it
	dd if=/dev/zero of="$dev1" || true
	dd if=/dev/zero of="$dev2" || true
	dd if=/dev/zero of="$dev3" || true
	dd if=/dev/zero of="$dev4" || true
	dd if=/dev/zero of="$dev5" || true
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
}

_test_fs_with_error() {
	mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# add original data
	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	# corrupt the original data on the underying dev
	# flip one bit in fileB, changing a 0x42 to 0x43
	# the bit is changed in the last 4096 byte block
	# of the file, so when reading back the file we
	# will get the first three 4096 byte blocks, for
	# a total of 12288 bytes before getting an error
	# on the last 4096 byte block.
	xxd "$dev1" > dev1.txt
	tac dev1.txt > dev1.rev
	sed -e '0,/4242 4242 4242 4242 4242 4242 4242 4242/ s/4242 4242 4242 4242 4242 4242 4242 4242/4242 4242 4242 4242 4242 4242 4242 4243/' dev1.rev > dev1.rev.bad
	tac dev1.rev.bad > dev1.bad
	xxd -r dev1.bad > "$dev1"
	rm dev1.txt dev1.rev dev1.rev.bad dev1.bad

	lvchange -ay $vg/$lv1
	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# read complete fileA which was not corrupted
	dd if=$mnt/fileA of=tmp bs=1k
	ls -l tmp
	stat -c %s tmp
	diff fileA tmp
	rm tmp

	# read partial fileB which was corrupted
	not dd if=$mnt/fileB of=tmp bs=1k
	ls -l tmp
	stat -c %s tmp | grep 12288
	not diff fileB tmp
	rm tmp

	umount $mnt
}

_test_fs_with_raid() {
	mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# add original data
	cp fileA $mnt
	cp fileB $mnt
	cp fileC $mnt

	umount $mnt
	lvchange -an $vg/$lv1

	xxd "$dev1" > dev1.txt
	tac dev1.txt > dev1.rev
	sed -e '0,/4242 4242 4242 4242 4242 4242 4242 4242/ s/4242 4242 4242 4242 4242 4242 4242 4242/4242 4242 4242 4242 4242 4242 4242 4243/' dev1.rev > dev1.rev.bad
	tac dev1.rev.bad > dev1.bad
	xxd -r dev1.bad > "$dev1"
	rm dev1.txt dev1.rev dev1.rev.bad dev1.bad

	lvchange -ay $vg/$lv1
	mount "$DM_DEV_DIR/$vg/$lv1" $mnt

	# read complete fileA which was not corrupted
	dd if=$mnt/fileA of=tmp bs=1k
	ls -l tmp
	stat -c %s tmp | grep 16384
	diff fileA tmp
	rm tmp

	# read complete fileB, corruption is corrected by raid
	dd if=$mnt/fileB of=tmp bs=1k
	ls -l tmp
	stat -c %s tmp | grep 16384
	diff fileB tmp
	rm tmp

	umount $mnt
}

_prepare_vg
lvcreate --integrity y -n $lv1 -l 8 $vg "$dev1"
_test_fs_with_error
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate -y --integrity internal -n $lv1 -l 8 $vg "$dev1"
_test_fs_with_error
lvchange -an $vg/$lv1
not lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate -an -n meta -L4M $vg "$dev2"
lvcreate --integrity y --integritymetadata meta -n $lv1 -l 8 $vg "$dev1"
_test_fs_with_error
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m1 --integrity y -n $lv1 -l 8 $vg
_test_fs_with_raid
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid1 -m2 --integrity y -n $lv1 -l 8 $vg
_test_fs_with_raid
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid4 --integrity y -n $lv1 -l 8 $vg
_test_fs_with_raid
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid5 --integrity y -n $lv1 -l 8 $vg
_test_fs_with_raid
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

_prepare_vg
lvcreate --type raid6 --integrity y -n $lv1 -l 8 $vg
_test_fs_with_raid
lvchange -an $vg/$lv1
lvconvert --integrity n $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg
