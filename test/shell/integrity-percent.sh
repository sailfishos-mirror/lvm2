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

# The eight 64MiB PVs are carved out of one backing device, which needs about
# 514MiB; declare the footprint so a host without it skips instead of failing.
SKIP_WITH_LOW_SPACE=520

. lib/inittest --skip-with-lvmpolld

aux have_integrity 1 5 0 || skip

aux prepare_devs 8 64

# 8 PVs of 64MiB with 3MiB extents hold 21 extents each, 168 in total.
vgcreate $SHARED -s 3m $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8"

# Wait until the integrity recalculation of the @count rimages of @name has
# finished, then list the integrity metadata LVs that sit next to them.
# The VG name is taken as @vgname so it does not shadow the global $vg, which
# is not the VG used by every call below.
wait_raid_integrity_synced() {
	local vgname=$1
	local name=$2
	local count=$3
	local i
	local imeta_lvs=()

	for i in $(seq 0 $(( count - 1 ))); do
		aux wait_recalc "$vgname/${name}_rimage_$i"
		imeta_lvs+=("$vgname/${name}_rimage_${i}_imeta")
	done
	lvs -a -o name,segtype "${imeta_lvs[@]}"
}

# Percent sizing must leave room for the per-image RAID and integrity metadata
# LVs, which are created after the request has been sized.
#
# raid1 with two images gets 168 / 2 = 84 extents per image.  One of them is the
# RAID metadata and three are the integrity metadata, so the data is reduced
# from 84 to 80 extents, i.e. 80 * 3MiB = 240MiB.  The integrity metadata of a
# 240MiB image needs (240 / 500 + 1) * 4MiB for the data plus the 4MiB initial
# journal, i.e. 8MiB rounded up to whole 3MiB extents = 9MiB.
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 100%VG $vg
wait_raid_integrity_synced "$vg" "$lv1" 2
check lv_field "$vg/$lv1" lv_size 240.00 --units m --nosuffix
check lv_field "$vg/${lv1}_rimage_0_imeta" lv_size 9.00 --units m --nosuffix

lvremove -ff $vg/$lv1

# raid10 with 2 mirrors and 2 stripes has 4 images: the stripe count is doubled
# for the layout and halved again by the parameter check, so the image count is
# mirrors * stripes = 4.  Each image may use 168 / 4 = 42 extents; after 1 RAID
# metadata and 3 integrity metadata extents that leaves 38 data extents per
# image, and a request of E extents needs E / 2 per image, so E is reduced to
# 76 extents, i.e. 76 * 3MiB = 228MiB.
lvcreate --type raid10 --raidintegrity y -n $lv2 -l 100%FREE -i 2 $vg
wait_raid_integrity_synced "$vg" "$lv2" 4
check lv_field "$vg/$lv2" lv_size 228.00 --units m --nosuffix

lvremove -ff $vg/$lv2

# raid5 with 3 data stripes and 1 parity device has 4 images, each allowed
# 168 / 4 = 42 extents.  After 1 RAID metadata and 3 integrity metadata extents
# that leaves 38 data extents per image, so the LV keeps 3 * 38 = 114 extents
# = 342MiB.
lvcreate --type raid5 --raidintegrity y -n $lv3 -l 100%VG -i 3 $vg
wait_raid_integrity_synced "$vg" "$lv3" 4
check lv_field "$vg/$lv3" lv_size 342.00 --units m --nosuffix

lvremove -ff $vg/$lv3

# An explicit journal size replaces the size buckets in
# _lv_size_bytes_to_integrity_meta_bytes(), so the metadata is no longer the
# 4MiB of the first scenario.  A 12MiB journal is 24576 sectors, exactly 4 whole
# 3MiB extents; the data-derived part is (240 / 500 + 1) * 4MiB = 4MiB, so the
# metadata needs 4MiB + 12MiB = 16MiB, i.e. 6 whole 3MiB extents.  raid1 may use
# 168 / 2 = 84 extents per image, leaving 84 - 1 - 6 = 77 data extents, so the
# LV is 77 * 3MiB = 231MiB with a 6 * 3MiB = 18MiB metadata LV.
lvcreate --type raid1 -m1 --raidintegrity y --integritysettings "journal_sectors=24576" \
	-n $lv4 -l 100%VG $vg
wait_raid_integrity_synced "$vg" "$lv4" 2
check lv_field "$vg/$lv4" lv_size 231.00 --units m --nosuffix
check lv_field "$vg/${lv4}_rimage_0_imeta" lv_size 18.00 --units m --nosuffix

lvremove -ff $vg/$lv4

# Leave a different number of free extents on each PV before percentage sizing.
# The reserves total 33 of the 168 extents, leaving 135 free: not a multiple of
# the 4 images, so the fit check reduces the LV to 58 extents = 174MiB and
# leaves three extents unused instead of depending on an exact packing.
pv_devs=("$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8")
reserves=(1 2 3 4 7 6 5 5)
for i in "${!pv_devs[@]}"; do
	lvcreate -an -Zn -l "${reserves[$i]}" -n ${lv2}_reserve_$i $vg "${pv_devs[$i]}"
done

