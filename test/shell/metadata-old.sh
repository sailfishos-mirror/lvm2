#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest

aux prepare_devs 3
get_devs

#
# Test "old metadata" repair which occurs when the VG is written
# and one of the PVs in the VG does not get written to, and then
# the PV reappears with the old metadata.  This can happen if
# a command is killed or crashes after writing new metadata to
# only some of the PVs in the VG, or if a PV is temporarily
# inaccessible while a VG is written.
#

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

#
# Test that vgck --updatemetadata will update old metadata.
#

lvcreate -n $lv1 -l1 -an $vg "$dev1"
lvcreate -n $lv2 -l1 -an $vg "$dev1"

aux disable_dev "$dev2"

pvs
pvs "$dev1"
not pvs "$dev2"
pvs "$dev3"
lvs $vg/$lv1
lvs $vg/$lv2

lvremove $vg/$lv2

aux enable_dev "$dev2"

pvs 2>&1 | tee out
grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

# fixes the old metadata on dev1
vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

#
# Test that any writing command will also update the
# old metadata.
#

lvcreate -n $lv2 -l1 -an $vg "$dev1"

aux disable_dev "$dev2"

pvs
pvs "$dev1"
not pvs "$dev2"
pvs "$dev3"
lvs $vg/$lv1
lvs $vg/$lv2

lvremove $vg/$lv2

aux enable_dev "$dev2"

pvs 2>&1 | tee out
grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

# fixes the old metadata on dev1
lvcreate -n $lv3 -l1 -an $vg

pvs 2>&1 | tee out
not grep "ignoring metadata seqno" out
pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

lvs $vg/$lv1
not lvs $vg/$lv2

vgremove -ff $vg
