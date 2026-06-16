#!/bin/bash
# test-configure-matrix.sh -- test LVM2 configure option combinations

set -euo pipefail

usage() {
	cat <<-EOF
	Usage: $0 [OPTIONS]

	Test LVM2 configure option combinations.  Runs ./configure (and
	optionally make) across a curated matrix of ~100 option combos.
	NOT exhaustive 2^N -- uses baselines, one-at-a-time toggles,
	dependency groups, known conflicts, and stress mixes.

	Options:
	  -j N       parallel make jobs (default: nproc)
	  -n         dry-run -- print test names + args, don't execute
	  -c         configure-only -- skip make
	  -s SRCDIR  source directory (default: script's parent/..)
	  -o OUTDIR  output/log directory (default: /tmp/lvm2-cfgtest)
	  -f FILTER  only run tests whose name matches grep -E pattern
	  -t N       run only test number N
	  -S N       start from test number N (skip earlier tests)
	  -l         list all tests with numbers and args, then exit
	  -C         continue -- skip PASS/XFAIL from prior run's state
	             log, retry FAIL tests, run any new/untested tests
	  -k         keep going on failure (default: stop at first)
	  -q         quiet -- no per-test stdout, only summary
	  -h         show this help

	Sections (each test is numbered sequentially):
	  1: Baselines          (default / minimal / maximal)
	  2: Single toggles     (one flag flipped from baseline)
	  3: Expected failures  (known conflicts -- MUST fail)
	  4: Dependency groups  (related options together)
	  5: Linking combos     (static / shared)
	  6: Build flags        (sanitizers, optimisation levels)
	  7: Stress combos      (unusual but valid multi-option mixes)
	  8: Segtype overrides  (default mirror/raid10/sparse)
	  9: Misc flags         (device-nodes, name-mangling, etc.)

	State tracking:
	  Results are recorded in OUTDIR/state.log (one line per test,
	  format NUM:NAME:RESULT, where RESULT is PASS, FAIL, or XFAIL).
	  This enables -C (continue) mode: fix the issue, re-run with -C
	  to skip passed tests and retry only the failures.

	Prerequisites:
	  gcc and ccache should be installed -- without ccache the
	  repeated compilations across ~100 configs will take ages.

	Typical workflow:
	  $0 -q -k          # full run, some may fail
	  # ... fix the issue ...
	  $0 -q -k -C       # retry failures + any new tests
	EOF
}

JOBS=$(nproc 2>/dev/null || echo 4)
DRY_RUN=0
CONFIGURE_ONLY=0
SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="/tmp/lvm2-cfgtest"
FILTER=""
RUN_TEST_NUM=""
START_FROM=1
LIST_ONLY=0
CONTINUE=0
KEEP_GOING=0
QUIET=0

while getopts "j:ncs:o:f:t:S:lCkqh" opt; do
	case $opt in
	j) JOBS=$OPTARG ;;
	n) DRY_RUN=1 ;;
	c) CONFIGURE_ONLY=1 ;;
	s) SRCDIR=$OPTARG ;;
	o) OUTDIR=$OPTARG ;;
	f) FILTER=$OPTARG ;;
	t) RUN_TEST_NUM=$OPTARG ;;
	S) START_FROM=$OPTARG ;;
	l) LIST_ONLY=1 ;;
	C) CONTINUE=1 ;;
	k) KEEP_GOING=1 ;;
	q) QUIET=1 ;;
	h) usage; exit 0 ;;
	*) usage >&2; exit 1 ;;
	esac
done

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

PASS=0
FAIL=0
XFAIL=0
SKIP=0
TOTAL=0
TESTNUM=0
FAILURES=""

# State file -- one line per completed test: "NUM:NAME:RESULT"
# RESULT is PASS, FAIL, or XFAIL
state_lookup() {
	local num=$1
	[ -n "$STATE_FILE" ] && [ -f "$STATE_FILE" ] || return 1
	local line
	line=$(grep "^${num}:" "$STATE_FILE" 2>/dev/null) || return 1
	echo "${line##*:}"
}

