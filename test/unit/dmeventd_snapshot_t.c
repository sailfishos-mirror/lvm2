/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "units.h"
#include "lib/misc/lib.h"
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <pthread.h>

#define process_event snapshot_test_process_event
#define register_device snapshot_test_register_device
#define unregister_device snapshot_test_unregister_device
#include "daemons/dmeventd/libdevmapper-event.h"
#define dmeventd_lvm2_init snapshot_test_lvm2_init
#define dmeventd_lvm2_exit snapshot_test_lvm2_exit
#define dmeventd_lvm2_lock snapshot_test_lvm2_lock
#define dmeventd_lvm2_unlock snapshot_test_lvm2_unlock
#define dmeventd_lvm2_command snapshot_test_lvm2_command
#define dmeventd_lvm2_run snapshot_test_lvm2_run
#include "daemons/dmeventd/plugins/lvm2/dmeventd_lvm.h"

/* Real status parser and policy code; no real device, mount, or signal I/O. */
static const char *_type, *_status;
static uint64_t _length;
static unsigned _policies, _reads, _signals, _mount_reads, _forks, _locks;
static int _policy_result, _info_result, _mount_result, _wait_status;
static pid_t _fork_result, _wait_result;

static const char *_task_name(const struct dm_task *dmt) { return "test-snapshot"; }
static int _task_info(struct dm_task *dmt, struct dm_info *info)
{
	memset(info, 0, sizeof(*info));
	info->major = 253;
	info->minor = 7;
	return _info_result;
}
static void *_next_target(struct dm_task *dmt, void *next, uint64_t *start,
			  uint64_t *length, char **type, char **params)
{
	T_ASSERT(!next);
	_reads++;
	*start = 0;
	*length = _length;
	*type = (char *) _type;
	*params = (char *) _status;
	return NULL;
}
static int _pthread_kill(pthread_t thread, int sig)
{
	T_ASSERT(pthread_equal(thread, pthread_self()));
	T_ASSERT_EQUAL(sig, SIGALRM);
	_signals++;
	return 0;
}
static FILE *_mounts_open(const char *path, const char *mode)
{
	FILE *f;

	T_ASSERT(!strcmp(path, "/proc/mounts"));
	T_ASSERT(!strcmp(mode, "r"));
	_mount_reads++;
	if (!_mount_result)
		return NULL;
	T_ASSERT(f = tmpfile());
	fputs("malformed\n/missing /skip ext4 rw 0 0\n"
	      "/regular /skip ext4 rw 0 0\n/other /skip ext4 rw 0 0\n"
	      "/snapshot /test-mount ext4 rw 0 0\n", f);
	rewind(f);
	return f;
}
static int _mount_stat(const char *path, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	if (!strcmp(path, "/missing"))
		return -1;
	st->st_mode = !strcmp(path, "/regular") ? S_IFREG : S_IFBLK;
	st->st_rdev = makedev(253, !strcmp(path, "/snapshot") ? 7 : 8);
	return 0;
}
static pid_t _fork(void)
{
	_forks++;
	T_ASSERT(_fork_result != 0); /* Never execute a real unmount. */
	return _fork_result;
}
static pid_t _waitpid(pid_t pid, int *status, int options)
{
	T_ASSERT_EQUAL(pid, 123);
	T_ASSERT_EQUAL(options, 0);
	*status = _wait_status;
	return _wait_result;
}

#define dm_task_get_name _task_name
#define dm_task_get_info _task_info
#define dm_get_next_target _next_target
#define pthread_kill _pthread_kill
#define fopen _mounts_open
#define stat(path, st) _mount_stat(path, st)
#define fork _fork
#define waitpid _waitpid
#undef DM_EVENT_LOG_FN
#define DM_EVENT_LOG_FN(subsys)
#include "daemons/dmeventd/plugins/snapshot/dmeventd_snapshot.c"

