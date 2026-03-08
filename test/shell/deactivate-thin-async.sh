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

# Measure async deactivation performance for thin volumes.
#
# Creates a thin pool with TEST_DEVS thin volumes using bulk
# metadata injection (vgcfgrestore + thin_restore), then
# measures activation and deactivation wall-clock times.
#
# Usage:
#   TEST_DEVS=1000 test/shell/deactivate-thin-async.sh

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

aux have_thin 1 0 0 || skip

test -n "$LVM_TEST_THIN_RESTORE_CMD" || LVM_TEST_THIN_RESTORE_CMD=$(which thin_restore) || skip
"$LVM_TEST_THIN_RESTORE_CMD" -V || skip

TEST_DEVS=${TEST_DEVS:-500}

# On low-memory boxes reduce
test "$(aux total_mem)" -gt 524288 || TEST_DEVS=200

POOL_DATA=200   # MiB
POOL_META=8     # MiB
CHUNK=64        # KiB
DBLOCK_SIZE=$(( CHUNK * 2 ))  # sectors (64k = 128 sectors)
NR_DBLOCKS=$(( POOL_DATA * 1024 / CHUNK ))

# Need enough backing space
aux prepare_pvs 1 $(( POOL_DATA + POOL_META * 3 + 20 ))
get_devs

vgcreate -s 1M "$vg" "${DEVICES[@]}"

# Create thin pool (no thin LVs) and a helper LV for metadata swap
lvcreate -T -L${POOL_DATA}M --poolmetadatasize ${POOL_META}M \
	 --chunksize ${CHUNK}k "$vg"/pool
lvcreate -L${POOL_META}M -n "$lv1" "$vg"

lvchange -an "$vg"

# --- Inject TEST_DEVS thin LVs into VG metadata ---

vgcfgbackup -f data "$vg"

awk -v N="$TEST_DEVS" '
# Update pool transaction_id from 0 to N
/transaction_id = 0/ && !txn_done {
	sub(/transaction_id = 0/, "transaction_id = " N)
	txn_done = 1
}

# Track brace depth inside logical_volumes to find its closing }
/logical_volumes/ { in_lv = 1; lv_depth = 0 }

in_lv && /{/ { lv_depth++ }

in_lv && /}/ {
	lv_depth--
	if (lv_depth == 0) {
		# Inject thin LV definitions before closing logical_volumes
		for (i = 1; i <= N; i++) {
			printf("\n\t\tthin%04d {\n", i)
			printf("\t\t\tid = \"%06d-aaaa-bbbb-cccc-dddd-eeee-%06d\"\n", i, i)
			print "\t\t\tstatus = [\"READ\", \"WRITE\", \"VISIBLE\"]"
			print "\t\t\tsegment_count = 1"
			print "\t\t\tsegment1 {"
			print "\t\t\t\tstart_extent = 0"
			print "\t\t\t\textent_count = 1"
			print "\t\t\t\ttype = \"thin\""
			print "\t\t\t\tthin_pool = \"pool\""
			printf("\t\t\t\ttransaction_id = %d\n", i)
			printf("\t\t\t\tdevice_id = %d\n", i)
			print "\t\t\t}"
			print "\t\t}"
		}
		in_lv = 0
	}
}

{ print }
' data > data_new

vgcfgrestore --force -f data_new "$vg"

# --- Generate matching thin-pool device metadata ---
awk -v N="$TEST_DEVS" -v DBS="$DBLOCK_SIZE" -v NRD="$NR_DBLOCKS" 'BEGIN {
	printf "<superblock uuid=\"\" time=\"1\" transaction=\"%d\" data_block_size=\"%d\" nr_data_blocks=\"%d\">\n", N, DBS, NRD
	for (i = 1; i <= N; i++)
		printf " <device dev_id=\"%d\" mapped_blocks=\"0\" transaction=\"%d\" creation_time=\"0\" snap_time=\"1\">\n </device>\n", i, i
	print "</superblock>"
}' > thin_meta.xml

lvchange -ay "$vg/$lv1"
"$LVM_TEST_THIN_RESTORE_CMD" -i thin_meta.xml -o "$DM_DEV_DIR/mapper/$vg-$lv1"

# Swap pool metadata with our prepared volume
lvconvert -y --chunksize ${CHUNK}k --thinpool "$vg"/pool --poolmetadata "$vg/$lv1"

# --- Timing ---

timestamp_ms() {
	echo $(( $(date +%s%N) / 1000000 ))
}

# ================================================
echo "Thin deactivation test: $TEST_DEVS thin LVs"
# ================================================

# Activate
echo ""
echo "--- Activating $TEST_DEVS thin LVs ---"
T0=$(timestamp_ms)
vgchange -ay "$vg"
T1=$(timestamp_ms)
ACT_MS=$(( T1 - T0 ))
echo "Activation:   ${ACT_MS} ms"

ACTIVE=$(dmsetup ls 2>/dev/null | grep -c "^${vg}-thin" || true)
echo "Active thin:  $ACTIVE"

# Deactivate with debug
echo ""
echo "--- Deactivating $TEST_DEVS thin LVs ---"
T0=$(timestamp_ms)
vgchange -an "$vg" -vvvv 2>deact_debug.log
T1=$(timestamp_ms)
DEACT_MS=$(( T1 - T0 ))
echo "Deactivation: ${DEACT_MS} ms"

REMAINING=$(dmsetup ls 2>/dev/null | grep -c "^${vg}-" || true)
echo "Remaining:    $REMAINING"

# Analysis from debug log
if [ -s deact_debug.log ]; then
	echo ""
	echo "=== Deactivation log ==="
	echo "  dm remove:  $(grep -c 'dm remove' deact_debug.log || true)"
	echo "  Removing:   $(grep -c 'Removing' deact_debug.log || true)"
	echo "  Deferred:   $(grep -c 'deferred\|Drained' deact_debug.log || true)"
fi

# Summary
# =============================================
echo "RESULTS: $TEST_DEVS thin LVs"
# ---------------------------------------------
printf "%-30s %6d ms\n" "Activation:" "$ACT_MS"
printf "%-30s %6d ms\n" "Deactivation:" "$DEACT_MS"
if [ "$DEACT_MS" -gt 0 ]; then
	printf "%-30s %6d us/LV\n" "Per-LV deact:" "$(( DEACT_MS * 1000 / TEST_DEVS ))"
fi

# Deactivation should be (much) faster than activation
test "$ACT_MS" -gt "$DEACT_MS"
test "$REMAINING" -eq 0
