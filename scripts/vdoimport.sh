#!/bin/bash -x
#
# Copyright (C) 2021 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
#
# Author: Zdenek Kabelac <zkabelac at redhat.com>
#
# Script for converting VDO volumes to lvm2 VDO LVs
#
# Needed utilities:
#   vdo, vdo2lvm, grep, awk, sed, lvm, readlink, dmsetup, mkdir
#

set -euE -o pipefail

TOOL=vdoimport

_SAVEPATH=$PATH
PATH="/sbin:/usr/sbin:/bin:/usr/sbin:$PATH"

# user may override lvm location by setting LVM_BINARY
LVM=${LVM_BINARY:-lvm}
VDO=${VDO_BINARY:-vdo}
VDOCONF=${VDOCONF:-}
READLINK="readlink"
READLINK_E="-e"
MKDIR="mkdir"

TEMPDIR="${TMPDIR:-/tmp}/${TOOL}_${RANDOM}$$"
DM_DEV_DIR="${DM_DEV_DIR:-/dev}"

DRY=0
VERB=
FORCE=



NAME="VGNAME/LVNAME"


tool_usage() {
	echo "${TOOL}: Utility to convert VDO volume to VDO LV."
	echo
	echo "	${TOOL} [options] <vdo_device_path>"
	echo
	echo "	Options:"
	echo "	  -h | --help	      Show this help message"
	echo "	  -v | --verbose      Be verbose"
	echo "	  -f | --force	      Bypass sanity checks"
	echo "	  -n | --dry-run      Print commands without running them"
	echo "	  -y | --yes	      Answer \"yes\" at any prompts"
	echo "	  --name	      Name for VG/LV of converted VDO volume"
	exit
}

verbose() {
	test -z "$VERB" || echo "$TOOL:" "$@"
}

# Support multi-line error messages
error() {
	for i in "$@" ;  do
		echo "$TOOL: $i" >&2
	done
	cleanup 1
}

dry() {
	if [ "$DRY" -ne 0 ]; then
		verbose "Dry execution" "$@"
		return 0
	fi
	verbose "Executing" "$@"
	"$@"
}

cleanup() {
	trap '' 2

	# error exit status for break
	exit "${1:-1}"
}

get_enabled_value_() {
	case "$1" in
	enabled) echo 1 ;;
	*) echo 0 ;;
	esac
}

get_kb_size_with_unit_() {
	case "$1" in
	*[kK]) echo $(( ${1%[kK]} )) ;;
	*[mM]) echo $(( ${1%[mM]} * 1024 )) ;;
	*[gG]) echo $(( ${1%[gG]} * 1024 * 1024 )) ;;
	*[tT]) echo $(( ${1%[tT]} * 1024 * 1024 * 1024 )) ;;
	*[pP]) echo $(( ${1%[pP]} * 1024 * 1024 * 1024 * 1024 )) ;;
	esac
}

get_mb_size_with_unit_() {
	case "$1" in
	*[mM]) echo $(( ${1%[mM]} )) ;;
	*[gG]) echo $(( ${1%[gG]} * 1024 )) ;;
	*[tT]) echo $(( ${1%[tT]} * 1024 * 1024 )) ;;
	*[pP]) echo $(( ${1%[pP]} * 1024 * 1024 * 1024 )) ;;
	esac
}

# Figure out largest possible extent size usable for VG
# $1   physical size
# $2   logical size
get_largest_extent_size_() {
	local max=4
	local i
	local d

	for i in 8 16 32 64 128 256 512 1024 2048 4096 ; do
		d=$(( $1 / i ))
		test $(( d * i )) -eq "$1" || break
		d=$(( $2 / i ))
		test $(( d * i )) -eq "$2" || break
		max=$i
	done
	echo $max
}

# detect LV on the given device
# dereference device name if it is symbolic link
detect_lv_() {
	local DEVICE=$1
	local MAJOR
	local MINOR
	local SYSVOLUME
	local MAJORMINOR

	DEVICE=${1/#"${DM_DEV_DIR}/"/}
	DEVICE=$("$READLINK" $READLINK_E "$DM_DEV_DIR/$DEVICE")
	test -n "$DEVICE" || error "Cannot get readlink \"$1\"."
	RDEVICE=$DEVICE
	case "$RDEVICE" in
	  # hardcoded /dev  since udev does not create these entries elsewhere
	  /dev/dm-[0-9]*)
		read -r <"/sys/block/${RDEVICE#/dev/}/dm/name" SYSVOLUME 2>&1 && DEVICE="$DM_DEV_DIR/mapper/$SYSVOLUME"
		read -r <"/sys/block/${RDEVICE#/dev/}/dev" MAJORMINOR 2>&1 || error "Cannot get major:minor for \"$DEVICE\"."
		MAJOR=${MAJORMINOR%%:*}
		MINOR=${MAJORMINOR##*:}
		;;
	  *)
		STAT=$(stat --format "MAJOR=\$((0x%t)) MINOR=\$((0x%T))" "$RDEVICE")
		test -n "$STAT" || error "Cannot get major:minor for \"$DEVICE\"."
		eval "$STAT"
		;;
	esac

	eval "$(dmsetup info -c -j "$MAJOR" -m "$MINOR" -o uuid,name --noheadings --nameprefixes --separator ' ')"
}

