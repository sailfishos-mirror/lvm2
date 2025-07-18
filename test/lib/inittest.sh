#!/usr/bin/env bash
# Copyright (C) 2011-2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

initskip() {
	test $# -eq 0 || echo "TEST SKIPPED:" "$@"
	exit 200
}

# Parse command line options
SKIP_ROOT_DM_CHECK=${SKIP_ROOT_DM_CHECK-}
SKIP_WITH_DEVICES_FILE=${SKIP_WITH_DEVICES_FILE-}
SKIP_WITH_LVMLOCKD=${SKIP_WITH_LVMLOCKD-}
SKIP_WITH_LVMPOLLD=${SKIP_WITH_LVMPOLLD-}
while [ "$#" -gt 0 ]; do
	case "$1" in
		--skip-root-dm-check)
			SKIP_ROOT_DM_CHECK=1
			;;
		--skip-with-devices-file)
			SKIP_WITH_DEVICES_FILE=1
			;;
		--skip-with-lvmpolld)
			SKIP_WITH_LVMPOLLD=1
			;;
		--skip-with-lvmlockd)
			SKIP_WITH_LVMLOCKD=1
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
	shift
done

# sanitize the environment
LANG=C
LC_ALL=C
TZ=UTC

# Put script name into variable, so it can used in external scripts
TESTNAME=${0##*/}
# Nice debug message
PS4='#${BASH_SOURCE[0]##*/}:${LINENO}+ '
export TESTNAME PS4

LVM_TEST_FLAVOUR=${LVM_TEST_FLAVOUR-ndev-vanilla}

LVM_TEST_BACKING_DEVICE=${LVM_TEST_BACKING_DEVICE-}
LVM_TEST_DEVDIR=${LVM_TEST_DEVDIR-}
LVM_TEST_NODEBUG=${LVM_TEST_NODEBUG-}
LVM_TEST_LVM1=${LVM_TEST_LVM1-}
LVM_TEST_FAILURE=${LVM_TEST_FAILURE-}
LVM_TEST_MULTI_HOST=${LVM_TEST_MULTI_HOST-}
# TODO: LVM_TEST_SHARED
SHARED=${SHARED-}

LVM_TEST_LVMLOCKD=${LVM_TEST_LVMLOCKD-}
LVM_TEST_LVMLOCKD_TEST=${LVM_TEST_LVMLOCKD_TEST-}
LVM_TEST_LVMPOLLD=${LVM_TEST_LVMPOLLD-}
LVM_TEST_DEVICES_FILE=${LVM_TEST_DEVICES_FILE-}
LVM_TEST_LOCK_TYPE_DLM=${LVM_TEST_LOCK_TYPE_DLM-}
LVM_TEST_LOCK_TYPE_SANLOCK=${LVM_TEST_LOCK_TYPE_SANLOCK-}
LVM_TEST_LOCK_TYPE_IDM=${LVM_TEST_LOCK_TYPE_IDM-}

SKIP_WITHOUT_CLVMD=${SKIP_WITHOUT_CLVMD-}
SKIP_WITH_CLVMD=${SKIP_WITH_CLVMD-}

SKIP_WITH_LOW_SPACE=${SKIP_WITH_LOW_SPACE-50}

test -n "$LVM_TEST_FLAVOUR" || { echo "NOTE: Empty flavour">&2; initskip; }
test -f "lib/flavour-$LVM_TEST_FLAVOUR" || { echo "NOTE: Flavour '$LVM_TEST_FLAVOUR' does not exist">&2; initskip; }
. "lib/flavour-$LVM_TEST_FLAVOUR"

test -n "$SKIP_WITHOUT_CLVMD" && test "$LVM_TEST_LOCKING" -ne 3 && initskip
test -n "$SKIP_WITH_CLVMD" && test "$LVM_TEST_LOCKING" = 3 && initskip

# When requested testing LVMLOCKD & LVMPOLLD - ignore skipping of marked test for lvmpolld
test -n "$LVM_TEST_LVMLOCKD" && test -n "$LVM_TEST_LVMPOLLD" && SKIP_WITH_LVMPOLLD=

test -n "$SKIP_WITH_LVMPOLLD" && test -n "$LVM_TEST_LVMPOLLD" && initskip

test -n "$SKIP_WITH_LVMLOCKD" && test -n "$LVM_TEST_LVMLOCKD" && initskip

test -n "$SKIP_WITH_DEVICES_FILE" && test -n "$LVM_TEST_DEVICES_FILE" && initskip

unset CDPATH

export LVM_TEST_BACKING_DEVICE LVM_TEST_DEVDIR LVM_TEST_NODEBUG LVM_TEST_FAILURE
export LVM_TEST_MULTI_HOST
export LVM_TEST_LVMLOCKD LVM_TEST_LVMLOCKD_TEST
export LVM_TEST_LVMPOLLD LVM_TEST_LOCK_TYPE_DLM LVM_TEST_LOCK_TYPE_SANLOCK LVM_TEST_LOCK_TYPE_IDM
export LVM_TEST_DEVICES_FILE
# grab some common utilities
. lib/utils

TESTOLDPWD=$(pwd)
COMMON_PREFIX="LVMTEST"
PREFIX="${COMMON_PREFIX}$$"

# Check we are not conflicting with some exiting setup
if test -z "$SKIP_ROOT_DM_CHECK" ; then
	d=$(dmsetup info -c -o name --noheadings --rows -S "suspended=Suspended||name=~${PREFIX}[^0-9]")
	case "$d" in
	"No devices found") ;;
	"") ;;
	*) die "DM table already has either suspended or $PREFIX prefixed devices: $d" ;;
	esac