state_record() {
	local num=$1 name=$2 result=$3
	[ -n "$STATE_FILE" ] || return 0
	# Remove any previous entry for this test, append new one
	if [ -f "$STATE_FILE" ]; then
		sed -i "/^${num}:/d" "$STATE_FILE"
	fi
	echo "${num}:${name}:${result}" >> "$STATE_FILE"
}

log_result() {
	local num=$1 name=$2 expect=$3 rc=$4
	TOTAL=$((TOTAL + 1))
	local label result
	printf -v label "%3d  %-55s" "$num" "$name"
	if [ "$expect" = "fail" ]; then
		if [ "$rc" -ne 0 ]; then
			XFAIL=$((XFAIL + 1))
			result="XFAIL"
			printf "  %s  XFAIL (expected)\n" "$label"
		else
			FAIL=$((FAIL + 1))
			result="FAIL"
			FAILURES="$FAILURES  $label (expected failure but passed)\n"
			printf "  %s  UNEXPECTED PASS\n" "$label"
		fi
	else
		if [ "$rc" -eq 0 ]; then
			PASS=$((PASS + 1))
			result="PASS"
			printf "  %s  PASS\n" "$label"
		else
			FAIL=$((FAIL + 1))
			result="FAIL"
			FAILURES="$FAILURES  $label\n"
			printf "  %s  FAIL\n" "$label"
		fi
	fi
	state_record "$num" "$name" "$result"
}

# run_test NAME EXPECT_RESULT CONFIGURE_ARGS...
# EXPECT_RESULT: "pass" or "fail"
run_test() {
	local name=$1
	local expect=$2
	shift 2
	local args="$*"

	TESTNUM=$((TESTNUM + 1))

	# Skip by number range
	if [ -n "$RUN_TEST_NUM" ] && [ "$TESTNUM" -ne "$RUN_TEST_NUM" ]; then
		return 0
	fi
	if [ "$TESTNUM" -lt "$START_FROM" ]; then
		return 0
	fi

	# Continue mode -- skip tests already passed/xfailed, re-run failures
	if [ "$CONTINUE" = 1 ]; then
		local prev
		if prev=$(state_lookup "$TESTNUM"); then
			case "$prev" in
			PASS)
				PASS=$((PASS + 1))
				TOTAL=$((TOTAL + 1))
				printf "  %3d  %-55s  PASS (cached)\n" "$TESTNUM" "$name"
				return 0
				;;
			XFAIL)
				XFAIL=$((XFAIL + 1))
				TOTAL=$((TOTAL + 1))
				printf "  %3d  %-55s  XFAIL (cached)\n" "$TESTNUM" "$name"
				return 0
				;;
			FAIL)
				printf "  %3d  %-55s  retrying...\n" "$TESTNUM" "$name"
				;;
			esac
		fi
	fi

	# Skip by name filter
	if [ -n "$FILTER" ] && ! echo "$name" | grep -qE "$FILTER"; then
		SKIP=$((SKIP + 1))
		return 0
	fi

	# List mode -- just print number + name
	if [ "$LIST_ONLY" = 1 ]; then
		printf "  %3d  %-55s  [%s]  %s\n" "$TESTNUM" "$name" "$expect" "$args"
		TOTAL=$((TOTAL + 1))
		return 0
	fi

	if [ "$DRY_RUN" = 1 ]; then
		printf "  %3d  %-55s  %s\n" "$TESTNUM" "$name" "$args"
		TOTAL=$((TOTAL + 1))
		return 0
	fi

	local logdir="$OUTDIR/$name"
	mkdir -p "$logdir"

	local rc=0

	# Clean previous build artifacts
	if [ -f "$SRCDIR/Makefile" ]; then
		make -C "$SRCDIR" distclean >/dev/null 2>&1 || true
	fi

	# Configure
	if [ "$QUIET" = 1 ]; then
		"$SRCDIR/configure" $args \
			>"$logdir/configure.log" 2>&1 || rc=$?
	else
		"$SRCDIR/configure" $args \
			2>&1 | tee "$logdir/configure.log" || rc=$?
	fi

	if [ "$rc" -ne 0 ]; then
		log_result "$TESTNUM" "$name" "$expect" "$rc"
		[ "$KEEP_GOING" = 1 ] || [ "$expect" = "fail" ] || exit 1
		return 0
	fi

	# Build (unless configure-only or expected to fail at configure)
	if [ "$CONFIGURE_ONLY" = 0 ] && [ "$expect" != "fail" ]; then
		if [ "$QUIET" = 1 ]; then
			make -C "$SRCDIR" -j"$JOBS" \
				>"$logdir/make.log" 2>&1 || rc=$?
		else
			make -C "$SRCDIR" -j"$JOBS" \
				2>&1 | tee "$logdir/make.log" || rc=$?
		fi
	fi

	log_result "$TESTNUM" "$name" "$expect" "$rc"
	if [ "$rc" -ne 0 ] && [ "$expect" != "fail" ] && [ "$KEEP_GOING" = 0 ]; then
		exit 1
	fi
}

