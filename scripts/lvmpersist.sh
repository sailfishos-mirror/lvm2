#!/bin/bash
#
# Copyright (C) 2024 Red Hat, Inc. All rights reserved.
#
# This file is part of LVM2.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.

set -o pipefail

# user may override lvm location by setting LVM_BINARY
LVM=${LVM_BINARY:-lvm}

IFS_NL='
'

errorexit() {
	echo "$1"
	if [[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ]]; then
		logger "${SCRIPTNAME}: $1"
	fi
	exit 1
}

logmsg() {
	echo "$1"
	if [[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ]]; then
		logger "${SCRIPTNAME}: $1"
	fi
}

# nvme commands
# register: nvme resv-register $dev --nrkey=$OURKEY --rrega=0
# unregister: nvme resv-register $dev --crkey=$OURKEY --rrega=1
# reserve: nvme resv-acquire $dev --crkey=$OURKEY --rtype=$NVME_PRTYPE --racqa=0
# release: nvme resv-release $dev --crkey=$OURKEY --rtype=$NVME_PRTYPE --rrela=0
# preempt-abort: nvme resv-acquire $dev --crkey=$OURKEY --rtype=$NVME_PRTYPE --racqa=2
# clear: nvme resv-release $dev --crkey=$OURKEY --rrela=1
#
# read keys/reservation: nvme resv-report $dev
# (add --eds if nvme resv-report --help lists the --eds option)

set_cmd() {
	dev=$1
	case "$dev" in
	  /dev/dm-*)
		cmdpersist="mpathpersist"
		cmdopts=""
		;;
	  /dev/mapper*)
		cmdpersist="mpathpersist"
		cmdopts=""
		;;
	  *)
		cmdpersist="sg_persist"
		cmdopts="--no-inquiry"
		;;
	esac
}

key_is_on_device() {
	dev=$1
	FINDKEY=$2
	set_cmd "$dev"

	if $cmdpersist $cmdopts --in --read-keys "$dev" 2>/dev/null | grep -q "${FINDKEY}"; then
		true
		return
	fi

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmdpersist read-keys error on $dev"
	fi
	
	false
	return
}

get_key_list() {
	local IFS=$IFS_NL
	dev=$1
	set_cmd "$dev"

	KEYS=( $($cmdpersist $cmdopts --in --read-keys "$dev" 2>/dev/null | grep "    0x" | xargs ) )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmdpersist read-keys error on $dev"
	fi
}

no_reservation_held() {
	dev=$1

	if $cmdpersist $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -q "there is NO reservation held"; then
		true
		return
	fi

	false
	return
}

get_current_reservation_desc() {
	dev=$1

	set_cmd "$dev"

	str=$( $cmdpersist $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -e "LU_SCOPE,\s\+type" )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmdpersist read-reservation error on $dev"
	fi

	if [[ -z $str ]]; then
		if no_reservation_held $dev; then
			DESC=none
		else
			DESC=error
		fi
		false
		return
	fi

	# Output format differs between commands:
	# sg_persist:   "scope: LU_SCOPE,  type: "
	# mpathpersist: "scope = LU_SCOPE, type = "

	if [[ "$str" == *"Exclusive Access, all registrants"* ]]; then
		# scsi type 8
		DESC=EAAR
		true
	elif [[ "$str" == *"Write Exclusive, all registrants"* ]]; then
		# scsi type 7
		DESC=WEAR
		true
	elif [[ "$str" == *"Exclusive Access, registrants only"* ]]; then
		# scsi type 6
		DESC=EARO
		true
	elif [[ "$str" == *"Write Exclusive, registrants only"* ]]; then
		# scsi type 5
		DESC=WERO
		true
	elif [[ "$str" == *"Exclusive Access"* ]]; then
		# scsi type 3
		DESC=EA
		true
	elif [[ "$str" == *"Write Exclusive"* ]]; then
		# scsi type 1
		DESC=WE
		true
	else
		DESC=unknown
		false
	fi
}

