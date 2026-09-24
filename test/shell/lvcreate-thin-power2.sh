#!/usr/bin/env bash

# Copyright (C) 2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
#
# test support for non-power-of-2 thin chunk size
#



export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld

#
# Main
#
aux have_thin 1 4 0 || skip

aux prepare_pvs 2 64
get_devs

vgcreate $SHARED -s 64K "$vg" "${DEVICES[@]}"

# Decline non-power-of-2 chunk size at the interactive prompt.
echo n | not lvcreate -l10 -c 192 -T $vg/pool0 2>err
grep "not a power of 2" err
grep "Do you really want to" err
grep "Aborted." err
not lvs $vg/pool0

# create non-power-of-2 pool
lvcreate -y -l100 -c 192 -T $vg/pool 2>err
grep "not a power of 2" err

check lv_field $vg/pool discards "passdown"

# check we cannot change discards settings
not lvchange --discard ignore $vg/pool
lvchange --discard nopassdown $vg/pool
check lv_field $vg/pool discards "nopassdown"

# must be multiple of 64KB
not lvcreate -l100 -c 168 -T $vg/pool1

# a non-power-of-2 chunk size from config only warns, it never prompts;
# stdin is closed so any prompt would abort the create
lvcreate --config 'allocation/thin_pool_chunk_size=192' -l100 -T $vg/pool_cfg </dev/null 2>err
grep "not a power of 2" err
check lv_field $vg/pool_cfg chunksize "192.00k"

# Decline non-power-of-2 chunk size when converting to a thin pool.
lvcreate -L10M -n conv1 $vg
lvcreate -L8M -n conv1meta $vg
echo n | not lvconvert -c 192 --thinpool $vg/conv1 --poolmetadata $vg/conv1meta 2>err
grep "not a power of 2" err
grep "Do you really want to" err
grep "Aborted." err
not lvs $vg/conv1_tdata

lvcreate -L10M -n conv2 $vg
lvcreate -L8M -n conv2meta $vg
lvconvert -y -c 192 --thinpool $vg/conv2 --poolmetadata $vg/conv2meta 2>err
grep "not a power of 2" err
check lv_field $vg/conv2 chunksize "192.00k"

vgremove -ff $vg
