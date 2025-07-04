#!/usr/bin/env bash

# Copyright (C) 2008-2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Exercise fsadm filesystem resize'


. lib/inittest --skip-with-lvmpolld

aux prepare_vg 1 400

# set to "skip" to avoid testing given fs and test warning result
# i.e. check_reiserfs=skip
check_ext2=
check_ext3=
check_xfs=
check_reiserfs=

which mkfs.ext2 || check_ext2=${check_ext2:-mkfs.ext2}
which mkfs.ext3 || check_ext3=${check_ext3:-mkfs.ext3}
which fsck.ext3 || check_ext3=${check_ext3:-fsck.ext3}
which mkfs.xfs || check_xfs=${check_xfs:-mkfs.xfs}
which xfs_check || {
	which xfs_repair || check_xfs=${check_xfs:-xfs_repair}
}
grep xfs /proc/filesystems || check_xfs=${check_xfs:-no_xfs}

which mkfs.reiserfs || check_reiserfs=${check_reiserfs:-mkfs.reiserfs}
which reiserfsck || check_reiserfs=${check_reiserfs:-reiserfsck}
modprobe reiserfs || true
grep reiserfs /proc/filesystems || check_reiserfs=${check_reiserfs:-no_reiserfs}

vg_lv=$vg/$lv1
vg_lv2=$vg/${lv1}bar
dev_vg_lv="$DM_DEV_DIR/$vg_lv"
dev_vg_lv2="$DM_DEV_DIR/$vg_lv2"
mount_dir="mnt"
mount_space_dir="mnt space dir"

test ! -d "$mount_dir" && mkdir "$mount_dir"
test ! -d "$mount_space_dir" && mkdir "$mount_space_dir"

cleanup_mounted_and_teardown()
{
	umount "$mount_dir" 2>/dev/null || true
	umount "$mount_space_dir" 2>/dev/null || true
	aux teardown
}

fscheck_ext3()
{
	# fsck with result code '1' is success
	fsck.ext3 -p -F -f "$dev_vg_lv" || test "$?" -eq 1
}

fscheck_xfs()
{
	if which xfs_repair ; then
		xfs_repair -n "$dev_vg_lv"
	else
		xfs_check "$dev_vg_lv"
	fi
}

fscheck_reiserfs()
{
	reiserfsck --check -p -f "$dev_vg_lv" </dev/null
}

check_missing()
{
	local t
	eval "t=\$check_$1"
	test -z "$t" && return 0
	test "$t" = skip && return 1
	echo "WARNING: fsadm test skipped $1 tests, $t tool is missing."
	# trick to get test listed with warning
	# should false;
	return 1
}

# Test for block sizes != 1024 (rhbz #480022)
lvcreate -n $lv1 -L20M $vg
lvcreate -n ${lv1}bar -L10M $vg
trap 'cleanup_mounted_and_teardown' EXIT

# prints help
fsadm

# check needs arg
not fsadm check

# check needs arg
not fsadm resize "$dev_vg_lv" 30M |& tee out
grep "Cannot get FSTYPE" out

if check_missing ext2; then
	mkfs.ext2 -b4096 -j "$dev_vg_lv"

	# Check 'check' works
	fsadm check $vg_lv
	# Check 'resize' without size parameter works
	fsadm resize $vg_lv
	fsadm --lvresize resize $vg_lv 30M
	# Fails - not enough space for 4M fs
	not fsadm -y --lvresize resize "$dev_vg_lv" 4M
	lvresize -L+10M --fs resize_fsadm $vg_lv
	lvreduce -L10M --fs resize_fsadm $vg_lv

	fscheck_ext3
	mount "$dev_vg_lv" "$mount_dir"
	not fsadm -y --lvresize resize $vg_lv 4M
	echo n | not lvresize -L4M --fs resize_fsadm -n $vg_lv
	lvresize -L+20M --fs resize_fsadm -n $vg_lv
	umount "$mount_dir"
	fscheck_ext3

	lvresize --fs ignore -y -L20M $vg_lv

	if which debugfs ; then
		mkfs.ext2 -b4096 -j "$dev_vg_lv"
		mount "$dev_vg_lv" "$mount_dir"
		touch "$mount_dir/file"
		umount "$mount_dir"
		# generate a 'repariable' corruption
		# so fsck returns code 1  (fs repaired)
		debugfs -R "clri file" -w "$dev_vg_lv"

		fsadm -v -f check "$dev_vg_lv"

		# corrupting again
		mount "$dev_vg_lv" "$mount_dir"
		touch "$mount_dir/file"
		umount "$mount_dir"
		debugfs -R "clri file" -w "$dev_vg_lv"

		mount "$dev_vg_lv" "$mount_dir"
		fsadm -v -y --lvresize resize $vg_lv 10M
		lvresize -L+10M -y --fs resize_fsadm -n $vg_lv
		umount "$mount_dir" 2>/dev/null || true
		fscheck_ext3
	fi
fi

if check_missing ext3; then
	mkfs.ext3 -b4096 -j "$dev_vg_lv"
	mkfs.ext3 -b4096 -j "$dev_vg_lv2"

	fsadm --lvresize resize $vg_lv 30M
	# Fails - not enough space for 4M fs
	not fsadm -y --lvresize resize "$dev_vg_lv" 4M
	lvresize -L+10M --fs resize_fsadm $vg_lv
	lvreduce -L10M --fs resize_fsadm $vg_lv

	fscheck_ext3
	mount "$dev_vg_lv" "$mount_dir"
	lvresize -L+10M --fs resize_fsadm $vg_lv
	mount "$dev_vg_lv2" "$mount_space_dir"
	fsadm --lvresize -e -y resize $vg_lv2 25M

	not fsadm -y --lvresize resize $vg_lv 4M
	echo n | not lvresize -L4M -r -n $vg_lv
	lvresize -L+20M --fs resize_fsadm -n $vg_lv
	lvresize -L-10M --fs resize_fsadm -y $vg_lv
	umount "$mount_dir"
	umount "$mount_space_dir"
	fscheck_ext3

	lvresize --fs ignore -y -L20M $vg_lv
fi

if check_missing xfs; then
	lvresize -L 300M $vg_lv
	mkfs.xfs -l internal -f "$dev_vg_lv"

	fsadm --lvresize resize $vg_lv 320M
	# Fails - not enough space for 4M fs
	lvresize -L+10M --fs resize_fsadm $vg_lv
	not lvreduce -L10M --fs resize_fsadm $vg_lv

	fscheck_xfs
	mount "$dev_vg_lv" "$mount_dir"
	lvresize -L+10M --fs resize_fsadm -n $vg_lv
	umount "$mount_dir"
	fscheck_xfs

	lvresize --fs ignore -y -L20M $vg_lv
fi

if check_missing reiserfs; then
	mkfs.reiserfs -s 513 -f "$dev_vg_lv"

	fsadm --lvresize resize $vg_lv 30M
	lvresize -L+10M --fs resize_fsadm $vg_lv
	fsadm --lvresize -y resize $vg_lv 10M

	fscheck_reiserfs
	mount "$dev_vg_lv" "$mount_dir"

	fsadm -y --lvresize resize $vg_lv 30M
	umount "$mount_dir"
	fscheck_reiserfs

	lvresize --fs ignore -y -L20M $vg_lv
fi

vgremove -ff $vg
