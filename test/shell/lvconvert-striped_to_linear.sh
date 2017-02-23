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

aux prepare_vg 5

# Create 4-way striped LV
lvcreate -aey --ty striped -i4 -L16M -n $lv1 $vg
check lv_field $vg/$lv1 segtype "striped"
check lv_field $vg/$lv1 stripes 4
echo y|mkfs -t ext4 $DM_DEV_DIR/$vg/$lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Upconvert to raid5_n
lvconvert -y --ty raid5 $vg/$lv1
check lv_field $vg/$lv1 segtype "raid5_n"
check lv_field $vg/$lv1 stripes 5
check lv_field $vg/$lv1 stripesize "64.00k"
fsck -fn $DM_DEV_DIR/$vg/$lv1
aux wait_for_sync $vg $lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Grow it *4 to keep the given fs
lvresize -L64M $vg/$lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1
check lv_first_seg_field $vg/$lv1 lv_size "64.00m"
aux wait_for_sync $vg $lv1

# Convert to 1 stripe
lvconvert -y -f --stripes 1 $vg/$lv1
fsck -fn $DM_DEV_DIR/$vg/$lv1
aux wait_for_sync $vg $lv1 1
lvconvert --stripes 1 $vg/$lv1
check lv_first_seg_field $vg/$lv1 stripes 2
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Convert to raid1
lvconvert -y --ty raid1 $vg/$lv1
check lv_first_seg_field $vg/$lv1 segtype "raid1"
check lv_first_seg_field $vg/$lv1 stripes 2
fsck -fn $DM_DEV_DIR/$vg/$lv1

# Convert to linear
lvconvert -y --ty linear $vg/$lv1
# lvconvert -y -m 0 $vg/$lv1
check lv_first_seg_field $vg/$lv1 segtype "linear"
check lv_first_seg_field $vg/$lv1 stripes 1
fsck -fn $DM_DEV_DIR/$vg/$lv1

vgremove -ff $vg
