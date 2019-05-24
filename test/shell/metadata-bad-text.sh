#!/usr/bin/env bash

# Copyright (C) 2008-2013,2018 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/inittest

aux prepare_devs 3
get_devs

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

dd if="$dev1" of=meta1 bs=4k count=2

sed 's/flags =/flagx =/' meta1 > meta1.bad

dd if=meta1.bad of="$dev1"

pvs 2>&1 | tee out
grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in one mda doesn't prevent using
# the VG since other mdas are fine and usable
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg


#
# Same test as above, but corrupt metadata text
# on two of the three PVs, leaving one good
# copy of the metadata.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

dd if="$dev1" of=meta1 bs=4k count=2
dd if="$dev2" of=meta2 bs=4k count=2

sed 's/READ/RRRR/' meta1 > meta1.bad
sed 's/seqno =/sss =/' meta2 > meta2.bad

dd if=meta1.bad of="$dev1"
dd if=meta2.bad of="$dev2"

pvs 2>&1 | tee out
grep "bad metadata text" out > out2
grep "$dev1" out2
grep "$dev2" out2

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in one mda doesn't prevent using
# the VG since other mdas are fine
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg

#
# Three PVs where two have one mda, and the third
# has two mdas.  The first mda is corrupted on all
# thee PVs, but the second mda on the third PV
# makes the VG usable.
#

dd if=/dev/zero of="$dev1" || true
dd if=/dev/zero of="$dev2" || true
dd if=/dev/zero of="$dev3" || true

pvcreate "$dev1"
pvcreate "$dev2"
pvcreate --pvmetadatacopies 2 "$dev3"

vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3"

pvs

dd if="$dev1" of=meta1 bs=4k count=2
dd if="$dev2" of=meta2 bs=4k count=2
dd if="$dev3" of=meta3 bs=4k count=2

sed 's/READ/RRRR/' meta1 > meta1.bad
sed 's/seqno =/sss =/' meta2 > meta2.bad
sed 's/id =/id/' meta3 > meta3.bad

dd if=meta1.bad of="$dev1"
dd if=meta2.bad of="$dev2"
dd if=meta3.bad of="$dev3"

pvs 2>&1 | tee out
grep "bad metadata text" out > out2
grep "$dev1" out2
grep "$dev2" out2
grep "$dev3" out2

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

# bad metadata in some mdas doesn't prevent using
# the VG if there's a good mda found
lvcreate -l1 $vg


vgck --updatemetadata $vg

pvs 2>&1 | tee out
not grep "bad metadata text" out

pvs "$dev1"
pvs "$dev2"
pvs "$dev3"

vgchange -an $vg
vgremove -ff $vg

