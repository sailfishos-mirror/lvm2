#!/usr/bin/env bash
# Copyright (C) 2011-2025 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# get.sh: get various values from volumes
#
# USAGE:
#  get pv_field PV field [pvs params]
#  get vg_field VG field [vgs params]
#  get lv_field LV field [lvs params]
#
#  get lv_devices LV     [lvs params]

# trims only leading prefix and suffix
__get__trim_() {
	local var=${1%"${1##*[! ]}"}  # remove trailing space characters
	echo "${var#"${var%%[! ]*}"}" # remove leading space characters
}

__get__pv_field() {
	local r
	r=$(pvs --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	__get__trim_ "$r"
}

__get__vg_field() {
	local r
	r=$(vgs --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	__get__trim_ "$r"
}

__get__lv_field() {
	local r
	r=$(lvs --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	__get__trim_ "$r"
}

__get__lv_first_seg_field() {
	local r
	read -r r < <(lvs --config 'log{prefix=""}' --unbuffered --noheadings -o "$2" "${@:3}" "$1")
	__get__trim_ "$r"
}

__get__lvh_field() {
	local r
	r=$(lvs -H --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	__get__trim_ "$r"
}

__get__lva_field() {
	local r
	r=$(lvs -a --config 'log{prefix=""}' --noheadings -o "$2" "${@:3}" "$1")
	__get__trim_ "$r"
}

__get__lv_devices() {
	__get__lv_field "$1" devices -a "${@:2}" | sed 's/([^)]*)//g; s/,/\n/g'
}

__get__lv_field_lv_() {
	__get__lv_field "$1" "$2" -a --unbuffered | tr -d '[]'
}

__get__lv_tree_devices_() {
	local lv="$1/$2"
	local type
	type=$(__get__lv_first_seg_field "$lv" segtype -a)
	#local orig
	#orig=$(__get__lv_field_lv_ "$lv" origin)
	# FIXME: should we count in also origins ?
	#test -z "$orig" || __get__lv_tree_devices_ $1 $orig
	case "$type" in
	linear|striped)
		__get__lv_devices "$lv"
		;;
	mirror|raid*)
		local log
		log=$(__get__lv_field_lv_ "$lv" mirror_log)
		test -z "$log" || __get__lv_tree_devices_ "$1" "$log"
		for i in $(__get__lv_devices "$lv"); do
			__get__lv_tree_devices_ "$1" "$i"
		done
		;;
	thin)
		__get__lv_tree_devices_ "$1" "$(__get__lv_field_lv_ "$lv" pool_lv)"
		;;
	thin-pool)
		__get__lv_tree_devices_ "$1" "$(__get__lv_field_lv_ "$lv" data_lv)"
		__get__lv_tree_devices_ "$1" "$(__get__lv_field_lv_ "$lv" metadata_lv)"
		;;
	cache)
		__get__lv_tree_devices_ "$1" "$(__get__lv_devices "$lv")"
		;;
	cache-pool)
		__get__lv_tree_devices_ "$1" "$(__get__lv_field_lv_ "$lv" data_lv)"
		__get__lv_tree_devices_ "$1" "$(__get__lv_field_lv_ "$lv" metadata_lv)"
		;;
	esac
}

__get__lv_tree_devices() {
	__get__lv_tree_devices_ "$@" | sort -u
}

__get__first_extent_sector() {
	__get__pv_field "$@" pe_start --units s --nosuffix
}
