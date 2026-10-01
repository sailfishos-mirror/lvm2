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

# Shared read-repair helpers for the integrity tests.
#
# They are sourced rather than run through 'aux' so they execute in the
# test's own shell: the caller's $mnt/$vg/$lv1 are visible, RAID_CORRUPTED
# is read back in place, and the flush step is a function instead of an
# eval'd string.  Source this before ". lib/inittest", which cds into
# $TESTDIR, so nothing here may touch the test environment at the top
# level.

#
# Capture the member layout of a raid LV while its DM tables still exist.
# Internal to raid_corrupt_data().
#
# _raid_capture_members LV [RAID_SEG]
#   LV        the raid LV, e.g. "$vg/$lv1"; the DM name is the LV with the
#             slash replaced by a dash
#   RAID_SEG  the segment holding the member layout when that differs from
#             LV, as for cached LVs where the raid is an origin segment
#             (e.g. "$vg/${lv1}_corig").  Defaults to LV.
#
# Sets RAID_DMNAME, RAID_LAYOUT and RAID_CHUNK from the raid table, the
# per-member RAID_MEMBER_DEVS/OFFSETS/LENGTHS (indexed the same way as the
# rimage_N names), and RAID_NR to the member count.  RAID_NR is 0 for
# mirrors (raid1/raid10), which have no parity and so need no mapping.
#
# Must be called while the LV is still active: deactivation removes the
# DM tables this reads.
#
_raid_capture_members() {
	local lv=$1 raid_seg=${2:-} i nr
	local -a raid_table linear_table

	[[ -n "$raid_seg" ]] || raid_seg=$lv
	RAID_DMNAME=${raid_seg//\//-}

	RAID_MEMBER_DEVS=()
	RAID_MEMBER_OFFSETS=()
	RAID_MEMBER_LENGTHS=()
	RAID_NR=0
	RAID_CHUNK=0
	RAID_LAYOUT=

	raid_table=( $(dmsetup table "$RAID_DMNAME") )
	RAID_LAYOUT=${raid_table[3]}
	case "${raid_table[3]}" in
	raid4|raid5_ls|raid6_zr)
		RAID_CHUNK=${raid_table[5]}
		nr=${raid_table[5 + raid_table[4]]}
		RAID_NR=$nr
		for ((i = 0; i < nr; i++)); do
			linear_table=( $(dmsetup table "${RAID_DMNAME}_rimage_${i}_iorig") )
			test "${#linear_table[@]}" -eq 5 && test "${linear_table[0]}" -eq 0 &&
				test "${linear_table[2]}" = linear || die "Expected a single linear image segment."
			RAID_MEMBER_DEVS[i]=$(get lv_field "${raid_seg}_rimage_${i}_iorig" devices -a)
			RAID_MEMBER_DEVS[i]=${RAID_MEMBER_DEVS[i]%%(*}
			RAID_MEMBER_OFFSETS[i]=${linear_table[4]}
			RAID_MEMBER_LENGTHS[i]=${linear_table[1]}
		done
		;;
	raid1|raid10) ;;
	*) die "Unsupported read-repair layout: ${raid_table[3]}" ;;
	esac
}

#
# Report whether member $1 holds the parity of the stripe that starts at
# member-relative sector $2.  Returns 0 for parity, 1 for data.
#
# dm-raid's raid4 uses fixed first parity; raid5_ls rotates left; raid6_zr
# rotates P right with Q following.  These are the rules in md's
# raid5_compute_sector().  Mirrors (RAID_NR 0) are all data.
#
_raid_member_is_parity() {
	local idx=$1 sector=$2 nr=$RAID_NR stripe parity

	test "$nr" -gt 0 || return 1

	stripe=$((sector / RAID_CHUNK))
	case "$RAID_LAYOUT" in
	raid4) parity=0 ;;
	raid5_ls) parity=$((nr - 1 - stripe % nr)) ;;
	raid6_zr)
		parity=$((stripe % nr))
		# Q sits immediately after P
		test "$idx" -eq "$(((parity + 1) % nr))" && return 0
		;;
	esac

	test "$idx" -eq "$parity"
}

