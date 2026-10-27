#!/usr/bin/env bash
# Copyright (C) 2011-2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# get.sh: get various values from volumes and io
#
# USAGE:
#  get pv_field PV field [pvs params]
#  get vg_field VG field [vgs params]
#  get lv_field LV field [lvs params]
#
#  get lv_devices LV     [lvs params]
#
#  get io_engine DEV
#  get io_count SYSCALL FILE [BATCH]

test -z "$BASH" || set -e -o pipefail

# trims only leading prefix and suffix
trim_() {
	rm -f debug.log             # drop log, command was ok
	local var=${1%"${1##*[! ]}"}  # remove trailing space characters
	echo "${var#"${var%%[! ]*}"}" # remove leading space characters
}

pv_field() {
	local r
	r=$(pvs --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	trim_ "$r"
}

vg_field() {
	local r
	r=$(vgs --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	trim_ "$r"
}

lv_field() {
	local r
	r=$(lvs --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	trim_ "$r"
}

lv_first_seg_field() {
	local r
	read -r r < <(lvs --config 'log{prefix=""}' --unbuffered --noheadings -o "$2" "${@:3}" "$1")
	trim_ "$r"
}

lvh_field() {
	local r
	r=$(lvs -H --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	trim_ "$r"
}

lva_field() {
	local r
	r=$(lvs -a --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	trim_ "$r"
}

lv_devices() {
	lv_field "$1" devices -a "${@:2}" | sed 's/([^)]*)//g; s/,/\n/g'
}

lv_field_lv_() {
	lv_field "$1" "$2" -a --unbuffered | tr -d '[]'
}

lv_tree_devices_() {
	local lv="$1/$2"
	local type
	type=$(lv_first_seg_field "$lv" segtype -a)
	#local orig
	#orig=$(lv_field_lv_ "$lv" origin)
	# FIXME: should we count in also origins ?
	#test -z "$orig" || lv_tree_devices_ $1 $orig
	case "$type" in
	linear|striped)
		lv_devices "$lv"
		;;
	mirror|raid*)
		local log
		log=$(lv_field_lv_ "$lv" mirror_log)
		test -z "$log" || lv_tree_devices_ "$1" "$log"
		local sub_devs
		sub_devs=( $(lv_devices "$lv") )
		for i in "${sub_devs[@]}"; do
			lv_tree_devices_ "$1" "$i"
		done
		;;
	thin)
		lv_tree_devices_ "$1" "$(lv_field_lv_ "$lv" pool_lv)"
		;;
	thin-pool)
		lv_tree_devices_ "$1" "$(lv_field_lv_ "$lv" data_lv)"
		lv_tree_devices_ "$1" "$(lv_field_lv_ "$lv" metadata_lv)"
		;;
	cache)
		lv_tree_devices_ "$1" "$(lv_devices "$lv")"
		;;
	cache-pool)
		lv_tree_devices_ "$1" "$(lv_field_lv_ "$lv" data_lv)"
		lv_tree_devices_ "$1" "$(lv_field_lv_ "$lv" metadata_lv)"
		;;
	esac
}

lv_tree_devices() {
	lv_tree_devices_ "$@" | sort -u
}

first_extent_sector() {
	pv_field "$@" pe_start --units s --nosuffix
}

# Print the io engines this build can create, probing each candidate with
# a verbose pvs run on device $1, in the order given.  Sync needs no
# engine setup, so it is always printed first; a requested engine that is
# missing makes pvs fall back to it, hence the "Created ..." check.  With
# no candidates given, every known engine is probed, so a new engine needs
# no edit at the callers.
io_engine() {
	local dev=$1
	local engine

	shift
	if [ $# -eq 0 ]; then
		set -- libaio threads iouring
	fi
	echo sync
	for engine in "$@"; do
		rm -f debug.log* io_engine.log
		pvs -vvv --config "global/io_engine=\"$engine\"" "$dev" \
			>io_engine.log 2>&1 || true
		if grep -q "Created.*$engine" io_engine.log debug.log* 2>/dev/null; then
			echo "$engine"
		fi
	done

	rm -f io_engine.log
	return 0
}

# Count the reads syscall $1 submitted in strace output file $2.
#
# io_uring batches the queued reads: one enter submits the initial scan,
# another submits the vg_read rescan.  A blocking wait may add an enter
# with zero submissions, so sum the SQEs submitted rather than counting
# syscalls.  When a batch size $3 is given, require the initial scan to
# be submitted in one enter of that size.
#
# The worker threads read with pread(), and -f is needed to trace them.
# Count only the label scan reads on the scanned devices, which -P
# selects, so the count does not depend on the read size.  Old strace
# names the 64-bit syscall "pread" and new strace names it "pread64", so
# accept either.  With -f, strace may split a call into an unfinished
# line and a resumed line.  The call starts once, on the line with the
# opening parenthesis, so count that line only and not the resumed line
# that completes the same call.
io_count() {
	local file=$2
	local batch=${3:-0}

	case "$1" in
	submit)
		awk '/^io_submit\(/ { n++ } END { print n + 0 }' "$file"
		;;
	enter)
		if [ "$batch" != 0 ]; then
			grep -q "^io_uring_enter([^,]*, $batch, 0," "$file" ||
				return 1
		fi
		awk -F'[(,]' '/^io_uring_enter\(/ { s += $3 } END { print s + 0 }' "$file"
		;;
	pread)
		awk '/pread(64)?\(/ { n++ } END { print n + 0 }' "$file"
		;;
	*)
		return 1
		;;
	esac
}

#set -x
unset LVM_VALGRIND
unset LVM_LOG_FILE_EPOCH
"$@"