device_supports_prdesc() {
	dev=$1
	desc=$2

	case "$desc" in
	WE)
		SUPPORTED="Write Exclusive: 1"
		;;
	EA)
		SUPPORTED="Exclusive Access: 1"
		;;
	WERO)
		SUPPORTED="Write Exclusive, registrants only: 1"
		;;
	EARO)
		SUPPORTED="Exclusive Access, registrants only: 1"
		;;
	WEAR)
		SUPPORTED="Write Exclusive, all registrants: 1"
		;;
	EAAR)
		SUPPORTED="Exclusive Access, all registrants: 1"
		;;
	*)
		logmsg "Unknown desc string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
		false
		return
		;;
	esac

	# Do not set_cmd here because for report-capabilities,
	# sg_persist works on mpath devs, but mpathpersist doesn't work.

	if sg_persist --in --report-capabilities "$dev" 2>/dev/null | grep -q "${SUPPORTED}"; then
		true
		return
	fi

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "sg_persist report-capabilities error on $dev"
	fi

	false
	return
}

check_device_types() {
	err=0
	FOUND_MPATH=0
	FOUND_SCSI=0

	for dev in "${DEVICES[@]}"; do
		case "$dev" in
	  	/dev/sd*)
			FOUND_SCSI=1
			;;
		/dev/dm-*)
			;&
		/dev/mapper*)
			MAJORMINOR=$(dmsetup info --noheadings -c -o major,minor "$dev")
			read -r <"/sys/dev/block/$MAJORMINOR/dm/uuid" DM_UUID 2>&1
			if [[ $DM_UUID == *"mpath-"* ]]; then
				FOUND_MPATH=1
			else
				logmsg "Device $dev dm uuid does not appear to be multipath ($DM_UUID)"
				err=1
			fi
			;;
	  	*)
			logmsg "Device type not supported: $dev."
			err=1
		esac
	done

	test "$err" -eq 1 && exit 1

	if [[ $FOUND_MPATH -eq 1 ]]; then
		which mpathpersist > /dev/null || errorexit "mpathpersist command not found."
		if ! grep "reservation_key file" /etc/multipath.conf > /dev/null; then
			echo "To use persistent reservations with multipath, run:"
			echo "  mpathconf --option reservation_key:file"
			echo "to configure multipath.conf, and then restart multipathd."
		fi
	fi
}

do_start() {
	err=0

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	check_device_types

	for dev in "${DEVICES[@]}"; do
		if ! device_supports_prdesc "$dev" "$PRDESC"; then
			logmsg "start $GROUP: $dev does not support reservation type $PRDESC."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	# Requires all devices in VG to support it.
	if [[ $APTPL -eq 1 ]]; then
		aptplopt="--param-aptpl"
	else
		aptplopt=""
	fi

	for dev in "${DEVICES[@]}"; do
		# Register our key
		set_cmd "$dev"
		$cmdpersist $cmdopts --out --register-ignore $aptplopt --param-sark="$OURKEY" "$dev" >/dev/null 2>&1

		test $? -eq 0 || logmsg "$cmdpersist register error on $dev"

		# Verify our registration
		if ! key_is_on_device "$dev" "$OURKEY" ; then
			# TODO: unregister those we've registered
			logmsg "start $GROUP: failed to register our key $OURKEY on $dev."
			exit 1
		fi
	done

	for dev in "${DEVICES[@]}"; do
		# Reserve the device
		set_cmd "$dev"
		$cmdpersist $cmdopts --out --reserve --param-rk="$OURKEY" --prout-type="$SCSI_PRTYPE" "$dev" >/dev/null 2>&1

		test $? -eq 0 || logmsg "$cmdpersist reserve error on $dev"

		# Verify reservation
		get_current_reservation_desc "$dev"
		if [[ "$DESC" != "$PRDESC" ]]; then
			logmsg "start $GROUP: failed reservation on $dev, need $PRDESC found $DESC."
			exit 1
		fi
	done

	logmsg "started $GROUP with key $OURKEY."
	exit 0
}

