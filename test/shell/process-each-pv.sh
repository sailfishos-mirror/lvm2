#!/usr/bin/env bash

# Copyright (C) 2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Exercise toollib process_each_pv'

. lib/inittest --skip-with-lvmpolld

aux prepare_devs 14

#
# process_each_pv is used by a number of pv commands:
# pvdisplay
# pvresize
# pvs
#
# process-each-pvresize.sh covers pvresize.
# process-each-vgreduce.sh covers vgreduce.
#


#
# set up
#
# use dev10 instead of dev1 because simple grep for
# dev1 matches dev10,dev11,etc
#

vgcreate $SHARED $vg1 "$dev10"
vgcreate $SHARED $vg2 "$dev2" "$dev3" "$dev4" "$dev5"
vgcreate $SHARED $vg3 "$dev6" "$dev7" "$dev8" "$dev9"

pvchange --addtag V2D3 "$dev3"
pvchange --addtag V2D4 "$dev4"
pvchange --addtag V2D45 "$dev4"
pvchange --addtag V2D5 "$dev5"
pvchange --addtag V2D45 "$dev5"

pvchange --addtag V3 "$dev6" "$dev7" "$dev8" "$dev9"
pvchange --addtag V3D9 "$dev9"

# orphan
pvcreate "$dev11"

# dev (a non-pv device)
pvcreate "$dev12"
pvremove "$dev12"

# dev13 is intentionally untouched so we can
# test that it is handled appropriately as a non-pv

# orphan
pvcreate "$dev14"


# check_pvs FILE expected-dev ...
# Verify each device in ALL_DEVS appears or not in FILE.
ALL_DEVS=("$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev10" "$dev11" "$dev12" "$dev13" "$dev14")

# Original bash & grep version
check_pvs_orig() {
	local file=$1
	shift
	local -A expected=()
	local d
	for d in "$@"; do
		expected["$d"]=1
	done
	for d in "${ALL_DEVS[@]}"; do
		if [[ ${expected["$d"]+set} ]]; then
			grep "$d" "$file" || die "Expected $d in $file"
		else
			not grep "$d" "$file" || die "Unexpected $d in $file"
		fi
	done
}

check_pvs() {
	local file=$1
	shift
	awk -v all="$(printf '%s\n' "${ALL_DEVS[@]}")" \
	    -v wanted="$(printf '%s\n' "$@")" '
	BEGIN {
		# Build set of expected devices from wanted list
		n = split(wanted, w, "\n")
		for (i = 1; i <= n; i++)
			if (w[i] != "") expect[w[i]] = 1
		# Everything in all but not in wanted is unwanted
		n = split(all, a, "\n")
		for (i = 1; i <= n; i++)
			if (!(a[i] in expect)) reject[a[i]] = 1
	}
	{
		for (d in expect) if (index($0, d)) found[d] = 1
		for (d in reject) if (index($0, d)) bad[d] = 1
	}
	END {
		for (d in expect)
			if (!(d in found))
				{ printf "Expected %s\n", d >"/dev/stderr"; e = 1 }
		for (d in bad)
			{ printf "Unexpected %s\n", d >"/dev/stderr"; e = 1 }
		exit (e ? 1 : 0)
	}' "$file" || { cat "$file" 2>/dev/null || true; die "Output file contains mismatched content!"; }
}


#
# test pvdisplay
#

# pv in vg
pvdisplay -s "$dev10" | tee err
check_pvs err "$dev10"

# pv not in vg (one orphan)
pvdisplay -s "$dev11" | tee err
check_pvs err "$dev11"

# dev is not a pv
not pvdisplay -s "$dev12" >err
check_pvs err

# two pvs in different vgs
pvdisplay -s "$dev10" "$dev2" | tee err
check_pvs err "$dev10" "$dev2"

# -a is invalid when used alone
not pvdisplay -a &>err
check_pvs err

# one pv and one orphan
pvdisplay -s "$dev10" "$dev11" | tee err
check_pvs err "$dev10" "$dev11"

