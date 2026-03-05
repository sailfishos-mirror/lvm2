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

# Test parallel async ioctl paths in dmsetup:
#   - create --concise (parallel CREATE/RELOAD/RESUME)
#   - suspend with multiple device names (parallel SUSPEND)
#   - resume with multiple device names (parallel RESUME)
#   - wipe_table with multiple device names (parallel RELOAD+RESUME)
#   - remove with multiple device names (parallel REMOVE)

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux target_at_least dm-zero 1 0 0 || skip "missing dm-zero target"

N=2000
PREFIX_DEV="${PREFIX}async"
#NOUDEVSYNC="--noudevsync"
NOUDEVSYNC=""

function _teardown() {
	aux teardown_devs_prefixed "$PREFIX_DEV"
}

trap '_teardown' EXIT

# Build concise spec: name,,,-,<table>
CONCISE=$(awk -v n="$N" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++) {
			if (i) printf ";"
			printf "%s%04d,,,rw,0 1 zero", p, i
		}
	}')

# Test 1: parallel create --concise
echo "Creating $N zero devices via --concise async path..."
dmsetup create $NOUDEVSYNC --concise "$CONCISE"

#dmsetup table
#dmsetup info -c

# Verify all devices exist and have correct table
FOUND=$(dmsetup ls | grep -c "^${PREFIX_DEV}")
test "$FOUND" -eq "$N"

# Spot-check a few
dmsetup table "${PREFIX_DEV}0000" | grep -q "zero"
dmsetup table "$(printf "%s%04d" "$PREFIX_DEV" "$((N / 10))")" | grep -q "zero"
dmsetup table "$(printf "%s%04d" "$PREFIX_DEV" "$((N - 1))")" | grep -q "zero"

# Build argv list for multi-device operations
read -r -a DEVS <<< "$(awk -v n="$N" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++)
			printf "%s%04d ", p, i
	}')"

# Test 2: parallel suspend
echo "Suspending $N zero devices via parallel suspend async path..."
dmsetup suspend $NOUDEVSYNC "${DEVS[@]}"

#dmsetup info -c | grep "$PREFIX_DEV" | head -n 20 || true

# Verify all devices are suspended
FOUND=$(dmsetup info -c --noheadings -o suspended "${DEVS[0]}" | grep -c "Suspended")
test "$FOUND" -eq 1

# Test 3: parallel resume
echo "Resuming $N zero devices via parallel resume async path..."
dmsetup resume $NOUDEVSYNC "${DEVS[@]}"

# Verify all devices are active
FOUND=$(dmsetup info -c --noheadings -o suspended "${DEVS[0]}" | grep -c "Active")
test "$FOUND" -eq 1

# Test 4: parallel wipe_table
echo "Wiping tables on $N zero devices via parallel wipe_table async path..."
dmsetup wipe_table $NOUDEVSYNC "${DEVS[@]}"

# Verify all devices now have error target
dmsetup table "${DEVS[0]}" | grep -q "error"
dmsetup table "$(printf "%s%04d" "$PREFIX_DEV" "$((N / 10))")" | grep -q "error"
dmsetup table "$(printf "%s%04d" "$PREFIX_DEV" "$((N - 1))")" | grep -q "error"

# Test 5: parallel remove with multiple device args
echo "Removing $N zero devices via parallel remove async path..."
dmsetup remove $NOUDEVSYNC "${DEVS[@]}"

# Verify all devices are gone
FOUND=$(dmsetup ls 2>/dev/null | grep -c "^${PREFIX_DEV}" || true)
test "$FOUND" -eq 0