# --------------------------------------------------------------------------
# Common base options -- keep builds out of system dirs
# --------------------------------------------------------------------------

BASE="--prefix=/tmp/lvm2-cfgtest-pfx"
BASE="$BASE --with-confdir=/tmp/lvm2-cfgtest-pfx/etc"
BASE="$BASE --with-default-system-dir=/tmp/lvm2-cfgtest-pfx/etc/lvm"
BASE="$BASE --with-systemdsystemunitdir=/tmp/lvm2-cfgtest-pfx/lib/systemd"

# --------------------------------------------------------------------------
# Detect available libraries -- gate tests that need them
# --------------------------------------------------------------------------

has_lib() { pkg-config --exists "$1" 2>/dev/null; }

HAVE_BLKID=0;      has_lib "blkid"              && HAVE_BLKID=1
HAVE_LIBNVME=0;     has_lib "libnvme >= 1.4"     && HAVE_LIBNVME=1
HAVE_SYSTEMD=0;     has_lib "systemd"            && HAVE_SYSTEMD=1
HAVE_SYSTEMD_221=0; has_lib "systemd >= 221"     && HAVE_SYSTEMD_221=1
HAVE_SYSTEMD_234=0; has_lib "systemd >= 234"     && HAVE_SYSTEMD_234=1
HAVE_UDEV=0;        has_lib "libudev >= 143"     && HAVE_UDEV=1
HAVE_SELINUX=0;     has_lib "libselinux"         && HAVE_SELINUX=1
HAVE_DLM=0;         has_lib "libdlm"             && HAVE_DLM=1
HAVE_DLMCONTROL=0;  has_lib "libdlmcontrol"      && HAVE_DLMCONTROL=1
HAVE_SANLOCK=0;     has_lib "libsanlock_client"   && HAVE_SANLOCK=1
HAVE_VALGRIND=0;    has_lib "valgrind"           && HAVE_VALGRIND=1
HAVE_READLINE=0;    pkg-config --exists readline 2>/dev/null && HAVE_READLINE=1
HAVE_EDITLINE=0;    pkg-config --exists libedit  2>/dev/null && HAVE_EDITLINE=1

mkdir -p "$OUTDIR"
STATE_FILE="$OUTDIR/state.log"

# --continue: check state file, report what we're resuming
if [ "$CONTINUE" = 1 ]; then
	if [ ! -f "$STATE_FILE" ]; then
		echo "No state file ($STATE_FILE) -- nothing to continue, running from start."
		CONTINUE=0
	else
		PREV_PASS=$(grep -c ':PASS$\|:XFAIL$' "$STATE_FILE" 2>/dev/null || true)
		PREV_FAIL=$(grep -c ':FAIL$' "$STATE_FILE" 2>/dev/null || true)
		echo "Continuing: $PREV_PASS passed, $PREV_FAIL to retry"
	fi
