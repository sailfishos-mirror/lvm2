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

# Test single lv cache options

SKIP_WITH_LVMPOLLD=1

. lib/inittest

mkfs_mount_umount()
{
        lvt=$1

        lvchange -ay $vg/$lvt

        mkfs.xfs -f -s size=4096 "$DM_DEV_DIR/$vg/$lvt"
        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        cp pattern1 "$mount_dir/pattern1"
        dd if=/dev/zero of="$mount_dir/zeros2M" bs=1M count=32 conv=fdatasync
        umount "$mount_dir"

        lvchange -an $vg/$lvt
}

mount_umount()
{
        lvt=$1

        lvchange -ay $vg/$lvt

        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        diff pattern1 "$mount_dir/pattern1"
        dd if="$mount_dir/zeros2M" of=/dev/null bs=1M count=32
        umount "$mount_dir"

        lvchange -an $vg/$lvt
}

aux have_cache 1 10 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mkdir -p "$mount_dir"

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1

aux prepare_devs 6

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"

#
# writethrough
# no corruption of cachevol
#

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 4 -an $vg "$dev3"
lvcreate -n $lv3 -l 4 -an $vg "$dev4"

lvconvert -y --type cache --cachevol $lv2 --cachemode writethrough $vg/$lv1

mkfs_mount_umount $lv1

lvconvert -y --repaircachevol $lv2 $vg/$lv3

# new cache is valid since it was just copied from existing
lvchange -ay $vg/$lv3
cache_check "$DM_DEV_DIR/$vg/$lv3"
lvchange -an $vg/$lv3

# use new cache for main lv
lvconvert -y --replacecachevol $lv2 $vg/$lv3

# old cache is valid since it was never damaged
lvchange -ay $vg/$lv2
cache_check "$DM_DEV_DIR/$vg/$lv2"
lvchange -an $vg/$lv2

mount_umount $lv1

lvremove $vg/$lv1
lvremove $vg/$lv2

#
# writeback
# no corruption of cachevol
#

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 4 -an $vg "$dev3"
lvcreate -n $lv3 -l 4 -an $vg "$dev4"

lvconvert -y --type cache --cachevol $lv2 --cachemode writeback $vg/$lv1

mkfs_mount_umount $lv1

lvconvert -y --repaircachevol $lv2 $vg/$lv3

# new cache is valid since it was just copied from existing
lvchange -ay $vg/$lv3
cache_check "$DM_DEV_DIR/$vg/$lv3"
lvchange -an $vg/$lv3

# use new cache for main lv
lvconvert -y --replacecachevol $lv2 $vg/$lv3

# old cache is valid since it was never damaged
lvchange -ay $vg/$lv2
cache_check "$DM_DEV_DIR/$vg/$lv2"
lvchange -an $vg/$lv2

mount_umount $lv1

lvremove $vg/$lv1
lvremove $vg/$lv2

#
# writethrough, unrepairable cachevol damage
#

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 4 -an $vg "$dev3"
lvcreate -n $lv3 -l 4 -an $vg "$dev4"

lvconvert -y --type cache --cachevol $lv2 --cachemode writethrough $vg/$lv1

mkfs_mount_umount $lv1

# damage the current cache so it's not repairable
# lvchange -y -ay $vg/$lv2
# dd if=/dev/zero of="$DM_DEV_DIR/mapper/$vg-$lv2" bs=1M count=1 oflag=direct
dd if=/dev/zero of="$dev3" bs=1M count=1 seek=1 oflag=direct
# verify it's not repairable
lvs -a $vg
lvchange -y -ay $vg/$lv2
lvs -a $vg
ls "$DM_DEV_DIR/mapper/$vg-$lv2"
not cache_check "$DM_DEV_DIR/mapper/$vg-$lv2"
lvchange -an $vg/$lv2

# cache_repair fails
not lvconvert -y --repaircachevol $lv2 $vg/$lv3

# drop the cache
lvconvert --splitcache $vg/$lv1

lvremove $vg/$lv1
lvremove $vg/$lv2
lvremove $vg/$lv3


#
# writeback, unrepairable cachevol damage
#

lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 4 -an $vg "$dev3"
lvcreate -n $lv3 -l 4 -an $vg "$dev4"

lvconvert -y --type cache --cachevol $lv2 --cachemode writeback $vg/$lv1

mkfs_mount_umount $lv1

# damage the current cache so it's not repairable
# lvchange -y -ay $vg/$lv2
# dd if=/dev/zero of="$DM_DEV_DIR/mapper/$vg-$lv2" bs=1M count=16 oflag=direct
dd if=/dev/zero of="$dev3" bs=1M count=1 seek=1 oflag=direct
# verify it's not repairable
lvs -a $vg
lvchange -y -ay $vg/$lv2
lvs -a $vg
ls "$DM_DEV_DIR/mapper/$vg-$lv2"
not cache_check "$DM_DEV_DIR/mapper/$vg-$lv2"
lvchange -an $vg/$lv2

# cache_repair fails
not lvconvert -y --repaircachevol $lv2 $vg/$lv3

# drop the cache
# N.B. this split doesn't seem to sync anything, and doesn't
# seem to see/report any problems.
lvconvert --splitcache $vg/$lv1

lvremove $vg/$lv1
lvremove $vg/$lv2
lvremove $vg/$lv3


#
# TODO: inflict some plausible cachevol damage that can be repaired
# by cache_repair.
#

