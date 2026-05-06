#!/bin/bash
# shellcheck disable=SC2329 # functions invoked indirectly via $CMD
#
# Copyright (C) 2007-2026 Red Hat, Inc. All rights reserved.
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
# Script for resizing devices (usable for LVM resize)
#
# Needed utilities:
#   mount, umount, mktemp, awk, blockdev, blkid, fsck, readlink, stat
#
# ext2/ext3/ext4: resize2fs, tune2fs
# reiserfs: resize_reiserfs, reiserfstune
# xfs: xfs_growfs, xfs_info, xfs_repair
# crypto_LUKS: cryptsetup
#
# Return values:
#   0 success
#   1 error
#   2 break detected
#   3 unsupported online filesystem check for given mounted fs

set -euE -o pipefail

TOOL="fsadm"

PATH="/sbin:/usr/sbin:/bin:/usr/bin"

tool_usage() {
	cat <<-EOF
	  ${TOOL}: Utility to resize or check the filesystem on a device

	  ${TOOL} [options] check <device>
	      - Check the filesystem on device using fsck

	  ${TOOL} [options] resize <device> [<new_size>[BKMGTPE]]
	      - Change the size of the filesystem on device to new_size

	  Options:
	      -h | --help	 Show this help message
	      -v | --verbose	 Be verbose
	      -e | --ext-offline Unmount filesystem before ext2/ext3/ext4 resize
	      -f | --force	 Bypass sanity checks
	      -n | --dry-run	 Print commands without running them
	      -l | --lvresize	 Resize given device (if it is LVM device)
	      -c | --cryptresize Resize given crypt device
	      -y | --yes	 Answer "yes" at any prompts

	  new_size - Absolute number of filesystem blocks to be in the filesystem,
	             or an absolute size using a suffix (in powers of 1024).
	             If new_size is not supplied, the whole device is used.

	EOF

	exit 0
}

# utilities
TUNE_EXT="tune2fs"
RESIZE_EXT="resize2fs"
TUNE_REISER="reiserfstune"
RESIZE_REISER="resize_reiserfs"
TUNE_XFS="xfs_info"
RESIZE_XFS="xfs_growfs"
XFS_CHECK="xfs_check"
# XFS_REPAIR -n is used when XFS_CHECK is not found
XFS_REPAIR="xfs_repair"
FSCK="fsck"
CRYPTSETUP="cryptsetup"
STAT="stat"

MOUNT="mount"
UMOUNT="umount"
MKDIR="mkdir"
RMDIR="rmdir"
BLOCKDEV="blockdev"
BLKID="blkid"
DATE="date"
AWK="awk"
READLINK="readlink"
READLINK_E="-e"

DMSETUP=${DMSETUP_BINARY:-dmsetup}
LVM=${LVM_BINARY:-lvm}

# _fsadm_cmd() passes --yes/--verbose/--force on command line but not --ext-offline,
# so EXTOFF must propagate via _FSADM_EXTOFF environment variable.
EXTOFF=${_FSADM_EXTOFF:-0}
DO_LVRESIZE=0
DO_CRYPTRESIZE=0
DRY=0
ACTION=
DEVICE=
FORCE=
NEWSIZE=
VERB=
YES=
TEMPDIR=
CRYPT_RESIZE=
FSTYPE="unknown"
VOLUME="unknown"
DM_DEV_DIR="${DM_DEV_DIR:-/dev}"
BLOCKSIZE=
BLOCKCOUNT=
MOUNTPOINT=
MOUNTED=
REMOUNT=
PROCDIR="/proc"
PROCMOUNTS="$PROCDIR/mounts"
PROCSELFMOUNTINFO="$PROCDIR/self/mountinfo"

verbose() {
	test -z "$VERB" || echo "$TOOL: $*"
}

warning() {
	echo "$TOOL: WARNING: $*" >&2
}

# Support multi-line error messages
error() {
	local i

	for i in "$@"; do
		echo "$TOOL: $i" >&2
	done

	exit 1
}

dry() {
	if [ "$DRY" -ne 0 ]; then
		verbose "Dry execution $*"
		return 0
	fi
	verbose "Executing $*"
	"$@"
}

# Verify required filesystem utilities are installed
validate_fs_tools() {
	case "$1" in
	  ext[234])
		if ! command -v "$TUNE_EXT" >/dev/null 2>&1 ||
		   ! command -v "$RESIZE_EXT" >/dev/null 2>&1; then
			error "Utilities $TUNE_EXT and $RESIZE_EXT required for $1." \
			      "Please install e2fsprogs package."
		fi ;;
	  reiserfs)
		if ! command -v "$TUNE_REISER" >/dev/null 2>&1 ||
		   ! command -v "$RESIZE_REISER" >/dev/null 2>&1; then
			error "Utilities $TUNE_REISER and $RESIZE_REISER required for $1." \
			      "Please install reiserfsprogs package."
		fi ;;
	  xfs)
		if ! command -v "$TUNE_XFS" >/dev/null 2>&1 ||
		   ! command -v "$RESIZE_XFS" >/dev/null 2>&1; then
			error "Utilities $TUNE_XFS and $RESIZE_XFS required for $1." \
			      "Please install xfsprogs package."
		fi ;;
	  crypto_LUKS)
		if ! command -v "$CRYPTSETUP" >/dev/null 2>&1; then
			error "$CRYPTSETUP utility required for $1." \
			      "Please install cryptsetup package."
		fi ;;
	esac
}

