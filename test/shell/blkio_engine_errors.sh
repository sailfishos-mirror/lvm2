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

# Test that every I/O engine is selectable and reports device
# read errors with the common "WARNING: <engine> io engine" prefix.
# Short-read handling is covered deterministically by
# test/unit/blkio_engine_t.c, so it is not asserted here.

. lib/inittest --skip-with-lvmpolld

aux prepare_devs 3

# Run a probe with the given config and assert the debug log says the
# expected engine was created.
_created_engine() {
	rm -f debug.log*
	pvs -vvv --config "$1" "$dev1" >out 2>&1 || true
	grep "Created.*$2" debug.log*
}

#
# Discover which engines can actually be created.
#
ENGINES=( $(get io_engine "$dev1") )

# Explicit io_engine=auto must select the best available async engine.
AUTO_ENGINE=sync
for engine in iouring threads libaio; do
	if printf '%s\n' "${ENGINES[@]}" | grep -qx "$engine"; then
		AUTO_ENGINE=$engine
		break
	fi
done
_created_engine 'global/io_engine="auto"' "$AUTO_ENGINE"

# Deprecated use_aio=0 must still enforce sync when io_engine is unset.
_created_engine 'global/use_aio=0' sync

# Explicit io_engine=auto must take precedence over deprecated use_aio=0.
if [ "$AUTO_ENGINE" != sync ]; then
	_created_engine 'global/io_engine="auto" global/use_aio=0' "$AUTO_ENGINE"
fi

vgcreate $SHARED -s 4k $vg "$dev1" "$dev2" "$dev3"
lvcreate -aey -n $lv -l 10 $vg

for engine in "${ENGINES[@]}"; do
	# Test 1: normal operation must not warn.
	vgs --config "global/io_engine=\"$engine\"" $vg |& tee err.log
	not grep "WARNING:" err.log

	#
	# Test 2: device read errors.
	# Error the first sectors so the PV label read fails.  Every engine
	# must report it using the same "WARNING: <engine> io engine" prefix.
	#
	aux error_dev "$dev1" 0:8
	pvs --config "global/io_engine=\"$engine\"" "$dev1" |& tee err.log || true
	aux enable_dev "$dev1"

	grep "WARNING: $engine io engine" err.log
done

vgremove -ff $vg
