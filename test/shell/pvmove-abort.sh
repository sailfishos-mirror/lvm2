#!/usr/bin/env bash

# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Check pvmove --abort behaviour when specific device is requested


. lib/inittest --skip-with-lvmlockd

aux lvmconf 'activation/raid_region_size = 16'

aux target_at_least dm-mirror 1 2 0 || skip

aux prepare_pvs 3 90

vgcreate -s 512k $vg "$dev1" "$dev2"
pvcreate --metadatacopies 0 "$dev3"
vgextend $vg "$dev3"

for mode in "--atomic" "" ;
do
for backgroundarg in "-b" "" ;
do

# Create multisegment LV
lvcreate -an -Zn -l10 -n $lv1 $vg "$dev1"
lvcreate -an -Zn -l20 -n $lv2 $vg "$dev2"

rm -f debug.log_DEBUG*

cmd1=(pvmove -i +2 $backgroundarg $mode "$dev1" "$dev3")
cmd2=(pvmove -i +2 $backgroundarg $mode "$dev2" "$dev3")

if test -z "$backgroundarg" ; then
	"${cmd1[@]}" &
	PVMOVE1_PID=$!
	"${cmd2[@]}" &
	PVMOVE2_PID=$!
	# Per-PV abort needs two concurrent pvmoves; do not wait on fixed
	# pvmove0/pvmove1 names (see wait_pvmove_lv_started_in_vg in aux).
	aux wait_pvmove_lv_started_in_vg "$vg" "$PVMOVE1_PID" "$PVMOVE2_PID"
else
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd1[@]}"
	LVM_TEST_TAG="kill_me_$PREFIX" "${cmd2[@]}"
fi

# remove specific device
pvmove --abort "$dev1"

# check if proper pvmove was canceled (by source PV, not pvmove index)
lvs -a -S 'name=~"^pvmove[0-9]+$"' -o move_pv --noheadings "$vg" | sort -u | tee out
not grep -F "$(basename -- "$dev1")" out
grep -F "$(basename -- "$dev2")" out

# remove any remaining pvmoves in progress
pvmove --abort

lvremove -ff $vg

# kill pvmove polling processes, just to speed-up test run
# not required as pvmove would exit with:
# 'No pvmove in progress - already finished or aborted.'
if test -z "$backgroundarg" ; then
	kill "$PVMOVE1_PID" "$PVMOVE2_PID" 2>/dev/null || true
	wait "$PVMOVE1_PID" "$PVMOVE2_PID" 2>/dev/null || true
fi
aux kill_tagged_processes

done
done

wait

vgremove -ff $vg
