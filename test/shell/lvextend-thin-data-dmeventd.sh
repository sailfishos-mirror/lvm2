#!/usr/bin/env bash

# Copyright (C) 2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test autoextension of thin data volume



export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld

# As we check for 'instant' reaction
# retry only few times
test_equal_() {
	for i in $(seq 1 4) ; do
		test "$(get lv_field $vg/pool data_percent)" = "$1" || return
		sleep 1
	done
}

aux have_thin 1 10 0 || skip

# set reserved stack size above dmeventd 300KiB stack
# ATM such value should be simply ignored
aux lvmconf "activation/thin_pool_autoextend_percent = 10" \
	    "activation/thin_pool_autoextend_threshold = 75" \
	    "activation/reserved_stack = 512"

aux prepare_dmeventd

aux prepare_pvs 3 256
get_devs

vgcreate $SHARED -s 256K "$vg" "${DEVICES[@]}"

lvcreate -L1M -c 64k -T $vg/pool
lvcreate -V1M $vg/pool -n $lv1

# Fill exactly 75%
dd if=/dev/zero of="$DM_DEV_DIR/mapper/$vg-$lv1" bs=786432c count=1 conv=fdatasync

# when everything calcs correctly thin-pool should be exactly 75% full now
# and the size should not have changed
pre="75.00"
test_equal_ $pre || die "Data percentage has changed!"


# Now trigger allocation of 1 extra pool chunk
dd if=/dev/zero of="$DM_DEV_DIR/mapper/$vg-$lv1" bs=1c count=1 seek=786433 conv=fdatasync

lvs -a -o+chunksize $vg
dmsetup table
dmsetup status

# If the watermark works well - dmeventd should have already resized data LV
test_equal_ $pre && die "Data percentage has NOT changed!"

vgremove -f $vg
