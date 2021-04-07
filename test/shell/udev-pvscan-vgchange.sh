#!/usr/bin/env bash

# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='udev rule and systemd unit run vgchange'

. lib/inittest

#
# To use this test, add two or more devices with real device ids,
# e.g. wwids, to a file, e.g.
# $ cat /tmp/devs
# /dev/sdb
# /dev/sdc
# /dev/sdd
#
# Specify this file as LVM_TEST_DEVICE_LIST=/tmp/devs
# when running the test.
#
# This test will wipe these devices.
#

if [ -z ${LVM_TEST_DEVICE_LIST+x} ]; then echo "LVM_TEST_DEVICE_LIST is unset" && skip; else echo "LVM_TEST_DEVICE_LIST is set to '$LVM_TEST_DEVICE_LIST'"; fi

test -e "$LVM_TEST_DEVICE_LIST" || skip

num_devs=$(cat $LVM_TEST_DEVICE_LIST | wc -l)

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"
PVS_LOOKUP_DIR="$RUNDIR/lvm/pvs_lookup"

_clear_online_files() {
	# wait till udev is finished
	aux udev_wait
	rm -f "$PVS_ONLINE_DIR"/*
	rm -f "$VGS_ONLINE_DIR"/*
	rm -f "$PVS_LOOKUP_DIR"/*
}

test -d "$PVS_ONLINE_DIR" || mkdir -p "$PVS_ONLINE_DIR"
test -d "$VGS_ONLINE_DIR" || mkdir -p "$VGS_ONLINE_DIR"
test -d "$PVS_LOOKUP_DIR" || mkdir -p "$PVS_LOOKUP_DIR"
_clear_online_files

aux prepare_real_devs

aux lvmconf 'devices/dir = "/dev"'
aux lvmconf 'devices/use_devicesfile = 1'
DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"
mkdir $DFDIR || true
not ls $DF

get_real_devs

wipe_all() {
	for dev in "${REAL_DEVICES[@]}"; do
		wipefs -a $dev
	done
}

wipe_all

touch $DF
for dev in "${REAL_DEVICES[@]}"; do
	pvcreate $dev
done

test $num_devs -gt 2 || skip

# 1 dev, 1 vg, 1 lv

vgcreate $vg1 "$dev1"
lvcreate -l1 -an -n $lv1 $vg1 "$dev1"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
BDEV1=$(basename "$dev1")

_clear_online_files
udevadm trigger --settle -c add /sys/block/$BDEV1

ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/vgs_online/$vg1"
systemctl status lvm-vgchange@$vg1 | tee out || true
grep Started out
check lv_field $vg1/$lv1 lv_active "active"

vgchange -an $vg1
vgremove -y $vg1


# 2 devs, 1 vg, 2 lvs

vgcreate $vg2 "$dev1" "$dev2"
lvcreate -l1 -an -n $lv1 $vg2 "$dev1"
lvcreate -l1 -an -n $lv2 $vg2 "$dev2"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID2=$(pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}')
BDEV1=$(basename "$dev1")
BDEV2=$(basename "$dev2")

_clear_online_files

udevadm trigger --settle -c add /sys/block/$BDEV1
ls "$RUNDIR/lvm/pvs_online/$PVID1"
not ls "$RUNDIR/lvm/vgs_online/$vg2"
systemctl status lvm-vgchange@$vg2 | tee out || true
not grep Started out
check lv_field $vg2/$lv1 lv_active ""
check lv_field $vg2/$lv2 lv_active ""

udevadm trigger --settle -c add /sys/block/$BDEV2
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/vgs_online/$vg2"
systemctl status lvm-vgchange@$vg2 | tee out || true
grep Started out
check lv_field $vg2/$lv1 lv_active "active"
check lv_field $vg2/$lv2 lv_active "active"

vgchange -an $vg2
vgremove -y $vg2


# 3 devs, 1 vg, 4 lvs, concurrent pvscans
# (attempting to have the pvscans run concurrently and race
# to activate the VG)

vgcreate $vg3 "$dev1" "$dev2" "$dev3"
lvcreate -l1 -an -n $lv1 $vg3 "$dev1"
lvcreate -l1 -an -n $lv2 $vg3 "$dev2"
lvcreate -l1 -an -n $lv3 $vg3 "$dev3"
lvcreate -l8 -an -n $lv4 -i 2 $vg3 "$dev1" "$dev2"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID2=$(pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID3=$(pvs "$dev3" --noheading -o uuid | tr -d - | awk '{print $1}')
BDEV1=$(basename "$dev1")
BDEV2=$(basename "$dev2")
BDEV3=$(basename "$dev3")

_clear_online_files

udevadm trigger -c add /sys/block/$BDEV1 &
udevadm trigger -c add /sys/block/$BDEV2 &
udevadm trigger -c add /sys/block/$BDEV3

sleep 5
aux udev_wait

ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
ls "$RUNDIR/lvm/vgs_online/$vg3"
systemctl status lvm-vgchange@$vg3 | tee out || true
grep Started out
check lv_field $vg3/$lv1 lv_active "active"
check lv_field $vg3/$lv2 lv_active "active"
check lv_field $vg3/$lv3 lv_active "active"
check lv_field $vg3/$lv4 lv_active "active"

vgchange -an $vg3
vgremove -y $vg3


# 3 devs, 1 vg, 4 lvs, concurrent pvscans, metadata on only 1 PV

wipe_all
rm $DF
touch $DF
pvcreate --metadatacopies 0 "$dev1"
pvcreate --metadatacopies 0 "$dev2"
pvcreate "$dev3"

vgcreate $vg4 "$dev1" "$dev2" "$dev3"
lvcreate -l1 -an -n $lv1 $vg4 "$dev1"
lvcreate -l1 -an -n $lv2 $vg4 "$dev2"
lvcreate -l1 -an -n $lv3 $vg4 "$dev3"
lvcreate -l8 -an -n $lv4 -i 2 $vg4 "$dev1" "$dev2"

PVID1=$(pvs "$dev1" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID2=$(pvs "$dev2" --noheading -o uuid | tr -d - | awk '{print $1}')
PVID3=$(pvs "$dev3" --noheading -o uuid | tr -d - | awk '{print $1}')
BDEV1=$(basename "$dev1")
BDEV2=$(basename "$dev2")
BDEV3=$(basename "$dev3")

_clear_online_files

udevadm trigger -c add /sys/block/$BDEV1 &
udevadm trigger -c add /sys/block/$BDEV2 &
udevadm trigger -c add /sys/block/$BDEV3

sleep 5
aux udev_wait

ls "$RUNDIR/lvm/pvs_online/$PVID1"
ls "$RUNDIR/lvm/pvs_online/$PVID2"
ls "$RUNDIR/lvm/pvs_online/$PVID3"
ls "$RUNDIR/lvm/vgs_online/$vg4"
systemctl status lvm-vgchange@$vg4 | tee out || true
grep Started out
check lv_field $vg4/$lv1 lv_active "active"
check lv_field $vg4/$lv2 lv_active "active"
check lv_field $vg4/$lv3 lv_active "active"
check lv_field $vg4/$lv4 lv_active "active"

vgchange -an $vg4
vgremove -y $vg4