do_stop() {
	err=0

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	# Removing reservation is not needed, we just remove our registration key.
	# The reservation will go away when the last key is removed.
	# sg_persist --out --no-inquiry --release --param-rk=${OURKEY} --prout-type=$SCSI_PRTYPE

	for dev in "${DEVICES[@]}"; do
		# Remove our registration key, we will no longer be able to write
		set_cmd "$dev"
		$cmdpersist $cmdopts --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1

		test $? -eq 0 || logmsg "$cmdpersist unregister error on $dev"

		if key_is_on_device "$dev" "$OURKEY" ; then
			logmsg "stop $GROUP: failed to unregister our key $OURKEY from $dev."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "stopped $GROUP with key $OURKEY."
	exit 0
}

do_clear() {
	err=0

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	# our key must be registered to do clear
	for dev in "${DEVICES[@]}"; do
		if ! key_is_on_device "$dev" "$OURKEY" ; then
			logmsg "clear $GROUP: our key $OURKEY is not registered on $dev (start to clear)."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	# clear releases the reservation and clears all registrations
	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"
		$cmdpersist $cmdopts --out --clear --param-rk="$OURKEY" "$dev" >/dev/null 2>&1

		test $? -eq 0 || logmsg "$cmdpersist clear error on $dev"

		# Real result is whether the dev now has no registrations and
		# reservation.

		get_key_list "$dev"
		if [[ -n "$KEYS" ]]; then
			logmsg "clear $GROUP: keys not cleared from $dev - ${KEYS[*]}"
			err=1
		fi

		if ! no_reservation_held "$dev"; then
			logmsg "clear $GROUP: reservation not cleared from $dev"
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "cleared $GROUP reservation and keys"
	exit 0
}

do_remove() {
	err=0

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	if [[ -z "$REMOVEKEY" ]]; then
		echo "Missing required option: --key."
		exit 1
	fi

	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"
		$cmdpersist $cmdopts --out --preempt-abort --param-sark="$REMOVEKEY" --param-rk="$OURKEY" --prout-type="$SCSI_PRTYPE" "$dev" >/dev/null 2>&1

		test $? -eq 0 || logmsg "$cmdpersist preempt-abort error on $dev"

		if key_is_on_device "$dev" "$REMOVEKEY" ; then
			logmsg "Failed to remove key $REMOVEKEY from $dev in $GROUP."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "Removed key $REMOVEKEY for $GROUP."
	exit 0
}

do_devtest() {
	check_device_types

	for dev in "${DEVICES[@]}"; do
		if [[ -n "$PRDESC" ]]; then
			if device_supports_prdesc "$dev" "$PRDESC"; then
				echo "Device $dev: supports type $PRDESC"
			else
				echo "Device $dev: does not support type $PRDESC"
			fi
		fi

		if [[ -n "$OURKEY" ]]; then
			if key_is_on_device "$dev" "$OURKEY" ; then
				echo "Device $dev: our key is registered $OURKEY"
			else
				echo "Device $dev: our key is not registered $OURKEY"
			fi
		fi

		get_key_list "$dev"
		if [[ -z "$KEYS" ]]; then
			echo "Device $dev: registered keys: none"
		else
			echo "Device $dev: registered keys: ${KEYS[*]}"
		fi

		get_current_reservation_desc "$dev"
		echo "Device $dev: reservation: $DESC"
	done
}

do_readkeys() {
	check_device_types

	for dev in "${DEVICES[@]}"; do
		get_key_list "$dev"
		if [[ -z "$KEYS" ]]; then
			echo "Device $dev: registered keys: none"
		else
			echo "Device $dev: registered keys: ${KEYS[*]}"
		fi
	done
}

do_readreservation() {
	check_device_types

	for dev in "${DEVICES[@]}"; do
		get_current_reservation_desc "$dev"
		echo "Device $dev: reservation: $DESC"
	done
}

usage() {
	echo "${SCRIPTNAME}: use persistent reservations on devices in an LVM VG."
	echo ""
	echo "${SCRIPTNAME} start --ourkey KEY --prtype PRTYPE --vg|--device ARG"
	echo "	Register key and reserve device(s)."
	echo ""
	echo "${SCRIPTNAME} stop --ourkey KEY --vg|--device ARG"
	echo "	Unregister key, dropping reservation."
	echo ""
	echo "${SCRIPTNAME} remove --ourkey KEY --key KEY --vg|--device ARG"
	echo "	Preempt and abort a key."
	echo ""
	echo "${SCRIPTNAME} clear --ourkey KEY --vg|--device ARG"
	echo "	Release reservation and clear registered keys."
	echo ""
	echo "${SCRIPTNAME} devtest [--ourkey KEY] --vg|--device ARG"
	echo "	Test if devices support PR and report PR state."
	echo ""
	echo "${SCRIPTNAME} readkeys --vg|--device ARG"
	echo "	Report registered keys."
	echo ""
	echo "${SCRIPTNAME} readreservation --vg|--device ARG"
	echo "	Report reservation."
	echo ""
	echo "Options:"
	echo "    --ourkey KEY"
	echo "        KEY is the local key."
	echo "    --key KEY"
	echo "        KEY is a another host's key."
	echo "    --prtype PRTYPE"
	echo "        The type of persistent reservation, see PRTYPE values below."
	echo "    --device PATH"
	echo "        Device to use. Repeat this option to use multiple devices."
	echo "    --vg VGNAME"
	echo "        If no devices are provided, all devices from VG will be used."
	echo ""
	echo "PRTYPE: persistent reservation type (use abbreviation with --prtype)."
	echo "        WE: Write Exclusive"
	echo "        EA: Exclusive Access"
	echo "        WERO: Write Exclusive – registrants only"
	echo "        EARO: Exclusive Access – registrants only"
	echo "        WEAR: Write Exclusive – all registrants"
	echo "        EAAR: Exclusive Access – all registrants"
	echo ""
}

#
# BEGIN SCRIPT
#
PATH="/sbin:/usr/sbin:/bin:/usr/sbin:$PATH"
SCRIPTNAME=$(basename "$0")

if [ $# -lt 1 ]; then
	usage
	exit 0
fi

which sg_persist > /dev/null || errorexit "sg_persist command not found."

DO_START=0
DO_STOP=0
DO_REMOVE=0
DO_CLEAR=0
DO_DEVTEST=0
DO_READKEYS=0
DO_READRESERVATION=0

CMD=$1
shift

case $CMD in
	start)
		DO_START=1
		;;
	stop)
		DO_STOP=1
		;;
	preempt-abort)
		DO_REMOVE=1
		;;
	clear)
		DO_CLEAR=1
		;;
	devtest)
		DO_DEVTEST=1
		;;
	readkeys)
		DO_READKEYS=1
		;;
	readreservation)
		DO_READRESERVATION=1
		;;
	help)
		usage
		exit 0
		;;
	-h)
		usage
		exit 0
		;;
	*)
		echo "Unknown command: $CMD."
		exit 1
		;;
esac

if [ "$UID" != 0 ] && [ "$EUID" != 0 ] && [ "$CMD" != "help" ]; then
	echo "${SCRIPTNAME} must be run as root."
	exit 1
fi

GETOPT="getopt"

OPTIONS=$("$GETOPT" -o h -l help,ourkey:,key:,prtype:,aptpl,device:,vg: -n "${SCRIPTNAME}" -- "$@")
eval set -- "$OPTIONS"

