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

# Reproducer for stale signature wiping issue:
#   'vdoformat: checkForSignaturesUsingBlkid failed' attempting to create a
#   VDO volume on top of an LVM thin (virtual) volume.
#
# The stale xfs_external_log is a valid XLOG record header (magic 0xFEEDBABE,
# h_version 1|2, h_len > 0, h_fmt 1|2|3) at a 512 sector aligned offset in the
# first 256 KiB of a device that is at least XFS_MIN_LOG_BYTES (10 MiB).  That
# is exactly what libblkid's probe_xfs_log() accepts, and both LVM's wipe and
# the external vdoformat use the very same libblkid probe.
#
# The bug is not signature detection, it is visibility on thin storage:
#
#   wipe_lv() wipes signatures BEFORE it zeroes the first 4 KiB
#   (lib/metadata/lv_manip.c).  On a thin LV with -Z n (no zeroing on
#   provision) the chunk holding the signature may still be UNPROVISIONED when
#   the wipe runs, so blkid reads zeros and finds nothing.  The zeroing that
#   follows provisions that first chunk from the pool's free list, which is
#   where stale data from previously freed thin blocks still lives -- a real
#   xfs_external_log at offset 65536 becomes visible only after LVM's own
#   write.  vdoformat then probes the device and aborts.  A second lvcreate
#   succeeds because the chunk is provisioned by then, so the wipe sees it.
#
# To hit this the signature has to sit in a pool block that is (a) stale, i.e.
# freed but not zeroed, and (b) handed out for the chunk containing the VDO
# data device's offset 0, so that LVM's own 4 KiB zero is what uncovers it.
# Seed the whole pool with the header, free it and then build a fresh thin LV:
# every pool block is stale, so whatever block the nested VG metadata and the
# VDO data LV's first chunk end up on carries the signature.  The chunk has to
# be larger than 64 KiB so that offset 65536 shares a chunk with offset 0.
#
# The VDO data device also has to satisfy vdoformat's minimum size (about
# 2.84 GiB here), so the nested VG is made 3.5 GiB while its backing thin pool
# has spare room for vdoformat's own writes.
#
# Solved by: convert_vdo_pool_lv() now wipes signatures on the data LV once
# more, immediately before it runs the userspace vdoformat.  That re-check
# happens after the first chunk has been zeroed, i.e. once the stale signature
# is visible, so it is gone before vdoformat's own blkid check runs.  Without
# that extra wipe this test dies with
# 'vdoformat: checkForSignaturesUsingBlkid failed on ...'.

. lib/inittest --skip-with-lvmpolld

aux have_vdo 6 2 0 || skip
aux have_thin 1 0 0 || skip

# The signature check that fails lives in the userspace vdoformat tool;
# kernel-only formatting never calls blkid, so there is nothing to reproduce.
aux have_vdoformat || skip

which blkid || skip

# Match RHEL defaults: wipe blkid signatures when zeroing new LVs.  Keep
# discards off so that freed thin blocks retain the stale signature.
aux lvmconf 'allocation/vdo_slab_size_mb = 128' \
	'allocation/wipe_signatures_when_zeroing_new_lvs = 1' \
	'devices/issue_discards = 0'

aux prepare_vg 1 12000

# Build a 1 MiB pattern holding a valid XLOG record header at chunk-relative
# offset 65536.  libblkid's probe_xfs_log() accepts it (magic 0xFEEDBABE,
# version 1|2, h_len > 0, h_fmt 1|2|3) at any 512-byte aligned offset in the
# first 256 KiB of a device of at least 10 MiB.
dd if=/dev/zero of=pat bs=1M count=1 2>/dev/null
printf '\xfe\xed\xba\xbe' | dd of=pat bs=1 seek=65536 conv=notrunc 2>/dev/null
printf '\x00\x00\x00\x02' | dd of=pat bs=1 seek=65544 conv=notrunc 2>/dev/null
printf '\x00\x00\x02\x00' | dd of=pat bs=1 seek=65548 conv=notrunc 2>/dev/null
printf '\x00\x00\x00\x01' | dd of=pat bs=1 seek=65836 conv=notrunc 2>/dev/null

# Thin pool with zeroing of newly provisioned blocks DISABLED.  1 MiB chunks so
# that offset 65536 lives in the same chunk as offset 0, and the nested VG's
# 1 MiB metadata area is exactly one chunk.  The pool is larger than the VDO
# LV built on top of it so that vdoformat has room to write.
lvcreate -L4G --chunksize 1m -T $vg/pool -Z n

# Tile the pattern over the whole seed LV (4 GiB / 1 MiB chunks) so every pool
# block carries the signature, then drop it: the blocks are freed but not
# zeroed (-Z n, no discards) and become the pool's stale free list.
seed_dev="$DM_DEV_DIR/$vg/seed"
lvcreate -V4G -T $vg/pool -n seed
cat $(printf 'pat %.0s' {1..4096}) > "$seed_dev"
echo "## signature on the seed thin LV:"
blkid -p "$seed_dev" | tee seed.blkid || true
grep -q xfs_external_log seed.blkid || skip "seed LV does not carry xfs_external_log"
lvremove -f $vg/seed

# A fresh thin LV, so its chunks are unprovisioned until LVM writes to them.
# This is the nested VG's PV and mirrors the QE report.
lvcreate -V3584M -T $vg/pool -n origin

aux extend_filter_LVMTEST
aux extend_devices "$DM_DEV_DIR/$vg/origin"
aux lvmconf "devices/scan_lvs = 1"

vgcreate $SHARED "$vg1" "$DM_DEV_DIR/$vg/origin"

# Create the VDO LV.  LVM's wipe reads the still-unprovisioned chunk as zeros,
# then its own 4 KiB zero uncovers the stale xfs_external_log at offset 65536.
# Without the re-wipe added to convert_vdo_pool_lv() this lvcreate dies with
# 'checkForSignaturesUsingBlkid failed'; with it the signature is wiped before
# vdoformat looks and the volume is created.
lvcreate --vdosettings "use_kernel_format=0" --type vdo -l100%VG --yes -n $lv1 -V 256M -vvvv "$vg1" 2>err || {
	cat err
	die "vdoformat found a stale signature on $vg1/$lv1 (RHEL-185993)"
}

cat err
rm -f pat seed.blkid err
lvremove -f "$vg1/$lv1"
vgremove -ff "$vg1"
vgremove -ff $vg
