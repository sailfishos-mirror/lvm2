#!/bin/sh
# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA2110-1301 USA

SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

which mkfs.ext4 || skip
aux have_raid 1 10 2 || skip

aux prepare_vg 4

# Create linear LV
lvcreate -aey -L16M -n$lv1 $vg
check lv_field $vg/$lv1 segtype "linear"
echo y|mkfs -t ext4 $DM_DEV_DIR/$vg/$lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Upconvert it to 2-legged raid1
lvconvert -y -m 1 --ty raid1 --regionsize 512K $vg/$lv1
check lv_field $vg/$lv1 segtype "raid1"
check lv_field $vg/$lv1 stripes 2
check lv_field $vg/$lv1 regionsize "512.00k"
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Convert 2-legged raid1 to raid5_n
lvconvert -y --ty raid5_n $vg/$lv1
check lv_field $vg/$lv1 segtype "raid5_n"
check lv_field $vg/$lv1 stripes 2
check lv_field $vg/$lv1 stripesize "64.00k"
check lv_field $vg/$lv1 regionsize "512.00k"
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Reshape it to to 3 stripes and 256K stripe size
lvconvert -y --stripes 3 --stripesize 256K $vg/$lv1
check lv_first_seg_field $vg/$lv1 stripes 4
check lv_first_seg_field $vg/$lv1 stripesize "256.00k"
fsck -fn $DM_DEV_DIR/$vg/$lv1
aux wait_for_sync $vg $lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Use the additonal space gained by adding stripes
resize2fs $DM_DEV_DIR/$vg/$lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Convert it to striped
# FIXME: _lvconvert fails here?
lvconvert -y --ty striped $vg/$lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1
check lv_first_seg_field $vg/$lv1 segtype "striped"
check lv_first_seg_field $vg/$lv1 stripes 3
check lv_first_seg_field $vg/$lv1 stripesize "256.00k"

vgremove -ff $vg
