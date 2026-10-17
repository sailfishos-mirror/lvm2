#!/usr/bin/env bash

# Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.


. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux prepare_devs 4

vgcreate $vg1 "$dev1"
vgchange --setautoactivation n $vg1
UUID1=$(vgs --noheadings -o vg_uuid $vg1 | xargs)
lvcreate -l1 -an -n $lv1 $vg1
dd if="$dev1" of="$dev2" bs=1M count=1
aux disable_dev "$dev1"
vgrename $vg1 $vg2
pvchange -u "$dev2"
aux enable_dev "$dev1"

vgs -o+uuid |tee out
grep $vg1 out | tee out1
grep $UUID1 out1
grep $vg2 out | tee out2
grep $UUID1 out2

vgs $vg1
vgs $vg2
lvs $vg1/$lv1
lvs $vg2/$lv1

lvremove $vg1/$lv1
lvremove $vg2/$lv1

lvcreate -l1 -an -n $lv2 $vg1
lvcreate -l1 -an -n $lv3 $vg2

vgchange -u $vg2

vgs -o uuid $vg1 |tee out
grep $UUID1 out

vgs -o uuid $vg2 |tee out
not grep $UUID1 out

vgremove -ff $vg1
vgremove -ff $vg2

# A VG using the VGID of vg1 and another VG using the name of vg1
# must not make vg1, or one of its PVs, disappear.  The name of vg1
# is a duplicate VG name, it must not refer to the other VG.
# args: PV for the VG with same VGID, PV for the VG with same name, PVs for vg1
_duplicate_vgid_and_vgname() {
	local samevgid=$1
	local samename=$2
	local dev
	shift 2

	vgcreate $vg1 "$@"
	vgchange --setautoactivation n $vg1
	UUID1=$(vgs --noheadings -o vg_uuid $vg1 | xargs)
	lvcreate -l1 -an -n $lv1 $vg1 "$1"
	dd if="$1" of="$samevgid" bs=1M count=1
	aux disable_dev "$@"
	vgreduce --removemissing $vg1
	vgrename $vg1 $vg2
	pvchange -u "$samevgid"
	vgcreate $vg1 "$samename"
	UUID2=$(vgs --noheadings -o vg_uuid $vg1 | xargs)
	aux enable_dev "$@"

	for dev in "$@"; do
		check pv_field "$dev" vg_name $vg1
		check pv_field "$dev" vg_uuid $UUID1
	done
	check pv_field "$samevgid" vg_name $vg2
	check pv_field "$samevgid" vg_uuid $UUID1
	check pv_field "$samename" vg_name $vg1
	check pv_field "$samename" vg_uuid $UUID2

	vgs --noheadings -o name,uuid | tee out
	grep $vg1 out | grep $UUID1
	grep $vg1 out | grep $UUID2
	grep $vg2 out | grep $UUID1

	# the name is ambiguous
	not vgs $vg1
	not lvs $vg1/$lv1
	not lvcreate -l1 -an -n $lv2 $vg1

	# fix the duplicates
	vgrename $UUID2 $vg3
	vgchange -u $vg2
	vgs 2>&1 | tee out
	not grep WARNING out
	check vg_field $vg1 vg_uuid $UUID1
	lvs $vg1/$lv1
	lvcreate -l1 -an -n $lv2 $vg1

	vgremove -ff $vg1
	vgremove -ff $vg2
	vgremove -ff $vg3
}

# devs are scanned in the order dev1, dev2, dev3, dev4
_duplicate_vgid_and_vgname "$dev1" "$dev2" "$dev3"
_duplicate_vgid_and_vgname "$dev3" "$dev1" "$dev2"
_duplicate_vgid_and_vgname "$dev2" "$dev3" "$dev1" "$dev4"