fi

echo "============================================================"
echo "LVM2 Configure Matrix Test"
echo "============================================================"
echo "Source:     $SRCDIR"
echo "Output:     $OUTDIR"
echo "Jobs:       $JOBS"
echo "Dry-run:    $DRY_RUN"
echo "Conf-only:  $CONFIGURE_ONLY"
echo "Continue:   $CONTINUE"
echo ""
echo "Detected libraries:"
printf "  blkid=%-3s  libnvme=%-3s  systemd=%-3s  udev=%-3s\n" \
	"$HAVE_BLKID" "$HAVE_LIBNVME" "$HAVE_SYSTEMD" "$HAVE_UDEV"
printf "  selinux=%-3s  dlm=%-3s  sanlock=%-3s  valgrind=%-3s\n" \
	"$HAVE_SELINUX" "$HAVE_DLM" "$HAVE_SANLOCK" "$HAVE_VALGRIND"
printf "  readline=%-3s  editline=%-3s  dlmcontrol=%-3s\n" \
	"$HAVE_READLINE" "$HAVE_EDITLINE" "$HAVE_DLMCONTROL"
echo ""
echo "State file: $STATE_FILE"
echo ""
echo "------------------------------------------------------------"
echo "Running tests..."
echo "------------------------------------------------------------"
echo ""

# ==========================================================================
# SECTION 1: Baselines (3 tests)
# ==========================================================================

section() { echo "--- Section $1: $2 (from test $((TESTNUM + 1))) ---"; }

section 1 "Baselines"

# 1. Pure defaults -- just ./configure
run_test "baseline-defaults" pass \
	$BASE

# 2. Minimal -- disable/strip everything possible
run_test "baseline-minimal" pass \
	$BASE \
	--without-blkid --without-libnvme --without-systemd --without-udev \
	--with-snapshots=none --with-mirrors=none \
	--with-thin=none --with-cache=none --with-vdo=none \
	--with-writecache=none --with-integrity=none \
	--disable-readline --disable-realtime --disable-selinux \
	--disable-blkid_wiping --disable-nvme-wwid \
	--disable-systemd-journal --disable-app-machineid --disable-sd-notify \
	--disable-o_direct --disable-nls

# 3. Maximal -- enable everything available on this system
MAXOPTS="$BASE --enable-debug --enable-cmdlib --enable-pkgconfig --enable-nls"
MAXOPTS="$MAXOPTS --enable-dmfilemapd --enable-lvmpolld"
MAXOPTS="$MAXOPTS --enable-dmeventd"
MAXOPTS="$MAXOPTS --enable-units-compat"
[ "$HAVE_UDEV" = 1 ]        && MAXOPTS="$MAXOPTS --enable-udev_sync --enable-udev_rules"
[ "$HAVE_SANLOCK" = 1 ]      && MAXOPTS="$MAXOPTS --enable-lvmlockd-sanlock"
[ "$HAVE_DLM" = 1 ]          && MAXOPTS="$MAXOPTS --enable-lvmlockd-dlm"
[ "$HAVE_DLMCONTROL" = 1 ]   && MAXOPTS="$MAXOPTS --enable-lvmlockd-dlmcontrol"

run_test "baseline-maximal" pass $MAXOPTS

echo ""

# ==========================================================================
# SECTION 2: One-at-a-time toggles from baseline (each flips ONE option)
# ==========================================================================

section 2 "Single option toggles"

# --- Segment types off individually ---
run_test "toggle-no-snapshots" pass $BASE --with-snapshots=none
run_test "toggle-no-mirrors" pass   $BASE --with-mirrors=none
run_test "toggle-no-thin" pass      $BASE --with-thin=none
run_test "toggle-no-cache" pass     $BASE --with-cache=none
run_test "toggle-no-vdo" pass       $BASE --with-vdo=none
run_test "toggle-no-writecache" pass $BASE --with-writecache=none
run_test "toggle-no-integrity" pass $BASE --with-integrity=none