fi

test -n "$LVM_TEST_DIR" || LVM_TEST_DIR=${TMPDIR:-/tmp}
test "$LVM_TEST_DIR" = "/dev" && die "Setting LVM_TEST_DIR=/dev is not supported!"

TESTDIR=$(mkdtemp "$LVM_TEST_DIR" "$PREFIX.XXXXXXXXXX") || \
	die "failed to create temporary directory in \"$LVM_TEST_DIR\""
RUNNING_DMEVENTD=$(pgrep dmeventd || true)

export TESTOLDPWD TESTDIR COMMON_PREFIX PREFIX RUNNING_DMEVENTD
LVM_LOG_FILE_EPOCH=DEBUG
LVM_LOG_FILE_MAX_LINES=${LVM_LOG_FILE_MAX_LINES-1000000}
LVM_EXPECTED_EXIT_STATUS=1
export LVM_LOG_FILE_EPOCH LVM_LOG_FILE_MAX_LINES LVM_EXPECTED_EXIT_STATUS

if test -z "$SKIP_ROOT_DM_CHECK" ; then
	# Teardown only with root
	test -n "$BASH" && trap 'set +vx; STACKTRACE; set -vx' ERR
	trap 'aux teardown' EXIT # don't forget to clean up
else
	trap 'cd $TESTOLDPWD; rm -rf "${TESTDIR:?}"' EXIT
fi

cd "$TESTDIR"
mkdir lib tmp

# Setting up symlink from $i to $TESTDIR/lib
# library libdevmapper-event-lvm2.so.2.03 is needed with name
test -n "${abs_top_builddir+varset}" && \
    find "$abs_top_builddir/daemons/dmeventd/plugins/" -name '*.so*' \
    -exec ln -s -t lib "{}" +
find "$TESTOLDPWD/lib" ! \( -name '*.sh' -o -name '*.[cdo]' \
    -o -name '*~' \)  -exec ln -s -t lib "{}" +
LD_LIBRARY_PATH="$TESTDIR/lib:$LD_LIBRARY_PATH"