# Handle fsck return codes according to fsck(8) exit code specification
# Exit codes are bitmask sums: 1 = corrected, 2 = reboot required,
# 4 = errors uncorrected, 8 = operational error,
# 16 = usage error, 32 = canceled, 128 = shared library error
accept_fsck() {
	local RET=0

	"$@" || RET=$?

	test "$RET" -eq 0 && return 0

	test $(( RET & 128 )) -ne 0 &&
		error "Fsck shared library error on \"$VOLUME\"."
	test $(( RET & 32 )) -ne 0 &&
		error "Fsck canceled by user request."
	test $(( RET & 16 )) -ne 0 &&
		error "Fsck usage or syntax error." \
		      "This may indicate a bug in $TOOL script."
	test $(( RET & 8 )) -ne 0 &&
		error "Fsck operational error on \"$VOLUME\"." \
		      "Check fsck command syntax or system resources."
	test $(( RET & 4 )) -ne 0 &&
		error "Filesystem errors left uncorrected on \"$VOLUME\"." \
		      "Manual intervention may be required."
	test $(( RET & 2 )) -ne 0 &&
		warning "Filesystem was corrected but system should be rebooted."
	test $(( RET & 1 )) -ne 0 &&
		verbose "Filesystem errors were corrected on \"$VOLUME\"."

	# 191 = 1|2|4|8|16|32|128 (all defined fsck exit code bits)
	test $(( RET & ~191 )) -ne 0 &&
		error "Fsck failed with unexpected return code $RET on \"$VOLUME\"."

	return 0
}

cleanup() {
	trap '' EXIT HUP INT QUIT ABRT TERM

	# reset MOUNTPOINT - avoid recursion
	if test -n "$TEMPDIR" && test "$MOUNTPOINT" = "$TEMPDIR"; then
		MOUNTPOINT=
		temp_umount
		TEMPDIR=
	fi
	# remove mktemp dir if it was created but not fully used
	if test -n "$TEMPDIR"; then
		"$RMDIR" "$TEMPDIR" 2>/dev/null || warning "Failed to remove \"$TEMPDIR\"."
		"$RMDIR" "${TEMPDIR%/*}" 2>/dev/null || warning "Failed to remove \"${TEMPDIR%/*}\"."
		TEMPDIR=
	fi
	if test -n "$REMOUNT"; then
		verbose "Remounting unmounted filesystem back."
		dry "$MOUNT" "$VOLUME" "$MOUNTED" ||
			warning "Failed to remount \"$VOLUME\" on \"$MOUNTED\"."
	fi
	trap - EXIT HUP INT QUIT ABRT TERM

	test "$1" -eq 2 && verbose "Break detected."

	if [ "$DO_LVRESIZE" -eq 2 ]; then
		# start LVRESIZE with the filesystem modification flag
		# and allow recursive call of fsadm
		_FSADM_EXTOFF=$EXTOFF
		export _FSADM_EXTOFF
		unset FSADM_RUNNING
		dry exec "$LVM" lvresize ${VERB:+"$VERB"} ${FORCE:+"$FORCE"} ${YES:+"$YES"} --fs resize_fsadm -L"${NEWSIZE_ORIG}b" "$VOLUME_ORIG"
	fi

	# error exit status for break
	exit "${1:-1}"
}

# convert parameter from Bytes/Kilo/Mega/Giga/Tera/Peta/Exa and blocks
# (2^(0/10/20/30/40/50/60))
# Sets: NEWSIZE, NEWBLOCKCOUNT
decode_size() {
	local NUM=${1%[bBkKmMgGtTpPeE]}

	case "$NUM" in
	  *[!0-9]*|"") error "Invalid size value \"$1\"." ;;
	esac

	local SCALE
	case "$1" in
	  *[bB]) SCALE=1 ;;
	  *[kK]) SCALE=$(( 1 << 10 )) ;;
	  *[mM]) SCALE=$(( 1 << 20 )) ;;
	  *[gG]) SCALE=$(( 1 << 30 )) ;;
	  *[tT]) SCALE=$(( 1 << 40 )) ;;
	  *[pP]) SCALE=$(( 1 << 50 )) ;;
	  *[eE]) SCALE=$(( 1 << 60 )) ;;
	      *) SCALE=$2 ;;
	esac
	NEWSIZE=$(( NUM * SCALE ))
	test "$NEWSIZE" -gt 0 || error "Size value overflow."
	#NEWBLOCKCOUNT=$(round_block_size $NEWSIZE $2)
	NEWBLOCKCOUNT=$(( NEWSIZE / $2 ))

	if [ "$DO_LVRESIZE" -eq 1 ]; then
		# start lvresize, but first cleanup mounted dirs
		DO_LVRESIZE=2
		exit 0
	fi
}

