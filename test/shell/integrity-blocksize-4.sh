#!/usr/bin/env bash

# Copyright (C) 2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Test integrity block sizes and read repair on scsi_debug
# devices with LBS 512 and PBS 4K.

. shell/integrity-basic.sh
. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip # too slow for valgrind
aux have_fs ext4 || skip
aux have_fs xfs || skip
aux have_integrity 1 5 0 || skip

# Kernel 6.1 added "dm integrity: clear the journal on suspend" (984bf2cc531e).
# Before this fix, the default ~2MB journal retains valid entries across
# deactivation, so replay_journal() on reactivation silently restores data
# sectors that corrupt_dev damaged -- giving 0 mismatches.
aux kernel_at_least  6 1 || skip

mnt="mnt"
mkdir -p "$mnt"

integrity_make_files 4096

_test_fs_with_read_repair() {
	# This test creates the filesystem itself (to control its block size),
	# so pass an empty mkfs command; test_fs_with_read_repair() still
	# mounts, copies the data files and unmounts before deactivating.
	test_fs_with_read_repair "$vg/$lv1" "$mnt" "" "" "" "" "$@"
}

LVRAIDSIZE=${LVRAIDSIZE:-300}

# scsi_debug devices with LBS 512 and PBS 4K
aux prepare_scsi_debug_dev $((2 * (LVRAIDSIZE + 100) + 200)) sector_size=512 physblk_exp=3
check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "512"
check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "4096"
aux prepare_devs 2 $((LVRAIDSIZE + 100))

vgcreate $vg "$dev1" "$dev2"
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

# Test: integrity bs auto (should be 512), ext4, read repair

lvcreate --type raid1 -m1 -n $lv1 -L ${LVRAIDSIZE} $vg
aux wait_recalc $vg/$lv1
aux clear_devs "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 512 explicit, ext4, read repair

lvcreate --type raid1 -m1 -n $lv1 -L ${LVRAIDSIZE} $vg
aux wait_recalc $vg/$lv1
aux clear_devs "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 4096 explicit, ext4, read repair

lvcreate --type raid1 -m1 -n $lv1 -L ${LVRAIDSIZE} $vg
aux wait_recalc $vg/$lv1
aux clear_devs "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 4096 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 4096'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 4096
mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs auto (should be 512), xfs, read repair

lvcreate --type raid1 -m1 -n $lv1 -L ${LVRAIDSIZE} $vg
aux wait_recalc $vg/$lv1
aux clear_devs "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 512 explicit, xfs, read repair

lvcreate --type raid1 -m1 -n $lv1 -L ${LVRAIDSIZE} $vg
aux wait_recalc $vg/$lv1
aux clear_devs "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 4096 explicit, xfs, read repair

lvcreate --type raid1 -m1 -n $lv1 -L ${LVRAIDSIZE} $vg
aux wait_recalc $vg/$lv1
aux clear_devs "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 4096 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 4096'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 4096
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
integrity_not_zero_mismatches $vg/$lv1
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg
aux cleanup_scsi_debug_dev
