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

# Measure parallel deactivation performance for linear LVs.
#
# Usage:
#   TEST_DEVS=50 test/shell/deactivate-parallel.sh
#
# Produces timing output showing:
#   - wall-clock time for activation and deactivation
#   - ioctl counts from debug logs
#   - deferred/drain events

. lib/inittest --skip-with-lvmpolld

# Configurable parameters
TEST_DEVS=${TEST_DEVS:-500}

# On low-memory boxes reduce
test "$(aux total_mem)" -gt 524288 || TEST_DEVS=200

aux prepare_pvs 1 $((TEST_DEVS + 10))
get_devs

vgcreate $SHARED -s 128K "$vg" "${DEVICES[@]}"

vgcfgbackup -f data "$vg"

# Generate TEST_DEVS linear LVs (1 extent each) via vgcfgrestore
awk -v TEST_DEVS="$TEST_DEVS" '/^\t\}/ {
    printf("\t}\n\tlogical_volumes {\n");
    cnt=0;
    for (i = 0; i < TEST_DEVS; i++) {
	printf("\t\tlv%04d  {\n", i);
	printf("\t\t\tid = \"%06d-1111-2222-3333-2222-1111-%06d\"\n", i, i);
	print "\t\t\tstatus = [\"READ\", \"WRITE\", \"VISIBLE\"]";
	print "\t\t\tsegment_count = 1";
	print "\t\t\tsegment1 {";
	print "\t\t\t\tstart_extent = 0";
	print "\t\t\t\textent_count = 1";
	print "\t\t\t\ttype = \"striped\"";
	print "\t\t\t\tstripe_count = 1";
	print "\t\t\t\tstripes = [";
	print "\t\t\t\t\t\"pv0\", " cnt++;
	printf("\t\t\t\t]\n\t\t\t}\n\t\t}\n");
      }
  }
  {print}
' data >data_new

vgcfgrestore -f data_new "$vg"

# Timestamp helper (milliseconds since epoch)
timestamp_ms() {
	echo $(( $(date +%s%N) / 1000000 ))
}

# Analyze a debug log for DM ioctl patterns
analyze_log() {
	local logfile="$1"
	local label="$2"

	if [ ! -s "$logfile" ]; then
		echo "  (no log for $label)"
		return
	fi

	echo "=== $label ==="

	# Ioctl counts
	echo "  dm remove:  $(grep -c 'dm remove' "$logfile")"
	echo "  dm info:    $(grep -c 'dm info' "$logfile")"

	# Removing (verbose log from _prepare_deactivate_node)
	local n_removing
	n_removing=$(grep -c 'Removing' "$logfile" || true)
	echo "  Removing:   $n_removing"

	# Deferred/drain events
	grep 'Draining\|deferred' "$logfile" | head -5 || true

	# Timestamps of first/last dm remove
	echo "  --- remove span ---"
	grep 'dm remove' "$logfile" > removes.tmp || true
	if [ -s removes.tmp ]; then
		sed -n '1s/^\([0-9:.]*\).*/  first: \1/p' removes.tmp
		sed -n '$s/^\([0-9:.]*\).*/  last:  \1/p' removes.tmp
	fi

	# Show Syncing/udev_wait timing
	grep 'Syncing device\|udev_wait\|dm_udev_wait\|Not syncing' "$logfile" | \
		head -3 | sed 's/^/  /' || true

	# Deactivating lines (one per LV from lv_deactivate)
	echo "  --- deactivate timeline (first 10) ---"
	grep 'Deactivating logical volume\|Deactivating .*tree' "$logfile" | \
		head -10 | sed 's/^\([0-9:.]*\).*Deactivat/  \1 Deactivat/' || true

	# Still present warnings
	local n_still
	n_still=$(grep -c 'still.*present' "$logfile" || true)
	if [ "${n_still:-0}" -gt 0 ]; then
		echo "  WARNING: $n_still 'still present' messages"
	fi
}

# ====================================================
echo "Parallel deactivation test: $TEST_DEVS linear LVs"
# ====================================================

#
# Phase 1: Activate
#
echo ""
echo "--- Activating $TEST_DEVS LVs ---"
T0=$(timestamp_ms)
vgchange -ay "$vg"
T1=$(timestamp_ms)
ACT_MS=$((T1 - T0))
echo "Activation:   ${ACT_MS} ms"

ACTIVE=$(dmsetup ls 2>/dev/null | grep -c "^${vg}-lv" || true)
echo "Active LVs:   $ACTIVE"

#
# Phase 2: Deactivate with debug
#
echo ""
echo "--- Deactivating $TEST_DEVS LVs ---"
T0=$(timestamp_ms)
vgchange -an "$vg" -vvvv 2>deact_debug.log
T1=$(timestamp_ms)
DEACT_MS=$((T1 - T0))
echo "Deactivation: ${DEACT_MS} ms"

REMAINING=$(dmsetup ls 2>/dev/null | grep -c "^${vg}-lv" || true)
echo "Remaining:    $REMAINING"

echo ""
analyze_log deact_debug.log "Deactivation"

#
# Summary
#
# =================================
echo "RESULTS: $TEST_DEVS linear LVs"
# ---------------------------------
printf "%-30s %6d ms\n" "Activation:" "$ACT_MS"
printf "%-30s %6d ms\n" "Deactivation:" "$DEACT_MS"
if [ "$DEACT_MS" -gt 0 ]; then
	printf "%-30s %6d us/LV\n" "Per-LV deact:" "$(( DEACT_MS * 1000 / TEST_DEVS ))"
fi

# Deactivation should be (much) faster than activation
test "$ACT_MS" -gt "$DEACT_MS"
test "$REMAINING" -eq 0
