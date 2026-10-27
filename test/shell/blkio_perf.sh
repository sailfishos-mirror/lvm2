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

# Test I/O engine performance - showing parallel I/O benefits

# Benchmark: runs only with extended tests (make check_local_extended).
. lib/inittest --skip-with-lvmpolld --with-extended

# Test 2 uses dm-delay to expose the parallel-I/O benefit.
aux target_at_least dm-delay 1 1 0 || skip "missing dm-delay target"

#
# I/O Engine Performance Test
#

TEST_CNT=${TEST_CNT-10}
TEST_DELAY=${TEST_DELAY-5}

# time output format
TIMEFORMAT='%R'

aux prepare_pvs "$TEST_CNT"

run_pvs() {
	pvs --config "global/io_engine=\"${ENGINES[$idx]}\""
}

run_cnt_pvs() {
	for i in $(seq 1 "$TEST_CNT"); do run_pvs; done
}

# Detect available engines (debug output needed for "Created ... io engine" messages)
ENGINES=( $(get io_engine "$dev1") )

#
# Sanity: every engine must return the same PVs before we time it.
#
for idx in $(seq 0 $((${#ENGINES[@]} - 1))); do
	run_pvs > out ||
		die "pvs failed with engine ${ENGINES[$idx]}"
	grep -qF "$dev1" out ||
		die "pvs did not report $dev1 with engine ${ENGINES[$idx]}"
done

#
# Test 1: Warm cache baseline
#
for idx in $(seq 0 $((${#ENGINES[@]} - 1))); do
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

for idx in $(seq 0 $((${#ENGINES[@]} - 1))); do
	sync

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

for idx in $(seq 1 $((${#ENGINES[@]} - 1))); do
	name="${ENGINES[$idx]}"
	elapsed="${warm[$idx]}"

	if [ "$name" = "libaio" ]; then
		# libaio may have overhead from io_destroy
		overhead=$(awk "BEGIN {printf \"%.0f\", ($elapsed - ${warm[0]}) * 1000}")
		if [ "$overhead" -gt 5 ]; then
			msg=" (+$overhead ms overhead)"
		else
			msg=" (same)"
		fi
	else
		# threads/iouring should be faster or same
		if awk "BEGIN {exit !(${warm[0]} > $elapsed + 0.01)}"; then
			speedup=$(awk "BEGIN {printf \"%.1f\", ${warm[0]} / $elapsed}")
			msg=" (${speedup}x faster)"
		else
			msg=" (same)"
		fi
	fi

	printf "  %-8s: %5s s%s\n" "$name" "$elapsed" "$msg"
done

#
# Test 2
#
printf "\nCold Cache + ${TEST_DELAY}ms Read Latency analysis:\n"
printf "  sync:    %5s s (baseline)\n" "${cold[0]}"

for idx in $(seq 1 $((${#ENGINES[@]} - 1))); do
	name="${ENGINES[$idx]}"
	elapsed="${cold[$idx]}"
	speedup=$(awk "BEGIN {printf \"%.1f\", ${cold[0]} / $elapsed}")
	printf "  %-8s %5s s (%.1fx faster)\n" "$name" "$elapsed" "$speedup"
done
) > out

cat out