parse_yaml2_() {
	local prefix="$2"
	local s
	local w
	local fs
	s='[[:space:]]*'
	w='[a-zA-Z0-9_]*'
	fs="$(echo @|tr @ '\034')"
	sed -ne "s|^\($s\)\($w\)$s:$s\"\(.*\)\"$s\$|\1$fs\2$fs\3|p" \
	    -e "s|^\($s\)\($w\)$s[:-]$s\(.*\)$s\$|\1$fs\2$fs\3|p" "$1" |
	awk -F"$fs" '{
	indent = length($1)/2;
	vname[indent] = $2;
	for (i in vname) {if (i > indent) {delete vname[i]}}
	    if (length($3) > 0) {
		vn=""; for (i=0; i<indent; i++) {vn=(vn)(vname[i])("_")}
		printf("%s%s%s=(\"%s\")\n", "'"$prefix"'",vn, $2, $3);
	    }
	}' | sed 's/_=/+=/g'
}

parse_yaml_() {
	local yaml_file=$1
	local prefix=$2
	local s
	local w
	local fs

	s='[[:space:]]*'
	w='[a-zA-Z0-9_.-]*'
	fs="$(echo @|tr @ '\034')"

	(
	    sed -e '/- [^\]'"[^\']"'.*: /s|\([ ]*\)- \([[:space:]]*\)|\1-\'$'\n''  \1\2|g' |

	    sed -ne '/^--/s|--||g; s|\"|\\\"|g; s/[[:space:]]*$//g;' \
		-e 's/\$/\\\$/g' \
		-e "/#.*[\"\']/!s| #.*||g; /^#/s|#.*||g;" \
		-e "s|^\($s\)\($w\)$s:$s\"\(.*\)\"$s\$|\1$fs\2$fs\3|p" \
		-e "s|^\($s\)\($w\)${s}[:-]$s\(.*\)$s\$|\1$fs\2$fs\3|p" |

	    awk -F"$fs" '{
		indent = length($1)/2;
		if (length($2) == 0) { conj[indent]="+";} else {conj[indent]="";}
		vname[indent] = $2;
		for (i in vname) {if (i > indent) {delete vname[i]}}
		    if (length($3) > 0) {
			vn=""; for (i=0; i<indent; i++) {vn=(vn)(vname[i])("_")}
			printf("%s%s%s%s=(\"%s\")\n", "'"$prefix"'",vn, $2, conj[indent-1], $3);
		    }
		}' |

	    sed -e 's/_=/+=/g' |

	    awk 'BEGIN {
		    FS="=";
		    OFS="="
		}
		/(-|\.).*=/ {
		    gsub("-|\\.", "_", $1)
		}
		{ print }'
	) < "$yaml_file"
}

read_yaml_() {
	local parse=0

	"$VDO" printConfigFile $VDOCONF >cfg

	while IFS= read -r line; do
	echo "$line"
		case "$line" in
		"*:*VDOService*")
			case "$line" in
			"*$1:*") parse=1 ;;
			*) parse=0 ;;
			esac
			;;
		esac
		if test "$parse" = yes ; then
			case "$line" in
			bioThreads*) ;;
			blockMapCacheSize*) ;;
			deduplication*)  echo "DEDUP $line" ;;
			esac
		fi
	done < cfg
}