lvcreate --type raid10 --raidintegrity y -n $lv2 -l 100%FREE -i 2 $vg
wait_raid_integrity_synced "$vg" "$lv2" 4
check lv_field "$vg/$lv2" lv_size 174.00 --units m --nosuffix

vgremove -ff $vg

# The fit must size the per-image RAID metadata from the extent count the
# allocator uses, not from the per-image data extent count.  For raid10 the
# allocator sizes the metadata from the total LV extent count while each image
# holds only half of it, and the two counts can fall in different metadata
# extents.  Sizing from the smaller count under-reserves by one extent, the fit
# approves a request the allocator cannot place, and lvcreate fails after the
# point of no return.
#
# Reaching that boundary means the metadata LV must step up one extent, which
# happens when the bitmap bits outgrow the fixed 8KiB header.  The step falls
# at 128 regions, so 128 extents of 4KiB, the smallest extent RAID allows on a
# 4KiB page host.  A 64KiB page host has no extent below 64KiB and would need
# 3.9M extents, about 240GiB, to reach the same step, and not even the smallest
# legal region size brings that into a testable LV size.  The scenario is
# therefore 4KiB-only by arithmetic, not by choice.
if test "$(getconf PAGESIZE)" = 4096 ; then
	vgcreate $SHARED -s 4k $vg2 "$dev1" "$dev2" "$dev3" "$dev4"

	# Leave 2140 free extents on every PV.  With four 4KiB extents per 512KiB
	# region, raid10 with two stripes has four images, one per PV, and each
	# image gets a 2140 extent budget.  The integrity metadata LV needs 2048
	# extents (4MiB of data-derived metadata plus the 4MiB initial journal),
	# so an image with E extents needs E / 2 data, rmeta and 2048 extents.
	#
	# The allocator gives rmeta three extents once the total extent count
	# reaches 178 (179136 bytes over 512KiB regions rounds into the second
	# region), while the per-image count of 89 still needs only two.  The fit
	# may therefore keep at most 178 extents: 89 + 3 + 2048 = 2140.  Sizing
	# rmeta from 89 instead lets the fit keep 180, and the allocator then
	# needs 90 + 3 + 2048 = 2141 and fails.
	pe_count=$(pvs --noheadings -o pv_pe_count "$dev1" | tr -d ' \n')
	fill=$(( pe_count - 2140 ))
	meta_devs=("$dev1" "$dev2" "$dev3" "$dev4")
	for i in "${!meta_devs[@]}"; do
		lvcreate -an -Zn -l "$fill" -n ${lv2}_fill_$i $vg2 "${meta_devs[$i]}"
	done

	lvcreate --type raid10 -i 2 --raidintegrity y -n $lv2 -l 100%FREE $vg2

	# 178 extents of 4KiB, with three metadata extents per image.
	check lv_field "$vg2/$lv2" lv_size 712.00 --units k --nosuffix
	check lv_field "$vg2/${lv2}_rmeta_0" lv_size 12.00 --units k --nosuffix
	check lv_field "$vg2/${lv2}_rimage_0_imeta" lv_size 8192.00 --units k --nosuffix

	vgremove -ff $vg2
fi

# A 1MiB extent size divides the 4MiB accounting unit, so the metadata size is
# already a whole number of extents and must not be rounded up again.  8 PVs of
# 64MiB with 1MiB extents hold 63 extents each, 504 in total; raid1 with two
# images may use 504 / 2 = 252 extents each.  The image needs 4MiB of data
# metadata plus the 4MiB initial journal, i.e. exactly 8 extents, leaving
# 252 - 1 - 8 = 243 data extents, so the LV is 243MiB with an 8MiB metadata LV.
vgcreate $SHARED -s 1m $vg2 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8"
lvcreate --type raid1 -m1 --raidintegrity y -n $lv5 -l 100%VG $vg2
wait_raid_integrity_synced "$vg2" "$lv5" 2
check lv_field "$vg2/$lv5" lv_size 243.00 --units m --nosuffix
check lv_field "$vg2/${lv5}_rimage_0_imeta" lv_size 8.00 --units m --nosuffix

vgremove -ff $vg2

# raid6 with 3 data stripes and 2 parity devices needs 5 images, one image
# per PV here.  5 PVs of 64MiB with 3MiB extents hold 21 extents each, 105 in
# total, so each image may use 105 / 5 = 21 extents.  After 1 RAID metadata
# and 3 integrity metadata extents that leaves 17 data extents per image, and
# the LV is reduced to 3 * 17 = 51 extents = 153MiB.
vgcreate $SHARED -s 3m $vg2 "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
lvcreate --type raid6 --raidintegrity y -n $lv4 -l 100%VG -i 3 $vg2
wait_raid_integrity_synced "$vg2" "$lv4" 5
check lv_field "$vg2/$lv4" lv_size 153.00 --units m --nosuffix

vgremove -ff $vg2

# No extent per image fits, so the request cannot be reduced to a usable size
# and must fail without leaving a partial LV behind.
vgcreate $SHARED -s 3m $vg2 "$dev1" "$dev2"

lvcreate -an -Zn -l 40 -n ${lv1}_fill $vg2

not lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 100%FREE $vg2 >out 2>&1
grep -F "Not enough space for raid LV with integrity." out
check lv_not_exists $vg2 $lv1

vgremove -ff $vg2
