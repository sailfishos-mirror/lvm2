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

test_description='devicesfile'

. lib/inittest

test -e "$LVM_TEST_DEVICE_LIST" || skip

num_devs=$(cat $LVM_TEST_DEVICE_LIST | wc -l)

aux prepare_real_devs

aux lvmconf 'devices/dir = "/dev"'
aux lvmconf 'devices/use_devicesfile = 1'
DFDIR="$LVM_SYSTEM_DIR/devices"
DF="$DFDIR/system.devices"
mkdir $DFDIR
not ls $DF

get_real_devs

wipe_all() {
	for dev in "${REAL_DEVICES[@]}"; do
		wipefs -a $dev
	done
}

wipe_all

# check each dev is added correctly to df

for dev in "${REAL_DEVICES[@]}"; do
	pvcreate $dev

	ls $DF

	pvs -o+uuid $dev
	maj=$(get pv_field "$dev" major)
	min=$(get pv_field "$dev" minor)
	pvid=`pvs $dev --noheading -o uuid | tr -d - | awk '{print $1}'`

	sys_wwid_file="/sys/dev/block/$maj:$min/device/wwid"
	sys_serial_file="/sys/dev/block/$maj:$min/device/serial"
	sys_dm_uuid_file="/sys/dev/block/$maj:$min/dm/uuid"
	sys_md_uuid_file="/sys/dev/block/$maj:$min/md/uuid"
	sys_loop_file="/sys/dev/block/$maj:$min/loop/backing_file"

	if test -e $sys_wwid_file; then
		sys_file=$sys_wwid_file
		idtype="sys_wwid"
	elif test -e $sys_serial_file; then
		sys_file=$sys_serial_file
		idtype="sys_serial"
	elif test -e $sys_dm_uuid_file; then
		sys_file=$sys_dm_uuid_file
		idtype="mpath_uuid"
	elif test -e $sys_md_uuid_file; then
		sys_file=$sys_md_uuid_file
		idtype="md_uuid"
	elif test -e $sys_loop_file; then
		sys_file=$sys_loop_file
		idtype="loop_file"
	else
		echo "no id type for device"
		skip
	fi

	idname=$(< $sys_file)

	rm -f idline
	grep IDNAME=$idname $DF | tee idline
	grep IDTYPE=$idtype idline
	grep DEVNAME=$dev idline
	grep PVID=$pvid idline
done

cp $DF df2

# vgcreate from existing pvs, already in df

vgcreate $vg ${REAL_DEVICES[@]}

vgremove $vg
rm $DF

# vgcreate from existing pvs, adding to df

vgcreate $vg ${REAL_DEVICES[@]}

grep IDNAME $DF > df.ids
grep IDNAME df2 > df2.ids
diff df.ids df2.ids

# check device id metadata fields

for dev in "${REAL_DEVICES[@]}"; do
	grep $dev $DF
	deviceid=`pvs $dev --noheading -o deviceid | awk '{print $1}'`
	deviceidtype=`pvs $dev --noheading -o deviceidtype | awk '{print $1}'`
	grep $dev $DF | grep $deviceid
	grep $dev $DF | grep $deviceidtype
	lvcreate -l1 $vg $dev
done

vgchange -an $vg
vgremove -y $vg

# check pvremove leaves devs in df but without pvid

for dev in "${REAL_DEVICES[@]}"; do
	maj=$(get pv_field "$dev" major)
	min=$(get pv_field "$dev" minor)
	pvid=`pvs $dev --noheading -o uuid | tr -d - | awk '{print $1}'`

	pvremove $dev
	grep $dev $DF
	not grep $pvid $DF
done

# Many of remaining tests require two devices
test $num_devs -gt 1 || skip

# check vgextend adds new dev to df, vgreduce leaves dev in df

rm $DF

vgcreate $vg $dev1
vgextend $vg $dev2
grep $dev1 $DF
grep $dev2 $DF
id1=`pvs $dev1 --noheading -o deviceid | awk '{print $1}'`
id2=`pvs $dev2 --noheading -o deviceid | awk '{print $1}'`
grep $id1 $DF
grep $id2 $DF
vgreduce $vg $dev2
grep $dev2 $DF
vgremove $vg

# check devs are not visible to lvm until added to df

rm $DF

# df needs to exist otherwise devicesfile feature turned off
touch $DF

not pvs $dev1
not pvs $dev2
pvs -a |tee all
not grep $dev1 all
not grep $dev2 all
not grep $dev1 $DF
not grep $dev2 $DF

pvcreate $dev1

pvs $dev1
not pvs $dev2
pvs -a |tee all
grep $dev1 all
not grep $dev2 all
grep $dev1 $DF
not grep $dev2 $DF

pvcreate $dev2

pvs $dev1
pvs $dev2
pvs -a |tee all
grep $dev1 all
grep $dev2 all
grep $dev1 $DF
grep $dev2 $DF

vgcreate $vg $dev1