# Sets: MAJOR, MINOR from a device node path
# Handles /dev/dm-* via sysfs, other devices via stat
detect_major_minor() {
	local STATOUT

	case "$1" in
	  /dev/dm-[0-9]*)
		IFS=: read -r MAJOR MINOR <"/sys/block/${1#/dev/}/dev" 2>/dev/null ||
			return 1
		;;
	  *)
		STATOUT=$("$STAT" --format '0x%t:0x%T' "$1") || return 1
		MAJOR=$(( ${STATOUT%%:*} ))
		MINOR=$(( ${STATOUT#*:} ))
		;;
	esac
}

# Outputs: MAJOR:MINOR from a dev_t value
decode_major_minor() {
	# 0x00000fff00  mask MAJOR
	# 0xfffff000ff  mask MINOR

	#MINOR=$(( $1 / 1048576 ))
	#MAJOR=$(( ($1 - ${MINOR} * 1048576) / 256 ))
	#MINOR=$(( $1 - ${MINOR} * 1048576 - ${MAJOR} * 256 + ${MINOR} * 256))

	echo "$(( ( $1 >> 8 ) & 4095 )):$(( ( ( $1 >> 12 ) & 268435200 ) | ( $1 & 255 ) ))"
}

# detect filesystem type on the given device and validate required tools
# not using blkid option '-o value' to be compatible with older version
# Sets: FSTYPE
detect_fstype() {
	FSTYPE=$("$BLKID" -c /dev/null -s TYPE "$1" || true)
	test -n "$FSTYPE" || error "Cannot get filesystem type of \"$1\"."
	FSTYPE=${FSTYPE##*TYPE=\"} # cut quotation marks
	FSTYPE=${FSTYPE%%\"*}
	verbose "\"$FSTYPE\" filesystem found on \"$1\"."
	validate_fs_tools "$FSTYPE"
}

# detect filesystem on the given device
# dereference device name if it is symbolic link
# Sets: VOLUME, VOLUME_ORIG, RVOLUME, FSTYPE, MAJOR, MINOR, MAJORMINOR
detect_fs() {
	local SYSVOLUME

	test -n "${VOLUME_ORIG-}" || VOLUME_ORIG=$1
	case "$1" in
	  "${DM_DEV_DIR}/"*) VOLUME=$1 ;;
	  *) VOLUME="$DM_DEV_DIR/$1" ;;
	esac
	VOLUME=$("$READLINK" "$READLINK_E" "$VOLUME" 2>/dev/null || true)
	test -n "$VOLUME" || error "Cannot get readlink \"$1\"."
	RVOLUME=$VOLUME
	case "$RVOLUME" in
	  # hardcoded /dev  since udev does not create these entries elsewhere
	  /dev/dm-[0-9]*)
		read -r SYSVOLUME <"/sys/block/${RVOLUME#/dev/}/dm/name" 2>/dev/null &&
			VOLUME="$DM_DEV_DIR/mapper/$SYSVOLUME"
		;;
	esac
	detect_major_minor "$RVOLUME" ||
		error "Cannot get major:minor for \"$VOLUME\"."
	MAJORMINOR="$MAJOR:$MINOR"
	detect_fstype "$VOLUME"
}

# Check that passed mounted MAJOR:MINOR is not matching $MAJOR:MINOR of resized $VOLUME
validate_mounted_major_minor() {
	if [ "$1" != "$MAJORMINOR" ]; then
		local REFNAME
		local CURNAME
		REFNAME=$("$DMSETUP" info -c -j "${1%%:*}" -m "${1##*:}" -o name --noheadings 2>/dev/null)
		CURNAME=$("$DMSETUP" info -c -j "$MAJOR" -m "$MINOR" -o name --noheadings 2>/dev/null)
		error "Cannot $ACTION device \"$VOLUME\" without umounting filesystem \"$MOUNTED\" first." \
		      "Mounted filesystem is using device \"$REFNAME\", but referenced device is \"$CURNAME\"." \
		      "Filesystem utilities currently do not support renamed devices."
	fi
}

# ATM fsresize & fsck tools are not able to work properly
# when mounted device has changed its name.
# So whenever such device no longer exists with original name
# abort further command processing
check_valid_mounted_device() {
	local VOL
	local CURNAME
	local MOUNTEDMAJORMINOR

	VOL=$("$READLINK" "$READLINK_E" "$1" 2>/dev/null || true)
	CURNAME=$("$DMSETUP" info -c -j "$MAJOR" -m "$MINOR" -o name --noheadings 2>/dev/null || true)
	local SUGGEST="Possibly device \"$1\" has been renamed to \"$CURNAME\"?"
	test -n "$CURNAME" || SUGGEST="Mounted volume is not a device mapper device."

	test -n "$VOL" ||
		error "Cannot access device \"$1\" referenced by mounted filesystem \"$MOUNTED\"." \
		"$SUGGEST" \
		"Filesystem utilities currently do not support renamed devices."

	detect_major_minor "$VOL" ||
		error "Cannot get major:minor for \"$VOL\" mounted on \"$MOUNTED\"."
	MOUNTEDMAJORMINOR="$MAJOR:$MINOR"

	validate_mounted_major_minor "$MOUNTEDMAJORMINOR"
}

# Sets: MOUNTED
detect_mounted_with_proc_self_mountinfo() {
	local MOUNTDEV
	local RAW

	# shellcheck disable=SC2016 # awk field refs, not shell vars
	RAW=$("$AWK" -v mm="$MAJORMINOR" '$3 == mm {print; exit}' "$PROCSELFMOUNTINFO" 2>/dev/null)

	# If not found in self mountinfo but device is open,
	# scan all /proc/*/mountinfo (handles cgroup mounts)
	if test -z "$RAW" &&
	   test "$("$DMSETUP" info -c --noheading -o open -j "$MAJOR" -m "$MINOR" 2>/dev/null)" -gt 0 2>/dev/null; then
		# shellcheck disable=SC2016 # awk field refs, not shell vars
		RAW=$(find "$PROCDIR" -maxdepth 2 -name mountinfo -print0 |
			xargs -0 "$AWK" -v mm="$MAJORMINOR" '$3 == mm {print; exit}' 2>/dev/null |
			head -1 2>/dev/null)
	fi

	# extract 5th field as mount point (printf %b decodes \040 etc.)
	MOUNTED=$(echo "$RAW" | cut -d ' ' -f 5)
	MOUNTED=$(printf '%b' "$MOUNTED")

	test -n "$MOUNTED" || return 1

	# extract 2nd field after ' - ' separator as mounted device
	MOUNTDEV=$(echo "${RAW##* - }" | cut -d ' ' -f 2)
	MOUNTDEV=$(printf '%b' "$MOUNTDEV")
	check_valid_mounted_device "$MOUNTDEV"
}

# With older systems without /proc/*/mountinfo we may need to check
# every mount point as cannot easily depend on the name of mounted
# device (which could have been renamed).
# We need to visit every mount point and check it's major minor
# Sets: MOUNTED
detect_mounted_with_proc_mounts() {
	local MOUNTDEV
	local STATOUT
	local i

	# Strategy 1: match by device name in /proc/mounts
	# shellcheck disable=SC2016 # awk field refs, not shell vars
	MOUNTED=$("$AWK" -v vol="$VOLUME" -v rvol="$RVOLUME" \
		'$1 == vol || $1 == rvol {print; exit}' "$PROCMOUNTS")

	# cut device name prefix and trim everything past mountpoint
	# printf translates \040 to spaces
	# /proc/mounts format: device mountpoint fstype options ...
	MOUNTDEV=$(printf '%b' "${MOUNTED%% *}")
	MOUNTED=${MOUNTED#* }
	MOUNTED=$(printf '%b' "${MOUNTED%% *}")

	# Strategy 2: fall back to mount command output
	if test -z "$MOUNTED"; then
		# will not work with spaces in paths
		# shellcheck disable=SC2016 # awk field refs, not shell vars
		MOUNTED=$(LC_ALL=C "$MOUNT" | "$AWK" -v vol="$VOLUME" -v rvol="$RVOLUME" \
						'$1 == vol || $1 == rvol {print; exit}')
		# mount format: device on mountpoint type fstype ...
		MOUNTDEV=${MOUNTED%% on *}
		MOUNTED=${MOUNTED##* on }
		MOUNTED=${MOUNTED% type *} # allow type in the mount name
	fi

	if test -n "$MOUNTED"; then
		check_valid_mounted_device "$MOUNTDEV"
		return 0  # mounted
	fi

	# Strategy 3: device is open but not found by name,
	# check every mount point against MAJOR:MINOR
	if test "$("$DMSETUP" info -c --noheading -o open -j "$MAJOR" -m "$MINOR" 2>/dev/null)" -gt 0 2>/dev/null; then
		while read -r i; do
			MOUNTDEV=$(printf '%b' "${i%% *}")
			MOUNTED=${i#* }
			MOUNTED=$(printf '%b' "${MOUNTED%% *}")
			STATOUT=$("$STAT" --format "%d" "$MOUNTED" 2>/dev/null) || continue
			if test "$(decode_major_minor "$STATOUT")" = "$MAJORMINOR"; then
				check_valid_mounted_device "$MOUNTDEV"
				return 0
			fi
		done < "$PROCMOUNTS"
	fi

	return 1  # nothing is mounted
}

# check if the given device is already mounted and where
# Sets: MOUNTED (mountpoint path or empty)
# FIXME: resolve swap usage and device stacking
detect_mounted() {
	if test -e "$PROCSELFMOUNTINFO"; then
		detect_mounted_with_proc_self_mountinfo
	elif test -e "$PROCMOUNTS"; then
		detect_mounted_with_proc_mounts
	else
		error "Cannot detect mounted device \"$VOLUME\"."
	fi
}

# get the full size of device in bytes
# Sets: DEVSIZE
detect_device_size() {
	local SSSIZE

	# check if blockdev supports getsize64
	DEVSIZE=$("$BLOCKDEV" --getsize64 "$VOLUME" 2>/dev/null || true)
	if test -z "$DEVSIZE"; then
		DEVSIZE=$("$BLOCKDEV" --getsize "$VOLUME" || true)
		test -n "$DEVSIZE" || error "Cannot read size of device \"$VOLUME\"."
		SSSIZE=$("$BLOCKDEV" --getss "$VOLUME" || true)
		test -n "$SSSIZE" || error "Cannot read sector size of device \"$VOLUME\"."
		DEVSIZE=$(( DEVSIZE * SSSIZE ))
	fi
}

# round up $1 / $2
# could be needed to guarantee 'at least given size'
# but it makes many troubles
round_up_block_size() {
	echo "$(( ( $1 + $2 - 1 ) / $2 ))"
}

# Sets: TEMPDIR
temp_mount() {
	if test -n "${TMPDIR-}" && test "${TMPDIR#/}" = "$TMPDIR"; then
		error "TMPDIR must be an absolute path."
	fi
	TEMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/${TOOL}_XXXXXXXXXX") || error "Failed to create temporary directory."
	TEMPDIR="${TEMPDIR}/m"
	dry "$MKDIR" -m 0000 "$TEMPDIR" || error "Failed to create temporary mount point \"$TEMPDIR\"."
	dry "$MOUNT" "$VOLUME" "$TEMPDIR" || error "Failed to mount \"$VOLUME\" on \"$TEMPDIR\"."
}

temp_umount() {
	dry "$UMOUNT" "$TEMPDIR" || error "Failed to umount \"$TEMPDIR\"."
	dry "$RMDIR" "${TEMPDIR}" || error "Failed to remove \"$TEMPDIR\"."
	dry "$RMDIR" "${TEMPDIR%/*}" || error "Failed to remove \"${TEMPDIR%/*}\"."
}

yes_no() {
	local ANS

	echo -n "$@" "? [Y|n] "

	[ -n "$YES" ] && { echo y; return 0; }

	while read -r -s -n 1 ANS; do
		case "$ANS" in
		  y|Y) echo y; return 0 ;;
		  n|N) break ;;
		  "")  [ -t 0 ] && { echo y; return 0; } ;;
		esac
	done

	echo n
	return 1
}

try_umount() {
	yes_no "Do you want to unmount \"$MOUNTED\"" ||
		error "Cannot proceed with mounted filesystem \"$MOUNTED\"."
	dry "$UMOUNT" "$MOUNTED"
}

validate_parsing() {
	if test -z "$BLOCKSIZE" || test -z "$BLOCKCOUNT"; then
		error "Cannot parse $1 output for \"$VOLUME\"."
	fi
}
####################################
# Resize ext2/ext3/ext4 filesystem
# - unmounted or mounted for upsize
# - unmounted for downsize
####################################
resize_ext() {
	local IS_MOUNTED=0
	local FLAG
	local i

	detect_mounted && IS_MOUNTED=1

	verbose "Parsing $TUNE_EXT -l \"$VOLUME\"."
	while read -r i; do
		case "$i" in
		  "Block size"*) BLOCKSIZE=${i##*  } ;;
		  "Block count"*) BLOCKCOUNT=${i##*  } ;;
		esac
	done <<-EOF
		$(LC_ALL=C "$TUNE_EXT" -l "$VOLUME")
	EOF
	validate_parsing "$TUNE_EXT"
	decode_size "$1" "$BLOCKSIZE"
	if test "$NEWBLOCKCOUNT" -lt "$BLOCKCOUNT" || test "$EXTOFF" -eq 1; then
		if test "$IS_MOUNTED" -eq 1; then
			verbose "\"$FSTYPE\" resizes only unmounted filesystem."
			try_umount
		fi
		REMOUNT=$MOUNTED
		if test -n "$MOUNTED"; then
			# Forced fsck -f for unmounted extX filesystem.
			case "$-" in
			*i*) FLAG=$YES ;;
			*)   FLAG="-p" ;;
			esac
			accept_fsck dry "$FSCK" -f ${FLAG:+"$FLAG"} "$VOLUME"
		fi
	fi

	verbose "Resizing filesystem on device \"$VOLUME\" to $NEWSIZE bytes ($BLOCKCOUNT -> $NEWBLOCKCOUNT blocks of $BLOCKSIZE bytes)."
	dry "$RESIZE_EXT" ${FORCE:+"$FORCE"} "$VOLUME" "$NEWBLOCKCOUNT"
}