# --- Library toggles ---
run_test "toggle-no-blkid" pass     $BASE --without-blkid
run_test "toggle-no-libnvme" pass   $BASE --without-libnvme
run_test "toggle-no-systemd" pass   $BASE --without-systemd
run_test "toggle-no-udev" pass      $BASE --without-udev

# --- Feature enables from default-off ---
run_test "toggle-debug" pass        $BASE --enable-debug
run_test "toggle-cmdlib" pass       $BASE --enable-cmdlib
run_test "toggle-pkgconfig" pass    $BASE --enable-pkgconfig
run_test "toggle-static-link" pass  $BASE --enable-static_link
run_test "toggle-no-shared" pass    $BASE --disable-shared
run_test "toggle-no-readline" pass  $BASE --disable-readline
run_test "toggle-no-realtime" pass  $BASE --disable-realtime
run_test "toggle-no-selinux" pass   $BASE --disable-selinux
run_test "toggle-no-devmapper" pass $BASE --disable-devmapper
run_test "toggle-no-ioctl" pass     $BASE --disable-ioctl
run_test "toggle-no-odirect" pass   $BASE --disable-o_direct
run_test "toggle-units-compat" pass $BASE --enable-units-compat
run_test "toggle-nls" pass          $BASE --enable-nls
run_test "toggle-dmfilemapd" pass   $BASE --enable-dmfilemapd

# Conditionally enable features needing libraries
[ "$HAVE_UDEV" = 1 ] && \
	run_test "toggle-udev-sync" pass $BASE --enable-udev_sync
[ "$HAVE_UDEV" = 1 ] && \
	run_test "toggle-udev-rules" pass $BASE --enable-udev_sync --enable-udev_rules

run_test "toggle-lvmpolld" pass     $BASE --enable-lvmpolld
run_test "toggle-dmeventd" pass     $BASE --enable-dmeventd --enable-cmdlib
run_test "toggle-ocf" pass          $BASE --enable-ocf

[ "$HAVE_VALGRIND" = 1 ] && \
	run_test "toggle-valgrind-pool" pass $BASE --enable-valgrind-pool
run_test "toggle-asan" pass         $BASE --enable-asan
run_test "toggle-tsan" pass         $BASE --enable-tsan

# Disable auto-detected features individually
run_test "toggle-no-blkid-wiping" pass   $BASE --disable-blkid_wiping
run_test "toggle-no-nvme-wwid" pass      $BASE --disable-nvme-wwid
run_test "toggle-no-systemd-journal" pass $BASE --disable-systemd-journal
run_test "toggle-no-app-machineid" pass  $BASE --disable-app-machineid
run_test "toggle-no-sd-notify" pass      $BASE --disable-sd-notify
run_test "toggle-no-fsadm" pass          $BASE --disable-fsadm
run_test "toggle-no-lvmimportvdo" pass   $BASE --disable-lvmimportvdo
run_test "toggle-no-blkdeactivate" pass  $BASE --disable-blkdeactivate

echo ""

# ==========================================================================
# SECTION 3: Known conflicts -- these MUST fail at configure time
# ==========================================================================

section 3 "Expected configure failures"

# asan + tsan = mutually exclusive
run_test "conflict-asan-tsan" fail \
	$BASE --enable-asan --enable-tsan

# dmeventd without cmdlib
run_test "conflict-dmeventd-no-cmdlib" fail \
	$BASE --enable-dmeventd --disable-cmdlib

# dmeventd without mirrors=internal
run_test "conflict-dmeventd-no-mirrors" fail \
	$BASE --enable-dmeventd --enable-cmdlib --with-mirrors=none

# cmdlib without shared linking
run_test "conflict-cmdlib-no-shared" fail \
	$BASE --enable-cmdlib --disable-shared

# udev_sync without udev library
run_test "conflict-udev-sync-no-udev" fail \
	$BASE --enable-udev_sync --without-udev

