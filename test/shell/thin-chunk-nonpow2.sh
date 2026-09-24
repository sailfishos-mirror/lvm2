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
#
# Test thin-pool with non-power-of-2 chunk size.
#
# The kernel bio_discard_limit() uses round_up()/round_down() macros
# that require power-of-2 arguments (they use bitwise ops internally).
# With non-power-of-2 discard_granularity (= thin-pool chunk size),
# bio split points get misaligned, producing partial-chunk discard bios
# that thin-pool silently drops.
#
# Introduced by kernel commit 9b15d109a6b2
#   "block: improve discard bio alignment in __blkdev_issue_discard()"
#
# Example with 192k (384 sector) chunk:
#   round_up(768, 384) = 1024 (wrong, should be 768)
#   A 384-sector discard at offset 768 splits into 256+128 sectors,
#   neither is a full chunk, both get dropped silently.

export LVM_TEST_THIN_REPAIR_CMD=${LVM_TEST_THIN_REPAIR_CMD-/bin/false}

. lib/inittest --skip-with-lvmpolld --skip-with-lvmlockd

command -v blkdiscard > /dev/null 2>&1 || skip

aux have_thin 1 0 0 || skip

aux prepare_vg 2 256

THIN_VOL_MIB=4
MAX_OFFSET_CHUNKS=4
THIN_VOL_BYTES=$(( THIN_VOL_MIB * 1024 * 1024 ))

# Result collection arrays for summary table
RES_N=0
declare -a RES_CHUNK RES_POW2 RES_T1 RES_T2 RES_T3 RES_T4

# Helper: get dm name for an LV
dm_name_() {
	dmsetup info -c --noheadings -o blkdevname "$1" 2>/dev/null || true
}

chunk_size_bytes_() {
	echo $(( ${1%[kK]} * 1024 ))
}

# Helper: get current data block usage from thin-pool status (used/total).
pool_data_blocks_() {
	local a

	a=( $(dmsetup status "$1") )
	case "${a[2]}" in
	thin-pool)
		echo "${a[5]}"
		return 0
	;;
	esac
	die "used/total data blocks not found in dmsetup status for $1"
}

# Helper: get just the used data block count (split the "used/total" echo on /)
pool_used_blocks_() {
	local blocks
	blocks=$(pool_data_blocks_ "$1")
	echo "${blocks%%/*}"
}

# Fill the thin volume used by discard tests.
thin_vol_fill_() {
	local DEV="$1"

	dd if=/dev/zero of="$DEV" bs=1M count=$THIN_VOL_MIB oflag=direct conv=fdatasync 2>/dev/null
}

# Allocate thin data for the byte range [OFFSET, OFFSET+LEN) before discard.
thin_vol_allocate_range_() {
	local DEV="$1"
	local OFFSET="$2"
	local LEN="$3"
	local CHUNK_BYTES="$4"
	local COUNT

	COUNT=$(( LEN / CHUNK_BYTES ))
	dd if=/dev/zero of="$DEV" bs=$CHUNK_BYTES count=$COUNT \
		seek=$(( OFFSET / CHUNK_BYTES )) oflag=direct conv=fdatasync 2>/dev/null
}

# Issue blkdiscard, print used block counts before and after.
# LEN "full" discards the whole device; otherwise -o OFFSET -l LEN is used.
discard_measure_() {
	local TPOOL="$1"
	local DEV="$2"
	local OFFSET="$3"
	local LEN="$4"
	local BEFORE AFTER RC=0

	BEFORE=$(pool_used_blocks_ "$TPOOL")
	if test "$LEN" = full ; then
		blkdiscard "$DEV" || RC=$?
	else
		blkdiscard -o "$OFFSET" -l "$LEN" "$DEV" || RC=$?
	fi
	AFTER=$(pool_used_blocks_ "$TPOOL")
	echo "$BEFORE $AFTER $RC"
}

discard_after_allocate_measure_() {
	local TPOOL="$1"
	local DEV="$2"
	local OFFSET="$3"
	local LEN="$4"
	local CHUNK_BYTES="$5"

	thin_vol_allocate_range_ "$DEV" "$OFFSET" "$LEN" "$CHUNK_BYTES"
	discard_measure_ "$TPOOL" "$DEV" "$OFFSET" "$LEN"
}