#############################
# Resize reiserfs filesystem
# - unmounted for upsize
# - unmounted for downsize
# Sets: REMOUNT
#############################
resize_reiser() {
	local i

	if detect_mounted; then
		verbose "ReiserFS resizes only unmounted filesystem."
		try_umount
	fi

	REMOUNT=$MOUNTED

	verbose "Parsing $TUNE_REISER \"$VOLUME\"."
	while read -r i; do
		case "$i" in
		  "Blocksize"*) BLOCKSIZE=${i##*: } ;;
		  "Count of blocks"*) BLOCKCOUNT=${i##*: } ;;
		esac
	done <<-EOF
		$(LC_ALL=C "$TUNE_REISER" "$VOLUME")
	EOF

	validate_parsing "$TUNE_REISER"
	decode_size "$1" "$BLOCKSIZE"
	verbose "Resizing filesystem on device \"$VOLUME\" to $NEWSIZE bytes ($BLOCKCOUNT -> $NEWBLOCKCOUNT blocks of $BLOCKSIZE bytes)."
	if [ -n "$YES" ]; then
		echo y | dry "$RESIZE_REISER" -s "$NEWSIZE" "$VOLUME"
	else
		dry "$RESIZE_REISER" -s "$NEWSIZE" "$VOLUME"
	fi
}

