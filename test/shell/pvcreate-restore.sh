#!/usr/bin/env bash

# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA


. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux prepare_vg 4

lvcreate --type snapshot -s -L10 -n $lv1 $vg --virtualsize 2T
lvcreate --type snapshot -s -L10 -n $lv2 $vg --virtualsize 4T
lvcreate --type snapshot -s -L10 -n $lv3 $vg --virtualsize 4194300M

aux extend_filter_LVMTEST
aux lvmconf "devices/scan_lvs = 1"
aux extend_devices "$DM_DEV_DIR/$vg/$lv1"
aux extend_devices "$DM_DEV_DIR/$vg/$lv2"
aux extend_devices "$DM_DEV_DIR/$vg/$lv3"

vgcreate $vg1 "$DM_DEV_DIR/$vg/$lv2"

vgcfgbackup -f vgback $vg1

UUID=$(get pv_field "$DM_DEV_DIR/$vg/$lv2" uuid)
pvremove -ff -y "$DM_DEV_DIR/$vg/$lv2"

# too small to fit
fail pvcreate --restorefile vgback --uuid $UUID "$DM_DEV_DIR/$vg/$lv1"

# still does not fit
fail pvcreate --restorefile vgback --uuid $UUID "$DM_DEV_DIR/$vg/$lv3"

pvcreate --restorefile vgback --uuid $UUID "$DM_DEV_DIR/$vg/$lv2"

vgremove -ff $vg
