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

#
# Main
#
aux have_vdo 6 2 0 || skip
aux have_fs ext4 || skip

aux prepare_pvs 2 6400
get_devs

#aux lvmconf 'allocation/vdo_use_read_cache = 1' 'allocation/vdo_read_cache_size_mb = 64'

#aux lvmconf 'allocation/vdo_use_compression = 0' 'allocation/vdo_use_deduplication = 0'

#aux lvmconf 'allocation/vdo_hash_zone_threads = 0' \
#	'allocation/vdo_logical_threads = 0' \
#	'allocation/vdo_physical_threads = 0' \
#	'allocation/vdo_cpu_threads = 1'

vgcreate $SHARED -s 64K "$vg" "${DEVICES[@]}"

# Create VDO device  (vdo-pool is ATM internal volume type)
lvcreate --type vdo -L4G -n $lv1 $vg/$lv2 >out 2>&1
# new vdoformat prints some more info
if grep "data slabs" out ; then
	# check we have match vdo_slab_size_mb == 128MB (aux.sh)
	# accept new & old vdoformat line (128.00 MB & 128 MB & 128.00 MiB)
	grep -E "each 128.*M.?B" out
fi

check lv_field $vg/$lv1 size "<1.24g"
check lv_field $vg/${lv2} size "4.00g"
check lv_field $vg/${lv2}_vdata size "4.00g"
check lv_field $vg/${lv1} data_percent "0.00"
lvremove -ff $vg

# Slab size must be a power of two: non-power-of-two values are rounded up
lvcreate --test --vdo -L4G -V8G -n $lv1 $vg/$lv2 \
	--vdosettings 'vdo_slab_size_mb=300' >out 2>&1
grep -E "Rounding VDO slab size up from 300.*M.?B to 512.*M.?B" out

# Upper bound: 20000 rounds up to the 32GiB maximum
lvcreate --test --vdo -L4G -V8G -n $lv1 $vg/$lv2 \
	--vdosettings 'vdo_slab_size_mb=20000' >out 2>&1
grep -E "Rounding VDO slab size up from .* to 32.*G.?B" out

# Powers of two (including the maximum) are left alone
for s in 256 32768 ; do
	lvcreate --test --vdo -L4G -V8G -n $lv1 $vg/$lv2 \
		--vdosettings "vdo_slab_size_mb=$s" >out 2>&1
	not grep "Rounding VDO slab size" out
done

# Above the maximum is rejected, not rounded
not lvcreate --test --vdo -L4G -V8G -n $lv1 $vg/$lv2 \
	--vdosettings 'vdo_slab_size_mb=33000' >out 2>&1
grep "out of range" out
not grep "Rounding VDO slab size" out

# Real creation uses the rounded size
lvcreate --vdo -L4G -V8G -n $lv1 $vg/$lv2 \
	--vdosettings 'vdo_slab_size_mb=300'
check lv_field $vg/$lv2 vdo_slab_size "512.00m"
lvremove -ff $vg

# Non-power-of-two slab size from lvm.conf is rounded up as well
aux lvmconf 'allocation/vdo_slab_size_mb = 300'
lvcreate --vdo -L4G -V8G -n $lv1 $vg/$lv2 >out 2>&1
grep -E "Rounding VDO slab size up from 300.*M.?B to 512.*M.?B" out
check lv_field $vg/$lv2 vdo_slab_size "512.00m"
lvremove -ff $vg

# --vdosettings overrides lvm.conf, a power of two is not rounded
lvcreate --vdo -L4G -V8G -n $lv1 $vg/$lv2 \
	--vdosettings 'vdo_slab_size_mb=256' >out 2>&1
not grep "Rounding VDO slab size" out
check lv_field $vg/$lv2 vdo_slab_size "256.00m"
lvremove -ff $vg

# Restore the default slab size set by aux.sh
aux lvmconf 'allocation/vdo_slab_size_mb = 128'


lvcreate --vdo -L4G -V8G -n $lv1 $vg/$lv2
check lv_field $vg/$lv1 size "8.00g"
check lv_field $vg/${lv2} size "4.00g"
check lv_field $vg/${lv2}_vdata size "4.00g"
lvs -a $vg

dmsetup table | grep $vg
dmsetup status | grep $vg

# Resize not yet supported
not lvresize -y $vg/$lv1
not lvresize -y $vg/${lv2}
not lvresize -y $vg/${lv2}_vdata

# Discard is very slow with VDO ATM so try to avoid it
#time blkdiscard "$DM_DEV_DIR/$vg/$lv1"
time mkfs.ext4 -E nodiscard "$DM_DEV_DIR/$vg/$lv1"
#time mkfs.ext4 "$DM_DEV_DIR/$vg/$lv1"
fsck -n "$DM_DEV_DIR/$vg/$lv1"

# vpool itself is NOT usable filesystem
not fsck -n "$DM_DEV_DIR/mapper/$vg-${lv2}"
# not usable even when there is no linear mapping on top of it
aux dmsetup remove ${vg}-$lv1
not fsck -n "$DM_DEV_DIR/mapper/$vg-${lv2}"

lvremove -ff $vg

# Unknown settings does not pass
# TODO: try to catch this in parser and 'fail'
not lvcreate --type vdo --vdosettings 'ack_Xthreads=4' -L10G -V1T -ky -n $lv1 $vg

lvcreate --type vdo --vdosettings 'ack_threads=4' -L10G -V1T -ky -n $lv1 $vg
check lv_field $vg/$lv1 vdo_ack_threads "4"
lvs -a $vg
lvremove -ff $vg

lvcreate --type vdo --vdosettings 'minimum_io_size=512' -L10G -V1T -ky -n $lv1 $vg
check lv_field $vg/$lv1 vdo_minimum_io_size "512b"
lvremove -ff $vg

lvcreate --type vdo --vdosettings 'minimum_io_size=4096' -L10G -V1T -ky -n $lv1 $vg
check lv_field $vg/$lv1 vdo_minimum_io_size "4.00k"
lvremove -ff $vg

# only 512 or 4096 are valid values  (and eventually 1 or 8 sectors)
not lvcreate --type vdo --vdosettings 'minimum_io_size=8000' -L10G -V1T -ky -n $lv1 $vg

vgremove -ff $vg
