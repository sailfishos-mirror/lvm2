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

# Check a device with an md ddf anchor header is recognized as md component


. lib/inittest --skip-with-lvmpolld

aux lvmconf 'devices/md_component_detection = 1'

aux prepare_devs 1

sectors=$(blockdev --getsz "$dev1")

# ddf anchor header is placed 512B or 128KB before the end of device
for i in 1 257 ; do
	# magic + standard CRC-32 (as written by mdadm) of the 512B header
	# with crc field set to 0xffffffff and remaining bytes zeroed
	{ printf '\xde\x11\xde\x11\x4d\x1e\x81\xa1' ; head -c 504 /dev/zero ; } | \
		dd of="$dev1" bs=512 iflag=fullblock seek=$(( sectors - i )) conv=fdatasync

	not pvcreate "$dev1" 2>&1 | tee err
	grep "device is an md component" err

	dd if=/dev/zero of="$dev1" bs=512 count=1 seek=$(( sectors - i )) conv=fdatasync
done

pvcreate "$dev1"