pvs $dev1
pvs $dev2
pvs -a |tee all
grep $dev1 all
grep $dev2 all
grep $dev1 $DF
grep $dev2 $DF

vgextend $vg $dev2

pvs $dev1
pvs $dev2
pvs -a |tee all
grep $dev1 all
grep $dev2 all
grep $dev1 $DF
grep $dev2 $DF

# check vgimportdevices VG

rm $DF
wipe_all

vgcreate $vg ${REAL_DEVICES[@]}
rm $DF
touch $DF

for dev in "${REAL_DEVICES[@]}"; do
	not pvs $dev
done

vgimportdevices $vg

for dev in "${REAL_DEVICES[@]}"; do
	pvs $dev
done

# check vgimportdevices -a

rm $DF
wipe_all

vgcreate $vg1 $dev1
vgcreate $vg2 $dev2

rm $DF

vgimportdevices -a

vgs $vg1
vgs $vg2

pvs $dev1
pvs $dev2

# check vgimportclone --importdevices

rm $DF
wipe_all

vgcreate $vg1 $dev1
vgimportdevices $vg1

dd if=$dev1 of=$dev2 bs=1M count=1

pvs $dev1
not pvs $dev2

grep $dev1 $DF
not grep $dev2 $DF

not vgimportclone $dev2

not grep $dev2 $DF

vgimportclone --basevgname $vg2 --importdevices $dev2

pvid1=`pvs $dev1 --noheading -o uuid | tr -d - | awk '{print $1}'`
pvid2=`pvs $dev2 --noheading -o uuid | tr -d - | awk '{print $1}'`
test "$pvid1" != "$pvid2" || die "same uuid"

id1=`pvs $dev1 --noheading -o deviceid | tr -d - | awk '{print $1}'`
id2=`pvs $dev2 --noheading -o deviceid | tr -d - | awk '{print $1}'`
test "$id1" != "$id2" || die "same device id"

grep $dev1 $DF
grep $dev2 $DF
grep $pvid1 $DF
grep $pvid2 $DF
grep $id1 $DF
grep $id2 $DF

vgs $vg1
vgs $vg2

#
# check lvmdevices
#

wipe_all
rm $DF

# set up pvs and save pvids/deviceids
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvcreate $dev
	vgcreate ${vg}_${count} $dev
	pvid=`pvs $dev --noheading -o uuid | tr -d - | awk '{print $1}'`
	did=`pvs $dev --noheading -o deviceid | awk '{print $1}'`
	echo dev $dev pvid $pvid did $did
	PVIDS[$count]=$pvid
	DEVICEIDS[$count]=$did
	count=$((  count + 1 ))
done

rm $DF
not lvmdevices
touch $DF
lvmdevices

# check lvmdevices --adddev
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	not pvs $dev
	lvmdevices --adddev $dev
	lvmdevices |tee out
	grep $dev out |tee idline
	grep $pvid idline
	grep $did idline
	grep $dev $DF
	pvs $dev
	count=$((  count + 1 ))
done

# check lvmdevices --deldev
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	pvs $dev
	lvmdevices --deldev $dev
	lvmdevices |tee out
	not grep $dev out
	not grep $pvid out
	not grep $did out
	not grep $dev $DF
	not pvs $dev
	count=$((  count + 1 ))
done

# check lvmdevices --addpvid
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	not pvs $dev
	lvmdevices --addpvid $pvid
	lvmdevices |tee out
	grep $dev out |tee idline
	grep $pvid idline
	grep $did idline
	grep $dev $DF
	pvs $dev
	count=$((  count + 1 ))
done

# check lvmdevices --delpvid
count=0
for dev in "${REAL_DEVICES[@]}"; do
	pvid=${PVIDS[$count]}
	did=${DEVICEIDS[$count]}
	pvs $dev
	lvmdevices --delpvid $pvid
	lvmdevices |tee out
	not grep $dev out
	not grep $pvid out
	not grep $did out
	not grep $dev $DF
	not pvs $dev
	count=$((  count + 1 ))
done

# check lvmdevice --check and --update
rm $DF
pvid1=${PVIDS[0]}
did1=${DEVICEIDS[0]}
lvmdevices --adddev $dev1
lvmdevices --adddev $dev2
cp $DF $DF.orig
rm $DF
sed "s/$pvid1/badpvid/" "$DF.orig" |tee $DF
not grep $pvid1 $DF
grep $did1 $DF

lvmdevices --check |tee out
grep $dev1 out
grep badpvid out
grep $pvid1 out
not grep $dev2 out

lvmdevices |tee out
grep $dev1 out |tee out1
grep badpvid out1
not grep $pvid1 out1
grep $dev2 out

lvmdevices --update

lvmdevices |tee out
grep $dev1 out
grep $dev2 out
not grep badpvid
grep $pvid1 out
grep $did1 out
grep $pvid1 $DF
grep $did1 $DF

#TODO: corrupt and repair did, devname


# check --devicesfile "" and use_devicesfile=0

# check non-default --devicesfile

