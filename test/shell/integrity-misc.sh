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


. lib/inittest --skip-with-lvmpolld

aux have_fs ext4 || skip
aux have_integrity 1 5 0 || skip
# Avoid 4K ramdisk devices on older kernels
aux kernel_at_least  5 10 || export LVM_TEST_PREFER_BRD=0

mnt="mnt"
mkdir -p "$mnt"

aux prepare_devs 5 64

# Use awk instead of anoyingly long log out from printf
#printf "%0.sA" {1..16384} >> fileA
awk 'BEGIN { while (z++ < 16384) printf "A" }' > fileA
awk 'BEGIN { while (z++ < 16384) printf "B" }' > fileB
awk 'BEGIN { while (z++ < 16384) printf "C" }' > fileC

# generate random data
dd if=/dev/urandom of=randA bs=512K count=2
dd if=/dev/urandom of=randB bs=512K count=3
dd if=/dev/urandom of=randC bs=512K count=4

_prepare_vg() {
	vgcreate $SHARED $vg "$dev1" "$dev2" "$dev3" "$dev4" "$dev5"
	pvs
}

_add_new_data_to_mnt() {
        mkfs.ext4 -b 4096 "$DM_DEV_DIR/$vg/$lv1"

        mount "$DM_DEV_DIR/$vg/$lv1" $mnt

        # add original data
        cp randA $mnt
        cp randB $mnt
        cp randC $mnt
        mkdir $mnt/1
        cp fileA $mnt/1
        cp fileB $mnt/1
        cp fileC $mnt/1
        mkdir $mnt/2
        cp fileA $mnt/2
        cp fileB $mnt/2
        cp fileC $mnt/2
}

_add_more_data_to_mnt() {
        mkdir $mnt/more
        cp fileA $mnt/more
        cp fileB $mnt/more
        cp fileC $mnt/more
        cp randA $mnt/more
        cp randB $mnt/more
        cp randC $mnt/more
}

_verify_data_on_mnt() {
	diff randA $mnt/randA
	diff randB $mnt/randB
	diff randC $mnt/randC
	diff fileA $mnt/1/fileA
	diff fileB $mnt/1/fileB
	diff fileC $mnt/1/fileC
	diff fileA $mnt/2/fileA
	diff fileB $mnt/2/fileB
	diff fileC $mnt/2/fileC
}

_verify_data_on_lv() {
        lvchange -ay $vg/$lv1
        mount "$DM_DEV_DIR/$vg/$lv1" $mnt
        _verify_data_on_mnt
        rm $mnt/randA
        rm $mnt/randB
        rm $mnt/randC
        rm -rf $mnt/1
        rm -rf $mnt/2
        umount $mnt
        lvchange -an $vg/$lv1
}

# lvrename
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
umount "$mnt"
lvrename $vg/$lv1 $vg/$lv2
mount "$DM_DEV_DIR/$vg/$lv2" "$mnt"
_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv2
lvremove $vg/$lv2
vgremove -ff $vg

# lvconvert --replace
# an existing dev is replaced with another dev
# lv must be active
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -o raidintegritymode $vg/$lv1 | grep journal
_add_new_data_to_mnt
lvconvert --replace "$dev1" $vg/$lv1 "$dev3"
lvs -a -o+devices $vg > out
cat out
grep "$dev2" out
grep "$dev3" out
not grep "$dev1" out
_add_more_data_to_mnt
_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# lvconvert --replace
# same as prev but with bitmap mode
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y --raidintegritymode bitmap -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
lvs -o raidintegritymode $vg/$lv1 | grep bitmap
_add_new_data_to_mnt
lvconvert --replace "$dev1" $vg/$lv1 "$dev3"
lvs -a -o+devices $vg > out
cat out
grep "$dev2" out
grep "$dev3" out
not grep "$dev1" out
_add_more_data_to_mnt
_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
_verify_data_on_lv
lvremove $vg/$lv1
vgremove -ff $vg

# lvconvert --repair
# while lv is active a device goes missing (with rimage,rmeta,imeta,orig).
# lvconvert --repair should replace the missing dev with another,
# (like lvconvert --replace does for a dev that's not missing).
_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
aux disable_dev "$dev2"
lvs -a -o+devices $vg > out
cat out
grep unknown out
lvconvert -vvvv -y --repair $vg/$lv1
lvs -a -o+devices $vg > out
cat out
not grep "$dev2" out
not grep unknown out
_add_more_data_to_mnt
_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
lvremove $vg/$lv1
aux enable_dev "$dev2"
vgremove -ff $vg

# lvchange activationmode
# a device is missing (with rimage,rmeta,imeta,iorig), the lv
# is already inactive, and it cannot be activated, with
# activationmode degraded or partial, or in any way,
# until integrity is removed.

_prepare_vg
lvcreate --type raid1 -m1 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
umount "$mnt"
lvchange -an $vg/$lv1
aux disable_dev "$dev2"
lvs -a -o+devices $vg
not lvchange -ay $vg/$lv1
not lvchange -ay --activationmode degraded $vg/$lv1
not lvchange -ay --activationmode partial $vg/$lv1
lvconvert --raidintegrity n $vg/$lv1
lvchange -ay --activationmode degraded $vg/$lv1
mount "$DM_DEV_DIR/$vg/$lv1" "$mnt"
_add_more_data_to_mnt
_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
lvremove $vg/$lv1
aux enable_dev "$dev2"
vgremove -ff $vg

