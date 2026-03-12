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

# Test --noasync option forces synchronous (sequential) behaviour.
# Verify that create --concise, suspend, resume, wipe_table
# and remove all work correctly with --noasync.

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux target_at_least dm-zero 1 0 0 || skip "missing dm-zero target"

NUM_DEVS=20
PREFIX_DEV="${PREFIX}sync"

# Build concise spec
CONCISE=$(awk -v n="$NUM_DEVS" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++) {
			if (i) printf ";"
			printf "%s%04d,,,rw,0 1 zero", p, i
		}
	}')

# Build argv list
read -r -a DEVS <<< "$(awk -v n="$NUM_DEVS" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++)
			printf "%s%04d ", p, i
	}')"

# Test 1: --noasync create --concise
dmsetup create --concise --noasync "$CONCISE"
test "$(dmsetup ls | grep -c "^${PREFIX_DEV}")" -eq "$NUM_DEVS"
dmsetup table "${DEVS[0]}" | grep -q "zero"

# Test 2: --noasync suspend
dmsetup suspend --noasync "${DEVS[@]}"
dmsetup info -c --noheadings -o suspended "${DEVS[0]}" | grep -q "Suspended"

# Test 3: --noasync resume
dmsetup resume --noasync "${DEVS[@]}"
dmsetup info -c --noheadings -o suspended "${DEVS[0]}" | grep -q "Active"

# Test 4: --noasync wipe_table
dmsetup wipe_table --noasync "${DEVS[@]}"
dmsetup table "${DEVS[0]}" | grep -q "error"

# Test 5: --noasync remove
dmsetup remove --noasync "${DEVS[@]}"
test "$(dmsetup ls 2>/dev/null | grep -c "^${PREFIX_DEV}" || true)" -eq 0

# Test 6: timing -- sync path should be slower than async
# Recreate devices for timing comparison
dmsetup create --concise "$CONCISE"

START_ASYNC=$(date +%s%3N)
dmsetup remove "${DEVS[@]}"
END_ASYNC=$(date +%s%3N)
ELAPSED_ASYNC=$((END_ASYNC - START_ASYNC))

# Recreate for sync test
dmsetup create --concise "$CONCISE"

START_SYNC=$(date +%s%3N)
dmsetup remove --noasync "${DEVS[@]}"
END_SYNC=$(date +%s%3N)
ELAPSED_SYNC=$((END_SYNC - START_SYNC))

echo "Async remove: ${ELAPSED_ASYNC}ms, sync remove: ${ELAPSED_SYNC}ms"

# Sync should be at least as slow as async
# (with 20 devices the difference should be visible)
test "$ELAPSED_SYNC" -ge "$ELAPSED_ASYNC" || {
	echo "WARNING: sync not slower than async (${ELAPSED_SYNC}ms vs ${ELAPSED_ASYNC}ms)"
	echo "(may happen on fast systems, not failing)"
}