DM_DEFAULT_NAME_MANGLING_MODE=none
DM_DEV_DIR="$TESTDIR/dev"
LVM_SYSTEM_DIR="$TESTDIR/etc"
TMPDIR="$TESTDIR/tmp"
# abort on the internal dm errors in the tests (allowing test user override)
DM_ABORT_ON_INTERNAL_ERRORS=${DM_ABORT_ON_INTERNAL_ERRORS:-1}
DM_DEBUG_WITH_LINE_NUMBERS=${DM_DEBUG_WITH_LINE_NUMBERS:-1}

export DM_DEFAULT_NAME_MANGLING_MODE DM_DEV_DIR LVM_SYSTEM_DIR DM_ABORT_ON_INTERNAL_ERRORS
mkdir "$LVM_SYSTEM_DIR" "$DM_DEV_DIR"
MACHINEID=$(uuidgen 2>/dev/null || echo "abcdefabcdefabcdefabcdefabcdefab")
echo "${MACHINEID//-/}" > "$LVM_SYSTEM_DIR/machine-id"   # remove all '-'
if test -n "$LVM_TEST_DEVDIR" ; then
	test -d "$LVM_TEST_DEVDIR" || die "Test device directory LVM_TEST_DEVDIR=\"$LVM_TEST_DEVDIR\" is not valid."
	DM_DEV_DIR=$LVM_TEST_DEVDIR
elif test -z "$SKIP_ROOT_DM_CHECK" ; then
	mknod "$DM_DEV_DIR/testnull" c 1 3 || die "mknod failed"
	echo >"$DM_DEV_DIR/testnull" || \
		die "Filesystem does support devices in $DM_DEV_DIR (mounted with nodev?)"
	# dmsetup makes here needed control entry if still missing
	dmsetup version || \
		die "Dmsetup in $DM_DEV_DIR can't report version?"
fi

echo "$TESTNAME" >TESTNAME
# Require 50M of free space in testdir
test "$(df -k -P . | awk '/\// {print $4}')" -gt $(( SKIP_WITH_LOW_SPACE * 1024 )) || \
	skip "Testing requires more than ${SKIP_WITH_LOW_SPACE}M of free space in directory $TESTDIR!\\n$(df -H | sed -e 's,^,## DF:   ,')"

echo "Kernel is $(uname -a)"
# Report SELinux mode
echo "Selinux mode is $(getenforce 2>/dev/null || echo not installed)."
free -m || true

df -h || true

# Set vars from utils now that we have TESTDIR/PREFIX/...
prepare_test_vars

# Set strict shell mode
# see: http://redsymbol.net/articles/unofficial-bash-strict-mode
test -n "$BASH" && set -euE -o pipefail

# Vars for harness
echo "@TESTDIR=$TESTDIR"
echo "@PREFIX=$PREFIX"

# Date of executed test
echo "## DATE: $(date || true)"

# Hostname IP address
HOSTNAME=$(hostname -I 2>/dev/null) || HOSTNAME=
HOSTNAME="$(hostname 2>/dev/null) ${HOSTNAME}" || true
echo "## HOST: $HOSTNAME"

if test -z "$SKIP_ROOT_DM_CHECK" ; then
	aux lvmconf
fi

test -n "$LVM_TEST_LVMPOLLD" && {
	export LVM_LVMPOLLD_SOCKET="$TESTDIR/lvmpolld.socket"
	export LVM_LVMPOLLD_PIDFILE="$TESTDIR/lvmpolld.pid"
	aux prepare_lvmpolld
}

export SHARED=""

if test -n "$LVM_TEST_LVMLOCKD" ; then
	if test -n "$LVM_TEST_LOCK_TYPE_SANLOCK" ; then
		aux lvmconf 'local/host_id = 1'
	fi

	export SHARED="--shared"
fi

# for check_lvmlockd_test, lvmlockd is restarted for each shell test.
# for check_lvmlockd_{sanlock,dlm}, lvmlockd is started once by
# aa-lvmlockd-{sanlock,dlm}-prepare.sh and left running for all shell tests.

if test -n "$LVM_TEST_LVMLOCKD_TEST" ; then
	aux prepare_lvmlockd
fi

echo "<======== Processing test: \"$TESTNAME\" ========>"

set -vx
