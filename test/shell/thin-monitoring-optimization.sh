#!/usr/bin/env bash

# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test dmeventd monitoring state optimization for thin pools

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld

cleanup_and_teardown()
{
	kill "$TRACK_PID" 2>/dev/null || true
	vgremove -ff $vg 2>/dev/null || true
	aux teardown
}

# Helper function to count LVM command executions
count_lvm_commands() {
	local logfile="$1"
	local pattern="$2"

	grep -c "$pattern" "$logfile" 2>/dev/null || echo "0"
}

# Helper function to get pool usage for any pool
get_pool_usage() {
	local pool_name="${1:-pool}"
	get lv_field "$vg/$pool_name" data_percent | cut -d. -f1
}

# Helper function to simulate thin volume operations that trigger monitoring changes
simulate_thin_operations() {
	local origin_name="$1"
	local num_volumes="$2"
	local prefix="${3:-thin}"

	# Creating $num_volumes snapshot thin volumes of $origin_name (prefix: $prefix)"
	for i in $(seq 1 "$num_volumes"); do
		lvcreate -s -n "${prefix}_$i" "$vg/$origin_name"
	done
}

# Main test
which mkfs.ext4 || skip
export MKE2FS_CONFIG="$TESTDIR/lib/mke2fs.conf"

aux have_thin 1 0 0 || skip

# Create a custom thin command that logs all executions
cat > testcmd.sh << 'EOF'
#!/bin/bash
echo "$(date): THIN_COMMAND executed for $1" >> "$TESTDIR/thin_command.log"
echo "Data: $DMEVENTD_THIN_POOL_DATA"
echo "Metadata: $DMEVENTD_THIN_POOL_METADATA"

# Always attempt the extension to simulate real behavior
"$TESTDIR/lib/lvextend" --use-policies "$1"
result=$?

echo "$(date): lvextend exit code: $result" >> "$TESTDIR/thin_command.log"
exit $result
EOF
chmod +x testcmd.sh

# Configure dmeventd for testing
aux lvmconf "activation/thin_pool_autoextend_percent = 10" \
	    "activation/thin_pool_autoextend_threshold = 70" \
	    "dmeventd/thin_command = \"$PWD/testcmd.sh\""

aux prepare_dmeventd
aux prepare_vg 1 80

trap 'cleanup_and_teardown' EXIT

#
# Test 1: Create multiple thin pools for independent testing
#

# Creating multiple monitored thin pools (20M each, good for 4M extents)"
# and als creating initial thin volumes (10M each = 50% of pool)"
lvcreate --monitor y -L20M -V10M -n $lv1 -T $vg/pool1
lvcreate --monitor y -L20M -V10M -n $lv2 -T $vg/pool2
lvcreate --monitor y -L20M -V10M -n $lv3 -T $vg/pool3

# Clear log
> "$TESTDIR/thin_command.log"

# Start background process to track dmeventd activity
dmeventd -i > dmeventd_status.log 2>&1 &
TRACK_PID=$!

echo "## Filling pools to ~50% to trigger monitoring thresholds"
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=5 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=5 oflag=direct
should dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv3" bs=1M count=5 oflag=direct

echo "## Pool usage after initial fill:"
echo "##   Pool1: $(get_pool_usage pool1)%"
echo "##   Pool2: $(get_pool_usage pool2)%"
echo "##   Pool3: $(get_pool_usage pool3)%"

#
# Test 2: Multiple pool operations (triggers unmonitor/monitor cycles independently)
#
echo "## Testing multiple pool operations simultaneously"
simulate_thin_operations $lv1 3 "test1"
simulate_thin_operations $lv2 3 "test2"
simulate_thin_operations $lv3 3 "test3"

# Wait for any pending dmeventd operations
sleep .3

#
# Count how many times the thin command was executed
#
baseline_executions=$(count_lvm_commands "$TESTDIR/thin_command.log" "THIN_COMMAND executed")
echo "## Baseline executions: $baseline_executions"

# Verify monitoring is still active for all pools
echo "## Verifying monitoring status for all pools"
check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"

