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

# Test bcache I/O backend error handling and short read reporting
# Verify that all backends properly report I/O errors and short reads with consistent messages

. lib/inittest --skip-with-lvmpolld

aux prepare_devs 3

test_backend() {
	local backend=$1

	# Test 1: Normal operation (baseline)
	vgs --config "global/io_backend=\"$backend\"" $vg |& tee err.log
	if grep -q "WARNING:" err.log ; then
		die "UNEXPECTED: Warnings in normal operation"
	fi

	# Test 2: Short read reporting with -vvvv

	# Create LV and snapshot - snapshot COW device initialization triggers short reads
	# bcache uses 128KB blocks, but small snapshot metadata is only 4KB, causing short reads
	lvcreate -s -vvvv -l 3 -n snap $vg/$lv --config "global/io_backend=\"$backend\"" >short.log 2>&1

	lvremove -ff $vg/snap 2>/dev/null || true

	# Check if backend is not available (fell back to another backend)
	if grep -q "bcache.*$backend.*not available" short.log; then
		echo "Skipping $backend (not available)"
		return 0
	fi

	# Check for short read debug messages
	# Format: "bcache BACKEND read/write offset %llu requested %llu got %llu."
	if ! grep "bcache $backend.*offset.*requested.*got" short.log; then
		cat short.log
		die "Expected 'bcache $backend offset ... requested ... got' message not found"
	fi

	# Test 3: Device with I/O errors

	# Inject errors on dev1
	aux error_dev "$dev1" 128:16

	# This should trigger I/O errors
	vgs --config "global/io_backend=\"$backend\"" $vg |& tee err.log

	# Restore dev1
	aux enable_dev "$dev1"

	# Check that we got appropriate warning messages
	# All backends use consistent format: "WARNING: bcache BACKEND"
	if ! grep "WARNING: bcache $backend" err.log; then
		die "Expected 'WARNING: bcache $backend' message"
	fi
}

# Create a VG to test with
vgcreate $SHARED -s 4k $vg "$dev1" "$dev2" "$dev3"
lvcreate -aey -n $lv -l 10 $vg

# Test each backend
for backend in sync threads uring async; do
	test_backend "$backend"
done

vgremove -ff $vg