########################
# Resize XFS filesystem
# - mounted for upsize
# - cannot downsize
# Sets: MOUNTPOINT
########################
resize_xfs() {
	local i

	if detect_mounted; then
		MOUNTPOINT=$MOUNTED
	else
		temp_mount
		MOUNTPOINT=$TEMPDIR
	fi

	verbose "Parsing $TUNE_XFS \"$MOUNTPOINT\"."
	while read -r i; do
		case "$i" in
		  "data"*) BLOCKSIZE=${i##*bsize=}; BLOCKCOUNT=${i##*blocks=} ;;
		esac
	done <<-EOF
		$(LC_ALL=C "$TUNE_XFS" "$MOUNTPOINT")
	EOF

	BLOCKSIZE=${BLOCKSIZE%%[^0-9]*}
	BLOCKCOUNT=${BLOCKCOUNT%%[^0-9]*}
	validate_parsing "$TUNE_XFS"
	decode_size "$1" "$BLOCKSIZE"
	if [ "$NEWBLOCKCOUNT" -gt "$BLOCKCOUNT" ]; then
		verbose "Resizing XFS mounted on \"$MOUNTPOINT\" to fill device \"$VOLUME\"."
		dry "$RESIZE_XFS" "$MOUNTPOINT"
	elif [ "$NEWBLOCKCOUNT" -eq "$BLOCKCOUNT" ]; then
		verbose "XFS filesystem already has the right size."
	else
		error "XFS filesystem shrinking is unsupported." \
		      "Current size: $(( BLOCKCOUNT * BLOCKSIZE )) bytes, requested: $NEWSIZE bytes."
	fi
}

