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
# engine before become_daemon() forks.  The libaio engine makes this
# visible in the trace: the label scan creates its context with
# io_setup(), so every such call must be matched by io_destroy() before
# the daemon forks and the child would inherit a dead context.

. lib/inittest --skip-with-lvmlockd --skip-with-lvmpolld

which strace || skip "strace not installed"
aux with_sanitizer && skip "sanitizer builds cannot be traced reliably"

aux prepare_vg 2

lvcreate -aey -l1 -n $lv1 $vg "$dev1"

# The libaio engine has distinct io_setup/io_destroy syscalls.  Make sure
# libaio is available and actually selected before tracing the daemon;
# another engine would leave no context to follow.
pvs -vvv --config 'global/io_engine="libaio"' "$dev1" >out 2>&1 || true
grep -q 'Created.*libaio' out || skip "libaio io engine not available"

# Old strace rejects the whole trace set when it does not know one of the
# names, e.g. clone3, and exits before tracing the command.  Probe each
# name once: both aio syscalls are required, while any one fork syscall is
# enough to find where the daemon forks.
aio_syscalls=(io_setup io_destroy)
fork_syscalls=(clone clone3 fork vfork)
trace_syscalls=()
fork_syscall_re=

for sc in "${aio_syscalls[@]}"; do
	strace -e trace="$sc" true >/dev/null 2>&1 ||
		skip "strace cannot trace $sc"
	trace_syscalls+=("$sc")
done

for sc in "${fork_syscalls[@]}"; do
	strace -e trace="$sc" true >/dev/null 2>&1 || continue
	trace_syscalls+=("$sc")
	fork_syscall_re="${fork_syscall_re:+$fork_syscall_re|}$sc"
done
[[ -n "$fork_syscall_re" ]] || skip "strace cannot trace any fork syscall"

printf -v trace_arg '%s,' "${trace_syscalls[@]}"
trace_arg=${trace_arg%,}

rm -f trace.out
LVM_TEST_TAG="kill_me_$PREFIX" strace -f -o trace.out \
	-e trace="$trace_arg" \
	pvmove -b --config 'global/io_engine="libaio"' "$dev1" "$dev2" ||
	die "pvmove failed under strace"

# Track the aio contexts live in each traced process.  A fork must never
# happen while the forking process still holds one, otherwise the child
# inherits a dead context.  Not every fork is the polldaemon fork (the
# command may fork helpers before the label scan runs), so the check is
# per fork, not "first fork in the trace".  strace -f prefixes each line
# with the pid; a call that is resumed is only counted at its start.
reason=$(awk -v fork_re="$fork_syscall_re" '
	{ pid = $1 }
	/io_setup\(/ { live[pid]++; created++; next }
	/io_destroy\(/ { live[pid]--; destroyed++; next }
	$0 ~ ("(" fork_re ")\\(") && $0 !~ /resumed/ {
		forks++
		if (live[pid] > 0) {
			bad = pid
			exit
		}
	}
	END {
		if (bad != "")
			print "aio context live across fork in pid " bad
		else if (!forks)
			print "no fork syscall traced"
		else if (!created)
			print "label scan created no aio context"
		else if (created != destroyed)
			print "aio contexts created: " created + 0 \
			      ", destroyed: " destroyed + 0
		else
			exit 0
		exit 1
	}' trace.out) ||
	die "label scan I/O engine was not destroyed before fork: $reason"

aux kill_tagged_processes
pvmove --abort 2>/dev/null || true
vgremove -ff $vg
