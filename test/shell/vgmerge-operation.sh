#!/usr/bin/env bash

# Copyright (C) 2007-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Test vgmerge operation'

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux prepare_pvs 4 64

# 'vgmerge succeeds with single linear LV in source VG'
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -l 4 -n $lv1 $vg1 "$dev1"
vgchange -an $vg1
check pvlv_counts $vg1 2 1 0
check pvlv_counts $vg2 2 0 0
vgmerge $vg2 $vg1
check pvlv_counts $vg2 4 1 0
vgremove -f $vg2

# 'vgmerge succeeds with single linear LV in source and destination VG'
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -l 4 -n $lv1 $vg1
lvcreate -l 4 -n $lv2 $vg2
vgchange -an $vg1
vgchange -an $vg2
check pvlv_counts $vg1 2 1 0
check pvlv_counts $vg2 2 1 0
vgmerge $vg2 $vg1
check pvlv_counts $vg2 4 2 0
vgremove -f $vg2

# 'vgmerge succeeds with linear LV + snapshots in source VG'
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -aey -l 16 -n $lv1 $vg1
lvcreate -l 4 -s -n $lv2 $vg1/$lv1
vgchange -an $vg1
check pvlv_counts $vg1 2 2 1
check pvlv_counts $vg2 2 0 0
vgmerge $vg2 $vg1
check pvlv_counts $vg2 4 2 1
lvremove -f $vg2/$lv2
vgremove -f $vg2

# 'vgmerge succeeds with mirrored LV in source VG'
vgcreate $vg1 "$dev1" "$dev2" "$dev3"
vgcreate $vg2 "$dev4"
lvcreate -aey -l 4 -n $lv1 --type mirror -m1 $vg1
vgchange -an $vg1
check pvlv_counts $vg1 3 1 0
check pvlv_counts $vg2 1 0 0
vgmerge $vg2 $vg1
check pvlv_counts $vg2 4 1 0
lvremove -f $vg2/$lv1
vgremove -f $vg2

# 'vgmerge rejects LV name collision'
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -l 4 -n $lv1 $vg1
lvcreate -l 4 -n $lv1 $vg2
vgchange -an $vg1
check pvlv_counts $vg1 2 1 0
check pvlv_counts $vg2 2 1 0
not vgmerge $vg2 $vg1 2>err
grep "Duplicate logical volume name \"$lv1\" in \"$vg2\" and \"$vg1" err
check pvlv_counts $vg1 2 1 0
check pvlv_counts $vg2 2 1 0
vgremove -f $vg1 $vg2


# 'vgmerge' handle pmspare for merged VG
if aux have_thin 1 5 0; then

# With disabled pmspare nothing is created
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -T -L8M $vg1/pool1 --poolmetadatasize 8M --poolmetadataspare n
lvcreate -T -L8M $vg2/pool2 --poolmetadatasize 4M --poolmetadataspare n
vgchange -an $vg1 $vg2

vgmerge --poolmetadataspare n $vg1 $vg2
check lv_not_exists $vg/lvol0_pmspare
vgremove -ff $vg1


# With pmspare handling there are one created
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -T -L8M $vg1/pool1 --poolmetadatasize 8M --poolmetadataspare n
lvcreate -T -L8M $vg2/pool2 --poolmetadatasize 4M --poolmetadataspare n
vgchange -an $vg1 $vg2

vgmerge $vg1 $vg2
check lv_field $vg1/lvol0_pmspare size "8.00m"
vgremove -ff $vg1


# When merged, bigger pmspare is preserved
vgcreate $vg1 "$dev1" "$dev2"
vgcreate $vg2 "$dev3" "$dev4"
lvcreate -T -L8M $vg1/pool1 --poolmetadatasize 8M
lvcreate -T -L8M $vg2/pool2 --poolmetadatasize 4M
vgchange -an $vg1 $vg2

vgmerge $vg1 $vg2

check lv_field $vg1/lvol0_pmspare size "8.00m"
vgremove -ff $vg1

fi
