#!/usr/bin/env bash

# Copyright (C) 2020 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='devices file with devnames'

. lib/inittest

aux lvmconf 'devices/hints = "none"'

aux prepare_devs 7

RUNDIR="/run"
test -d "$RUNDIR" || RUNDIR="/var/run"
PVS_ONLINE_DIR="$RUNDIR/lvm/pvs_online"
VGS_ONLINE_DIR="$RUNDIR/lvm/vgs_online"
PVS_LOOKUP_DIR="$RUNDIR/lvm/pvs_lookup"

_clear_online_files() {
        # wait till udev is finished
        aux udev_wait
        rm -f "$PVS_ONLINE_DIR"/*
        rm -f "$VGS_ONLINE_DIR"/*
        rm -f "$PVS_LOOKUP_DIR"/*
}

DFDIR="$LVM_SYSTEM_DIR/devices"
mkdir $DFDIR
DF="$DFDIR/system.devices"
ORIG="$DFDIR/orig.devices"

#
# Test with use_devicesfile=0 (no devices file is being applied by default)
#

aux lvmconf 'devices/use_devicesfile = 1'

not ls $DF
pvcreate $dev1
ls $DF
grep $dev1 $DF
grep IDTYPE=devname $DF

pvcreate $dev2
grep $dev2 $DF

pvcreate $dev3
grep $dev3 $DF

vgcreate $vg1 $dev1 $dev2

# PVID with dashes for matching pvs -o+uuid output
OPVID1=`pvs $dev1 --noheading -o uuid | awk '{print $1}'`
OPVID2=`pvs $dev2 --noheading -o uuid | awk '{print $1}'`
OPVID3=`pvs $dev3 --noheading -o uuid | awk '{print $1}'`

# PVID without dashes for matching devices file fields
PVID1=`pvs $dev1 --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID2=`pvs $dev2 --noheading -o uuid | tr -d - | awk '{print $1}'`
PVID3=`pvs $dev3 --noheading -o uuid | tr -d - | awk '{print $1}'`

lvmdevices --deldev $dev3

not grep $dev3 $DF
not grep $PVID3 $DF
not pvs $dev3

cp $DF $ORIG

lvcreate -l4 -an -i2 -n $lv1 $vg1

#
# when wrong idname devname is outside DF it's corrected if search_for=1
# by a general cmd, or by lvmdevices --addpvid
#
# when wrong idname devname is outside DF it's not found or corrected if
# search_for=0 by a general cmd, but will be by lvmdevices --addpvid
#
# when wrong idname devname is inside DF it's corrected if search_for=0|1
# by a general cmd, or by lvmdevices --addpvid
# 
# pvscan --cache -aay does not update DF when devname= is wrong
#
# pvscan --cache -aay when idname devname is wrong:
# every dev is read and then skipped if pvid is not in DF
#
# commands still work with incorrect devname=
# . and they automatically correct the devname=
#


#
# idname changes to become incorrect, devname remains unchanged and correct
# . change idname to something outside DF
# . change idname to match another DF entry
# . swap idname of two DF entries
#

# edit DF idname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" $ORIG > $DF
cat $DF
# pvs reports correct info 
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep $OPVID3 out
not grep $dev3 out
grep $OPVID1 out |tee out2
grep $dev1 out2
# pvs fixed the DF
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
cat $DF

sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" $ORIG > $DF
cat $DF
# lvcreate uses correct dev
lvcreate -l1 -n $lv2 -an $vg1 $dev1
# lvcreate fixed the DF
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
# pvs reports correct dev
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep $OPVID3 out
not grep $dev3 out
grep $OPVID1 out |tee out2
grep $dev1 out2
lvremove $vg1/$lv2
cat $DF

sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" $ORIG > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
cat $DF

# edit DF idname, s/dev1/dev2/, creating two entries with same idname

sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" $ORIG > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
# pvs fixed the DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
cat $DF

sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" $ORIG > $DF
cat $DF
# lvcreate uses correct dev
lvcreate -l1 -n $lv2 -an $vg1 $dev1
# lvcreate fixed the DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
lvremove $vg1/$lv2
cat $DF

sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" $ORIG > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
cat $DF

# edit DF idname, swap dev1 and dev2

sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp2.devices > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
# pvs fixed the DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
cat $DF

sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp2.devices > $DF
cat $DF
# lvcreate uses correct dev
lvcreate -l1 -n $lv2 -an $vg1 $dev1
# lvcreate fixed the DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
lvremove $vg1/$lv2
cat $DF

sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp2.devices > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
cat $DF


#
# idname remains correct, devname changes to become incorrect
# . change devname to something outside DF
# . change devname to match another DF entry
# . swap devname of two DF entries
#

# edit DF devname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" $ORIG > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep $OPVID3 out
not grep $dev3 out
grep $OPVID1 out |tee out2
grep $dev1 out2
# pvs fixed the DF
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
cat $DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" $ORIG > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
cat $DF

# edit DF devname, s/dev1/dev2/, creating two entries with same devname

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" $ORIG > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
# pvs fixed the DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep $PVID2 $DF |tee out
grep DEVNAME=$dev2 out
cat $DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" $ORIG > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
cat $DF

# edit DF devname, swap dev1 and dev2

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
# pvs fixed the DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep $PVID2 $DF |tee out
grep DEVNAME=$dev2 out
cat $DF

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
grep $PVID1 $DF |tee out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep IDNAME=$dev2 out
cat $DF


#
# idname and devname change, both become incorrect
# . change idname&devname to something outside DF
# . change idname&devname to match another DF entry
# . swap idname&devname of two DF entries
#

# edit DF idname&devname, s/dev1/dev3/, where new dev is not in DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" $ORIG > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" tmp1.devices > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
not grep $OPVID3 out
not grep $dev3 out
grep $OPVID1 out |tee out2
grep $dev1 out2
# pvs fixed the DF
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep IDNAME=$dev1 out
cat $DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev3|" $ORIG > tmp1.devices
sed -e "s|IDNAME=$dev1|IDNAME=$dev3|" tmp1.devices > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
not grep $PVID3 $DF
not grep $dev3 $DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep IDNAME=$dev1 out
cat $DF

# edit DF idname&devname, s/dev1/dev2/, creating two entries with same devname

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" tmp1.devices > $DF
sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" tmp1.devices > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
# pvs fixed the DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep DEVNAME=$dev2 out
grep IDNAME=$dev2 out
cat $DF

sed -e "s|DEVNAME=$dev1|DEVNAME=$dev2|" tmp1.devices > $DF
sed -e "s|IDNAME=$dev1|IDNAME=$dev2|" tmp1.devices > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep DEVNAME=$dev2 out
grep IDNAME=$dev2 out
cat $DF

# edit DF devname, swap dev1 and dev2

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > tmp3.devices
sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" tmp3.devices > tmp4.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp4.devices > tmp5.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp5.devices > $DF
cat $DF
# pvs reports correct info
pvs -o+uuid | tee pvs.out
grep $vg1 pvs.out > out
grep $OPVID1 out |tee out2
grep $dev1 out2
grep $OPVID2 out |tee out2
grep $dev2 out2
# pvs fixed the DF
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep DEVNAME=$dev2 out
grep IDNAME=$dev2 out
cat $DF

sed -e "s|DEVNAME=$dev1|DEVNAME=tmpname|" $ORIG > tmp1.devices
sed -e "s|DEVNAME=$dev2|DEVNAME=$dev1|" tmp1.devices > tmp2.devices
sed -e "s|DEVNAME=tmpname|DEVNAME=$dev2|" tmp2.devices > tmp3.devices
sed -e "s|IDNAME=$dev1|IDNAME=tmpname|" tmp3.devices > tmp4.devices
sed -e "s|IDNAME=$dev2|IDNAME=$dev1|" tmp4.devices > tmp5.devices
sed -e "s|IDNAME=tmpname|IDNAME=$dev2|" tmp5.devices > $DF
cat $DF
# lvmdevices fixes the DF
lvmdevices --update
grep $PVID1 $DF |tee out
grep DEVNAME=$dev1 out
grep IDNAME=$dev1 out
grep $PVID2 $DF |tee out
grep DEVNAME=$dev2 out
grep IDNAME=$dev2 out
cat $DF


#
# other tests:
# pvscan --cache -aay when idname and/or devname are wrong
# DF entry for device that's not a PV which changes name
# check hint file is correct when devnames are changing
# test with/without hints enabled
# s/dev1/dev3/ where dev3 is outside DF and is not a PV
# find case where df is updated in both validate and find_renamed_devs
# get_hints skips hints because unmatched device ids
# validate_hints skips hints because invalid device ids
# partitions of mpath and loop
#

vgremove -ff $vg1
