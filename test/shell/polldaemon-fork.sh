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

# Verify the background polldaemon releases the label scan and its I/O
# engine before become_daemon() forks.

. lib/inittest --skip-with-lvmlockd --skip-with-lvmpolld

which strace || skip
aux with_sanitizer && skip

aux prepare_vg 2

lvcreate -aey -l1 -n $lv1 $vg "$dev1"

# The libaio engine has distinct io_setup/io_destroy syscalls.  Make sure
# libaio is available and actually selected before tracing the daemon.
pvs -vvv --config 'global/io_engine="libaio"' "$dev1" >out 2>&1 || true
grep -q 'Created.*libaio' out || skip

rm -f trace.out
LVM_TEST_TAG="kill_me_$PREFIX" strace -f -o trace.out \
	-e trace=io_setup,io_destroy,clone,clone3,fork,vfork \
	pvmove -b --config 'global/io_engine="libaio"' "$dev1" "$dev2"

# The parent must destroy the label scan's aio context before forking.
awk '
{
	if (fork_seen)
		next
	if (/io_setup\(/)
		setups++
	if (/io_destroy\(/)
		destroys++
	if (/(clone|clone3|fork|vfork)\(/)
		fork_seen = 1
}
END {
	if (!setups || !fork_seen || setups != destroys)
		exit 1
}
' trace.out || die "label scan I/O engine was not destroyed before fork"

aux kill_tagged_processes
pvmove --abort 2>/dev/null || true
vgremove -ff $vg