# Helper: dump discard-related sysfs queue parameters
dump_discard_sysfs_() {
	local dmname="$1"
	local sysdir="/sys/block/$dmname/queue"

	echo "=== sysfs queue for $dmname ==="
	for f in discard_granularity discard_max_bytes \
		 logical_block_size physical_block_size \
		 minimum_io_size optimal_io_size ; do
		test -f "$sysdir/$f" && echo "  $f = $(< "$sysdir/$f")" || true
	done
}

# Record one result row before running chunk-size scenarios.
chunk_discard_begin_row_() {
	local CHUNK="$1"
	local CHUNK_BYTES

	CHUNK_BYTES=$(chunk_size_bytes_ "$CHUNK")
	CHUNK_DISCARD_IDX=$RES_N
	RES_N=$(( RES_N + 1 ))
	RES_CHUNK[$CHUNK_DISCARD_IDX]="$CHUNK"
	if test $(( CHUNK_BYTES & (CHUNK_BYTES - 1) )) -eq 0 ; then
		RES_POW2[$CHUNK_DISCARD_IDX]="yes"
	else
		RES_POW2[$CHUNK_DISCARD_IDX]="no"
	fi
	RES_T1[$CHUNK_DISCARD_IDX]="N/A"
	RES_T2[$CHUNK_DISCARD_IDX]="N/A"
	RES_T3[$CHUNK_DISCARD_IDX]="N/A"
	RES_T4[$CHUNK_DISCARD_IDX]="N/A"
}

# Create thin pool and volume; print TPOOL, DEV, and CHUNK_BYTES (tab-separated).
chunk_discard_create_pool_() {
	local CHUNK="$1"
	local POOLNAME="$2"
	local CHUNK_BYTES
	local CHUNK_SECTORS
	local TPOOL="${vg}-${POOLNAME}-tpool"
	local DEV="$DM_DEV_DIR/$vg/tvol"

	CHUNK_BYTES=$(chunk_size_bytes_ "$CHUNK")
	CHUNK_SECTORS=$(( CHUNK_BYTES / 512 ))

	echo >&2
	echo "###################################################" >&2
	echo "# Testing chunk_size=$CHUNK ($CHUNK_BYTES bytes, $CHUNK_SECTORS sectors)" >&2
	echo "###################################################" >&2

	lvcreate -y -L64M -T "$vg/$POOLNAME" --chunksize "$CHUNK" --discards passdown >&2
	check lv_field "$vg/$POOLNAME" chunksize "${CHUNK%[kK]}.00k" >&2

	lvcreate -V${THIN_VOL_MIB}M -T "$vg/$POOLNAME" -n tvol >&2

	printf '%s\t%s\t%s\n' "$TPOOL" "$DEV" "$CHUNK_BYTES"
}

chunk_discard_prepare_volume_() {
	local DEV="$1"
	local TPOOL="$2"
	local CHUNK="$3"
	local CHUNK_BYTES="$4"
	local TVOL_DM

	TVOL_DM=$(dm_name_ "${vg}-tvol")
	echo "--- Thin volume: $TVOL_DM ---"
	test -n "$TVOL_DM" && dump_discard_sysfs_ "$TVOL_DM"

	if test -n "$TVOL_DM" ; then
		local GRAN
		GRAN=$(< "/sys/block/$TVOL_DM/queue/discard_granularity")
		echo "  Expected discard_granularity=$CHUNK_BYTES, got=$GRAN"
	fi

	thin_vol_fill_ "$DEV"

	echo "After filling: $(pool_used_blocks_ "$TPOOL") data blocks used ($(pool_data_blocks_ "$TPOOL"))"
}

test_full_discard_() {
	local TPOOL="$1"
	local DEV="$2"
	local IDX="$3"
	local FILLED AFTER_FULL

	echo
	echo "--- Test 1: Full-device blkdiscard ---"

	read -r FILLED AFTER_FULL DISCARD_RC < <(discard_measure_ "$TPOOL" "$DEV" 0 full)
	echo "After full blkdiscard: $AFTER_FULL blocks used (was $FILLED)"

	if test "$DISCARD_RC" -ne 0 ; then
		echo "WARNING: full blkdiscard failed (exit $DISCARD_RC)"
		RES_T1[$IDX]="FAIL(blkdiscard)"
	elif test "$AFTER_FULL" -ne 0 ; then
		echo "WARNING: full blkdiscard left $AFTER_FULL blocks allocated!"
		RES_T1[$IDX]="FAIL($AFTER_FULL left)"
	else
		RES_T1[$IDX]="OK"
	fi
}