#
# Corrupt one file-data block per backing device, never touching parity.
# Internal to raid_corrupt_data(); kept separate for readability only.
#
# _raid_corrupt_data MAX_CORRUPT DEV [DEV ...]
#   MAX_CORRUPT  stop after this many corruptions, or empty to try every
#                device listed
#   DEV ...      backing devices, as named by _raid_capture_members
#
# A pattern match on a backing PV can identify parity rather than file data,
# and parity reproduces the written pattern exactly when the other members
# are zeroed.  Corrupting parity alone leaves a read-repair test with no
# integrity mismatches, so matches landing on a parity member are skipped
# and the write is retried at the next data match.
#
# Fails unless at least one corruption lands.  An array that was never
# written reads back correctly and reports no integrity mismatches, so a run
# that corrupts nothing is indistinguishable from a pass to every assertion
# the caller makes afterwards; it has to fail here instead.  A caller that
# legitimately cannot corrupt anything must not call this at all.
#
# Call _raid_capture_members first, while the LV is still active.  The LV
# must be deactivated before this runs and is left deactivated, so the
# caller controls reactivation and any assertions that depend on it.
#
_raid_corrupt_data() {
	local max_corrupt=$1 corrupted=0 dev i position sector corrupt_len
	local -a matches

	# The unit every offset and length below is in: 'dmsetup table' reports
	# 512-byte sectors, both for the raid chunk and for the linear images.
	local sector_size=512

	shift

	printf BBBBBBBBCBBBBBBBB > corrupt.expected
	# Taken from the file so the length written, the length read back and
	# the length used to place the corruption cannot drift apart.
	corrupt_len=$(( $(wc -c < corrupt.expected) ))

	for dev in "$@"; do
		blockdev --flushbufs "$dev"
		if test "$RAID_NR" -gt 0; then
			for ((i = 0; i < RAID_NR; i++)); do
				test "$dev" != "${RAID_MEMBER_DEVS[i]}" || break
			done
			test "$i" -lt "$RAID_NR" || die "No RAID image on $dev."
		fi
		matches=( $(LC_ALL=C grep -oba -F BBBBBBBBBBBBBBBBB "$dev" | cut -d: -f1) ) || true
		for position in "${matches[@]}"; do
			if test "$RAID_NR" -gt 0; then
				# The sector holding the middle of the write, not its
				# first byte: a match near a sector boundary otherwise
				# names a stripe the corruption only reaches partly,
				# which is the very decision _raid_member_is_parity()
				# has to get right.
				sector=$(((position + corrupt_len / 2) / sector_size - RAID_MEMBER_OFFSETS[i]))
				test "$sector" -ge 0 && test "$sector" -lt "${RAID_MEMBER_LENGTHS[i]}" || continue
				_raid_member_is_parity "$i" "$sector" && continue
			fi
			# Write the selected match, not an earlier parity match.
			dd if=corrupt.expected of="$dev" bs=1 seek="$position" conv=notrunc,fdatasync
			blockdev --flushbufs "$dev"
			dd if="$dev" of=corrupt.actual bs=1 skip="$position" count="$corrupt_len"
			cmp corrupt.expected corrupt.actual
			corrupted=$((corrupted + 1))
			break
		done
		[[ -n "$max_corrupt" ]] && { test "$corrupted" -lt "$max_corrupt" || break; }
	done

	# Every assertion the caller makes after this needs a damaged array:
	# an untouched array reads back correctly and reports no integrity
	# mismatches, so a run that corrupts nothing cannot fail anywhere.
	# MAX_CORRUPT only caps the damage to what the array can still recover
	# from; which of the listed devices actually hold the pattern depends on
	# where the filesystem placed the data, so one is all that is required.
	test "$corrupted" -ge 1 ||
		die "Corrupted nothing, no read repair to verify on: $*"

	RAID_CORRUPTED=$corrupted
}

#
# Deactivate a raid LV and corrupt file data on its backing devices,
# skipping parity.  Leaves the LV deactivated.
#
# Usage: raid_corrupt_data LV RAID_SEG MAX_CORRUPT DEV [DEV ...]
#   LV           the raid LV, e.g. "$vg/$lv1"
#   RAID_SEG     the segment holding the member layout when that differs from
#                LV, as for cached LVs where the raid is an origin segment
#                (e.g. "$vg/${lv1}_corig").  Empty when LV is itself the raid.
#   MAX_CORRUPT  stop after this many corruptions, or empty to try every
#                device listed
#   DEV ...      backing devices to corrupt
#
# All three leading arguments are always passed; use "" for the ones that do
# not apply.  Putting MAX_CORRUPT before the device list lets a caller
# forward its own trailing devices as "$@".
#
# The LV must be active on entry (the DM tables are read to find the member
# layout) and is left deactivated, so the caller decides when to reactivate
# and what to assert in between.  Use this when the test needs to observe the
# damaged array itself; test_fs_with_read_repair() is the batteries-included
# version that also populates a filesystem and checks read repair.
#
# Fails unless at least one corruption lands, and then sets RAID_CORRUPTED to
# the number of corruptions written.
#
raid_corrupt_data() {
	local lv=$1 raid_seg=$2 max_corrupt=$3
	shift 3

	_raid_capture_members "$lv" "$raid_seg"
	lvchange -an "$lv"

	_raid_corrupt_data "$max_corrupt" "$@"
}

