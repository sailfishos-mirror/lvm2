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

# Resize of a cache pool (or its data/metadata sub-LVs) is not supported
# and must be reported as a user error, not fail later inside allocation.


. lib/inittest --skip-with-lvmpolld

aux have_cache 1 3 0 || skip

aux prepare_vg 2 80

lvcreate --yes --type cache-pool -n $lv1 -L 57M $vg "$dev1"
lvs -a $vg -o+devices

not lvextend --yes -L+21M $vg/$lv1 "$dev2" 2>&1 | tee err
grep "Resize of cache pool" err

not lvreduce --yes -L-1M $vg/$lv1 2>&1 | tee err
grep "Resize of cache pool" err

not lvextend --yes -l+1 $vg/${lv1}_cdata "$dev2" 2>&1 | tee err
grep "Resize of cache pool" err

not lvextend --yes -l+1 $vg/${lv1}_cmeta "$dev2" 2>&1 | tee err
grep "Resize of cache pool" err

check lv_field $vg/$lv1 lv_size "57.00m"
check lv_field $vg/${lv1}_cdata lv_size "57.00m"

vgremove -ff $vg