test_offset_discards_() {
	local TPOOL="$1"
	local DEV="$2"
	local CHUNK_BYTES="$3"
	local IDX="$4"
	local NUM_CHUNKS TEST_CHUNKS PASS FAIL i OFFSET BEFORE AFTER RESULT

	echo
	echo "--- Test 2: Single-chunk discards at specific offsets ---"

	PASS=0
	FAIL=0
	NUM_CHUNKS=$(( THIN_VOL_BYTES / CHUNK_BYTES ))
	TEST_CHUNKS=$(( NUM_CHUNKS < MAX_OFFSET_CHUNKS ? NUM_CHUNKS : MAX_OFFSET_CHUNKS ))

	for i in $(seq 0 $(( TEST_CHUNKS - 1 )) ) ; do
		OFFSET=$(( i * CHUNK_BYTES ))

		read -r BEFORE AFTER DISCARD_RC < <(discard_after_allocate_measure_ "$TPOOL" "$DEV" \
			"$OFFSET" "$CHUNK_BYTES" "$CHUNK_BYTES")

		if test "$DISCARD_RC" -ne 0 ; then
			RESULT="FAIL (blkdiscard exit $DISCARD_RC)"
			FAIL=$(( FAIL + 1 ))
		elif test "$AFTER" -le "$(( BEFORE - 1 ))" ; then
			RESULT="OK"
			PASS=$(( PASS + 1 ))
		else
			RESULT="FAIL (discard silently dropped)"
			FAIL=$(( FAIL + 1 ))
		fi

		echo "  offset=$OFFSET ($((OFFSET/1024))k, chunk#$i): before=$BEFORE after=$AFTER -- $RESULT"
	done

	echo "  Summary: $PASS passed, $FAIL failed out of $TEST_CHUNKS offsets"
	RES_T2[$IDX]="$PASS/$TEST_CHUNKS"
}

test_two_chunk_discard_() {
	local TPOOL="$1"
	local DEV="$2"
	local CHUNK_BYTES="$3"
	local IDX="$4"
	local BEFORE3 AFTER3 FREED3
	local LEN=$(( CHUNK_BYTES * 2 ))

	echo
	echo "--- Test 3: Two-chunk discard at offset=$CHUNK_BYTES ---"

	read -r BEFORE3 AFTER3 DISCARD_RC < <(discard_after_allocate_measure_ "$TPOOL" "$DEV" \
		"$CHUNK_BYTES" "$LEN" "$CHUNK_BYTES")
	echo "  before=$BEFORE3 after=$AFTER3 (expected $((BEFORE3 - 2)) if both chunks freed)"

	FREED3=$(( BEFORE3 - AFTER3 ))
	RES_T3[$IDX]="$FREED3/2"
	if test "$DISCARD_RC" -ne 0 ; then
		echo "  WARNING: blkdiscard failed (exit $DISCARD_RC)"
		RES_T3[$IDX]="FAIL(blkdiscard)"
	elif test "$AFTER3" -le "$(( BEFORE3 - 2 ))" ; then
		echo "  OK: both chunks freed"
	else
		echo "  WARNING: expected 2 chunks freed, got $FREED3"
	fi
}

test_fstrim_discard_() {
	local TPOOL="$1"
	local DEV="$2"
	local IDX="$3"

	if command -v mkfs.ext4 > /dev/null 2>&1 && command -v fstrim > /dev/null 2>&1 ; then
		echo
		echo "--- Test 4: ext4 fstrim ---"

		mkfs.ext4 -E nodiscard "$DEV" > /dev/null 2>&1
		local MNT="mnt_$$"
		mkdir -p "$MNT"
		if ! mount "$DEV" "$MNT" ; then
			echo "  SKIP: unable to mount $DEV, fstrim test not run"
			rmdir "$MNT" 2>/dev/null || true
			return
		fi

		dd if=/dev/zero of="$MNT/testfile" bs=1M count=2 oflag=direct conv=fdatasync 2>/dev/null
		sync
		local BEFORE4 AFTER4 FREED4
		BEFORE4=$(pool_used_blocks_ "$TPOOL")

		rm -f "$MNT/testfile"
		sync

		fstrim -v "$MNT" 2>&1 || true

		AFTER4=$(pool_used_blocks_ "$TPOOL")
		FREED4=$(( BEFORE4 - AFTER4 ))
		echo "  fstrim freed $FREED4 chunks (before=$BEFORE4, after=$AFTER4)"

		if test "$FREED4" -eq 0 ; then
			echo "  WARNING: fstrim freed nothing!"
			RES_T4[$IDX]="FAIL(0)"
		else
			RES_T4[$IDX]="OK($FREED4)"
		fi

		umount "$MNT"
	fi
}