#
# Populate a filesystem, corrupt file data on its raid images, and verify
# that dm-raid reads the data back correctly.
#
# Usage: test_fs_with_read_repair LV MNT RAID_SEG MKFS_CMD FLUSH_FN MAX_CORRUPT DEV [DEV ...]
#   LV           the volume to mkfs/mount, e.g. "$vg/$lv1"
#   MNT          mount point, relative to $TESTDIR or absolute
#   RAID_SEG     the underlying raid segment, or empty when LV is itself the
#                raid.  Needed for cached LVs, where LV is the cache device
#                and the raid is an origin segment (e.g. "$vg/${lv1}_corig").
#   MKFS_CMD     mkfs command to run before mounting, or empty to skip the
#                mkfs step (the caller has already created the filesystem)
#   FLUSH_FN     optional name of a function run after the files are copied
#                and before deactivation, e.g. clearing a writecache
#   MAX_CORRUPT  stop after this many corruptions, or empty for every device
#   DEV ...      backing devices to corrupt
#
# MAX_CORRUPT and FLUSH_FN come last so callers can forward their own
# trailing device list as "$@".
#
# The mkfs step is the only optional part; the mount/copy/unmount always
# runs, and the final re-read is unconditional because reading the
# corrupted sector is what makes dm-integrity detect the mismatch.
#
# Fails unless at least one corruption lands - see raid_corrupt_data().  A
# cache or writecache in front of the raid can keep the written files off the
# backing devices entirely, in which case there is nothing to corrupt and
# nothing to read repair, and the caller must not run this.
#
test_fs_with_read_repair() {
	local lv=$1 mnt=$2 raid_seg=$3 mkfs_cmd=$4 flush_fn=$5 max_corrupt=$6
	shift 6

	[[ -n "$mkfs_cmd" ]] && $mkfs_cmd "$DM_DEV_DIR/$lv"

	mount "$DM_DEV_DIR/$lv" "$mnt"

	# Not every caller populates the randA/randB/randC files (e.g.
	# integrity-blocksize-4.sh only writes fileA/fileB/fileC), so copy them
	# only when present.
	[[ -f randA ]] && cp randA "$mnt"
	[[ -f randB ]] && cp randB "$mnt"
	[[ -f randC ]] && cp randC "$mnt"
	cp fileA "$mnt"
	cp fileB "$mnt"
	cp fileC "$mnt"

	[[ -n "$flush_fn" ]] && "$flush_fn"

	umount "$mnt"

	# Capture the member mapping before deactivation removes the DM tables.
	raid_corrupt_data "$lv" "$raid_seg" "$max_corrupt" "$@"

	lvchange -ay $lv

	mount "$DM_DEV_DIR/$lv" "$mnt"
	cmp -b "$mnt/fileA" fileA
	cmp -b "$mnt/fileB" fileB
	cmp -b "$mnt/fileC" fileC
	umount "$mnt"
}

#
# Write the standard integrity test data files: fileA/fileB/fileC and the
# random randA/randB/randC.
#
# integrity_make_files [FILEB_B_COUNT]
#   FILEB_B_COUNT  number of leading "B" bytes in fileB; the rest, up to
#                  16384, is "b".  The default (16384) is all "B".  The
#                  split variants cover two 4K chunks so RAID6 can corrupt
#                  two data members, excluding parity.
#
integrity_make_files() {
	local b_count=${1:-16384}

	awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
	awk -v n="$b_count" 'BEGIN { while (z++ < n) printf "B" ; while (z++ < 16384) printf "b" }' > fileB
	awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC
}

#
# Write the random randA/randB/randC files.
#
# integrity_make_rand [BS]
#   BS  dd block size; default 512K
#
integrity_make_rand() {
	local bs=${1:-512K}

	dd if=/dev/urandom of=randA bs="$bs" count=2 2>/dev/null
	dd if=/dev/urandom of=randB bs="$bs" count=3 2>/dev/null
	dd if=/dev/urandom of=randC bs="$bs" count=4 2>/dev/null
}