# Find active LUKS device on original volume
# 1) look for LUKS device with well-known UUID format (CRYPT-LUKS[12]-<uuid>-<dmname>)
# 2) the dm-crypt device has to be on top of original device (don't support detached LUKS headers)
# Sets: CRYPT_NAME, CRYPT_DATA_OFFSET
detect_luks_device() {
	local LUKS_VERSION=
	local LUKS_UUID=

	CRYPT_NAME=""
	CRYPT_DATA_OFFSET=""

	# shellcheck disable=SC2016 # awk field refs, not shell vars
	read -r LUKS_VERSION LUKS_UUID <<-EOF
		$("$CRYPTSETUP" luksDump "$VOLUME" 2>/dev/null |
			"$AWK" -F: '/Version:/ { gsub(/[[:space:]]/, "", $2); v = $2 }
				/UUID:/    { gsub(/[[:space:]-]/, "", $2); u = $2 }
				END        { print v, u }')
	EOF

	case "$LUKS_VERSION" in
	  1|2) ;;
	  *) error "Unsupported LUKS version \"$LUKS_VERSION\" on volume \"$VOLUME\"." ;;
	esac

	case "$LUKS_UUID" in
	  *[!0-9a-fA-F]*|"") error "Invalid LUKS UUID on volume \"$VOLUME\"." ;;
	esac
	LUKS_UUID="CRYPT-LUKS$LUKS_VERSION-${LUKS_UUID}-"

	CRYPT_NAME=$("$DMSETUP" info -c --noheadings -S "UUID=~^$LUKS_UUID&&segments=1&&devnos_used='$MAJOR:$MINOR'" -o name)
	test -n "$CRYPT_NAME" && CRYPT_DATA_OFFSET=$("$DMSETUP" table "$CRYPT_NAME" 2>/dev/null | cut -d ' ' -f 8 || true)

	# LUKS device must be active and mapped over volume where detected
	if [ -z "$CRYPT_NAME" ] || [ -z "$CRYPT_DATA_OFFSET" ]; then
		error "Cannot find active LUKS device for \"$VOLUME\"." \
		      "LUKS device must be unlocked before resizing:" \
		      "  cryptsetup luksOpen \"$VOLUME\" <name>"
	fi
	case "$CRYPT_DATA_OFFSET" in
	  *[!0-9]*) error "Invalid LUKS data offset \"$CRYPT_DATA_OFFSET\" for \"$VOLUME\"." ;;
	esac
}

######################################
# Resize active LUKS device
# - LUKS must be active for fs resize
# Sets: VOLUME, NEWSIZE, CRYPT_RESIZE, CRYPT_RESIZE_BLOCKS
######################################
resize_luks() {
	local L_NEWSIZE
	local L_NEWBLOCKCOUNT
	local NAME
	detect_luks_device

	NAME=$CRYPT_NAME

	verbose "Found active LUKS device \"$NAME\" for volume \"$VOLUME\"."

	decode_size "$1" 512

	test $(( NEWSIZE % 512 )) -eq 0 ||
		error "New size is not sector aligned."

	test $(( NEWBLOCKCOUNT - CRYPT_DATA_OFFSET )) -ge 1 ||
		error "New size is smaller than minimum ($(( (CRYPT_DATA_OFFSET + 1) * 512 )) bytes) for LUKS volume \"$VOLUME\"."

	L_NEWBLOCKCOUNT=$(( NEWBLOCKCOUNT - CRYPT_DATA_OFFSET ))
	L_NEWSIZE=$(( L_NEWBLOCKCOUNT * 512 ))

	VOLUME="$DM_DEV_DIR/mapper/$NAME"
	detect_device_size

	if [ "$DEVSIZE" -gt "$L_NEWSIZE" ]; then
		# shrink fs on LUKS device first
		resize "$DM_DEV_DIR/mapper/$NAME" "$L_NEWSIZE"b
	else
		# grow: validate inner fs tools before LUKS modification
		detect_fstype "$VOLUME"
	fi

	# resize LUKS device
	dry "$CRYPTSETUP" resize "$NAME" --size "$L_NEWBLOCKCOUNT" ||
		error "Failed to resize LUKS device \"$NAME\"." \
		      "Target size: $L_NEWSIZE bytes ($L_NEWBLOCKCOUNT sectors)."

	if [ "$DEVSIZE" -le "$L_NEWSIZE" ]; then
		# grow fs on top of LUKS device
		resize "$DM_DEV_DIR/mapper/$NAME" "$L_NEWSIZE"b
	fi
}

