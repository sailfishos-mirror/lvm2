#!/usr/bin/env bash

# Copyright (C) 2026 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Background atomic pvmove on RAID1, then lvremove -f on the RAID LV and
# vgremove. Orphaned pvmove metadata must be dropped so vgremove succeeds.

test_description='pvmove RAID: vgremove after lvremove during background pvmove'

. lib/inittest --skip-with-lvmlockd

aux have_raid 1 3 5 || skip

# Six PVs: dev1-dev3 for RAID legs, dev4 spare, dev5-dev6 for pvmove target
aux prepare_vg 6 32

lvcreate --yes --type raid1 -m 2 -L 18M -n $lv1 $vg "$dev1" "$dev2" "$dev3"

LVM_TEST_TAG="kill_me_$PREFIX" pvmove --atomic -i +2 -b "$dev1" "$dev6"

get lv_field $vg name -a >out
grep "\[pvmove" out

lvremove -f $vg/$lv1

get lv_field $vg name -a >out
not grep "\[pvmove" out

vgremove -f $vg

not vgs $vg 2>/dev/null

aux kill_tagged_processes