#define DMEVENTD_UNIT_LVM2_PREFIX snapshot_test
#include "dmeventd_unit_lvm2_lock.h"
int snapshot_test_lvm2_command(struct dm_pool *mem, char *buffer, size_t size,
			      const char *cmd, const char *device)
{
	T_ASSERT(!strcmp(cmd, "lvextend --use-policies"));
	return dm_snprintf(buffer, size, "test-policy") >= 0;
}
int snapshot_test_lvm2_run(const char *cmd)
{
	T_ASSERT_EQUAL(_locks, 1);
	T_ASSERT(!strcmp(cmd, "test-policy"));
	_policies++;
	return _policy_result;
}
static void *_init(void)
{
	void *state = NULL;

	_policies = _reads = _signals = _mount_reads = _forks = _locks = 0;
	_policy_result = _info_result = _mount_result = 1;
	_fork_result = _wait_result = 123;
	_wait_status = 0;
	_type = "snapshot";
	_status = "10/100 2";
	_length = 1024;
	T_ASSERT(snapshot_test_register_device("test-snapshot", "test-uuid", 0, 0, &state));
	return state;
}
static void _exit_fixture(void *fixture)
{
	T_ASSERT_EQUAL(_locks, 0);
	T_ASSERT(snapshot_test_unregister_device("test-snapshot", "test-uuid", 0, 0, &fixture));
}
static void _event(void *fixture, unsigned policies)
{
	/* Task accessors above do not dereference the opaque handle. */
	snapshot_test_process_event((struct dm_task *) fixture, DM_EVENT_TIMEOUT, &fixture);
	T_ASSERT_EQUAL(_policies, policies);
}
static void _usage(void *fixture, unsigned used, unsigned total, unsigned policies)
{
	char status[80];

	T_ASSERT(dm_snprintf(status, sizeof(status), "%u/%u 2", used, total) >= 0);
	_status = status;
	_event(fixture, policies);
	_status = NULL;
}
/*
 * Snapshot thresholds are inclusive: usage steps fire at the exact crossing
 * and jumps over several steps schedule each one. */
static void _thresholds(void *fixture)
{
	/* Snapshot checks use inclusive step boundaries. */
	static const unsigned used[] = { 2, 49, 50, 50, 54, 55, 56, 81, 81, 95, 99, 100 };
	static const unsigned calls[] = { 0, 0, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5 };
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(used); ++i)
		_usage(fixture, used[i], 100, calls[i]);
}
/*
 * A successful policy no-op at 100% must not repeat on every event.  The
 * snapshot is still valid until the kernel reports invalidation. */
static void _full(void *fixture)
{
	_status = "100/100 2";
	_event(fixture, 1);
	_event(fixture, 1);
	T_ASSERT_EQUAL(_signals, 0);
	T_ASSERT_EQUAL(_mount_reads, 0);
	T_ASSERT_EQUAL(_forks, 0);
	_status = "Invalid";
	_event(fixture, 1);
	_status = NULL;
	T_ASSERT_EQUAL(_signals, 1);
	T_ASSERT_EQUAL(_mount_reads, _info_result ? 1 : 0);
	T_ASSERT_EQUAL(_forks, (_info_result && _mount_result) ? 1 : 0);
	_event(fixture, 1);
	T_ASSERT_EQUAL(_reads, 3);
	T_ASSERT_EQUAL(_signals, 1);
}
/*
 * A failed extension at 100% still gets a backoff retry while the snapshot
 * remains valid.  A later manual resize re-arms the threshold. */
static void _full_failure(void *fixture)
{
	_policy_result = 0;
	_status = "100/100 2";
	_event(fixture, 1);
	_event(fixture, 1); /* Postponed by backoff. */
	_policy_result = 1;
	_event(fixture, 2);
	_event(fixture, 2); /* Successful no-op is not repeated. */
	T_ASSERT_EQUAL(_signals, 0);
	T_ASSERT_EQUAL(_mount_reads, 0);
	T_ASSERT_EQUAL(_forks, 0);
	_status = "100/200 2";
	_event(fixture, 3);
	_status = NULL;
	T_ASSERT_EQUAL(_signals, 0);
	T_ASSERT_EQUAL(_reads, 5);
}
/*
 * A failed extension retries after the backoff while usage is unchanged; a
 * successful extension resets the failure so no further retry follows. */
static void _retry(void *fixture)
{
	_policy_result = 0;
	_usage(fixture, 81, 100, 1);
	_usage(fixture, 81, 100, 1);
	_policy_result = 1;
	_usage(fixture, 81, 100, 2);
	_usage(fixture, 81, 100, 2);
}
/*
 * Resizing the snapshot re-arms the threshold from the new size. */
static void _resize(void *fixture)
{
	_usage(fixture, 81, 100, 1);
	_usage(fixture, 162, 200, 2);
	_usage(fixture, 162, 400, 2);
	_usage(fixture, 200, 400, 3);
}
/*
 * Malformed status and a wrong or missing target leave monitoring usable;
 * the next valid event fires again. */
