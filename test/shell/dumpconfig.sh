#!/usr/bin/env bash

# Copyright (C) 2011 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA



. lib/inittest --skip-with-lvmpolld

flatten() {
	cat > flatten.config
	for s in $(grep -E '^[a-z]+ {$' flatten.config | sed -e 's,{$,,'); do
		sed -e "/^$s/,/^}/p;d" flatten.config | sed -e '1d;$d' | sed -e "s,^[ \t]*,$s/,";
	done
}

# FIXME: Either make longer start delay,
#  or even better do not initialize
#  locking for commands like 'dumpconfig'
#aux lvmconf "global/locking_type=0"
eval "$(lvmconfig global/etc)"

lvm dumpconfig -f lvmdumpconfig
flatten < lvmdumpconfig | sort > config.dump
flatten < "$etc/lvm.conf" | sort > config.input
# check that dumpconfig output corresponds to the lvm.conf input
diff -wu config.input config.dump

# and that merging multiple config files (through tags) works
lvm dumpconfig -f lvmdumpconfig
flatten < lvmdumpconfig | not grep 'log/verbose=1'
lvm dumpconfig -f lvmdumpconfig
flatten < lvmdumpconfig | grep 'log/indent=1'

aux lvmconf 'tags/@foo {}'
echo 'log { verbose = 1 }' > "$etc/lvm_foo.conf"
lvm dumpconfig -f lvmdumpconfig
flatten < lvmdumpconfig | grep 'log/verbose=1'
lvm dumpconfig -f lvmdumpconfig
flatten < lvmdumpconfig | grep 'log/indent=1'
rm -f "$etc/lvm_foo.conf"

# Test that subsections are correctly merged via --config + --mergedconfig.
# This exercises the _merge_section code path for nested subsection
# insertion and merging (nodes with v == NULL).

# Subsection not present in base config - should be inserted whole
lvm dumpconfig --mergedconfig \
  --config 'allocation { cache_settings { smq { migration_threshold = 2048 } } }' \
  -f lvmdumpconfig
grep 'migration_threshold=2048' lvmdumpconfig

# Subsection merged alongside a flat setting in the same top-level section
lvm dumpconfig --mergedconfig \
  --config 'allocation { maximise_cling = 0 cache_settings { smq { migration_threshold = 1024 } } }' \
  -f lvmdumpconfig
grep 'maximise_cling=0' lvmdumpconfig
grep 'migration_threshold=1024' lvmdumpconfig

# Existing base config settings in the same top-level section survive the merge
grep 'zero_metadata=' lvmdumpconfig

# Test --list output with unregistered subsection values.
# --list uses _out_line_list and _cfg_node_make_path which walk the parent
# chain, so these verify that parent pointers are correct throughout.

# --type full --list: clones current config into def tree, exercises
# dm_config_clone_node_with_mem parent pointer fix and _cfg_node_make_path
lvm dumpconfig --type full --mergedconfig --list \
  --config 'allocation { cache_settings { smq { migration_threshold = 2048 } } }' \
  -f lvmdumpconfig
grep 'allocation/cache_settings/smq/migration_threshold=2048' lvmdumpconfig

# --type current --list: uses parsed tree directly, exercises the
# _file() dangling parent pointer fix and _cfg_node_make_path
lvm dumpconfig --type current --mergedconfig --list \
  --config 'allocation { cache_settings { smq { migration_threshold = 2048 } } }' \
  -f lvmdumpconfig
grep 'allocation/cache_settings/smq/migration_threshold=2048' lvmdumpconfig

# The CFG_DEFAULT_COMMENTED flag is what decides whether a section's
# braces are commented out in "--type default" output - not the nesting
# depth. Flagged sections are commented out at every level, while
# unflagged sections stay uncommented regardless of depth.
lvm dumpconfig --type default -f lvmdumpconfig

# CFG_DEFAULT_COMMENTED at the top level: braces commented out
grep -E '^# metadata \{' lvmdumpconfig
grep -E '^# tags \{' lvmdumpconfig

# CFG_DEFAULT_COMMENTED when nested: braces commented out just the same
grep -E '^[[:space:]]+# cache_settings \{' lvmdumpconfig
grep -E '^[[:space:]]+# tag \{' lvmdumpconfig

# No CFG_DEFAULT_COMMENTED flag: braces stay uncommented
grep -E '^global \{' lvmdumpconfig
grep -E '^activation \{' lvmdumpconfig
grep -E '^report \{' lvmdumpconfig
not grep -E '^# (global|activation|report) \{' lvmdumpconfig