# When disks for raid images are missing, vgreduce --removemissing --force
# should remove the missing images from the raid LV.
_prepare_vg
lvcreate --type raid1 -m2 --raidintegrity y -n $lv1 -l 8 $vg "$dev1" "$dev2" "$dev3"
aux wait_recalc $vg/${lv1}_rimage_0
aux wait_recalc $vg/${lv1}_rimage_1
aux wait_recalc $vg/${lv1}_rimage_2
aux wait_recalc $vg/$lv1
_add_new_data_to_mnt
aux disable_dev "$dev2"
lvs -a -o+devices,segtype $vg |tee out
# The vgreduce uses error target for missing image
vgreduce --removemissing --force $vg
lvs -a -o+devices,segtype $vg |tee out
cat out
not grep "$dev2" out
grep error out
_add_more_data_to_mnt
_verify_data_on_mnt
umount "$mnt"
lvchange -an $vg/$lv1
lvremove $vg/$lv1
aux enable_dev "$dev2"
vgremove -ff $vg

# Create a raid1 with per-image integrity over all three devices.
_create_raid1_with_integrity() {
	lvcreate --yes --type raid1 -m 2 --nosync -l 8 --raidintegrity y -n $lv1 $vg "$dev1" "$dev2" "$dev3"
	aux wait_recalc $vg/${lv1}_rimage_0
	aux wait_recalc $vg/${lv1}_rimage_1
	aux wait_recalc $vg/${lv1}_rimage_2
	aux wait_recalc $vg/$lv1
}

# A reduction that fails once the temporary names are committed must put the
# original names back, so check that on an array of its own rather than
# inside the loop over the image being removed.
_test_extraction_name_collision() {
	# Reserved component names can only be introduced through metadata,
	# where the LV name is the key of its block.
	lvcreate -an -l 1 -n $lv2 $vg "$dev4"
	vgcfgbackup -f backup $vg
	sed -e "s/^\(\t*\)$lv2 {/\1${lv1}_rimage__0_extracted {/" backup >backupnew
	grep "${lv1}_rimage__0_extracted {" backupnew
	not grep "$lv2 {" backupnew
	lvchange -an $vg/$lv1
	vgcfgrestore -f backupnew $vg
	lvchange -ay $vg/$lv1
	lvs -a --noheadings -o lv_name,lv_uuid,segtype,devices $vg > before
	vgs --noheadings -o vg_seqno $vg > before-seqno
	dmsetup table -S "name=~^$vg-" | sort > before-table
	test -s before-table
	not lvconvert --yes -m 1 $vg/$lv1 "$dev1"
	lvs -a --noheadings -o lv_name,lv_uuid,segtype,devices $vg > after
	vgs --noheadings -o vg_seqno $vg > after-seqno
	dmsetup table -S "name=~^$vg-" | sort > after-table
	diff before after
	# The temporary names were loaded into the kernel by the pre-flight,
	# so the rollback has to reload the original names as well.
	diff before-table after-table
	# The reduction failed after the pre-flight committed its temporary
	# names, so the rollback has to write the metadata back and the seqno
	# moves on. It must not be replayed from the rollback metadata copy, which
	# would put a lower seqno than the one already on disk.
	seqno_after=$(cat after-seqno)
	seqno_before=$(cat before-seqno)
	test $seqno_after -gt $seqno_before
	lvremove -ff $vg/${lv1}_rimage__0_extracted
}

# Reduce raid image count on raid1 with integrity (RHEL-242389)
pvcreate -ff -y "$dev2"
_prepare_vg

_create_raid1_with_integrity
_test_extraction_name_collision
lvremove -ff $vg/$lv1

for remove_dev in "$dev1" "$dev2" "$dev3"; do
	_create_raid1_with_integrity

	# Rejected requests must leave names, layout and metadata seqno intact.
	lvs -a --noheadings -o lv_name,lv_uuid,segtype,devices $vg > before
	vgs --noheadings -o vg_seqno $vg > before-seqno
	not lvconvert --yes -m 0 $vg/$lv1
	not lvconvert --yes -m 1 $vg/$lv1 "$dev4"
	lvs -a --noheadings -o lv_name,lv_uuid,segtype,devices $vg > after
	vgs --noheadings -o vg_seqno $vg > after-seqno
	diff before after
	diff before-seqno after-seqno

	lvconvert --yes -m 1 $vg/$lv1 "$remove_dev"
	lvs -a -o+devices $vg | tee out
	not grep "$remove_dev" out
	for keep_dev in "$dev1" "$dev2" "$dev3"; do
		test "$keep_dev" = "$remove_dev" || grep "$keep_dev" out
	done
	for index in 0 1; do
		check lv_field $vg/${lv1}_rimage_$index segtype integrity -a
		check lv_field $vg/${lv1}_rimage_${index}_imeta segtype linear -a
		check lv_field $vg/${lv1}_rimage_${index}_iorig segtype linear -a
		check lv_field $vg/${lv1}_rmeta_$index segtype linear -a
	done
	dmsetup info -c -o name > dm-info
	not grep "${vg}-${lv1}_rimage_.*extracted" dm-info
	not grep "${vg}-${lv1}_rimage__" dm-info
	lvchange -an $vg/$lv1
	lvchange -ay $vg/$lv1
	lvremove -ff $vg/$lv1
done
vgremove -ff $vg
