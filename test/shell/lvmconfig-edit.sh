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

# Test lvmconfig --edit for config file modification.

. lib/inittest --skip-with-lvmpolld

eval "$(lvmconfig global/etc)"

#
# Test --edit with --file on a standalone config file (not lvm.conf)
#

# Set a single value
rm -f edit1.conf
lvmconfig --edit global/use_lvmlockd=1 --file edit1.conf
grep 'use_lvmlockd=1' edit1.conf

# Set a string value
rm -f edit2.conf
lvmconfig --edit global/locking_dir=\"/run/lock/lvm\" --file edit2.conf
grep 'locking_dir="/run/lock/lvm"' edit2.conf

# Multiple edits in one command
rm -f edit3.conf
lvmconfig --edit log/verbose=1 --edit log/indent=0 --file edit3.conf
grep 'verbose=1' edit3.conf
grep 'indent=0' edit3.conf

# Edit an existing file, changing a value
rm -f edit4.conf
lvmconfig --edit log/verbose=1 --file edit4.conf
grep 'verbose=1' edit4.conf
lvmconfig --edit log/verbose=0 --file edit4.conf
grep 'verbose=0' edit4.conf
not grep 'verbose=1' edit4.conf

# Add a second setting to an existing file
lvmconfig --edit log/indent=1 --file edit4.conf
grep 'verbose=0' edit4.conf
grep 'indent=1' edit4.conf

#
# Test removal (value=-)
#

rm -f edit5.conf
lvmconfig --edit log/verbose=1 --edit log/indent=1 --file edit5.conf
grep 'verbose=1' edit5.conf
grep 'indent=1' edit5.conf

lvmconfig --edit log/verbose=- --file edit5.conf
not grep 'verbose' edit5.conf
grep 'indent=1' edit5.conf

# Remove last field in section
lvmconfig --edit log/indent=- --file edit5.conf
not grep 'indent' edit5.conf

#
# Test --input / --output for file-to-file transformation
#

rm -f input.conf output.conf
lvmconfig --edit global/use_lvmlockd=0 --file input.conf

# Read from input, apply edit, write to output
lvmconfig --input input.conf --edit global/use_lvmlockd=1 --output output.conf
grep 'use_lvmlockd=1' output.conf
# Input file unchanged
grep 'use_lvmlockd=0' input.conf

# Read from input, write to stdout (no --output, no --edit)
lvmconfig --input input.conf > stdout.conf
grep 'use_lvmlockd=0' stdout.conf

#
# Test auto-selection of lvm.conf vs lvmlocal.conf
#

cp "$etc/lvm.conf" "$etc/lvm.conf.bak"

# Non-local section should target lvm.conf
lvmconfig --edit log/verbose=1
grep 'verbose=1' "$etc/lvm.conf"

cp "$etc/lvm.conf.bak" "$etc/lvm.conf"

# Local section should target lvmlocal.conf
lvmconfig --edit local/host_id=1
grep 'host_id=1' "$etc/lvmlocal.conf"
rm -f "$etc/lvmlocal.conf"

# Cannot mix local and non-local sections
invalid lvmconfig --edit local/host_id=1 --edit log/verbose=1 2>err
grep 'Cannot mix' err

#
# Test backup creation in old_conf
#

rm -rf "$etc/old_conf"

rm -f edit6.conf
lvmconfig --edit log/verbose=1 --file edit6.conf
lvmconfig --edit log/verbose=2 --file edit6.conf
# Backup dir should exist for files in system_dir but not for
# arbitrary paths, so no old_conf created for edit6.conf.

# Edits to lvm.conf should create backups
cp "$etc/lvm.conf.bak" "$etc/lvm.conf"
rm -rf "$etc/old_conf"

lvmconfig --edit log/verbose=1
test -d "$etc/old_conf"
NBACKUP=$(ls "$etc/old_conf/" | wc -l)
test "$NBACKUP" -eq 1

lvmconfig --edit log/verbose=2
NBACKUP=$(ls "$etc/old_conf/" | wc -l)
test "$NBACKUP" -eq 2

# Verify backup preserves custom comments from original file
cp "$etc/lvm.conf.bak" "$etc/lvm.conf"
rm -rf "$etc/old_conf"
echo '# my custom comment foo=1' >> "$etc/lvm.conf"
grep 'my custom comment foo=1' "$etc/lvm.conf"
lvmconfig --edit log/verbose=1
BACKUP=$(ls "$etc/old_conf/" | head -1)
grep 'my custom comment foo=1' "$etc/old_conf/$BACKUP"

cp "$etc/lvm.conf.bak" "$etc/lvm.conf"
rm -rf "$etc/old_conf"

#
# Test invalid edit specifications
#

# Missing equals sign
invalid lvmconfig --edit global/use_lvmlockd --file bad.conf 2>err
grep 'Invalid edit' err

# Missing section (no slash)
invalid lvmconfig --edit use_lvmlockd=1 --file bad.conf 2>err
grep 'Invalid edit' err

#
# Test --withcomments
#

rm -f comments.conf
lvmconfig --edit log/verbose=1 --withcomments --file comments.conf
grep 'verbose=1' comments.conf
# --withcomments should include comment lines
grep '^[[:space:]]*#' comments.conf

#
# Test --withspaces
#

rm -f spaces.conf
lvmconfig --edit log/verbose=1 --withspaces --file spaces.conf
grep 'verbose = 1' spaces.conf

#
# Test editing files with existing content
#

# Edit a file that has an empty section (node with child=NULL, v=NULL)
cat > existing1.conf <<'EOF'
tags {
}
log {
	verbose=0
}
EOF
lvmconfig --edit log/verbose=1 --file existing1.conf
grep 'verbose=1' existing1.conf

# Edit a file with multiple sections, adding to one
cat > existing2.conf <<'EOF'
log {
	verbose=0
	indent=1
}
global {
	use_lvmlockd=0
}
EOF
lvmconfig --edit global/use_lvmlockd=1 --file existing2.conf
grep 'use_lvmlockd=1' existing2.conf
# Existing values in other sections preserved
grep 'verbose=0' existing2.conf
grep 'indent=1' existing2.conf

# Edit a file that has only empty sections
cat > existing3.conf <<'EOF'
tags {
}
global {
}
EOF
lvmconfig --edit global/use_lvmlockd=1 --file existing3.conf
grep 'use_lvmlockd=1' existing3.conf

# Edit a file adding a new section not in the original
cat > existing4.conf <<'EOF'
log {
	verbose=1
}
EOF
lvmconfig --edit global/use_lvmlockd=1 --file existing4.conf
grep 'use_lvmlockd=1' existing4.conf
grep 'verbose=1' existing4.conf

# Remove a field from a file with multiple sections
cat > existing5.conf <<'EOF'
log {
	verbose=1
	indent=1
}
global {
	use_lvmlockd=1
}
EOF
lvmconfig --edit log/verbose=- --file existing5.conf
not grep 'verbose' existing5.conf
grep 'indent=1' existing5.conf
grep 'use_lvmlockd=1' existing5.conf

# Edit a value and remove a value in one command
cat > existing6.conf <<'EOF'
log {
	verbose=0
	indent=1
	syslog=0
}
EOF
lvmconfig --edit log/verbose=1 --edit log/syslog=- --file existing6.conf
grep 'verbose=1' existing6.conf
grep 'indent=1' existing6.conf
not grep 'syslog' existing6.conf

