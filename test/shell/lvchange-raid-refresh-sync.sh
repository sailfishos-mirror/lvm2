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

# lvchange --refresh must tolerate a live raid table that still
# carries a dmsetup-injected "sync" flag after a forced resync.

. lib/inittest --skip-with-lvmpolld

# Injecting "sync" below reloads the table of a *live* raid device.
# That is only reliable on dm-raid >= 1.14: older kernels construct the
# new array already at table load (dm_table_add_target() calls the
# constructor) and hang or oops while the old array is torn down during
# the resume swap.  Same requirement as the other raid table reload
# test, lvconvert-raid-reshape-stripes-load-reload.sh.
aux have_raid 1 14 0 || skip
aux prepare_vg 2

lvcreate --yes --type raid1 -m 1 -n $lv1 -L 16M $vg
aux wait_for_sync $vg $lv1

# Inject "sync" into the active table (QE dm_force_raid_resync pattern).
# Table: <start> <len> raid <raid_type> <#params> <params...>
old=$(dmsetup table "$vg-$lv1")
new=$(echo "$old" | awk '{
	count = $5 + 1
	printf "%s %s %s %s %d %s sync", $1, $2, $3, $4, count, $6
	for (i = 7; i <= NF; i++)
		printf " %s", $i
	printf "\n"
}')
echo "$new" | dmsetup load "$vg-$lv1"
dmsetup suspend "$vg-$lv1"
dmsetup resume "$vg-$lv1"

aux wait_for_sync $vg $lv1

# Active table still has "sync"; LVM metadata does not.  Refresh must
# suppress the rejected reload rather than fail with EINVAL.  Only the
# active side is ignored, so a new table asking for "sync" still reloads.
dmsetup table "$vg-$lv1" | tee out
grep -q " sync " out
lvchange --refresh $vg/$lv1

vgremove -ff $vg