# blkid_wiping explicitly enabled without blkid library
run_test "conflict-blkid-wiping-no-blkid" fail \
	$BASE --enable-blkid_wiping --without-blkid

# nvme-wwid explicitly enabled without libnvme
run_test "conflict-nvme-wwid-no-libnvme" fail \
	$BASE --enable-nvme-wwid --without-libnvme

# notify-dbus without systemd
run_test "conflict-notify-dbus-no-systemd" fail \
	$BASE --enable-notify-dbus --without-systemd

# systemd-journal explicitly enabled without systemd
run_test "conflict-journal-no-systemd" fail \
	$BASE --enable-systemd-journal --without-systemd

# app-machineid explicitly enabled without systemd
run_test "conflict-machineid-no-systemd" fail \
	$BASE --enable-app-machineid --without-systemd

# sd-notify explicitly enabled without systemd
run_test "conflict-sd-notify-no-systemd" fail \
	$BASE --enable-sd-notify --without-systemd

echo ""

# ==========================================================================
# SECTION 4: Dependency groups -- test related options together
# ==========================================================================

section 4 "Dependency groups"

# --- Segment types: all on vs all off ---
run_test "group-all-segtypes-off" pass \
	$BASE \
	--with-snapshots=none --with-mirrors=none --with-thin=none \
	--with-cache=none --with-vdo=none --with-writecache=none \
	--with-integrity=none

run_test "group-all-segtypes-on" pass \
	$BASE \
	--with-snapshots=internal --with-mirrors=internal \
	--with-thin=internal --with-cache=internal --with-vdo=internal \
	--with-writecache=internal --with-integrity=internal

# --- Thin without other segtypes ---
run_test "group-thin-only" pass \
	$BASE \
	--with-snapshots=none --with-mirrors=none --with-cache=none \
	--with-vdo=none --with-writecache=none --with-integrity=none \
	--with-thin=internal

# --- Cache without other segtypes ---
run_test "group-cache-only" pass \
	$BASE \
	--with-snapshots=none --with-mirrors=none --with-thin=none \
	--with-vdo=none --with-writecache=none --with-integrity=none \
	--with-cache=internal

# --- No external libraries at all ---
run_test "group-no-libs" pass \
	$BASE \
	--without-blkid --without-libnvme --without-systemd --without-udev \
	--disable-selinux --disable-readline

# --- Systemd feature group: all systemd-dependent features off ---
run_test "group-no-systemd-features" pass \
	$BASE --without-systemd \
	--disable-systemd-journal --disable-app-machineid \
	--disable-sd-notify --disable-blkid_wiping

# --- Systemd feature group: all systemd-dependent features on ---
if [ "$HAVE_SYSTEMD_221" = 1 ]; then
	run_test "group-all-systemd-features" pass \
		$BASE --enable-notify-dbus \
		--enable-systemd-journal --enable-sd-notify
fi
if [ "$HAVE_SYSTEMD_234" = 1 ]; then
	run_test "group-all-systemd-features-234" pass \
		$BASE --enable-notify-dbus \
		--enable-systemd-journal --enable-sd-notify \
		--enable-app-machineid
fi

# --- Locking group ---
LOCK_OPTS=""
[ "$HAVE_SANLOCK" = 1 ] && LOCK_OPTS="$LOCK_OPTS --enable-lvmlockd-sanlock"
[ "$HAVE_DLM" = 1 ]     && LOCK_OPTS="$LOCK_OPTS --enable-lvmlockd-dlm"
[ "$HAVE_DLMCONTROL" = 1 ] && LOCK_OPTS="$LOCK_OPTS --enable-lvmlockd-dlmcontrol"
if [ -n "$LOCK_OPTS" ]; then
	run_test "group-all-locking" pass $BASE $LOCK_OPTS
fi

# --- Locking disabled ---
run_test "group-no-locking" pass \
	$BASE --disable-use-lvmlockd --disable-use-lvmpolld

