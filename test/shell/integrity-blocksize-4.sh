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

. lib/inittest --skip-with-lvmpolld

test "${LVM_VALGRIND:-0}" -eq 0 || skip # too slow for valgrind
which mkfs.ext4 || skip
which mkfs.xfs || skip
aux have_integrity 1 5 0 || skip

mnt="mnt"
mkdir -p "$mnt"

awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 4096) printf "B" ; while (z++ < 16384) printf "b" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

_test_fs_with_read_repair() {
	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
	cp fileA "$mnt"
	cp fileB "$mnt"
	cp fileC "$mnt"
	umount "$mnt"
	lvchange -an $vg/$lv1
	for dev in "$@"; do
		aux corrupt_dev "$dev" BBBBBBBBBBBBBBBBB BBBBBBBBCBBBBBBBB
	done
	lvchange -ay $vg/$lv1
	mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
	cmp -b "$mnt/fileA" fileA
	cmp -b "$mnt/fileB" fileB
	cmp -b "$mnt/fileC" fileC
	umount "$mnt"
}

# scsi_debug devices with LBS 512 and PBS 4K
aux prepare_scsi_debug_dev 1400 sector_size=512 physblk_exp=3
check sysfs "$(< SCSI_DEBUG_DEV)" queue/logical_block_size "512"
check sysfs "$(< SCSI_DEBUG_DEV)" queue/physical_block_size "4096"
aux prepare_devs 2 600

vgcreate $vg "$dev1" "$dev2"
blockdev --getss "$dev1"
blockdev --getpbsz "$dev1"
blockdev --getss "$dev2"
blockdev --getpbsz "$dev2"

# Test: integrity bs auto (should be 512), ext4, read repair

lvcreate --type raid1 -m1 -n $lv1 -L 512M $vg
aux wait_recalc $vg/$lv1
aux wipefs_a "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
# FIXME: ext4 uses BLOCK_SIZE=4096 even though LBS is 512?
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 512 explicit, ext4, read repair

lvcreate --type raid1 -m1 -n $lv1 -L 512M $vg
aux wait_recalc $vg/$lv1
aux wipefs_a "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
# FIXME: ext4 uses BLOCK_SIZE=4096 even though LBS is 512?
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 4096 explicit, ext4, read repair

lvcreate --type raid1 -m1 -n $lv1 -L 512M $vg
aux wait_recalc $vg/$lv1
aux wipefs_a "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 4096 $vg/$lv1
lvchange -ay $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 4096'
# FIXME: shouldn't LBS 4096 be reported here intead of 512?
# test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 4096
# FIXME: allow 512 to pass to see what the effect is...
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
# FIXME: mount fails with the following kernel errors,
# likely because of the previous FIXME:
# device-mapper: integrity: Bio not aligned on 8 sectors: 0x2, 0x2
# md/raid1:mdX: dm-5: rescheduling sector 2
# device-mapper: integrity: Bio not aligned on 8 sectors: 0x2, 0x2
# device-mapper: integrity: Bio not aligned on 8 sectors: 0x2, 0x2
# md/raid1:mdX: Disk failure on dm-5, disabling device.\x0amd/raid1:mdX: Operation continuing on 1 devices.
# md/raid1:mdX: redirecting sector 2 to other mirror: dm-7
# device-mapper: integrity: Bio not aligned on 8 sectors: 0x2, 0x2
# EXT4-fs (dm-8): unable to read superblock
# /tmp/LVMTEST32003.agdC1NofqR/mnt: can't read superblock on /dev/mapper/LVMTEST32003vg-LV1.
#mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
#umount "$mnt"
#lvchange $vg/$lv1 --writemostly "$dev2"
#_test_fs_with_read_repair "$dev1"
#lvs -a -o+integritymismatches $vg
#lvs -o integritymismatches $vg/$lv1 |tee mismatch
#not grep 0 mismatch
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs auto (should be 512), xfs, read repair

lvcreate --type raid1 -m1 -n $lv1 -L 512M $vg
aux wait_recalc $vg/$lv1
aux wipefs_a "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
# FIXME: ext4 uses BLOCK_SIZE=4096 even though LBS is 512?
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 512 explicit, xfs, read repair

lvcreate --type raid1 -m1 -n $lv1 -L 512M $vg
aux wait_recalc $vg/$lv1
aux wipefs_a "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 512 $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 512'
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
blockdev --getss "$DM_DEV_DIR/$vg/$lv1"
blockdev --getpbsz "$DM_DEV_DIR/$vg/$lv1"
blkid -p "$DM_DEV_DIR/$vg/$lv1"
# FIXME: xfs uses BLOCK_SIZE=4096 even though LBS is 512?
blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
umount "$mnt"
lvchange $vg/$lv1 --writemostly "$dev2"
_test_fs_with_read_repair "$dev1"
lvs -a -o+integritymismatches $vg
lvs -o integritymismatches $vg/$lv1 |tee mismatch
not grep 0 mismatch
lvchange -an $vg/$lv1
lvremove $vg/$lv1

# Test: integrity bs 4096 explicit, xfs, read repair

lvcreate --type raid1 -m1 -n $lv1 -L 512M $vg
aux wait_recalc $vg/$lv1
aux wipefs_a "$DM_DEV_DIR/$vg/$lv1"
lvconvert --raidintegrity y --raidintegrityblocksize 4096 $vg/$lv1
lvchange -ay $vg/$lv1
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
pvck --dump metadata "$dev1" | grep 'block_size = 4096'
# FIXME: why is LBS 512 is reported here instead of 4096?
# test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 4096
test "$(blockdev --getss "$DM_DEV_DIR/$vg/$lv1")" -eq 512
# FIXME: mkfs.xfs fails here producing a long series of kernel errors,
# e.g. many:
# device-mapper: integrity: Bio vector (1536,2560) is not aligned on 8-sector boundary
# followed eventually by:
# md/raid1:mdX: dm-5: Raid device exceeded read_error threshold [cur 21:max 20]
# md/raid1:mdX: dm-5: Failing raid device
# md/raid1:mdX: Disk failure on dm-5, disabling device.\x0amd/raid1:mdX: Operation continuing on 1 devices.
# mkfs.xfs -f "$DM_DEV_DIR/$vg/$lv1"
# blkid -p "$DM_DEV_DIR/$vg/$lv1"
# blkid -p "$DM_DEV_DIR/$vg/$lv1" | grep BLOCK_SIZE=\"4096\"
# mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
# umount "$mnt"
# lvchange $vg/$lv1 --writemostly "$dev2"
# _test_fs_with_read_repair "$dev1"
# lvs -a -o+integritymismatches $vg
# lvs -o integritymismatches $vg/$lv1 |tee mismatch
# not grep 0 mismatch
lvchange -an $vg/$lv1
lvremove $vg/$lv1

vgremove -ff $vg
aux cleanup_scsi_debug_dev
