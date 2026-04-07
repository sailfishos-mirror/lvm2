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

# Test that every bcache I/O backend is selectable and reports device
# read errors with the common "WARNING: bcache <backend>" prefix.
# Short-read handling is covered deterministically by
# test/unit/io_engine_t.c, so it is not asserted here.

. lib/inittest --skip-with-lvmpolld

aux prepare_devs 3

#
# Discover which backends can actually be created.  The creation message is
# written to debug.log, not to pvs stdout/stderr.
#
BACKENDS=(sync)
for backend in threads uring async; do
	rm -f debug.log*
	pvs -vvv --config "global/io_backend=\"$backend\"" "$dev1" >out 2>&1 || true
	grep -q "Created bcache.*$backend" debug.log* 2>/dev/null && BACKENDS+=("$backend")
done

# Legacy use_aio=1 must select the best available async backend, not pin libaio.
AUTO_BACKEND=sync
for backend in uring threads async; do
	if printf '%s\n' "${BACKENDS[@]}" | grep -qx "$backend"; then
		AUTO_BACKEND=$backend
		break
	fi
done
rm -f debug.log*
pvs -vvv --config 'global/io_backend="auto" global/use_aio=1' "$dev1" >out 2>&1 || true
grep "Created bcache.*$AUTO_BACKEND" debug.log*

vgcreate $SHARED -s 4k $vg "$dev1" "$dev2" "$dev3"
lvcreate -aey -n $lv -l 10 $vg

for backend in "${BACKENDS[@]}"; do
	# Test 1: normal operation must not warn.
	vgs --config "global/io_backend=\"$backend\"" $vg |& tee err.log
	not grep "WARNING:" err.log

	#
	# Test 2: device read errors.
	# Error the first sectors so the PV label read fails.  Every backend
	# must report it using the same "WARNING: bcache <backend>" prefix.
	#
	aux error_dev "$dev1" 0:8
	pvs --config "global/io_backend=\"$backend\"" "$dev1" |& tee err.log || true
	aux enable_dev "$dev1"

	grep "WARNING: bcache $backend" err.log
done

vgremove -ff $vg