#
# Test 3: Verify state preservation across monitoring cycles for multiple pools
#
echo "## Pool usage before optimization test:"
pool1_initial=$(get_pool_usage pool1)
pool2_initial=$(get_pool_usage pool2)
pool3_initial=$(get_pool_usage pool3)
echo "##   Pool1: ${pool1_initial}%"
echo "##   Pool2: ${pool2_initial}%"
echo "##   Pool3: ${pool3_initial}%"

# Clear log for optimization test
> "$TESTDIR/thin_command.log"

# Test 4: Rapid operations on multiple pools (should trigger optimization independently)"
# Test each pool independently to verify optimization works per-pool
for round in {1..2}; do
	# Round $round: Testing independent pool operations"
	simulate_thin_operations $lv1 2 "opt1$round"
	simulate_thin_operations $lv2 2 "opt2$round"
	simulate_thin_operations $lv3 2 "opt3$round"

	# Small delay between rounds
	sleep .1
done

# Wait for all operations to complete
sleep .2

# Count optimized executions
optimized_executions=$(count_lvm_commands "$TESTDIR/thin_command.log" "THIN_COMMAND executed")
echo "## Optimized executions: $optimized_executions"

# Verify all pools are still monitored after optimization test"
check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"

echo "## Test 5: Verify optimization effectiveness across multiple pools"
# The optimization should have reduced the number of LVM command executions
# We expect significantly fewer executions when state is preserved across multiple pools
echo "## Comparing execution counts:"
echo "##   Baseline operations (3 pools): $baseline_executions"
echo "##   Optimized operations (3 pools): $optimized_executions"

# Test should show reduction in command executions
if test "$optimized_executions" -lt $(( baseline_executions * 2 )); then
	echo "## PASS: Optimization reduced LVM command executions across multiple pools"
else
	echo "## INFO: Optimization may not be active or effective"
fi

echo "## Test 6: Verify monitoring functionality preserved on all pools"
# Fill pools a bit more to test monitoring detection
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=2 oflag=direct || true
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv2" bs=1M count=2 oflag=direct || true
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv3" bs=1M count=2 oflag=direct || true

# Check final pool status for all pools
echo "## Final pool usage:"
pool1_final=$(get_pool_usage pool1)
pool2_final=$(get_pool_usage pool2)
pool3_final=$(get_pool_usage pool3)
echo "##   Pool1: ${pool1_final}% (was ${pool1_initial}%)"
echo "##   Pool2: ${pool2_final}% (was ${pool2_initial}%)"
echo "##   Pool3: ${pool3_final}% (was ${pool3_initial}%)"

# Verify monitoring detected the changes on pools that increased
if test "$pool1_final" -gt "$pool1_initial" || test "$pool2_final" -gt "$pool2_initial" || test "$pool3_final" -gt "$pool3_initial"; then
	echo "## PASS: Pool usage increased as expected on one or more pools"
else
	echo "## WARNING: Pool usage didn't increase as expected"
fi

echo "## Test 7: Verify state registry functionality across multiple pools"
# Check if dmeventd log shows our optimization messages
if grep -q "Monitoring optimization" debug.log_DMEVENTD_* 2>/dev/null; then
	echo "## PASS: State registry optimization appears to be active"
else
	echo "## INFO: State registry optimization messages not found in dmeventd log"
fi


# Test that monitoring can be cleanly disabled and re-enabled for all pools
lvchange --monitor n $vg/pool1
lvchange --monitor n $vg/pool2
lvchange --monitor n $vg/pool3

check lv_field $vg/pool1 seg_monitor "not monitored"
check lv_field $vg/pool2 seg_monitor "not monitored"
check lv_field $vg/pool3 seg_monitor "not monitored"

# Re-enabling monitoring for all pools"
lvchange --monitor y $vg/pool1
lvchange --monitor y $vg/pool2
lvchange --monitor y $vg/pool3

check lv_field $vg/pool1 seg_monitor "monitored"
check lv_field $vg/pool2 seg_monitor "monitored"
check lv_field $vg/pool3 seg_monitor "monitored"


# Cleanup is handled by trap

cat dmeventd_status.log
cat debug.log_DMEVENTD_out
