#!/usr/bin/env bash

# Copyright (C) 2023 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# testing conversion of volumes to thin-pool with VDO data LV


export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

prepare_lvs() {
	lvremove -f $vg
	lvcreate -L10M -n $lv1 $vg
	lvcreate -L8M -n $lv2 $vg
}

#
# Main
#
aux have_thin 1 0 0 || skip
aux have_vdo 6 2 0 || skip
which mkfs.ext4 || skip

aux prepare_vg 4 6400

# Convert to thin-pool with VDO backend from existing VG/LV
lvcreate -L5G --name $lv1 $vg

# Keep volume in use for at least 6 seconds
# - lvm retries for ~5sec to deactivate (and longer with valgrind)
sleep 9 < "$DM_DEV_DIR/$vg/$lv1" &
SLEEP_PID=$!

# Volume in use cannot be converted
fail lvconvert -y --type thin-pool $vg/$lv1 --pooldatavdo y

kill "$SLEEP_PID" || true
# Wait for sleep to not use LV anymore
wait

# No extra volume should appear in VG after failure
test "$(get vg_field $vg lv_count)" -eq "1"

mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
# Conversion caught present filesystem and should fail
fail lvconvert -Wy --type thin-pool -c 256K --deduplication n --pooldatavdo y $vg/$lv1

# No extra volume should appear in VG after failure
test "$(get vg_field $vg lv_count)" -eq "1"

# With --yes it should work over prompt
lvconvert --yes -Wy --type thin-pool -c 256K --deduplication n --pooldatavdo y $vg/$lv1

check lv_field $vg/$lv1 segtype thin-pool
check lv_field $vg/${lv1}_tdata segtype vdo -a
check lv_field $vg/${lv1}_tdata vdo_deduplication "" -a  # deduplication should be disabled

lvremove -f $vg


# convert to thin-pool with VDO backend from existing VDO VG/LV
lvcreate -L5G --vdo --compression y --name $lv1 $vg
lvconvert --yes -Wy --type thin-pool --pooldatavdo y --compression n --vdosettings 'ack_threads=4' $vg/$lv1

check lv_field $vg/$lv1 segtype thin-pool
check lv_field $vg/${lv1}_tdata segtype vdo -a
check lv_field $vg/${lv1}_tdata vdo_compression "" -a  # compression should be disabled


lvremove -f $vg

vgremove -ff $vg