# Test a single chunk size for discard correctness at various offsets
# Usage: test_chunk_discard <chunk_size> <pool_name>
test_chunk_discard_() {
	local CHUNK="$1"
	local POOLNAME="$2"
	local IDX TPOOL DEV CHUNK_BYTES

	chunk_discard_begin_row_ "$CHUNK"
	IDX=$CHUNK_DISCARD_IDX
	IFS=$'\t' read -r TPOOL DEV CHUNK_BYTES < <(chunk_discard_create_pool_ "$CHUNK" "$POOLNAME")
	chunk_discard_prepare_volume_ "$DEV" "$TPOOL" "$CHUNK" "$CHUNK_BYTES"

	test_full_discard_ "$TPOOL" "$DEV" "$IDX"
	test_offset_discards_ "$TPOOL" "$DEV" "$CHUNK_BYTES" "$IDX"
	test_two_chunk_discard_ "$TPOOL" "$DEV" "$CHUNK_BYTES" "$IDX"
	# ext4 mount/fstrim is slow; run only with the full chunk-size matrix.
	if test "${LVM_TEST_THIN_CHUNK_NONPOW2_FULL:-}" = 1 ; then
		test_fstrim_discard_ "$TPOOL" "$DEV" "$IDX"
	fi
	lvremove -f "$vg/$POOLNAME"
}


echo "============================================================"
echo "  Non-power-of-2 thin-pool chunk size discard test"
echo "============================================================"
echo
echo "Kernel: $(uname -r)"
echo

# Default: one non-pow2 size plus one power-of-2 control.
test_chunk_discard_ 192k pool1

test_chunk_discard_ 128k pool3

# Optional full chunk-size matrix (slow: extra sizes, ext4 fstrim per pool).
# Set LVM_TEST_THIN_CHUNK_NONPOW2_FULL=1 to enable.
if test "${LVM_TEST_THIN_CHUNK_NONPOW2_FULL:-}" = 1 ; then
	test_chunk_discard_ 384k pool2

	test_chunk_discard_ 256k pool4

	test_chunk_discard_ 320k pool5

	test_chunk_discard_ 576k pool6
fi

(
printf "\n"
printf "============================================================\n"
printf "  SUMMARY: Non-power-of-2 chunk size discard test\n"
printf "============================================================\n"
printf "  Kernel: %s\n\n" "$(uname -r)"
printf "  %-8s %-5s %-16s %-10s %-8s %s\n" \
	"Chunk" "Pow2" "Full-discard" "Offsets" "2-chunk" "fstrim"
printf "  %-8s %-5s %-16s %-10s %-8s %s\n" \
	"-----" "----" "------------" "-------" "-------" "------"
for i in $(seq 0 $(( RES_N - 1 )) ) ; do
	printf "  %-8s %-5s %-16s %-10s %-8s %s\n" \
		"${RES_CHUNK[$i]}" "${RES_POW2[$i]}" "${RES_T1[$i]}" \
		"${RES_T2[$i]}" "${RES_T3[$i]}" "${RES_T4[$i]}"
done
printf "\n"
) > out

cat out

check_thin_chunk_discard_summary_() {
	local i t2_pass t2_total t3_freed SUMMARY_FAIL=0

	for i in $(seq 0 $(( RES_N - 1 )) ) ; do
		# Non-pow2 rows document kernel discard quirks; only the pow2
		# control must pass for the test to succeed.
		test "${RES_POW2[$i]}" = "yes" || continue

		case ${RES_T1[$i]} in
		OK) ;;
		*) SUMMARY_FAIL=1 ;;
		esac

		t2_pass=${RES_T2[$i]%%/*}
		t2_total=${RES_T2[$i]#*/}
		if test "$t2_pass" != "$t2_total" ; then
			SUMMARY_FAIL=1
		fi

		t3_freed=${RES_T3[$i]%%/*}
		case "$t3_freed" in
		''|*[!0-9]*) SUMMARY_FAIL=1 ;;
		*) test "$t3_freed" -lt 2 && SUMMARY_FAIL=1 ;;
		esac

		case ${RES_T4[$i]} in
		N/A) ;;
		OK*) ;;
		*) SUMMARY_FAIL=1 ;;
		esac
	done

	test "$SUMMARY_FAIL" -eq 0 || die "thin pool discard checks failed"
}

check_thin_chunk_discard_summary_

vgremove -ff $vg
