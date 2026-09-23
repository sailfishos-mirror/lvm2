#!/usr/bin/env bash

# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Exercise cache flushing is abortable



. lib/inittest --skip-with-lvmpolld

aux have_cache 1 3 0 || skip

aux prepare_vg

# Slow origin writeback so SIGINT can land during flush (zero origin is instant).
ORIGIN_DELAY_MS=30
SECTOR_SIZE=512
SECTORS_PER_MIB=2048
# dm-cache kernel status: dirty_blocks is field 14 (0-based index 13).
DM_CACHE_STATUS_DIRTY_IDX=13

ORIGIN_DEV=$dev3
ORIGIN_PE=$(( $(get pv_field "$ORIGIN_DEV" pv_pe_count) - $(get pv_field "$ORIGIN_DEV" pv_pe_alloc_count) ))
SIZE_MB=$(( ORIGIN_PE * SECTOR_SIZE / SECTORS_PER_MIB - 4 ))
test "$SIZE_MB" -gt 8 || SIZE_MB=8
lvcreate -L$((SIZE_MB * 2))M --type zero -n cpool $vg
lvconvert -y --type cache-pool --chunksize 32k $vg/cpool "$dev1"
lvcreate -l "$ORIGIN_PE" -n $lv1 $vg "$ORIGIN_DEV"
lvconvert -y -H --chunksize 32k --cachemode writeback --cachepool $vg/cpool $vg/$lv1

#
# Ensure cache gets promoted blocks
#
for i in $(seq 1 4) ; do
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=$SIZE_MB oflag=direct || true
dd if="$DM_DEV_DIR/$vg/$lv1" of=/dev/null bs=1M count=$SIZE_MB iflag=direct || true
done

aux delay_dev "$ORIGIN_DEV" 0 "$ORIGIN_DELAY_MS" "$(get first_extent_sector "$ORIGIN_DEV"):"
dd if=/dev/zero of="$DM_DEV_DIR/$vg/$lv1" bs=1M count=$SIZE_MB

lvdisplay --maps $vg

test "$(get lv_field $vg/$lv1 cache_dirty_blocks)" -gt 0 || {
	lvdisplay --maps $vg
	skip "Cannot make a dirty writeback cache LV."
}

# Tee lvconvert -vvvv into logconvert and the test log (fd 3).  Do not run
# other lvm tools while fd 3 is open: they inherit the pipe and may hang/leak.
exec 3> >(tee logconvert)
LVM_TEST_TAG="kill_me_$PREFIX" lvconvert -vvvv --splitcache $vg/$lv1 >&3 2>&1 &
PID_CONVERT=$!
saw_cleaner=0
sent_kill=0
for i in {1..200}; do
	out=$(dmsetup status --noflush "$vg-$lv1")
	if [[ "$out" =~ [[:space:]]cleaner[[:space:]] ]]; then
	    saw_cleaner=1
	    read -ra st <<< "$out"
	    dirty=${st[DM_CACHE_STATUS_DIRTY_IDX]:-0}
	    if test "$dirty" -gt 0; then
		kill -INT $PID_CONVERT 2>/dev/null || true
		sent_kill=1
		break
	    fi
	fi
	kill -0 $PID_CONVERT 2>/dev/null || break
	sleep 0.01
done
test "$saw_cleaner" -eq 1 || die "Waited for cleaner policy on $vg/$lv1 too long!"
test "$sent_kill" -eq 1 || die "Cache on $vg/$lv1 became clean before interrupt could be sent"

# extra time in case we are in some slow 'flushing' suspend
sleep 0.5
aux enable_dev "$ORIGIN_DEV"
wait "$PID_CONVERT" || true
# close 'tee' descriptor
exec 3>&-

#cat logconvert || true

# Problem of this test is, in older kernels, even the initial change to cleaner
# policy table line causes long suspend which in practice is cleaning all the
# dirty blocks - so the test can't really break the cache clearing.
#
# So the failure of test is reported only for recent kernels > 5.6
# and skipped otherwise - as those can't be fixed anyway
grep -E "Flushing.*aborted" logconvert || {
	cat logconvert || true
	vgremove -f $vg
	aux kernel_at_least 5 6 || skip "Cache missed to abort flushing with older kernel"
	die "Flushing of $vg/$lv1 not aborted ?"
}

# check the table got restored
check grep_dmsetup table $vg-$lv1 "writeback"
lvdisplay --maps $vg

vgremove -f $vg