# Sets: CRYPT_RESIZE_BLOCKS, CRYPT_RESIZE (grow|shrink)
detect_crypt_device() {
	local CRYPT_TYPE
	local NEWSIZE # preserve global NEWSIZE

	command -v "$CRYPTSETUP" >/dev/null 2>&1 ||
		error "$CRYPTSETUP utility required to resize crypt device." \
		      "Please install cryptsetup package."

	# shellcheck disable=SC2016 # awk field refs, not shell vars
	CRYPT_TYPE=$("$CRYPTSETUP" status "$1" 2>/dev/null | "$AWK" '/type:/ {print $NF; exit}' || true)

	test -n "$CRYPT_TYPE" ||
		error "Failed to detect crypt device type on \"$1\"." \
		      "Device may not be active or not a valid crypt device."

	case "$CRYPT_TYPE" in
	  LUKS[12]|PLAIN)
		verbose "\"$1\" crypt device is type \"$CRYPT_TYPE\"." ;;
	  *)
		error "Unsupported crypt type \"$CRYPT_TYPE\" on device \"$1\"." \
		      "Only LUKS1, LUKS2, and PLAIN types are supported."
	esac

	decode_size "$2" 512

	test $(( NEWSIZE % 512 )) -eq 0 ||
		error "New size is not sector aligned."

	CRYPT_RESIZE_BLOCKS=$NEWBLOCKCOUNT

	if [ "$DEVSIZE" -ge "$NEWSIZE" ]; then
		CRYPT_RESIZE="shrink"
	else
		CRYPT_RESIZE="grow"
	fi
}

#################################
# Resize active crypt device
#  (on direct user request only)
#################################
resize_crypt() {
	dry "$CRYPTSETUP" resize "$1" --size "$CRYPT_RESIZE_BLOCKS" ||
		error "Failed to resize crypt device \"$1\"." \
		      "Target size: $CRYPT_RESIZE_BLOCKS sectors."
}

####################
# Resize filesystem
# Sets: NEWSIZE, NEWSIZE_ORIG
####################
resize() {
	local CMD

	NEWSIZE=$2
	detect_fs "$1"
	detect_device_size
	verbose "Device \"$VOLUME\" size is $DEVSIZE bytes."

	# if the size parameter is missing use device size
	test -z "$NEWSIZE" && NEWSIZE=${DEVSIZE}b
	NEWSIZE_ORIG=${NEWSIZE_ORIG:-$NEWSIZE}

	test "$DO_CRYPTRESIZE" -ne 0 &&
		detect_crypt_device "$VOLUME_ORIG" "$NEWSIZE_ORIG"

	test "$CRYPT_RESIZE" = "grow" &&
		resize_crypt "$VOLUME_ORIG"

	case "$FSTYPE" in
	  ext[234])	CMD=resize_ext ;;
	  reiserfs)	CMD=resize_reiser ;;
	  xfs)		CMD=resize_xfs ;;
	  crypto_LUKS)	CMD=resize_luks ;;
	  *) error "Filesystem \"$FSTYPE\" on device \"$VOLUME\" is not supported by this tool." ;;
	esac

	"$CMD" "$NEWSIZE" ||
		error "\"$FSTYPE\" resize failed on \"$VOLUME\"." \
		      "Target size: $NEWSIZE bytes."

	test "$CRYPT_RESIZE" = "shrink" &&
		resize_crypt "$VOLUME_ORIG"

	return 0
}

####################################
# Calculate diff between two dates
#  LC_ALL=C input is expected the
#  only one supported
####################################
diff_dates() {
	local D1
	local D2

	if ! D1=$("$DATE" -u -d"$1" +%s 2>/dev/null) ||
	   ! D2=$("$DATE" -u -d"$2" +%s 2>/dev/null); then
		verbose "Cannot parse date \"$1\" or \"$2\"."
		echo 1
		return
	fi
	echo "$(( D1 - D2 ))"
}

check_luks() {
	detect_luks_device

	check "$DM_DEV_DIR/mapper/$CRYPT_NAME"
}

###################
# Check filesystem
###################
check() {
	local FLAG
	local LASTMOUNT
	local LASTCHECKED
	local LASTDIFF
	local i

	detect_fs "$1"
	if detect_mounted; then
		verbose "Skipping filesystem check for device \"$VOLUME\" as the filesystem is mounted on \"$MOUNTED\".";
		exit 3
	fi

	case "$FSTYPE" in
	  ext[234])
		LASTMOUNT=""
		LASTCHECKED=""
		while read -r i; do
			case "$i" in
			  "Last mount time"*) LASTMOUNT=${i##*: } ;;
			  "Last checked"*) LASTCHECKED=${i##*: } ;;
			esac
		done <<-EOF
			$(LC_ALL=C "$TUNE_EXT" -l "$VOLUME")
		EOF
		case "$LASTMOUNT" in
		  *"n/a") ;; # nothing to do - system was not mounted yet
		  *)
			LASTDIFF=$(diff_dates "$LASTMOUNT" "$LASTCHECKED")
			if test "$LASTDIFF" -gt 0; then
				verbose "Filesystem has not been checked after the last mount, using fsck -f."
				FORCE="-f"
			fi
			;;
		esac
	esac

	case "$FSTYPE" in
	  xfs)  if command -v "$XFS_REPAIR" >/dev/null 2>&1; then
			# Prefer modern xfs_repair -n over deprecated xfs_check
			# FIXME: for small devices we need to force_geometry,
			# since we run in '-n' mode, it shouldn't be problem.
			# Think about better way....
			dry "$XFS_REPAIR" -n -o force_geometry "$VOLUME" ||
				error "XFS repair check failed on \"$VOLUME\"." \
				      "Filesystem may have errors requiring repair."
		elif command -v "$XFS_CHECK" >/dev/null 2>&1; then
			# Fallback to xfs_check for very old systems (pre-2012)
			dry "$XFS_CHECK" "$VOLUME" ||
				error "XFS check failed on \"$VOLUME\"."
		else
			error "Neither xfs_repair nor xfs_check found." \
			      "Please install xfsprogs package."
		fi
		;;
	  ext[234]|reiserfs)
	        # check if executed from interactive shell environment
		case "$-" in
		  *i*) FLAG=$YES ;;
		  *)   FLAG="-p" ;;
		esac
		accept_fsck dry "$FSCK" ${FORCE:+"$FORCE"} ${FLAG:+"$FLAG"} "$VOLUME"
		;;
	  crypto_LUKS)
		check_luks || error "LUKS check failed on \"$VOLUME\"."
		;;
	  *)
		error "Filesystem \"$FSTYPE\" on device \"$VOLUME\" is not supported by this tool." ;;
	esac
}

