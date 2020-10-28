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

SKIP_WITH_LVMPOLLD=1

. lib/inittest

mkfs_mount_unmount()
{
        lvt=$1

        mkfs.xfs -f "$DM_DEV_DIR/$vg/$lvt"
        mount "$DM_DEV_DIR/$vg/$lvt" "$mount_dir"
        cp pattern1 "$mount_dir/pattern1"
        umount "$mount_dir"
}

setup_thin_lvs()
{
	pool=$1

	for i in $(seq 1 4); do
		lvcreate --type thin -V1G -n th$i --thinpool $pool $vg
		mkfs_mount_unmount th${i}
		lvchange -an $vg/th${i}
	done
}

diff_thin_lvs()
{
	for i in $(seq 1 4); do
        	diff pattern1 "${mount_dir}_${i}/pattern1"
        	diff pattern2 "${mount_dir}_${i}/pattern2"
	done
}

mount_thin_lvs()
{
	for i in $(seq 1 4); do
		lvchange -ay $vg/th$i
		mount "$DM_DEV_DIR/$vg/th$i" "${mount_dir}_${i}"
	done
}

unmount_thin_lvs()
{
	for i in $(seq 1 4); do
		umount "${mount_dir}_${i}"
		lvchange -an $vg/th${i}
	done
}

write_thin_lvs()
{
	for i in $(seq 1 4); do
        	cp pattern2 "${mount_dir}_${i}/pattern2"
	done
}

aux have_writecache 1 0 0 || skip
which mkfs.xfs || skip

mount_dir="mnt"
mount_dir_1="mnt1"
mount_dir_2="mnt2"
mount_dir_3="mnt3"
mount_dir_4="mnt4"
mkdir -p "$mount_dir"
for i in $(seq 1 4); do
	mkdir -p "${mount_dir}_${i}"
done

# generate random data
dd if=/dev/urandom of=pattern1 bs=512K count=1
dd if=/dev/urandom of=pattern2 bs=512 count=15

aux prepare_devs 6 40

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6"

#
# writecache as thin pool data
# splitcache while inactive
#

# lv1 holds thin pool data and uses writecache
# lv2 holds cachevol for writecache
# lv3 holds thin pool metadata
lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 2 -an $vg "$dev3"
lvcreate -n $lv3 -l 2 -an $vg "$dev4"
lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1
lvconvert -y --type thin-pool --poolmetadata $lv3 --poolmetadataspare n $vg/$lv1

setup_thin_lvs $lv1
mount_thin_lvs
write_thin_lvs
unmount_thin_lvs

lvchange -an $vg/$lv1
lvconvert --splitcache --cachesettings cleaner=0 $vg/${lv1}_tdata
lvs -o segtype $vg/$lv2 | grep linear

mount_thin_lvs
diff_thin_lvs
unmount_thin_lvs

lvremove -y $vg/$lv1
lvremove -y $vg/$lv2

#
# writecache as thin pool data
# splitcache while active
#

# lv1 holds thin pool data and uses writecache
# lv2 holds cachevol for writecache
# lv3 holds thin pool metadata
lvcreate -n $lv1 -l 16 -an $vg "$dev1" "$dev2"
lvcreate -n $lv2 -l 2 -an $vg "$dev3"
lvcreate -n $lv3 -l 2 -an $vg "$dev4"
lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1
lvconvert -y --type thin-pool --poolmetadata $lv3 --poolmetadataspare n $vg/$lv1

setup_thin_lvs $lv1
mount_thin_lvs
write_thin_lvs

# FIXME: splitcache setting cleaner on tdata writecache doesn't work,
# bypassing that with cleaner=0 here.

lvconvert --splitcache --cachesettings cleaner=0 $vg/${lv1}_tdata
lvs -o segtype $vg/$lv2 | grep linear

diff_thin_lvs
unmount_thin_lvs

mount_thin_lvs
diff_thin_lvs
unmount_thin_lvs

lvremove -y $vg/$lv1
lvremove -y $vg/$lv2


#
# add writecache to raid, then use writecache for thin pool data
#

# lv1 holds thin pool data and uses writecache
# lv2 holds cachevol for writecache
# lv3 holds thin pool metadata
lvcreate --type raid1 -m1 -n $lv1 -l 16 -an $vg "$dev1" "$dev2" "$dev5" "$dev6"
lvcreate -n $lv2 -l 2 -an $vg "$dev3"
lvcreate -n $lv3 -l 2 -an $vg "$dev4"
lvconvert -y --type writecache --cachevol $lv2 $vg/$lv1
lvconvert -y --type thin-pool --poolmetadata $lv3 --poolmetadataspare n $vg/$lv1

setup_thin_lvs $lv1
mount_thin_lvs
write_thin_lvs

lvconvert --splitcache --cachesettings cleaner=0 $vg/${lv1}_tdata
lvs -o segtype $vg/$lv2 | grep linear

diff_thin_lvs
unmount_thin_lvs

mount_thin_lvs
diff_thin_lvs
unmount_thin_lvs

lvremove -y $vg/$lv1
lvremove -y $vg/$lv2


#
# remove writecache from thin pool data when cachevol is missing
#

#
# FIXME: add writecache to existing thin pool data
#

#
# FIXME: thin pool data cannot be extended when it uses writecache
#

vgremove -ff $vg