# --- dmeventd + lvmpolld + cmdlib (daemon group) ---
run_test "group-all-daemons" pass \
	$BASE --enable-dmeventd --enable-cmdlib --enable-lvmpolld

# --- dmeventd + lvmpolld + dmfilemapd ---
run_test "group-all-daemons-plus-filemapd" pass \
	$BASE --enable-dmeventd --enable-cmdlib --enable-lvmpolld \
	--enable-dmfilemapd

# --- UDev group: sync + rules + rule-exec-detection ---
if [ "$HAVE_UDEV" = 1 ]; then
	run_test "group-full-udev" pass \
		$BASE --enable-udev_sync --enable-udev_rules \
		--enable-udev-rule-exec-detection
fi

# --- readline vs editline ---
if [ "$HAVE_READLINE" = 1 ]; then
	run_test "group-readline-on" pass $BASE --enable-readline
fi
if [ "$HAVE_EDITLINE" = 1 ]; then
	run_test "group-editline-on" pass \
		$BASE --disable-readline --enable-editline
fi
run_test "group-no-line-editing" pass \
	$BASE --disable-readline

echo ""

# ==========================================================================
# SECTION 5: Linking combinations
# ==========================================================================

section 5 "Linking combinations"

# Static link + debug
run_test "link-static-debug" pass \
	$BASE --enable-static_link --enable-debug

# Static link -- no shared (pure static build)
run_test "link-static-no-shared" pass \
	$BASE --enable-static_link --disable-shared

# No shared, no cmdlib (since cmdlib needs shared)
run_test "link-no-shared-no-cmdlib" pass \
	$BASE --disable-shared

echo ""

# ==========================================================================
# SECTION 6: Build-affecting combos (sanitizers, profiling, optimization)
# ==========================================================================

section 6 "Build flags"

run_test "build-asan-debug" pass $BASE --enable-asan --enable-debug
run_test "build-tsan-debug" pass $BASE --enable-tsan --enable-debug
run_test "build-profiling" pass  $BASE --enable-profiling
run_test "build-O0" pass         $BASE --with-optimisation=-O0
run_test "build-O3" pass         $BASE --with-optimisation=-O3
run_test "build-Os" pass         $BASE --with-optimisation=-Os

echo ""

# ==========================================================================
# SECTION 7: Stress combos -- unusual but valid multi-option mixes
# ==========================================================================

section 7 "Stress combos"

# Minimal + static
run_test "stress-minimal-static" pass \
	$BASE --enable-static_link --disable-shared \
	--without-blkid --without-libnvme --without-systemd --without-udev \
	--with-snapshots=none --with-mirrors=none \
	--with-thin=none --with-cache=none --with-vdo=none \
	--with-writecache=none --with-integrity=none \
	--disable-readline --disable-realtime --disable-selinux \
	--disable-blkid_wiping --disable-nvme-wwid \
	--disable-nls

# Debug + all daemons + all segtypes
run_test "stress-debug-full" pass \
	$BASE --enable-debug --enable-dmeventd --enable-cmdlib \
	--enable-lvmpolld --enable-dmfilemapd

# No segtypes but with daemons (dmeventd needs mirrors!)
run_test "stress-daemons-no-segtypes" fail \
	$BASE --enable-dmeventd --enable-cmdlib \
	--with-mirrors=none

# Devmapper disabled + all segtypes on
run_test "stress-no-devmapper-segtypes" pass \
	$BASE --disable-devmapper

# Everything off except core
run_test "stress-core-only" pass \
	$BASE \
	--without-blkid --without-libnvme --without-systemd --without-udev \
	--with-snapshots=none --with-mirrors=none \
	--with-thin=none --with-cache=none --with-vdo=none \
	--with-writecache=none --with-integrity=none \
	--disable-readline --disable-realtime --disable-selinux \
	--disable-devmapper --disable-blkid_wiping --disable-nvme-wwid \
	--disable-systemd-journal --disable-app-machineid --disable-sd-notify \
	--disable-ioctl --disable-o_direct --disable-nls \
	--disable-fsadm --disable-lvmimportvdo --disable-blkdeactivate

