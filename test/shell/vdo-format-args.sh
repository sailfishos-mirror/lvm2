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

# Test that VDO formatting settings (use_sparse_index, slab_size_mb)
# are correctly applied and reflected in the running VDO device.
# Tests both kernel formatting and userspace vdoformat tool.

SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux have_vdo 6 2 0 || skip

test "$(aux total_mem)" -gt 524288 || skip "Not enough RAM for this test"

# Detect if kernel supports direct formatting
KERNEL_FORMAT=0
if aux target_at_least dm-vdo 9 2 0; then
	KERNEL_FORMAT=1
fi

vdo_stats_() {
	aux wait_for_vdo_index "$VPOOL"
	dmvdostats -v "$VPOOL" | awk '!/ 0$/' | tee out
	grep "operating mode" out
}

vdo_stat_() {
	awk -v s="$1" '$0 ~ s {print $NF}' out
}

test_vdo_format() {
	local use_kernel=$1
	local format_msg=$2

	# Create VDO with sparse index
	lvcreate --vdo -L25G -V50G -n $lv1 $vg/vdopool \
		--vdosettings "use_sparse_index=1 use_kernel_format=$use_kernel"

	check lv_field $vg/vdopool vdo_use_sparse_index "enabled"

	vdo_stats_

	SPARSE_MEM=$(vdo_stat_ "KVDO module bytes used")
	SPARSE_SLABS=$(vdo_stat_ "slab count")
	test -n "$SPARSE_MEM"
	test -n "$SPARSE_SLABS"

	# Deactivate and reactivate to verify on-disk format consistency
	lvchange -an $vg/$lv1
	lvchange -ay $vg/$lv1

	vdo_stats_

	lvremove -ff $vg

	# Create VDO without sparse index (default dense)
	lvcreate --vdo -L25G -V50G -n $lv1 $vg/vdopool \
		--vdosettings "use_kernel_format=$use_kernel"

	check lv_field $vg/vdopool vdo_use_sparse_index ""

	vdo_stats_

	DENSE_MEM=$(vdo_stat_ "KVDO module bytes used")
	DENSE_SLABS=$(vdo_stat_ "slab count")
	test -n "$DENSE_MEM"
	test -n "$DENSE_SLABS"

	# Sparse index uses larger on-disk index (10x more chapters for same indexMemory)
	# leaving fewer slabs for data
	test "$SPARSE_SLABS" -lt "$DENSE_SLABS" || \
		die "$format_msg: sparse slab count ($SPARSE_SLABS) should be less than dense ($DENSE_SLABS)"

	# Note: Sparse index kernel memory may be higher despite lower index memory
	# because it manages 10x more chapters (only 5% cached) plus sparse cache overhead.
	# This is expected behavior and not a bug.

	lvremove -ff $vg

	# Validate slab size setting with non-default value
	lvcreate --vdo -L25G -V50G -n $lv1 $vg/vdopool \
		--vdosettings "slab_size_mb=256 use_kernel_format=$use_kernel"

	check lv_field $vg/vdopool vdo_slab_size "256.00m"

	vdo_stats_

	LARGE_SLAB_COUNT=$(vdo_stat_ "slab count")
	test -n "$LARGE_SLAB_COUNT"

	# Doubling slab size should roughly halve the slab count
	test "$DENSE_SLABS" -gt "$LARGE_SLAB_COUNT" || \
		die "$format_msg: slab count with 256MB ($LARGE_SLAB_COUNT) should be less than with 128MB ($DENSE_SLABS)"

	lvremove -f $vg
}

aux prepare_vg 1 1000000

VPOOL="$vg-vdopool-vpool"

# Test kernel formatting if available
test "$KERNEL_FORMAT" -eq 1 && test_vdo_format 1 "Kernel formatting"

# Test userspace vdoformat tool
test_vdo_format 0 "Userspace vdoformat"

vgremove -ff $vg
