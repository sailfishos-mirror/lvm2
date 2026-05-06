#!/usr/bin/env bash
# Copyright (C) 2011-2025 Red Hat, Inc.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

. lib/paths

CMD=${0##*/}

# the exec is important, because otherwise fatal signals go unnoticed
# use 'exec -a' to preserve argv[0] for multi-call binary dispatch
if [[ -n "$abs_top_builddir" ]]; then
    exec -a "$CMD" "$abs_top_builddir/libdm/dm-tools/dmsetup" "$@"
else # we are testing the dmsetup on $PATH
    PATH=$(echo "$PATH" | sed -e 's,[^:]*lvm2-testsuite[^:]*:,,g')
    exec -a "$CMD" dmsetup "$@"
fi
