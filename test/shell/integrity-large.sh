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

aux have_integrity 1 5 0 || skip
which mkfs.xfs || skip

mnt="mnt"
mkdir -p $mnt

# raid1 LV needs to be extended to 512MB to test imeta being exended
aux prepare_devs 2 600

for i in `seq 1 16384`; do echo -n "A" >> fileA; done
for i in `seq 1 16384`; do echo -n "B" >> fileB; done
for i in `seq 1 16384`; do echo -n "C" >> fileC; done

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

_sync_percent() {
	local checklv=$1
	get lv_field "$checklv" sync_percent | cut -d. -f1
}

_wait_recalc() {
	local checklv=$1

	for i in $(seq 1 10) ; do
		sync=$(_sync_percent "$checklv")
		echo "sync_percent is $sync"

		if test "$sync" = "100"; then
			return
		fi

		sleep 1
	done

	echo "timeout waiting for recalc"
	return 1
}

# lvextend to 512MB is needed for the imeta LV to
# be extended from 4MB to 8MB.

_prepare_vg
lvcreate --type raid1 -m1 -n $lv1 -l 8 $vg
lvchange -an $vg/$lv1
lvchange -ay $vg/$lv1
_add_data_to_lv
lvconvert --raidintegrity y $vg/$lv1
_wait_recalc $vg/$lv1
lvs -a -o+devices $vg
_verify_data_on_lv
lvchange -an $vg/$lv1
lvextend -L 512M $vg/$lv1
lvs -a -o+devices $vg
lvchange -ay $vg/$lv1
_verify_data_on_lv
_wait_recalc $vg/$lv1
lvs -a -o+devices $vg
check lv_field $vg/${lv1}_rimage_0_imeta size "8.00m"
check lv_field $vg/${lv1}_rimage_1_imeta size "8.00m"
lvchange -an $vg/$lv1
lvremove $vg/$lv1
vgremove -ff $vg

