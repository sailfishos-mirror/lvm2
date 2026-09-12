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

# Test dmeventd handling of malformed, truncated and oversized client
# messages, and recovery after a client stops reading its reply.
# The helper uses the raw daemon protocol, no monitored devices needed.

. lib/inittest --skip-with-lvmpolld

command -v dmeventd-client >/dev/null || skip "dmeventd-client not available"

aux prepare_dmeventd

for i in {1..50}; do
	dmeventd-client hello && break
	sleep .2
done
dmeventd-client hello

# Malformed numeric registration fields are a protocol error
dmeventd-client badreg abc 10
dmeventd-client badreg 1 xyz
dmeventd-client badreg 4294967296 10
dmeventd-client hello

# A truncated message is discarded and the daemon resynchronizes
dmeventd-client truncated
for i in {1..30}; do
	grep -q "Discarding truncated message" debug.log_DMEVENTD_out && break
	sleep .5
done
grep -q "Discarding truncated message" debug.log_DMEVENTD_out
dmeventd-client hello

# An oversized message is dropped together with its leftovers
dmeventd-client oversize
sleep 1
dmeventd-client hello

# Plain requests still work
dmeventd-client status >/dev/null
dmeventd-client hello

# A client that never reads its reply must not wedge the daemon
if [ "${LVM_TEST_DMEVENTD_SLOW:-0}" = 1 ]; then
	dmeventd-client fill
	dmeventd-client request
	for i in {1..100}; do
		grep -q "Client does not read reply" debug.log_DMEVENTD_out && break
		sleep .5
	done
	grep -q "Client does not read reply" debug.log_DMEVENTD_out
	dmeventd-client drain
	dmeventd-client hello
fi
