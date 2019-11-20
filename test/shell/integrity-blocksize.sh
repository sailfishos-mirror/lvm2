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

aux have_integrity 1 5 0 || skip

losetup -h | grep sector-size || skip

dd if=/dev/zero of=loopa bs=$((1024*1024)) count=64 2> /dev/null
dd if=/dev/zero of=loopb bs=$((1024*1024)) count=64 2> /dev/null
dd if=/dev/zero of=loopc bs=$((1024*1024)) count=64 2> /dev/null
dd if=/dev/zero of=loopd bs=$((1024*1024)) count=64 2> /dev/null
LOOP1=$(losetup -f loopa --show)
LOOP2=$(losetup -f loopb --show)
LOOP3=$(losetup -f loopc --sector-size 4096 --show)
LOOP4=$(losetup -f loopd --sector-size 4096 --show)

echo $LOOP1
echo $LOOP2
echo $LOOP3
echo $LOOP4

aux extend_filter "a|$LOOP1|"
aux extend_filter "a|$LOOP2|"
aux extend_filter "a|$LOOP3|"
aux extend_filter "a|$LOOP4|"

aux lvmconf 'devices/scan = "/dev"'

vgcreate $vg1 $LOOP1 $LOOP2
vgcreate $vg2 $LOOP3 $LOOP4

# lvcreate on dev512, result 512
lvcreate --type raid1 -m1 --raidintegrity y -l 8 -n $lv1 $vg1
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# lvcreate on dev4k, result 4k
lvcreate --type raid1 -m1 --raidintegrity y -l 8 -n $lv1 $vg2
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvremove -y $vg2/$lv1

# lvcreate --bs 512 on dev4k, result fail
not lvcreate --type raid1 -m1 --raidintegrity y --raidintegrityblocksize 512 -l 8 -n $lv1 $vg2

# lvcreate --bs 4096 on dev512, result 4k
lvcreate --type raid1 -m1 --raidintegrity y --raidintegrityblocksize 4096 -l 8 -n $lv1 $vg1
pvck --dump metadata $LOOP1 | grep 'block_size = 4096'
lvremove -y $vg1/$lv1

# Test an unknown fs block size by simply not creating a fs on the lv.

# lvconvert on dev512, fsunknown, result 512
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg1
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg1/$lv1
lvconvert --raidintegrity y $vg1/$lv1
pvck --dump metadata $LOOP1 | grep 'block_size = 512'
lvremove -y $vg1/$lv1

# lvconvert on dev4k, fsunknown, fail
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg2/$lv1
not lvconvert --raidintegrity y $vg2/$lv1
lvremove -y $vg2/$lv1

# lvconvert --bs 4k on dev4k, fsunknown, result 4k
lvcreate --type raid1 -m1 -l 8 -n $lv1 $vg2
# clear any residual fs so that libblkid cannot find an fs block size
aux wipefs_a /dev/$vg2/$lv1
lvconvert --raidintegrity y --raidintegrityblocksize 4096 $vg2/$lv1
pvck --dump metadata $LOOP3 | grep 'block_size = 4096'
lvremove -y $vg2/$lv1

# TODO: testing with fs block sizes requires a libblkid version that returns BLOCK_SIZE

vgremove -ff $vg1
vgremove -ff $vg2

losetup -d $LOOP1
losetup -d $LOOP2
losetup -d $LOOP3
losetup -d $LOOP4
rm loopa
rm loopb
rm loopc
rm loopd

