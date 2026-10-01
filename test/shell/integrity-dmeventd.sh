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

aux have_fs ext4 || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

aux lvmconf 'activation/raid_fault_policy = "allocate"'

aux prepare_dmeventd

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 6 64

integrity_make_files
integrity_make_rand

_prepare_vg() {
	integrity_prepare_vg "$dev1" "$dev2" "$dev3" "$dev4"
}

# Wait for dmeventd repair to complete by checking if specified devices
# are no longer present in lvs output. Times out after 11 seconds.
# Usage: _wait_for_repair dev1 [dev2 ...]
_wait_for_repair() {
	local dev
	touch "$mnt/touch"
	sync
	for i in {1..13}; do
		sleep 1
		lvs -a -o+devices $vg > out 2>&1 || true
		for dev in "$@"; do
			grep -q "$dev" out && continue 2
		done
		# All devices gone, repair completed
		return 0
	done
	# Print diagnostics before die() drops the debug logs.
	{
		echo "## dmeventd repair did not complete for: $*"
		echo "## lvs state:"
		cat out 2>/dev/null || true
		echo "## dmeventd log tail:"
		tail -n 200 debug.log_DMEVENTD_out 2>/dev/null || true
	} >&2
	die "dmeventd repair timeout - expected devices removed: $*."
}

# raid1, one device fails, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvcreate --type raid1 -m 2 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_for_sync $vg $lv1
integrity_add_new_data_to_mnt

aux disable_dev "$dev2"

# wait for dmeventd to call lvconvert --repair which should
# replace dev2 with dev4
_wait_for_repair "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev4" out

integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt

aux enable_dev "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev4" out
grep "$dev1" out
grep "$dev3" out

umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# raid1, two devices fail, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type raid1 -m 2 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_for_sync $vg $lv1
integrity_add_new_data_to_mnt

aux disable_dev "$dev1" "$dev2"

# wait for dmeventd to call lvconvert --repair which should
# replace dev1 and dev2 with dev4 and dev5
_wait_for_repair "$dev1" "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev4" out
grep "$dev5" out
grep "$dev3" out

integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt

aux enable_dev "$dev1" "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
not grep "$dev2" out
grep "$dev4" out
grep "$dev5" out
grep "$dev3" out

umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# raid6, one device fails, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"
lvcreate --type raid6 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_recalc $vg/${lv1}_rimage_4
aux wait_for_sync $vg $lv1
integrity_add_new_data_to_mnt

aux disable_dev "$dev2"

# wait for dmeventd to call lvconvert --repair which should
# replace dev2 with dev6
_wait_for_repair "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev6" out

integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt

aux enable_dev "$dev2"

lvs -a -o+devices $vg | tee out
not grep "$dev2" out
grep "$dev6" out

umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# raid10, one device fails, dmeventd calls repair

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type raid10 --raidintegrity y --ignoremonitoring -l 8 -n $lv1 $vg "$dev1" "$dev2" "$dev3" "$dev4"
lvchange --monitor y $vg/$lv1
lvs -a -o+devices $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/${lv1}_rimage_3
aux wait_for_sync $vg $lv1
integrity_add_new_data_to_mnt

aux disable_dev "$dev1"

# wait for dmeventd to call lvconvert --repair which should
# replace dev1 with dev5
_wait_for_repair "$dev1"

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
grep "$dev5" out

integrity_add_more_data_to_mnt
integrity_verify_data_on_mnt

aux enable_dev "$dev1"

lvs -a -o+devices $vg | tee out
not grep "$dev1" out
grep "$dev5" out

umount "$mnt"
lvchange -an $vg/$lv1
integrity_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg
