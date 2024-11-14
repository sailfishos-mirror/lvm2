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
	echo "  ${SCRIPTNAME}: $1"
	if [[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ]]; then
		logger "${SCRIPTNAME}: $1"
	fi
	exit 1
}

logmsg() {
	echo "  ${SCRIPTNAME}: $1"
	if [[ "$DO_START" -eq 1 || "$DO_STOP" -eq 1 || "$DO_REMOVE" -eq 1 ]]; then
		logger "${SCRIPTNAME}: $1"
	fi
}

# nvme commands
# register: nvme resv-register --nrkey=$OURKEY --rrega=0
# unregister: nvme resv-register --crkey=$OURKEY --rrega=1
# reserve: nvme resv-acquire --crkey=$OURKEY --rtype=$NVME_PRTYPE --racqa=0
# release: nvme resv-release --crkey=$OURKEY --rtype=$NVME_PRTYPE --rrela=0
# preempt-abort: nvme resv-acquire --crkey=$OURKEY --rtype=$NVME_PRTYPE --racqa=2
# clear: nvme resv-release --crkey=$OURKEY --rrela=1

set_cmd() {
	dev=$1
	case "$dev" in
	  /dev/nvme*)
		cmd="nvme"
		cmdopts=""
		;;
	  /dev/dm-*)
		cmd="mpathpersist"
		cmdopts=""
		;;
	  /dev/mapper*)
		cmd="mpathpersist"
		cmdopts=""
		;;
	  *)
		cmd="sg_persist"
		cmdopts="--no-inquiry"
		;;
	esac
}

key_is_on_device_nvme() {
	FINDKEY_DEC=$(printf '%u' $FINDKEY)

	if nvme resv-report --eds -o json "$dev" 2>/dev/null | grep -q "\"rkey\"\:${FINDKEY_DEC}"; then
		true
		return
	fi

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd resv-report error on $dev"
	fi
	
	false
	return
}

key_is_on_device_scsi() {
	# grep with space to avoid matching the line "PR generation=0x..."
	# end-of-line matching required to avoid 0x123ab matching 0x123abc
	FINDKEY=" $FINDKEY$"

	if $cmd $cmdopts --in --read-keys "$dev" 2>/dev/null | grep -q "${FINDKEY}"; then
		true
		return
	fi

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd read-keys error on $dev"
	fi
	
	false
	return
}

key_is_on_device() {
	dev=$1
	FINDKEY=$2
	set_cmd "$dev"

	if [[ "$cmd" -eq "nvme" ]]; then
		key_is_on_device_nvme $dev $FINDKEY
	else
		key_is_on_device_scsi $dev $FINDKEY
	fi
}

get_key_list_nvme() {
	local IFS=$IFS_NL
	dev=$1
	set_cmd "$dev"

	# json/jq output is only decimal
	KEYS=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.regctlext[].rkey' | xargs printf '0x%x ')

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd read-keys error on $dev"
	fi

	if [[ "$KEYS" == "0x0 " ]]; then
		KEYS=""
	fi
}

get_key_list_scsi() {
	local IFS=$IFS_NL
	dev=$1
	set_cmd "$dev"

	if $cmd $cmdopts --in --read-keys "$dev" 2>/dev/null | grep -q "there are NO registered reservation keys"; then
		KEYS=""
		return
	fi

	KEYS=( $($cmd $cmdopts --in --read-keys "$dev" 2>/dev/null | grep "    0x" | xargs ) )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "$cmd read-keys error on $dev"
	fi
}

get_key_list() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" -eq "nvme" ]]; then
		get_key_list_nvme $dev
	else
		get_key_list_scsi $dev
	fi
}

get_current_reservation_holder_nvme() {
	dev=$1

	# get rkey from the regctlext section with rcsts=1

	str=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.regctlext | map(select(.rcsts == 1)) | .[].rkey' | xargs printf '0x%x')

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "nvme resv-report error on $dev"
		HOLDER=0
		false
		return
	fi

	if [[ -z $str ]]; then
		logmsg "nvme resv-report holder output not found $dev"
		HOLDER=0
		false
		return
	fi

	HOLDER=$str
}

get_current_reservation_holder_scsi() {
	dev=$1

	# TODO: combine with get_current_reservation_desc to
	# run a single sg_persist for holder and type.

	set_cmd "$dev"

	str=$( $cmd $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -e "Key\s*=\s*0x" | xargs )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		if no_reservation_held $dev; then
			HOLDER=0
		else
			logmsg "$cmd read-reservation error on $dev"
			HOLDER=0
		fi
		false
		return
	fi

	if [[ -z $str ]]; then
		if no_reservation_held $dev; then
			HOLDER=0
		else
			logmsg "$cmd read-reservation holder output not found $dev"
			HOLDER=0
		fi
		false
		return
	fi

	HOLDER="${str:4}"
}

get_current_reservation_holder() {
	dev=$1
	cur_type=$2

	# holder is not relevant for WEAR/EAAR
	if [[ "$cur_type" == "WEAR" || "$cur_type" == "EAAR" ]]; then
		HOLDER=0
		return
	fi

	set_cmd "$dev"

	if [[ "$cmd" -eq "nvme" ]]; then
		get_current_reservation_holder_nvme $dev
	else
		get_current_reservation_holder_scsi $dev
	fi
}

get_current_reservation_desc_nvme() {
	dev=$1
	
	str=$(nvme resv-report --eds -o json "$dev" 2>/dev/null | jq '.rtype')

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		logmsg "nvme resv-report error on $dev"
		DESC=error
		false
		return
	fi

	if [[ -z $str ]]; then
		logmsg "nvme resv-report no reservation type for $dev"
		DESC=error
		false
		return
	fi

	case "$str" in
	0)
		DESC=none
		false
		;;
	1)
		DESC=WE
		true
		;;
	2)
		DESC=EA
		true
		;;
	3)
		DESC=WERO
		true
		;;
	4)
		DESC=EARO
		true
		;;
	5)
		DESC=WEAR
		true
		;;
	6)
		DESC=EAAR
		true
		;;
	*)
		echo "Unknown PR value"
		exit 1
		;;
	esac
}

get_current_reservation_desc_scsi() {
	dev=$1

	str=$( $cmd $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -e "LU_SCOPE,\s\+type" )

	if [ "${PIPESTATUS[0]}" -ne "0" ]; then
		if no_reservation_held $dev; then
			DESC=none
		else
			logmsg "$cmd read-reservation error on $dev"
			DESC=error
		fi
		false
		return
	fi

	if [[ -z $str ]]; then
		if no_reservation_held $dev; then
			DESC=none
		else
			logmsg "$cmd read-reservation type output not found $dev"
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

get_current_reservation_desc() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" -eq "nvme" ]]; then
		get_current_reservation_desc_nvme $dev
	else
		get_current_reservation_desc_scsi $dev
	fi
}

no_reservation_held_nvme() {
	dev=$1

	get_current_reservation_desc_nvme $dev

	if [[ "$DESC" == "none" ]]; then
		true
		return
	fi

	false
	return
}

no_reservation_held_scsi() {
	dev=$1

	if $cmd $cmdopts --in --read-reservation "$dev" 2>/dev/null | grep -q "there is NO reservation held"; then
		true
		return
	fi

	false
	return
}

no_reservation_held() {
	dev=$1
	set_cmd "$dev"

	if [[ "$cmd" -eq "nvme" ]]; then
		no_reservation_held_nvme $dev
	else
		no_reservation_held_scsi $dev
	fi
}

device_supports_prdesc_nvme() {
	dev=$1

	if nvme resv-report --eds "$dev" > /dev/null 2>&1; then
		true
		return
	fi

	false
	return
}

device_supports_prdesc_scsi() {
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
		logmsg "unknown desc string (choose WE/EA/WERO/EARO/WEAR/EAAR)."
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

device_supports_prdesc() {
	dev=$1
	desc=$2
	set_cmd "$dev"

	if [[ "$cmd" -eq "nvme" ]]; then
		device_supports_prdesc_nvme $dev $desc
	else
		device_supports_prdesc_scsi $dev $desc
	fi
}

check_device_types() {
	err=0
	FOUND_MPATH=0
	FOUND_SCSI=0
	FOUND_NVME=0

	for dev in "${DEVICES[@]}"; do
		case "$dev" in
	  	/dev/nvme*)
			FOUND_NVME=1
			;;
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
				logmsg "device $dev dm uuid does not appear to be multipath ($DM_UUID)"
				err=1
			fi
			;;
	  	*)
			logmsg "device type not supported for $dev."
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

	if [[ $FOUND_SCSI -eq 1 ]]; then
		which sg_persist > /dev/null || errorexit "sg_persist command not found."
	fi

	if [[ $FOUND_NVME -eq 1 ]]; then
		which nvme > /dev/null || errorexit "nvme command not found."
	fi
}

undo_register() {
	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" -eq "nvme" ]]; then
			nvme resv-register --crkey="$OURKEY" --rrega=1 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi
		test $? -eq 0 || logmsg "$cmd unregister error on $dev"
	done
}

do_register_nvme() {
	dev=$1

	# If our previous key is still registered, then we must use
	# rrega=2 and iekey.  If our previous key has been removed,
	# then we must use rrega=0.

	if ! nvme resv-register --nrkey="$OURKEY" --rrega=0 "$dev" >/dev/null 2>&1; then
		if ! nvme resv-register --nrkey="$OURKEY" --rrega=2 --iekey "$dev" >/dev/null 2>&1; then
			logmsg "$cmd register error on $dev"
			false
			return
		fi
	fi
}

do_register_scsi() {
	dev=$1
	set_cmd "$dev"

	if ! $cmd $cmdopts --out --register-ignore $aptplopt --param-sark="$OURKEY" "$dev" >/dev/null 2>&1; then
		logmsg "$cmd register error on $dev"
		false
		return
	fi
}

do_register() {
	if [[ "$cmd" -eq "nvme" ]]; then
		do_register_nvme $1
	else
		do_register_scsi $1
	fi
}

do_takeover() {

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	if [[ -z "$REMKEY" ]]; then
		echo "Missing required option: --removekey."
		exit 1
	fi

	# Requires all devices in VG to support it.
	if [[ $APTPL -eq 1 ]]; then
		aptplopt="--param-aptpl"
	else
		aptplopt=""
	fi

	for dev in "${DEVICES[@]}"; do
		if ! key_is_on_device "$dev" "$REMKEY" ; then
			logmsg "start $GROUP specified key to remove $REMKEY not found on $dev."
			exit 1
		fi
	done

	err=0

	# Register our key

	for dev in "${DEVICES[@]}"; do
		if ! do_register "$dev"; then
			err=1
			break
		fi
	done

	if [[ "$err" -eq 1 ]]; then
		logmsg "start $GROUP failed to register our key."
		undo_register
		exit 1
	fi

	# Reserve the device

	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" -eq "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --prkey="$REMKEY" --rtype="$NVME_PRTYPE" --racqa=2 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --preempt-abort --param-sark="$REMKEY" --param-rk="$OURKEY" --prout-type="$SCSI_PRTYPE" "$dev" >/dev/null 2>&1
		fi

		if [[ "$?" -ne 0 ]]; then
			logmsg "start $GROUP failed to preempt-abort $REMKEY on $dev."
			undo_register
			exit 1
		fi
	done

	logmsg "started $GROUP with key $OURKEY."
	exit 0
}

do_start() {
	err=0

	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	for dev in "${DEVICES[@]}"; do
		if ! device_supports_prdesc "$dev" "$PRDESC"; then
			logmsg "start $GROUP $dev does not support reservation type $PRDESC."
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

	err=0

	# Register our key on devices

	for dev in "${DEVICES[@]}"; do
		if ! do_register "$dev"; then
			err=1
			break
		fi
	done

	if [[ "$err" -eq 1 ]]; then
		logmsg "start $GROUP failed to register our key."
		undo_register
		exit 1
	fi

	# Reserve devices

	for dev in "${DEVICES[@]}"; do

		# For type WEAR/EAAR, once it's acquired (by the first
		# host), it cannot be acquired more times by other hosts
		# (the command fails for nvme), so if WEAR/EAAR is
		# requested, first check if that reservation type already
		# exists.

		if [[ "$PRDESC" == "WEAR" || "$PRDESC" == "EAAR" ]]; then
			get_current_reservation_desc $dev
			if [[ "$DESC" == "$PRDESC" ]]; then
				continue
			fi
		fi

		set_cmd "$dev"

		if [[ "$cmd" -eq "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --rtype="$NVME_PRTYPE" --racqa=0 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --reserve --param-rk="$OURKEY" --prout-type="$SCSI_PRTYPE" "$dev" >/dev/null 2>&1
		fi

		if [[ "$?" -ne 0 ]]; then
			logmsg "start $GROUP failed to reserve $dev."
			undo_register
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

		if [[ "$cmd" -eq "nvme" ]]; then
			nvme resv-register --crkey="$OURKEY" --rrega=1 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --register --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd unregister error on $dev"

		if key_is_on_device "$dev" "$OURKEY" ; then
			logmsg "stop $GROUP failed to unregister our key $OURKEY from $dev."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "stopped $GROUP with key $OURKEY."
	exit 0
}

do_clear() {
	if [[ -z "$OURKEY" ]]; then
		echo "Missing required option: --ourkey."
		exit 1
	fi

	# our key must be registered to do clear.
	# we want to clear any/all PR state that we can find on the devs,
	# so just skip any devs that we cannot register with, and clear
	# what we can.

	for dev in "${DEVICES[@]}"; do
		if ! key_is_on_device "$dev" "$OURKEY" ; then
			if ! do_register "$dev"; then
				logmsg "clear $GROUP skip $dev without registration"
			fi
		fi
	done

	err=0

	# clear releases the reservation and clears all registrations
	for dev in "${DEVICES[@]}"; do
		set_cmd "$dev"

		if [[ "$cmd" -eq "nvme" ]]; then
			nvme resv-release --crkey=$OURKEY --rrela=1 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --clear --param-rk="$OURKEY" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd clear error on $dev"

		# Real result is whether the dev now has no registrations and
		# reservation.

		get_key_list "$dev"
		if [[ -n "$KEYS" ]]; then
			logmsg "clear $GROUP keys not cleared from $dev - ${KEYS[*]}"
			err=1
		fi

		if ! no_reservation_held "$dev"; then
			logmsg "clear $GROUP reservation not cleared from $dev"
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

	if [[ -z "$REMKEY" ]]; then
		echo "Missing required option: --removekey."
		exit 1
	fi

	for dev in "${DEVICES[@]}"; do
		if ! key_is_on_device "$dev" "$OURKEY" ; then
			logmsg "cannot remove $REMKEY from $dev without ourkey $OURKEY being registered"
			continue
		fi

		set_cmd "$dev"

		if [[ "$cmd" -eq "nvme" ]]; then
			nvme resv-acquire --crkey="$OURKEY" --prkey="$REMKEY" --racqa=2 "$dev" >/dev/null 2>&1
		else
			$cmd $cmdopts --out --preempt-abort --param-sark="$REMKEY" --param-rk="$OURKEY" --prout-type="$SCSI_PRTYPE" "$dev" >/dev/null 2>&1
		fi

		test $? -eq 0 || logmsg "$cmd preempt-abort error on $dev"

		if key_is_on_device "$dev" "$REMKEY" ; then
			logmsg "failed to remove key $REMKEY from $dev in $GROUP."
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	logmsg "removed key $REMKEY for $GROUP."
	exit 0
}

do_devtest() {
	err=0

	for dev in "${DEVICES[@]}"; do
		if [[ -n "$PRDESC" ]]; then
			if device_supports_prdesc "$dev" "$PRDESC"; then
				echo "Device $dev: supports type $PRDESC"
			else
				echo "Device $dev: does not support type $PRDESC"
				err=1
			fi
		fi
	done

	test "$err" -eq 1 && exit 1

	exit 0
}

do_checkkey() {
	err=0

	for dev in "${DEVICES[@]}"; do
		if key_is_on_device "$dev" "$OURKEY" ; then
			echo "Device $dev: has key $OURKEY"
		else
			echo "Device $dev: does not have key $OURKEY"
			err=1
		fi
	done

	test "$err" -eq 1 && exit 1

	exit 0
}

do_readkeys() {
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
	for dev in "${DEVICES[@]}"; do
		get_current_reservation_desc "$dev"
		get_current_reservation_holder "$dev" $DESC
		echo "Device $dev: reservation: $DESC holder $HOLDER"
	done
}

usage() {
	echo "${SCRIPTNAME}: use persistent reservations on devices in an LVM VG."
	echo ""
	echo "${SCRIPTNAME} start --ourkey KEY --prtype PRTYPE --vg|--device ARG"
	echo "	Register key and reserve device(s)."
	echo ""
	echo "${SCRIPTNAME} start --ourkey KEY --prtype PRTYPE --removekey REMKEY --vg|--device ARG"
	echo "	Register key and reserve device(s), replacing reservation holder by prempt-abort."
	echo ""
	echo "${SCRIPTNAME} stop --ourkey KEY --vg|--device ARG"
	echo "	Unregister key, dropping reservation."
	echo ""
	echo "${SCRIPTNAME} remove --ourkey KEY --removekey REMKEY --vg|--device ARG"
	echo "	Preempt-abort a key."
	echo ""
	echo "${SCRIPTNAME} clear --ourkey KEY --vg|--device ARG"
	echo "	Release reservation and clear registered keys."
	echo ""
	echo "${SCRIPTNAME} devtest [--prtype PRTYPE] --vg|--device ARG"
	echo "	Test if devices support PR."
	echo ""
	echo "${SCRIPTNAME} check-key --key KEY --vg|--device ARG"
	echo "	Check if a key is registered."
	echo ""
	echo "${SCRIPTNAME} read-keys --vg|--device ARG"
	echo "	Display registered keys."
	echo ""
	echo "${SCRIPTNAME} read-reservation --vg|--device ARG"
	echo "	Display reservation."
	echo ""
	echo "${SCRIPTNAME} read --vg|--device ARG"
	echo "	Display registered keys and reservation."
	echo ""
	echo "Options:"
	echo "    --ourkey KEY"
	echo "        KEY is the local key."
	echo "    --removekey REMKEY"
	echo "        REMKEY is a another host's key to remove."
	echo "    --key KEY"
	echo "        KEY is any key to check."
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
	echo "        WERO: Write Exclusive – registrants only (not yet supported)"
	echo "        EARO: Exclusive Access – registrants only (not yet supported)"
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

DO_START=0
DO_STOP=0
DO_REMOVE=0
DO_CLEAR=0
DO_DEVTEST=0
DO_CHECKKEY=0
DO_READKEYS=0
DO_READRESERVATION=0
DO_READ=0

CMD=$1
shift

case $CMD in
	start)
		DO_START=1
		;;
	stop)
		DO_STOP=1
		;;
	remove)
		DO_REMOVE=1
		;;
	clear)
		DO_CLEAR=1
		;;
	devtest)
		DO_DEVTEST=1
		;;
	check-key)
		DO_CHECKKEY=1
		;;
	read-keys)
		DO_READKEYS=1
		;;
	read-reservation)
		DO_READRESERVATION=1
		;;
	read)
		DO_READ=1
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

OPTIONS=$("$GETOPT" -o h -l help,ourkey:,removekey:,key:,prtype:,aptpl,device:,vg: -n "${SCRIPTNAME}" -- "$@")
eval set -- "$OPTIONS"

while true
do
	case $1 in
	--ourkey)
		OURKEY=$2;
		shift; shift
		;;
	--key)
		KEY=$2;
		shift; shift
		;;
	--removekey)
		REMKEY=$2;
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

if [[ "$DO_REMOVE" -eq 1 && -z "$PRTYPESTR" ]]; then
	echo "Missing required option: --prtype"
	exit 1
fi

if [[ "$DO_DEVTEST" -eq 1 && -z "$PRTYPESTR" ]]; then
	PRTYPESTR="WE"
fi

if [[ "$DO_CHECKKEY" -eq 1 && -z "$KEY" ]]; then
	echo "Missing required option: --key"
	exit 1
fi

if [[ "$DO_CHECKKEY" -eq 0 && -n "$KEY" ]]; then
	echo "Invalid option: --key"
	exit 1
fi

if [[ "$DO_CHECKKEY" -eq 1 ]]; then
	OURKEY="$KEY"
fi

# Verify valid digits in keys.
# Convert hex keys to lowercase (to match output of sg_persist)
# Convert decimal keys (without 0x prefix) to hex strings with 0x prefix.
# Leading 0s are not allowed because sg_persist drops them in output, so
# subsequent string matching of keys fails.

DECDIGITS='^[0-9]+$'
HEXDIGITS='^[0-9a-zA-Z]+$'

if [[ -n "$OURKEY" && "$OURKEY" == "0x0"* ]]; then
	echo "Leading 0s are not permitted in keys."
	exit 1
fi

if [[ -n "$REMKEY" && "$REMKEY" == "0x0"* ]]; then
	echo "Leading 0s are not permitted in keys."
	exit 1
fi


if [[ -n "$OURKEY" && "$OURKEY" != "0x"* ]]; then
	if [[ "$OURKEY" =~ $DECDIGITS ]]; then
		OURKEY=$(printf '%x\n' "$OURKEY")
		OURKEY=0x${OURKEY}
		if [[ -n "$KEY" ]]; then
			echo "Using key: $OURKEY"
		else
			echo "Using ourkey: $OURKEY"
		fi
	else
		echo "Invalid decimal digits in key: $OURKEY (use 0x prefix for hex key)"
		exit 1
	fi
fi

if [[ -n "$OURKEY" && "$OURKEY" == "0x"* ]]; then
	if [[ ! "$OURKEY" =~ $HEXDIGITS ]]; then
		echo "Invalid hex digits in key: $OURKEY"
		exit 1
	fi
	OURKEY="${OURKEY,,}"
fi

if [[ -n "$REMKEY" && "$REMKEY" != "0x"* ]]; then
	if [[ "$REMKEY" =~ $DECDIGITS ]]; then
		REMKEY=$(printf '%x\n' "$REMKEY")
		REMKEY=0x${REMKEY}
		echo "Using removekey: $REMKEY"
	else
		echo "Invalid decimal digits in key: $REMKEY (use 0x prefix for hex key)"
		exit 1
	fi
fi

if [[ -n "$REMKEY" && "$REMKEY" == "0x"* ]]; then
	if [[ ! "$REMKEY" =~ $HEXDIGITS ]]; then
		echo "Invalid hex digits in key: $REMKEY"
		exit 1
	fi
	REMKEY="${REMKEY,,}"
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
		# TODO: figure out the model of usage when
		# the reservation holder goes away.
		echo "WERO is not yet supported."
		exit 1
		;;
	EARO)
		# Exclusive Access - registrants only
		PRDESC=EARO
		PRTYPE=4
		SCSI_PRTYPE=6
		NVME_PRTYPE=4
		# TODO: figure out the model of usage when
		# the reservation holder goes away.
		echo "EARO is not yet supported."
		exit 1
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

if [[ "$DO_START" -eq 1 && -n "$REMKEY" ]]; then
	check_device_types
	do_takeover
elif [[ "$DO_START" -eq 1 ]]; then
	check_device_types
	do_start
elif [[ "$DO_STOP" -eq 1 ]]; then
	do_stop
elif [[ "$DO_REMOVE" -eq 1 ]]; then
	do_remove
elif [[ "$DO_CLEAR" -eq 1 ]]; then
	do_clear
elif [[ "$DO_DEVTEST" -eq 1 ]]; then
	check_device_types
	do_devtest
elif [[ "$DO_CHECKKEY" -eq 1 ]]; then
	check_device_types
	do_checkkey
elif [[ "$DO_READKEYS" -eq 1 ]]; then
	check_device_types
	do_readkeys
elif [[ "$DO_READRESERVATION" -eq 1 ]]; then
	check_device_types
	do_readreservation
elif [[ "$DO_READ" -eq 1 ]]; then
	check_device_types
	do_readkeys
	do_readreservation
fi