# one pv and one dev (dev refers to a non-pv device)
not pvdisplay -s "$dev10" "$dev12" >err
check_pvs err "$dev10"

# one orphan and one dev
not pvdisplay -s "$dev11" "$dev12" >err
check_pvs err "$dev11"

# all pvs (pvs in vgs and orphan pvs)
pvdisplay -s | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev14"

# all devs (pvs in vgs, orphan pvs, and devs)
pvdisplay -a -C | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev12" "$dev13" "$dev14"

# pv and orphan and dev
not pvdisplay -s "$dev9" "$dev11" "$dev12" >err
check_pvs err "$dev9" "$dev11"

# -s option not allowed with -a -C
not pvdisplay -s -a -C >err
check_pvs err

# pv and all (all ignored)
pvdisplay -a -C "$dev9" | tee err
check_pvs err "$dev9"

# orphan and all (all ignored)
pvdisplay -a -C "$dev11" | tee err
check_pvs err "$dev11"

# one tag
pvdisplay -s @V2D3 | tee err
check_pvs err "$dev3"

# two tags
pvdisplay -s @V2D3 @V2D45 | tee err
check_pvs err "$dev3" "$dev4" "$dev5"

# tag and pv
pvdisplay -s @V2D3 "$dev4" | tee err
check_pvs err "$dev3" "$dev4"

# tag and orphan
pvdisplay -s @V2D3 "$dev11" | tee err
check_pvs err "$dev3" "$dev11"

# tag and dev
not pvdisplay -s @V2D3 "$dev12" >err
check_pvs err "$dev3"

# tag and all (all ignored)
pvdisplay @V2D3 -a -C | tee err
check_pvs err "$dev3"

# tag and pv redundant
pvdisplay -s @V2D3 "$dev3" | tee err
check_pvs err "$dev3"


#
# test pvs
#

# pv in vg
pvs "$dev10" | tee err
check_pvs err "$dev10"

# pv not in vg (one orphan)
pvs "$dev11" | tee err
check_pvs err "$dev11"

# dev is not a pv
not pvs "$dev12" >err
check_pvs err

# two pvs in different vgs
pvs "$dev10" "$dev2" | tee err
check_pvs err "$dev10" "$dev2"

# one pv and one orphan
pvs "$dev10" "$dev11" | tee err
check_pvs err "$dev10" "$dev11"

# one pv and one dev
not pvs "$dev10" "$dev12" >err
check_pvs err "$dev10"

# one orphan and one dev
not pvs "$dev11" "$dev12" >err
check_pvs err "$dev11"

# all pvs (pvs in vgs and orphan pvs)
pvs | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev14"

# all devs (pvs in vgs, orphan pvs, and devs)
pvs -a | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev12" "$dev13" "$dev14"

# pv and orphan and dev
not pvs "$dev9" "$dev11" "$dev12" >err
check_pvs err "$dev9" "$dev11"

# pv and all (all ignored)
pvs -a "$dev9" | tee err
check_pvs err "$dev9"

# orphan and all (all ignored)
pvs -a "$dev11" | tee err
check_pvs err "$dev11"

# one tag
pvs @V2D3 | tee err
check_pvs err "$dev3"

# two tags
pvs @V2D3 @V2D45 | tee err
check_pvs err "$dev3" "$dev4" "$dev5"

# tag and pv
pvs @V2D3 "$dev4" | tee err
check_pvs err "$dev3" "$dev4"

# tag and orphan
pvs @V2D3 "$dev11" | tee err
check_pvs err "$dev3" "$dev11"

# tag and dev
not pvs @V2D3 "$dev12" >err
check_pvs err "$dev3"

# tag and all (all ignored)
pvs @V2D3 -a | tee err
check_pvs err "$dev3"

# tag and pv redundant
pvs @V2D3 "$dev3" | tee err
check_pvs err "$dev3"


#
# tests including pvs without mdas
#