# asan + full daemons
run_test "stress-asan-daemons" pass \
	$BASE --enable-asan --enable-debug \
	--enable-dmeventd --enable-cmdlib --enable-lvmpolld

# tsan + full daemons
run_test "stress-tsan-daemons" pass \
	$BASE --enable-tsan --enable-debug \
	--enable-dmeventd --enable-cmdlib --enable-lvmpolld

# dbus-service (requires python3 + pyudev + dbus modules)
# Only test configure -- python deps may be missing
if [ "$HAVE_SYSTEMD_221" = 1 ]; then
	run_test "stress-dbus-service" pass \
		$BASE --enable-dbus-service --enable-notify-dbus
fi

# Static + debug + no libs
run_test "stress-static-debug-no-libs" pass \
	$BASE --enable-static_link --disable-shared --enable-debug \
	--without-blkid --without-libnvme --without-systemd --without-udev \
	--disable-readline --disable-selinux

# Full locking daemons
if [ "$HAVE_SANLOCK" = 1 ] && [ "$HAVE_DLM" = 1 ]; then
	run_test "stress-full-locking" pass \
		$BASE --enable-lvmlockd-sanlock --enable-lvmlockd-dlm \
		--enable-lvmpolld --enable-dmeventd --enable-cmdlib
fi
if [ "$HAVE_SANLOCK" = 1 ] && [ "$HAVE_DLM" = 1 ] && [ "$HAVE_DLMCONTROL" = 1 ]; then
	run_test "stress-full-locking-dlmcontrol" pass \
		$BASE --enable-lvmlockd-sanlock --enable-lvmlockd-dlm \
		--enable-lvmlockd-dlmcontrol \
		--enable-lvmpolld --enable-dmeventd --enable-cmdlib
fi

echo ""

# ==========================================================================
# SECTION 8: Default segtype overrides
# ==========================================================================

section 8 "Default segtype overrides"

run_test "segtype-mirror-mirror" pass \
	$BASE --with-default-mirror-segtype=mirror

run_test "segtype-raid10-mirror" pass \
	$BASE --with-default-raid10-segtype=mirror

run_test "segtype-sparse-snapshot" pass \
	$BASE --with-default-sparse-segtype=snapshot

echo ""

# ==========================================================================
# SECTION 9: Misc flags that don't affect compilation much
# ==========================================================================

section 9 "Misc flags"

run_test "misc-device-nodes-create" pass \
	$BASE --with-device-nodes-on=create

run_test "misc-device-nodes-resume" pass \
	$BASE --with-device-nodes-on=resume

run_test "misc-name-mangling-none" pass \
	$BASE --with-default-name-mangling=none

run_test "misc-name-mangling-hex" pass \
	$BASE --with-default-name-mangling=hex

run_test "misc-devices-file-on" pass \
	$BASE --with-default-use-devices-file=1

run_test "misc-event-activation-off" pass \
	$BASE --with-default-event-activation=0

run_test "misc-symvers-no" pass \
	$BASE --with-symvers=no

echo ""

# ==========================================================================
# Summary
# ==========================================================================

echo "============================================================"
echo "SUMMARY"
echo "============================================================"
echo ""
echo "  Total:    $TOTAL"
echo "  Passed:   $PASS"
echo "  XFailed:  $XFAIL  (expected failures)"
echo "  Failed:   $FAIL"
echo "  Skipped:  $SKIP"
echo ""

if [ -n "$FAILURES" ]; then
	echo "FAILURES:"
	printf "$FAILURES"
	echo ""
fi

if [ "$DRY_RUN" = 1 ]; then
	echo "(dry-run mode -- nothing was executed)"
fi

echo "Logs in: $OUTDIR"
echo ""

[ "$FAIL" -eq 0 ] || exit 1
