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

# Test dmsetup remove --retry with busy devices held open by background
# processes. The retry logic should wait for the holders to exit and
# then successfully remove all devices.

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux target_at_least dm-zero 1 0 0 || skip "missing dm-zero target"
# Needed to cleanup any test leftover devices
aux prepare_devs 1

TEST_DEVS=${TEST_DEVS:-6}
# Hold time must exceed retry delay to trigger retries,
# but be shorter than max retry window (25 * 0.2s = 5s)
HOLD_TIME=${HOLD_TIME:-2}
PREFIX_DEV="${PREFIX}retry"

# Wait for all test devices to disappear.
# Keeps waiting as long as device count is decreasing (progress).
# Fails if no progress for 5 seconds.
_wait_removed() {
	local cnt last_cnt=999999 stall=0

	while true; do
		cnt=$(dmsetup ls 2>/dev/null | grep -c "^${PREFIX_DEV}" || true)
		test "$cnt" -eq 0 && return 0

		if test "$cnt" -lt "$last_cnt"; then
			# making progress -- reset stall counter
			stall=0
		else
			stall=$((stall + 1))
		fi
		last_cnt=$cnt

		# 10 polls * 0.5s = 5s without progress
		if test "$stall" -ge 10; then
			echo "No progress: $cnt devices remaining"
			dmsetup info -c || true
			dmsetup ls || true
			return 1
		fi
		sleep .5
	done
}

# Generate concise spec for creation
_gen_spec() {
	awk -v n="$TEST_DEVS" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++) {
			if (i) printf ";"
			printf "%s%04d,LVM-%064d-%s,,rw,0 1 zero", p, i, i, p
		}
	}'
}

# Build device name array
read -r -a DEVS <<< "$(awk -v n="$TEST_DEVS" -v p="$PREFIX_DEV" '
	BEGIN {
		for (i = 0; i < n; i++)
			printf "%s%04d ", p, i
	}')"

# Test 1: async --retry remove with all devices held open briefly
# Create devices
_gen_spec | dmsetup create --concise
test "$(dmsetup ls | grep -c "^${PREFIX_DEV}")" -eq "$TEST_DEVS"

# Hold every device open with a background sleep
HOLDER_PIDS=()
for d in "${DEVS[@]}"; do
	sleep "$HOLD_TIME" < "$DM_DEV_DIR/mapper/$d" &
	HOLDER_PIDS+=( $! )
done

# Confirm devices are open (open count > 0)
test "$(dmsetup info -c --noheadings -o open "${DEVS[0]}")" -gt 0

# Remove with --retry: should block briefly then succeed once sleeps exit
dmsetup remove --retry "${DEVS[@]}"

# Verify all removed
_wait_removed

# Clean up any lingering holders (should already be done)
for pid in "${HOLDER_PIDS[@]}"; do
	kill "$pid" 2>/dev/null || true
done
wait

# Test 2: async --force --deferred remove with mixed busy/free devices
# Even-numbered devices are held open, odd-numbered are free.
# --force wipes table to error first (RELOAD sync + RESUME async),
# drains and waits for udev, then issues REMOVE --deferred without cookie.
# Free (odd) devices are removed immediately, busy (even) devices are
# scheduled for kernel deferred removal -- no uevent is generated.
_gen_spec | dmsetup create --concise
test "$(dmsetup ls | grep -c "^${PREFIX_DEV}")" -eq "$TEST_DEVS"

HOLDER_PIDS=()
for (( i = 0; i < TEST_DEVS; i += 2 )); do
	sleep "$HOLD_TIME" < "$DM_DEV_DIR/mapper/${DEVS[$i]}" &
	HOLDER_PIDS+=( $! )
done

test "$(dmsetup info -c --noheadings -o open "${DEVS[0]}")" -gt 0
test "$(dmsetup info -c --noheadings -o open "${DEVS[1]}")" -eq 0

# --force wipes to error (holders get EIO), then deferred removes
dmsetup -vvvv remove --force --deferred "${DEVS[@]}"

# Wait for holders to exit so deferred remove completes
#for pid in "${HOLDER_PIDS[@]}"; do
#	kill "$pid" 2>/dev/null || true
#done
kill "${HOLDER_PIDS[@]}" 2>/dev/null || true
wait

# All devices should be gone after holders released
_wait_removed

# Test 3: --force --retry with half devices held open briefly
# Even-numbered devices are held open for 1s -- these get EBUSY on
# first remove attempt and need retry.  Odd-numbered devices are free
# and removed immediately after table wipe.
_gen_spec | dmsetup create --concise
test "$(dmsetup ls | grep -c "^${PREFIX_DEV}")" -eq "$TEST_DEVS"

HOLDER_PIDS=()
for (( i = 0; i < TEST_DEVS; i += 2 )); do
	sleep "$HOLD_TIME" < "$DM_DEV_DIR/mapper/${DEVS[$i]}" &
	HOLDER_PIDS+=( $! )
done

test "$(dmsetup info -c --noheadings -o open "${DEVS[0]}")" -gt 0
test "$(dmsetup info -c --noheadings -o open "${DEVS[1]}")" -eq 0

dmsetup -vvvv remove --force --retry "${DEVS[@]}"

# Holders should have exited during retry window
kill "${HOLDER_PIDS[@]}" 2>/dev/null || true
wait

_wait_removed
