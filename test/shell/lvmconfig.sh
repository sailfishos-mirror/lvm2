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

# Test lvmconfig --typeconfig values and other options.

. lib/inittest --skip-with-lvmpolld

eval "$(lvmconfig global/etc)"

# Make a non-default setting so diff/current have something to show.
aux lvmconf 'log/verbose=1'

#
# --typeconfig values
#

# current: shows settings that would be applied (includes our change)
lvmconfig --typeconfig current > out
grep 'verbose=1' out

# default: shows all settings with default values
lvmconfig --typeconfig default > out
grep 'verbose=0' out
# CFG_DEFAULT_COMMENTED settings must be commented in default output
grep -E '^[[:space:]]+#.*verbose=0' out

# diff: shows only settings modified from defaults
lvmconfig --typeconfig diff > out
grep 'verbose=1' out

# full: every setting with current value, user-modified values uncommented
lvmconfig --typeconfig full > out
grep 'verbose=1' out
# The user-modified value must appear uncommented
grep -E '^[[:space:]]+verbose=1' out
# A default-valued setting must also appear uncommented in full output
grep -E '^[[:space:]]+checks=1' out
# full output should contain many sections
grep 'global {' out
grep 'log {' out
grep 'backup {' out
grep 'activation {' out

# list: prints all config names without values
lvmconfig --typeconfig list > out
grep 'log/verbose' out
not grep '=' out

# missing: settings absent from config files
lvmconfig --typeconfig missing > out
# Our verbose=1 is present in config, so it should NOT appear uncommented as missing
not grep '^[[:space:]]*verbose=' out

# new: settings added since a version
lvmconfig --typeconfig new --sinceversion 2.3.0 > out
# Output should contain only recently added settings (if any)
# At minimum the command should succeed

# profilable: settings that can be set from a profile
lvmconfig --typeconfig profilable > out
grep 'global {' out

# profilable-command: command-profilable settings only
lvmconfig --typeconfig profilable-command > out
grep 'global {' out

# profilable-metadata: metadata-profilable settings only
lvmconfig --typeconfig profilable-metadata > out
grep 'allocation {' out

# invalid --typeconfig value
invalid lvmconfig --typeconfig bogus 2>err
grep 'Invalid argument' err

#
# --validate
#

# Valid config
lvmconfig --validate > out
grep 'LVM configuration valid' out

# --validate and --typeconfig are mutually exclusive
invalid lvmconfig --validate --typeconfig default

#
# --list
#

# --list is equivalent to --typeconfig list --withsummary
lvmconfig --list > out
grep 'log/verbose' out

# --list with --withcomments is invalid
invalid lvmconfig --list --withcomments

# --list with --valuesonly is invalid
invalid lvmconfig --list --valuesonly

#
# --withcomments
#

lvmconfig --typeconfig default --withcomments > out
grep '^[[:space:]]*#' out
grep 'verbose' out

#
# --withversions
#

lvmconfig --typeconfig default --withversions > out
grep 'since version' out

#
# --withsummary (used with list)
#

lvmconfig --typeconfig list --withsummary > out
grep 'log/verbose' out

#
# --withspaces
#

lvmconfig --typeconfig current --withspaces > out
grep 'verbose = 1' out

#
# --valuesonly
#

lvmconfig --typeconfig current --valuesonly log/verbose > out
grep '1' out
not grep 'verbose' out

#
# --withgeneralpreamble / --withlocalpreamble
#

lvmconfig --typeconfig default --withgeneralpreamble > out
# The general preamble is a block comment at the top
grep '^#' out

lvmconfig --typeconfig default --withlocalpreamble > out
grep '^#' out

#
# --ignoreadvanced
#

lvmconfig --typeconfig default > full_out
lvmconfig --typeconfig default --ignoreadvanced > adv_out
# ignoreadvanced output should be same size or smaller
test "$(wc -l < adv_out)" -le "$(wc -l < full_out)"

#
# --ignoreunsupported
#

lvmconfig --typeconfig default --ignoreunsupported > unsup_out
test "$(wc -l < unsup_out)" -le "$(wc -l < full_out)"

# --ignoreunsupported and --showunsupported are mutually exclusive
invalid lvmconfig --typeconfig default --ignoreunsupported --showunsupported

#
# --ignorelocal
#

lvmconfig --typeconfig default --ignorelocal > local_out
not grep '^local {' local_out

#
# --showdeprecated
#

lvmconfig --typeconfig default --showdeprecated > dep_out
test "$(wc -l < dep_out)" -ge "$(wc -l < full_out)"

#
# --file (output to file)
#

lvmconfig --typeconfig current --file dump_out.conf
grep 'verbose=1' dump_out.conf

#
# --mergedconfig
#

# --mergedconfig with --typeconfig current
lvmconfig --typeconfig current --mergedconfig > out
grep 'verbose=1' out

# --mergedconfig with --typeconfig full
lvmconfig --typeconfig full --mergedconfig > out
grep 'verbose=1' out

# --mergedconfig with other types is invalid
invalid lvmconfig --typeconfig default --mergedconfig

#
# --atversion
#

lvmconfig --typeconfig default --atversion 2.3.0 > out
# Should succeed and produce output
test -s out

# --atversion requires --type
invalid lvmconfig --atversion 2.3.0

# --atversion conflicts with --sinceversion
invalid lvmconfig --typeconfig new --atversion 2.3.0 --sinceversion 2.2.0

# --atversion with current or full is invalid
invalid lvmconfig --typeconfig current --atversion 2.3.0
invalid lvmconfig --typeconfig full --atversion 2.3.0

#
# --sinceversion
#

# --sinceversion requires --type new
lvmconfig --typeconfig new --sinceversion 2.3.0 > out

invalid lvmconfig --typeconfig default --sinceversion 2.3.0

#
# Querying specific config paths
#

lvmconfig log/verbose > out
grep 'verbose=1' out

lvmconfig log > out
grep 'verbose=1' out

# Multiple paths
lvmconfig log/verbose log/syslog > out
grep 'verbose=1' out
grep 'syslog=0' out

#
# --ignoreadvanced and --ignoreunsupported with --typeconfig current
#

# These are not allowed with current
invalid lvmconfig --typeconfig current --ignoreadvanced
invalid lvmconfig --typeconfig current --ignoreunsupported
