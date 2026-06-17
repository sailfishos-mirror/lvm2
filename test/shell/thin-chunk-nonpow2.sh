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

which blkdiscard || skip

aux have_thin 1 0 0 || skip

aux prepare_vg 2 256

# Result collection arrays for summary table
RES_N=0
declare -a RES_CHUNK RES_POW2 RES_T1 RES_T2 RES_T3 RES_T4

# Helper: get dm name for an LV
dm_name_() {
	dmsetup info -c --noheadings -o blkdevname "$1" 2>/dev/null
}

# Helper: get current data block usage from thin-pool status
# Returns "used/total" data block counts
pool_data_blocks_() {
	dmsetup status "$1" | awk '{print $6}'
}

# Helper: get just the used data block count
pool_used_blocks_() {
	pool_data_blocks_ "$1" | cut -d/ -f1
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

# Test a single chunk size for discard correctness at various offsets
# Usage: test_chunk_discard <chunk_size> <pool_name>
test_chunk_discard_() {
	local CHUNK="$1"
	local POOLNAME="$2"
	local TPOOL="${vg}-${POOLNAME}-tpool"
	local CHUNK_BYTES
	local CHUNK_SECTORS
	local DEV="$DM_DEV_DIR/$vg/tvol"
	local TVOL_DM

	# Parse chunk size to bytes (handle k/K suffix)
	CHUNK_BYTES=$(( ${CHUNK%[kK]} * 1024 ))
	CHUNK_SECTORS=$(( CHUNK_BYTES / 512 ))

	local IS_POW2="no"
	test $(( CHUNK_BYTES & (CHUNK_BYTES - 1) )) -eq 0 && IS_POW2="yes"

	local IDX=$RES_N
	RES_N=$(( RES_N + 1 ))
	RES_CHUNK[$IDX]="$CHUNK"
	RES_POW2[$IDX]="$IS_POW2"
	RES_T1[$IDX]="N/A"
	RES_T2[$IDX]="N/A"
	RES_T3[$IDX]="N/A"
	RES_T4[$IDX]="N/A"

	echo
	echo "###################################################"
	echo "# Testing chunk_size=$CHUNK ($CHUNK_BYTES bytes, $CHUNK_SECTORS sectors)"
	echo "###################################################"

	lvcreate -y -L64M -T "$vg/$POOLNAME" --chunksize "$CHUNK" --discards passdown
	check lv_field "$vg/$POOLNAME" chunksize "${CHUNK%[kK]}.00k"

	# Create thin volume -- 4M is enough for offset tests
	lvcreate -V4M -T "$vg/$POOLNAME" -n tvol

	TVOL_DM=$(dm_name_ "${vg}-tvol")
	echo "--- Thin volume: $TVOL_DM ---"
	test -n "$TVOL_DM" && dump_discard_sysfs_ "$TVOL_DM"

	# Verify discard_granularity matches chunk size
	if test -n "$TVOL_DM" ; then
		local GRAN
		GRAN=$(< "/sys/block/$TVOL_DM/queue/discard_granularity")
		echo "  Expected discard_granularity=$CHUNK_BYTES, got=$GRAN"
	fi

	# Fill the entire thin volume to allocate all chunks
	dd if=/dev/zero of="$DEV" bs=1M count=4 oflag=direct conv=fdatasync 2>/dev/null

	local FILLED
	FILLED=$(pool_used_blocks_ "$TPOOL")
	echo "After filling: $FILLED data blocks used ($(pool_data_blocks_ "$TPOOL"))"

	#
	# Test 1: Full-device blkdiscard -- should free everything
	#
	echo
	echo "--- Test 1: Full-device blkdiscard ---"

	blkdiscard "$DEV"

	local AFTER_FULL
	AFTER_FULL=$(pool_used_blocks_ "$TPOOL")
	echo "After full blkdiscard: $AFTER_FULL blocks used (was $FILLED)"

	if test "$AFTER_FULL" -ne 0 ; then
		echo "WARNING: full blkdiscard left $AFTER_FULL blocks allocated!"
		RES_T1[$IDX]="FAIL($AFTER_FULL left)"
	else
		RES_T1[$IDX]="OK"
	fi

	#
	# Test 2: Targeted single-chunk discards at various offsets
	# This is where the bug manifests -- certain offsets cause
	# round_up() to misalign the bio split, producing partial-chunk
	# discards that get silently dropped.
	#
	echo
	echo "--- Test 2: Single-chunk discards at specific offsets ---"

	local OFFSET EXPECTED_FREE ACTUAL_FREE RESULT
	local PASS=0
	local FAIL=0

	# Test offsets at each chunk boundary within our 4M volume
	# 4M / chunk_size = number of chunks
	local NUM_CHUNKS=$(( 4 * 1024 * 1024 / CHUNK_BYTES ))
	# Limit to first 8 chunks or total, whichever is less
	local TEST_CHUNKS=$(( NUM_CHUNKS < 8 ? NUM_CHUNKS : 8 ))

	for i in $(seq 0 $(( TEST_CHUNKS - 1 )) ) ; do
		OFFSET=$(( i * CHUNK_BYTES ))

		# Fill the volume first
		dd if=/dev/zero of="$DEV" bs=1M count=4 oflag=direct conv=fdatasync 2>/dev/null
		local BEFORE
		BEFORE=$(pool_used_blocks_ "$TPOOL")

		# Discard exactly one chunk at this offset
		blkdiscard -o "$OFFSET" -l "$CHUNK_BYTES" "$DEV" 2>&1 || true

		local AFTER
		AFTER=$(pool_used_blocks_ "$TPOOL")
		EXPECTED_FREE=$(( BEFORE - 1 ))

		if test "$AFTER" -le "$EXPECTED_FREE" ; then
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

	#
	# Test 3: Two-chunk discard spanning a chunk boundary
	#
	echo
	echo "--- Test 3: Two-chunk discard at offset=$CHUNK_BYTES ---"

	dd if=/dev/zero of="$DEV" bs=1M count=4 oflag=direct conv=fdatasync 2>/dev/null
	local BEFORE3
	BEFORE3=$(pool_used_blocks_ "$TPOOL")

	blkdiscard -o "$CHUNK_BYTES" -l "$(( CHUNK_BYTES * 2 ))" "$DEV" 2>&1 || true

	local AFTER3
	AFTER3=$(pool_used_blocks_ "$TPOOL")
	echo "  before=$BEFORE3 after=$AFTER3 (expected $((BEFORE3 - 2)) if both chunks freed)"

	local FREED3=$(( BEFORE3 - AFTER3 ))
	RES_T3[$IDX]="$FREED3/2"
	if test "$AFTER3" -le "$(( BEFORE3 - 2 ))" ; then
		echo "  OK: both chunks freed"
	else
		echo "  WARNING: expected 2 chunks freed, got $FREED3"
	fi

	#
	# Test 4: fstrim if ext4 is available
	#
	if which mkfs.ext4 > /dev/null 2>&1 && which fstrim > /dev/null 2>&1 ; then
		echo
		echo "--- Test 4: ext4 fstrim ---"

		mkfs.ext4 -E nodiscard "$DEV" > /dev/null 2>&1
		local MNT="mnt_$$"
		mkdir -p "$MNT"
		mount "$DEV" "$MNT"

		dd if=/dev/zero of="$MNT/testfile" bs=1M count=2 oflag=direct conv=fdatasync 2>/dev/null
		sync
		local BEFORE4
		BEFORE4=$(pool_used_blocks_ "$TPOOL")

		rm -f "$MNT/testfile"
		sync

		fstrim -v "$MNT" 2>&1 || true

		local AFTER4
		AFTER4=$(pool_used_blocks_ "$TPOOL")
		local FREED4=$(( BEFORE4 - AFTER4 ))
		echo "  fstrim freed $FREED4 chunks (before=$BEFORE4, after=$AFTER4)"

		RES_T4[$IDX]="$FREED4 freed"
		if test "$FREED4" -eq 0 ; then
			echo "  WARNING: fstrim freed nothing!"
		fi

		umount "$MNT"
	fi

	lvremove -f "$vg/$POOLNAME"
}


echo "============================================================"
echo "  Non-power-of-2 thin-pool chunk size discard test"
echo "============================================================"
echo
echo "Kernel: $(uname -r)"
echo

# Test non-power-of-2 chunk sizes
test_chunk_discard_ 192k pool1

test_chunk_discard_ 384k pool2

# Test power-of-2 as control group
test_chunk_discard_ 128k pool3

test_chunk_discard_ 256k pool4

# More non-power-of-2 sizes
test_chunk_discard_ 320k pool5

test_chunk_discard_ 576k pool6

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

vgremove -ff $vg
