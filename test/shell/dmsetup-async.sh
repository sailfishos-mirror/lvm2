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

# Test async ioctl paths in dmsetup:
#   - create --concise (async CREATE/RELOAD/RESUME)
#   - suspend with multiple device names (async SUSPEND)
#   - resume with multiple device names (async RESUME)
#   - wipe_table with multiple device names (async RELOAD+RESUME)
#   - remove with multiple device names (async REMOVE)

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux target_at_least dm-zero 1 0 0 || skip "missing dm-zero target"
# Needed to cleanup any test leftover devices
aux prepare_devs 1

TEST_DEVS=${TEST_DEVS:-2000}
PREFIX_DEV="${PREFIX}async"

# Generate concise spec with LVM- uuids
_gen_spec() {
	awk -v n="$TEST_DEVS" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++) {
			if (i) printf ";"
			printf "%s%04d,LVM-%064d-%s,,rw,0 1 zero", p, i, i, p
		}
	}'
}

# Build argv list for multi-device operations
read -r -a DEVS <<< "$(awk -v n="$TEST_DEVS" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++)
			printf "%s%04d ", p, i
	}')"

# Test 1: async create --concise (via stdin pipe)
echo "Creating $TEST_DEVS zero devices via --concise async path..."
_gen_spec | dmsetup create --concise

# Verify all created and have correct table
test "$(dmsetup ls | grep -c "^${PREFIX_DEV}")" -eq "$TEST_DEVS"
dmsetup table "${DEVS[0]}" | grep -q "zero"
dmsetup info -c --noheadings -o uuid "${DEVS[0]}" | grep -q "^LVM-"

# Test 2: async suspend
echo "Suspending $TEST_DEVS zero devices via async suspend async path..."
dmsetup suspend "${DEVS[@]}"

# Verify suspended
dmsetup info -c --noheadings -o suspended "${DEVS[0]}" | grep -q "Suspended"

# Test 3: async resume
echo "Resuming $TEST_DEVS zero devices via async resume async path..."
dmsetup resume "${DEVS[@]}"

# Verify resumed
dmsetup info -c --noheadings -o suspended "${DEVS[0]}" | grep -q "Active"

# Test 4: async wipe_table
echo "Wiping tables on $TEST_DEVS zero devices via async wipe_table async path..."
dmsetup wipe_table "${DEVS[@]}"

# Verify wiped to error target
dmsetup table "${DEVS[0]}" | grep -q "error"

# Test 5: async remove with multiple device args
echo "Removing $TEST_DEVS zero devices via async remove async path..."
dmsetup remove "${DEVS[@]}"

# Verify all removed
test "$(dmsetup ls 2>/dev/null | grep -c "^${PREFIX_DEV}" || true)" -eq 0
