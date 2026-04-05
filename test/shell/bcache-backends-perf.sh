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

# Test bcache I/O backend performance - showing parallel I/O benefits

. lib/inittest --skip-with-lvmpolld

#
# Bcache I/O Backend Performance Test
#

TEST_CNT=${TEST_CNT-10}
TEST_DELAY=${TEST_DELAY-5}

# Supported beckends - auto-discovers others
BACKENDS=( sync )

# time output format
TIMEFORMAT='%R'

aux prepare_pvs "$TEST_CNT"

run_pvs() {
	pvs --config "global/io_backend=\"${BACKENDS[$idx]}\""
}

run_cnt_pvs() {
	for i in $(seq 1 "$TEST_CNT"); do run_pvs; done
}

# Detect available backends (debug output needed for "Created bcache" messages)
for backend in async threads uring; do
	pvs -vvv --config "global/io_backend=\"$backend\"" "$dev1" >log 2>&1
	grep -q "Created bcache.*$backend" log && BACKENDS+=("$backend")
done

#
# Test 1: Warm cache baseline
#
for idx in $(seq 0 $((${#BACKENDS[@]} - 1))); do
	warm[$idx]=$( (time run_cnt_pvs) |& tail -n1 )
	echo "  ${warm[$idx]} s"
done

#
# Test 2: With read latency
#
# Add delay to all devices
for i in $(seq 1 "$TEST_CNT"); do
	eval "aux delay_dev \"\$dev$i\" $TEST_DELAY 0"
done

for idx in $(seq 0 $((${#BACKENDS[@]} - 1))); do
	sync
	echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
	sleep 0.1

	cold[$idx]=$( (time run_pvs) |& tail -n1 )
	echo "  ${cold[$idx]} s"
done

#
# Use subshell for nice table on exit
# (using $warm[] and $cold[])
#

(
#
# Statistics Summary
#
# Test 1
#
printf "\nWarm Cache ($TEST_CNT iterations) analysis:\n"
printf "  sync:    %5s s (baseline)\n" "${warm[0]}"

for idx in $(seq 1 $((${#BACKENDS[@]} - 1))); do
	name="${BACKENDS[$idx]}"
	time="${warm[$idx]}"

	if [ "$name" = "async" ]; then
		# async may have overhead from io_destroy
		overhead=$(awk "BEGIN {printf \"%.0f\", ($time - ${warm[0]}) * 1000}")
		if [ "$overhead" -gt 5 ]; then
			msg=" (+$overhead ms overhead)"
		else
			msg=" (same)"
		fi
	else
		# threads/uring should be faster or same
		if awk "BEGIN {exit !(${warm[0]} > $time + 0.01)}"; then
			speedup=$(awk "BEGIN {printf \"%.1f\", ${warm[0]} / $time}")
			msg=" (${speedup}x faster)"
		else
			msg=" (same)"
		fi
	fi

	printf "  %-8s: %5s s%s\n" "$name" "$time" "$msg"
done

#
# Test 2
#
printf "\nCold Cache + ${TEST_DELAY}ms Read Latency analysis:\n"
printf "  sync:    %5s s (baseline)\n" "${cold[0]}"

for idx in $(seq 1 $((${#BACKENDS[@]} - 1))); do
	name="${BACKENDS[$idx]}:"
	time="${cold[$idx]}"
	speedup=$(awk "BEGIN {printf \"%.1f\", ${cold[0]} / $time}")
	printf "  %-8s %5s s (%.1fx faster)\n" "$name" "$time" "$speedup"
done
) > out

cat out