validate_override() {
	local MODE
	local OPATH
	local VAL

	eval "VAL=\${$1-}" # bash: VAL="${!1-}"
	test -z "$VAL" && return 0
	test "${VAL#/}" != "$VAL" ||
		error "$1 must be an absolute path."

	# -f is sufficient here, stat below catches non-existent paths
	if ! OPATH=$("$READLINK" -f "$VAL") ||
	   ! MODE=$("$STAT" -c '%u %a' "$OPATH") ||
	   test "${MODE%% *}" != "0"; then
		error "$1 \"$VAL\" must be accessible and owned by root."
	fi

	MODE=${MODE##* }
	test $(( (MODE / 10 % 10 & 2) | (MODE % 10 & 2) )) -eq 0 ||
		error "$1 \"$OPATH\" must not be group or world writable."
}

#############################
# start point of this script
# - parsing parameters
#############################
trap 'cleanup $?' EXIT
trap 'cleanup 2' HUP INT QUIT ABRT TERM

# test some prerequisites
for i in "$TUNE_EXT" "$RESIZE_EXT" "$TUNE_REISER" "$RESIZE_REISER" \
	"$TUNE_XFS" "$RESIZE_XFS" "$MOUNT" "$UMOUNT" "$MKDIR" \
	"$RMDIR" "$BLOCKDEV" "$BLKID" "$AWK" "$READLINK" "$STAT" \
	"$DATE" "$FSCK" "$XFS_CHECK" "$XFS_REPAIR" "$LVM" "$DMSETUP"; do
	test -n "$i" || error "Required command definitions in the script are missing!"
done

"$READLINK" -e / >/dev/null 2>&1 || READLINK_E="-f"
TEST64BIT=$(( 1000 * 1000000000000 ))
test "$TEST64BIT" -eq 1000000000000000 || error "Shell does not handle 64bit arithmetic."
test "$("$DATE" -u -d"Jan 01 00:00:01 1970" +%s)" -eq 1 || error "Date translation does not work."

test "$#" -eq 0 && tool_usage

while [ "$#" -ne 0 ]; do
	# Normalize: strip all '-' after leading '--' so e.g. --dry-run matches --dryrun
	case "$1" in
	  --*) ARG="--$(printf '%s' "${1#--}" | tr -d '-')" ;;
	  *) ARG=$1 ;;
	esac
	case "$ARG" in
	  "") ;;
	  -h|--help)		tool_usage ;;
	  -c|--cryptresize)	DO_CRYPTRESIZE=1 ;;
	  -e|--extoffline)	EXTOFF=1 ;;
	  -f|--force)		FORCE="-f" ;;
	  -l|--lvresize)	DO_LVRESIZE=1 ;;
	  -n|--dryrun)		DRY=1 ;;
	  -v|--verbose)		VERB="-v" ;;
	  -y|--yes)		YES="-y" ;;
	  check)	test -n "${2-}" || error "Missing <device>. (see: $TOOL --help)"
			ACTION=$1; shift; DEVICE=$1 ;;
	  resize)	test -n "${2-}" || error "Missing <device>. (see: $TOOL --help)"
			ACTION=$1; shift; DEVICE=$1
			test -n "${2-}" && { shift; NEWSIZE=$1; } ;;
	  *) error "Wrong argument \"$1\". (see: $TOOL --help)"
	esac
	shift
done

# test if we are not invoked recursively
if test "${FSADM_RUNNING-}" = "$TOOL"; then
	verbose "Skipping, already running (FSADM_RUNNING set)."
	exit 0
fi

# Validate DM_DEV_DIR by checking its control device is root-owned
if test ! -c "$DM_DEV_DIR/mapper/control" ||
   test "$("$STAT" -c '%u' "$DM_DEV_DIR/mapper/control")" != "0"; then
	DM_DEV_DIR="/dev" # fallback to /dev
fi

# overridden binaries must be absolute paths and owned by root for security
validate_override DMSETUP_BINARY
validate_override LVM_BINARY

"$DMSETUP" version >/dev/null 2>&1 ||
	error "Could not run dmsetup binary \"$DMSETUP\"."

"$LVM" version >/dev/null 2>&1 ||
	error "Could not run lvm binary \"$LVM\"."

test "$EXTOFF" -eq 1 2>/dev/null || EXTOFF=0

case "$ACTION" in
  check)  check "$DEVICE"
	  ;;
  resize) export FSADM_RUNNING=$TOOL
	  resize "$DEVICE" "$NEWSIZE"
	  ;;
  *)	  error "Missing command. (see: $TOOL --help)"
esac

exit 0