while true
do
	case $1 in
	--ourkey)
		OURKEY=$2;
		shift; shift
		;;
	--key)
		REMOVEKEY=$2;
		shift; shift
		;;
	--aptpl)
		APTPL=1
		shift
		;;
	--prtype)
		PRTYPESTR=$2
		shift; shift;
		;;
	--device)
		LAST_DEVICE=$2
		DEVICES+=("$LAST_DEVICE")
		shift; shift
		;;
	--vg)
		VGNAME=$2;
		shift; shift
		;;
	-h|--help)
		usage
		shift
		exit 0
		;;
	--)
		shift
		break
		;;
	*)
		echo "Unknown option \"$1\."
		exit 1
		;;
    esac
done

#
# Missing required options
#

if [[ -z "$LAST_DEVICE" && -z "$VGNAME" ]]; then
	echo "Missing required option: --vg or --device."
	exit 1
fi

if [[ "$DO_START" -eq 1 && -z "$PRTYPESTR" ]]; then
	echo "Missing required option: --prtype"
	exit 1
fi

# We use the internal numerical values defined in include/uapi/linux/pr.h
# which are not the same as scsi/nvme-specific values.

if [[ -n "$PRTYPESTR" ]]; then
	case "$PRTYPESTR" in
	WE)
		# Write Exclusive
		PRDESC=WE
		PRTYPE=1
		SCSI_PRTYPE=1
		NVME_PRTYPE=1
		;;
	EA)
		# Exclusive Access
		PRDESC=EA
		PRTYPE=2
		SCSI_PRTYPE=3
		NVME_PRTYPE=2
		;;
	WERO)
		# Write Exclusive - registrants only
		PRDESC=WERO
		PRTYPE=3
		SCSI_PRTYPE=5
		NVME_PRTYPE=3
		;;
	EARO)
		# Exclusive Access - registrants only
		PRDESC=EARO
		PRTYPE=4
		SCSI_PRTYPE=6
		NVME_PRTYPE=4
		;;
	WEAR)
		# Write Exclusive - all registrants
		PRDESC=WEAR
		PRTYPE=5
		SCSI_PRTYPE=7
		NVME_PRTYPE=5
		;;
	EAAR)
		# Exclusive Access - all registrants
		PRDESC=EAAR
		PRTYPE=6
		SCSI_PRTYPE=8
		NVME_PRTYPE=6
		;;
	*)
		echo "Unknown PRTYPE string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
		exit 1
		;;
	esac
fi

#
# Set devices
#

# Add a --devicesfile option that can be used for this vgs command?
get_devices_from_vg() {
	local IFS=:
	DEVICES=( $("$LVM" vgs --nolocking --noheadings --separator : --sort pv_uuid --o pv_name --rows --config log/prefix=\"\" "$VGNAME") )
}

if [[ -z "$LAST_DEVICE" && -n "$VGNAME" ]]; then
	get_devices_from_vg
fi

FIRST_DEVICE="$DEVICES"

if [[ -z "$FIRST_DEVICE" ]]; then
	echo "Missing required --vg or --device."
	exit 1
fi

# Prefix some log messages with VGNAME, or if no VGNAME is set,
# use "sda" (for one device), or "sda:sdz" (for multiple devices).
if [[ -n "$VGNAME" ]]; then
	GROUP=$VGNAME
else
	if [[ "$FIRST_DEVICE" == "$LAST_DEVICE" ]]; then
		GROUP=$(basename "$FIRST_DEVICE")
	else
		GROUP="$(basename "$FIRST_DEVICE"):$(basename "$LAST_DEVICE")"
	fi
fi


#
# Main program function
#

if [[ "$DO_START" -eq 1 ]]; then
	do_start
elif [[ "$DO_STOP" -eq 1 ]]; then
	do_stop
elif [[ "$DO_REMOVE" -eq 1 ]]; then
	do_remove
elif [[ "$DO_CLEAR" -eq 1 ]]; then
	do_clear
elif [[ "$DO_DEVTEST" -eq 1 ]]; then
	do_devtest
elif [[ "$DO_READKEYS" -eq 1 ]]; then
	do_readkeys
elif [[ "$DO_READRESERVATION" -eq 1 ]]; then
	do_readreservation
fi