convert2lvm_() {
	local DEVICE=$1
	local VGNAME=${NAME%/*}
	local LVNAME=${NAME#*/}
	local VDONAME
	local TRVDONAME
	local EXTENTSZ
	local IS_LV=1

	detect_lv_ "$DEVICE"
	case "$DM_UUID" in
		LVM-*)	eval "$(dmsetup splitname --nameprefixes --noheadings --separator ' ' "$DM_NAME")"
			if [ -z "$VGNAME" ] || [ "$VGNAME" = "$LVNAME" ]  ; then
				VGNAME=$DM_VG_NAME
			elif test "$VGNAME" != "$DM_VG_NAME" ; then
				error "Volume group name \"$VGNAME\" does not match name \"$DM_VG_NAME\" for device \"$DEVICE\"."
			fi
			;;
		*) IS_LV=0 ;;
	esac

	verbose "Checked whether device $1 is already LV ($IS_LV)."

	"$MKDIR" -p -m 0000 "$TEMPDIR" || error "Failed to create $TEMPDIR."

	verbose "Getting YAML VDO configuration."
	"$VDO" printConfigFile $VDOCONF >"$TEMPDIR/vdoconf.yml"

	VDONAME=$(awk -v DNAME="$DEVICE" '/.*VDOService$/ {VNAME=substr($1, 0, length($1) - 1)} /[[:space:]]*device:/ { if ($2 ~ DNAME) {print VNAME}}' "$TEMPDIR/vdoconf.yml")
	TRVDONAME=$(echo "$VDONAME" | tr '-' '_')
	eval "$(parse_yaml_ "$TEMPDIR/vdoconf.yml" vdo | grep "$TRVDONAME" | sed -e "s/vdoconfig_vdos_$TRVDONAME/vdo/g")"

	vdo_logicalSize=$(get_kb_size_with_unit_ "$vdo_logicalSize")
	vdo_physicalSize=$(get_kb_size_with_unit_ "$vdo_physicalSize")

	verbose "Going to convert physical sized VDO device $vdo_physicalSize KiB."
	verbose "With logical volume of size $vdo_logicalSize KiB."

	PARAMS=$(cat <<EOF
allocation {
	vdo_use_compression = $(get_enabled_value_ "$vdo_compression")
	vdo_use_deduplication = $(get_enabled_value_ "$vdo_deduplication")
	vdo_use_metadata_hints=1
	vdo_minimum_io_size = $vdo_logicalBlockSize
	vdo_block_map_cache_size_mb = $(get_mb_size_with_unit_ "$vdo_blockMapCacheSize")
	vdo_block_map_period = $vdo_blockMapPeriod
	vdo_check_point_frequency = $vdo_indexCfreq
	vdo_use_sparse_index = $(get_enabled_value_ "$vdo_indexSparse")
	vdo_index_memory_size_mb = $(awk "BEGIN {print $vdo_indexMemory * 1024}")
	vdo_slab_size_mb = $(get_mb_size_with_unit_ "$vdo_blockMapCacheSize")
	vdo_ack_threads = $vdo_ackThreads
	vdo_bio_threads = $vdo_bioThreads
	vdo_bio_rotation = $vdo_bioRotationInterval
	vdo_cpu_threads = $vdo_cpuThreads
	vdo_hash_zone_threads = $vdo_hashZoneThreads
	vdo_logical_threads = $vdo_logicalThreads
	vdo_physical_threads = $vdo_physicalThreads
	vdo_write_policy = $vdo_writePolicy
	vdo_max_discard = $(( $(get_kb_size_with_unit_ "$vdo_maxDiscardSize") * 1024 ))
	vdo_pool_header_size = 0
}
EOF
)
	#echo "$PARAMS"

	dry "$VDO" stop $VDOCONF --name "$VDONAME"

	if [ "$IS_LV" = "0" ]; then
		verbose "Moving VDO header by 2MiB."
		dry "$VDO" convert $VDOCONF --force --name "$VDONAME"

		dry "$LVM" pvcreate --dataalignment 2M "$DEVICE"

		# Figure free space in this PV
		# after 'vdo2lvm' call there is +2M free space at header of device
		pvfree=$("$LVM" pvs -o devsize --units b --nosuffix --noheadings "$DEVICE")
		pvfree=$(( pvfree / 1024 - 2048 ))  # to KiB
	else
		pvfree=$("$LVM" lvs -o size --units b --nosuffix --noheadings "$VGNAME/$LVNAME")
	fi

	# select largest possible extent size that can exactly express both sizes
	EXTENTSZ=$(get_largest_extent_size_ "$pvfree" "$vdo_logicalSize")

	if [ "$IS_LV" = "0" ]; then
		verbose "Creating VG \"${NAME%/*}\" with extent size $EXTENTSZ KiB."
		dry "$LVM" vgcreate -s "${EXTENTSZ}k" "$VGNAME" "$DEVICE"

		verbose "Creating VDO data LV using whole VG."
		dry "$LVM" lvcreate -Zn -Wn -l100%VG -n "${LVNAME}_vpool" "$VGNAME"
	else
		vg_extent_size=$("$LVM" vgs -o vg_extent_size --units b --nosuffix --noheadings "$VGNAME")
		vg_extent_size=$(( vg_extent_size / 1024 ))

		test "$vg_extent_size" -le "$EXTENTSZ" || error "Please vgchange extent_size to at most $EXTENTSZ KiB."
		verbose "Renaming existing LV to be used as _vdata volume for VDO pool LV."
		dry "$LVM" lvrename "$VGNAME/$LVNAME" "$VGNAME/${LVNAME}_vpool"
	fi

	verbose "Converting to VDO pool."
	dry "$LVM" lvconvert --config "$PARAMS" -y -Zn -V "${vdo_logicalSize}k" -n "$LVNAME" --type vdo-pool "$VGNAME/${LVNAME}_vpool"

	rm -fr "$TEMPDIR"
}

#############################
# start point of this script
# - parsing parameters
#############################
trap "cleanup 2" 2

if [ "$#" -eq 0 ] ; then
	tool_usage
fi

while [ "$#" -ne 0 ]
do
	 case "$1" in
	  "") ;;
	  "-h"|"--help") tool_usage ;;
	  "-v"|"--verbose") VERB="-v" ;;
	  "-n"|"--dry-run") DRY=1 ;;
	  "-f"|"--force") FORCE="-f" ;;
	  "-y"|"--yes") YES="-y" ;;
	  "--name") shift; NAME=$1 ;;
	  "-*") error "Wrong argument \"$1\". (see: $TOOL --help)" ;;
	  *) DEVICENAME=$1 ;;
	esac
	shift
done

# do conversion
convert2lvm_ "$DEVICENAME"