#
# Create the test VG.
#
# integrity_prepare_vg [--clear] DEV...
#   --clear  zero the devices first, so a pattern search on them only finds
#            the file data written by the test and not stale content
#
integrity_prepare_vg() {
	local clear=0

	[[ "$1" = "--clear" ]] && { clear=1; shift; }
	[[ "$clear" -eq 0 ]] || aux clear_devs "$@"

	vgcreate $SHARED $vg "$@"
	pvs
}

#
# Populate a fresh filesystem on the current LV, leaving it mounted.
#
# integrity_add_new_data_to_mnt [MKFS_CMD]
#   MKFS_CMD  mkfs command; default "mkfs.ext4 -b 4096"
#
integrity_add_new_data_to_mnt() {
	local mkfs_cmd=${1:-mkfs.ext4 -b 4096}

	$mkfs_cmd "$DM_DEV_DIR/$vg/$lv1"

	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"

	cp randA "$mnt"
	cp randB "$mnt"
	cp randC "$mnt"
	mkdir "$mnt/1"
	cp fileA "$mnt/1"
	cp fileB "$mnt/1"
	cp fileC "$mnt/1"
	mkdir "$mnt/2"
	cp fileA "$mnt/2"
	cp fileB "$mnt/2"
	cp fileC "$mnt/2"
}

#
# Add a second copy of the data under $mnt/more.
#
integrity_add_more_data_to_mnt() {
	mkdir "$mnt/more"
	cp fileA "$mnt/more"
	cp fileB "$mnt/more"
	cp fileC "$mnt/more"
	cp randA "$mnt/more"
	cp randB "$mnt/more"
	cp randC "$mnt/more"
}

#
# Compare the files under $mnt against their originals.
#
integrity_verify_data_on_mnt() {
	cmp -b randA "$mnt/randA"
	cmp -b randB "$mnt/randB"
	cmp -b randC "$mnt/randC"
	cmp -b fileA "$mnt/1/fileA"
	cmp -b fileB "$mnt/1/fileB"
	cmp -b fileC "$mnt/1/fileC"
	cmp -b fileA "$mnt/2/fileA"
	cmp -b fileB "$mnt/2/fileB"
	cmp -b fileC "$mnt/2/fileC"
}

#
# Reactivate the current LV, verify the data, drop the copied files again
# and deactivate.
#
integrity_verify_data_on_lv() {
	lvchange -ay "$vg/$lv1"
	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
	integrity_verify_data_on_mnt
	rm "$mnt/randA" "$mnt/randB" "$mnt/randC"
	rm -rf "$mnt/1" "$mnt/2"
	umount "$mnt"
	lvchange -an "$vg/$lv1"
}

#
# Populate a fresh filesystem on the current LV and unmount it again.
#
# integrity_add_data_to_lv [MKFS_CMD]
#
integrity_add_data_to_lv() {
	integrity_add_new_data_to_mnt "$@"
	umount "$mnt"
}

#
# Assert each LV reports a non-zero integritymismatches count.
#
# "not grep ' 0 ' mismatch" cannot do this: lvs right-aligns the value at the
# end of the line, so it is never followed by a space and the pattern matches
# nothing at all - not a real zero either, which makes the assertion pass
# unconditionally.  Compare the field instead, and reject an empty value so
# that an LV without integrity cannot pass as one with mismatches.
#
integrity_not_zero_mismatches() {
	local lv

	for lv in "$@"; do
		not check lv_field "$lv" integritymismatches ""
		not check lv_field "$lv" integritymismatches "0"
	done
}

#
# Sourced by the tests above; run directly it is also a small test: a raid5
# with integrity must read a corrupted member back correctly.
#
[[ "${BASH_SOURCE[0]}" = "$0" ]] || return 0

. lib/inittest

aux have_fs ext4 || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least 5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 3 64

integrity_make_files

integrity_prepare_vg --clear "$dev1" "$dev2" "$dev3"
lvcreate --type raid5 --raidintegrity y -n "$lv1" -l 8 "$vg" "$dev1" "$dev2" "$dev3"
lvs -a -o name,segtype,devices,sync_percent "$vg"
aux wait_recalc "$vg/${lv1}_rimage_0"
aux wait_recalc "$vg/${lv1}_rimage_1"
aux wait_recalc "$vg/${lv1}_rimage_2"
aux wait_recalc "$vg/$lv1"

test_fs_with_read_repair "$vg/$lv1" "$mnt" "" "mkfs.ext4 -b 4096" "" 1 "$dev1" "$dev2" "$dev3"
integrity_not_zero_mismatches "$vg/$lv1"

lvchange -an "$vg/$lv1"
lvconvert --raidintegrity n "$vg/$lv1"
lvremove "$vg/$lv1"
vgremove -ff "$vg"