static void _invalid_text(void *fixture)
{
	const char *bad[] = { "", "garbage", "10/bad" };
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(bad); ++i) {
		_status = bad[i];
		_event(fixture, 0);
	}
	_type = "linear";
	_event(fixture, 0);
	_type = NULL;
	_event(fixture, 0);
	_type = "snapshot";
	T_ASSERT_EQUAL(_signals, 0);
	_usage(fixture, 50, 100, 1);
}
static void _terminal(void *fixture, const char *status)
{
	_status = status;
	_event(fixture, 0);
	T_ASSERT_EQUAL(_signals, 1);
	T_ASSERT_EQUAL(_mount_reads, _info_result ? 1 : 0);
	T_ASSERT_EQUAL(_forks, (_info_result && _mount_result) ? 1 : 0);
	_event(fixture, 0);
	T_ASSERT_EQUAL(_reads, 1);
	T_ASSERT_EQUAL(_signals, 1);
}
/*
 * An invalid snapshot is unmounted once (SIGALRM); repeated events do not
 * unmount it again. */
static void _invalid(void *fixture) { _terminal(fixture, "Invalid"); }
/*
 * An overflowed snapshot is likewise unmounted only once. */
static void _overflow(void *fixture) { _terminal(fixture, "Overflow"); }
/*
 * A zero-sized snapshot ends monitoring without an unmount. */
static void _zero_total(void *fixture) { _terminal(fixture, "0/0 0"); }
/*
 * Without device identity no unmount is attempted. */
static void _info_failure(void *fixture) { _info_result = 0; _invalid(fixture); }
/*
 * An unavailable /proc/mounts list suppresses the unmount. */
static void _mount_failure(void *fixture) { _mount_result = 0; _invalid(fixture); }
/*
 * A failed unmount fork leaves the snapshot mounted. */
static void _fork_failure(void *fixture) { _fork_result = -1; _invalid(fixture); }
/*
 * A failed wait still counts the unmount attempt as delivered, so the next
 * event does not restart it. */
static void _wait_failure(void *fixture) { _wait_result = -1; _invalid(fixture); }
/*
 * An unmount that exits with failure is not re-run on the next event. */
static void _unmount_failure(void *fixture) { _wait_status = 1 << 8; _invalid(fixture); }
/*
 * An unmount killed by SIGTERM is not re-run on the next event. */
static void _unmount_signal(void *fixture) { _wait_status = SIGTERM; _invalid(fixture); }
/*
 * Only data sectors count against the snapshot threshold: metadata-heavy
 * usage with free data sectors keeps monitoring and does not unmount. */
static void _provisioned(void *fixture)
{
	_length = 80;
	_usage(fixture, 81, 100, 1); /* 79 data sectors: still needs monitoring. */
	_usage(fixture, 82, 100, 1); /* 80 data sectors: fully provisioned. */
	T_ASSERT_EQUAL(_signals, 1);
	T_ASSERT_EQUAL(_mount_reads, 0);
}

void dmeventd_snapshot_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_init, _exit_fixture);

	T_ASSERT(ts);
#define TEST(name, desc, fn) register_test(ts, "/dmeventd/snapshot/" name, desc, fn)
	TEST("thresholds", "inclusive boundaries and usage jumps", _thresholds);
	TEST("full", "100% status monitored until kernel invalidates", _full);
	TEST("full-failure", "failed extension at 100% retries then resize", _full_failure);
	TEST("retry", "retry failed extension at unchanged usage", _retry);
	TEST("resize", "resize rearms policy threshold", _resize);
	TEST("invalid-text", "recover from malformed status and wrong targets", _invalid_text);
	TEST("invalid", "unmount invalid snapshot only once", _invalid);
	TEST("overflow", "unmount overflowed snapshot", _overflow);
	TEST("zero-total", "zero size ends monitoring", _zero_total);
	TEST("info-failure", "no unmount without device identity", _info_failure);
	TEST("mount-failure", "handle unavailable mounts list", _mount_failure);
	TEST("fork-failure", "handle unmount fork failure", _fork_failure);
	TEST("wait-failure", "handle unmount wait failure", _wait_failure);
	TEST("unmount-failure", "handle unsuccessful unmount", _unmount_failure);
	TEST("unmount-signal", "handle signalled unmount", _unmount_signal);
	TEST("provisioned", "exclude metadata from provisioned data", _provisioned);
#undef TEST
	dm_list_add(all_tests, &ts->list);
}