# remove old config
vgremove $vg1
vgremove $vg2
vgremove $vg3
pvremove "$dev11"
pvremove "$dev14"

# new config with some pvs that have zero mdas

# for vg1
pvcreate "$dev10"

# for vg2
pvcreate "$dev2" --metadatacopies 0
pvcreate "$dev3"
pvcreate "$dev4"
pvcreate "$dev5"

# for vg3
pvcreate "$dev6" --metadatacopies 0
pvcreate "$dev7" --metadatacopies 0
pvcreate "$dev8" --metadatacopies 0
pvcreate "$dev9"

# orphan with mda
pvcreate "$dev11"
# orphan without mda
pvcreate "$dev14" --metadatacopies 0

# non-pv devs
# dev12
# dev13

vgcreate $SHARED $vg1 "$dev10"
vgcreate $SHARED $vg2 "$dev2" "$dev3" "$dev4" "$dev5"
vgcreate $SHARED $vg3 "$dev6" "$dev7" "$dev8" "$dev9"

pvchange --addtag V2D3 "$dev3"
pvchange --addtag V2D4 "$dev4"
pvchange --addtag V2D45 "$dev4"
pvchange --addtag V2D5 "$dev5"
pvchange --addtag V2D45 "$dev5"

pvchange --addtag V3 "$dev6" "$dev7" "$dev8" "$dev9"
pvchange --addtag V3D8 "$dev8"
pvchange --addtag V3D9 "$dev9"


#
# pvdisplay including pvs without mdas
#

# pv with mda
pvdisplay -s "$dev10" | tee err
check_pvs err "$dev10"

# pv without mda
pvdisplay -s "$dev2" | tee err
check_pvs err "$dev2"

# orphan with mda
pvdisplay -s "$dev11" | tee err
check_pvs err "$dev11"

# orphan without mda
pvdisplay -s "$dev14" | tee err
check_pvs err "$dev14"

# pv with mda, pv without mda, orphan with mda, orphan without mda
pvdisplay -s "$dev10" "$dev2" "$dev11" "$dev14" | tee err
check_pvs err "$dev10" "$dev2" "$dev11" "$dev14"

# tag referring to pv with mda and pv without mda
pvdisplay -s @V3 | tee err
check_pvs err "$dev6" "$dev7" "$dev8" "$dev9"

# tag referring to one pv without mda
pvdisplay -s @V3D8 | tee err
check_pvs err "$dev8"

# all pvs (pvs in vgs and orphan pvs)
pvdisplay -s | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev14"

# all devs (pvs in vgs, orphan pvs, and devs)
pvdisplay -a -C | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev12" "$dev13" "$dev14"

#
# pvs including pvs without mdas
#

# pv with mda
pvs "$dev10" | tee err
check_pvs err "$dev10"

# pv without mda
pvs "$dev2" | tee err
check_pvs err "$dev2"

# orphan with mda
pvs "$dev11" | tee err
check_pvs err "$dev11"

# orphan without mda
pvs "$dev14" | tee err
check_pvs err "$dev14"

# pv with mda, pv without mda, orphan with mda, orphan without mda
pvs "$dev10" "$dev2" "$dev11" "$dev14" | tee err
check_pvs err "$dev10" "$dev2" "$dev11" "$dev14"

# tag referring to pv with mda and pv without mda
pvs @V3 | tee err
check_pvs err "$dev6" "$dev7" "$dev8" "$dev9"

# tag referring to one pv without mda
pvs @V3D8 | tee err
check_pvs err "$dev8"

# all pvs (pvs in vgs and orphan pvs)
pvs | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev14"

# all devs (pvs in vgs, orphan pvs, and devs)
pvs -a | tee err
check_pvs err "$dev10" "$dev2" "$dev3" "$dev4" "$dev5" "$dev6" "$dev7" "$dev8" "$dev9" "$dev11" "$dev12" "$dev13" "$dev14"

vgremove $vg1 $vg2 $vg3
