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
#include <sys/wait.h>

#define process_event vdo_test_process_event
#define register_device vdo_test_register_device
#define unregister_device vdo_test_unregister_device
#include "daemons/dmeventd/libdevmapper-event.h"

/* Pool dm_task transport mocks are shared with the thin unit test. */
#define dmeventd_lvm2_init vdo_test_lvm2_init
#define dmeventd_lvm2_exit vdo_test_lvm2_exit
#define dmeventd_lvm2_lock vdo_test_lvm2_lock
#define dmeventd_lvm2_unlock vdo_test_lvm2_unlock
#define dmeventd_lvm2_command vdo_test_lvm2_command
#define dmeventd_lvm2_run vdo_test_lvm2_run
#include "daemons/dmeventd/plugins/lvm2/dmeventd_lvm.h"

static int _vdo_pool_setenv(const char *name, const char *value, int overwrite)
{
	T_ASSERT(overwrite);
	if (!strcmp(name, "LVM_RUN_BY_DMEVENTD"))
		T_ASSERT(!strcmp(value, "1"));
	else {
		T_ASSERT(!strcmp(name, "DMEVENTD_VDO_POOL"));
		T_ASSERT(!strcmp(value, "51"));
	}
	return 0;
}
#define DMEVENTD_UNIT_POOL_SETENV(name, value, overwrite) \
	_vdo_pool_setenv(name, value, overwrite)
#include "dmeventd_unit_pool_transport.h"
#define _current _dmeventd_unit_pool_current
#define _fresh _dmeventd_unit_pool_fresh

#include "dmeventd_unit_pool_scenarios.h"

#undef DM_EVENT_LOG_FN
#define DM_EVENT_LOG_FN(subsys)
#include "daemons/dmeventd/plugins/vdo/dmeventd_vdo.c"

#define DMEVENTD_UNIT_LVM2_PREFIX vdo_test
#include "dmeventd_unit_lvm2_lock.h"
int vdo_test_lvm2_command(struct dm_pool *mem, char *buffer, size_t size,
			  const char *cmd, const char *device)
{
	return dm_snprintf(buffer, size, "lvm test-policy") >= 0;
}
int vdo_test_lvm2_run(const char *cmd)
{
	T_ASSERT_EQUAL(_locks, 1);
	T_ASSERT(!strcmp(cmd, "test-policy"));
	_policies++;
	return _policy_result;
}

static void *_init(void)
{
	void *state = NULL;

	_policies = _reads = _creates = _destroys = _runs = _no_flush = 0;
	_forks = _waits = _locks = 0;
	_policy_result = _create_result = _uuid_result = _run_result = 1;
	_fork_result = 123;
	_wait_result = 0;
	_wait_status = 0;
	_current.type = _fresh.type = "vdo";
	_current.status = _fresh.status = "8:1 normal - online online 10 100";
	T_ASSERT(vdo_test_register_device("test-pool", "test-uuid", 0, 0, &state));
	return state;
}

static void _exit_fixture(void *fixture)
{
	struct dso_state *state = fixture;

	T_ASSERT_EQUAL(_locks, 0);
	/* Synthetic children must never reach unregister's real kill/sleep. */
	state->pid = -1;
	T_ASSERT(vdo_test_unregister_device("test-pool", "test-uuid", 0, 0, &fixture));
}

static void _event(void *fixture, enum dm_event_mask mask, unsigned policies)
{
	vdo_test_process_event((struct dm_task *) &_current, mask, &fixture);
	T_ASSERT_EQUAL(_policies, policies);
}

static void _usage(void *fixture, unsigned data, unsigned size, unsigned policies)
{
	char status[160];

	T_ASSERT(dm_snprintf(status, sizeof(status),
		"8:1 normal - online online %u %u", data, size) >= 0);
	_current.status = status;
	_event(fixture, DM_EVENT_TIMEOUT, policies);
	_current.status = NULL;
}

/*
 * Strict greater-than step boundaries (>50%, >55%, ...), per VDO plugin.
 */
static void _thresholds(void *fixture)
{
	unsigned i;

	for (i = 0; i < DMEVENTD_UNIT_POOL_STRICT_THRESHOLD_STEPS; ++i)
		_usage(fixture, dmeventd_unit_pool_strict_threshold_used[i], 100,
		       dmeventd_unit_pool_strict_threshold_calls[i]);
}

/*
 * A policy call that leaves the pool full advances the threshold to 100%;
 * while the pool stays at 100% the policy is not probed again. */
static void _full(void *fixture)
{
	_usage(fixture, 100, 100, 1);
	_usage(fixture, 100, 100, 1);
	_usage(fixture, 100, 100, 1);
}

/*
 * A failed policy on a still-full pool keeps the power-of-2 backoff active,
 * so the extension is retried after each retry spacing. */
static void _full_failure(void *fixture)
{
	_policy_result = 0;
	_usage(fixture, 100, 100, 1);
	_usage(fixture, 100, 100, 1);
	_usage(fixture, 100, 100, 2);
	_usage(fixture, 100, 100, 2);
	_usage(fixture, 100, 100, 2);
	_usage(fixture, 100, 100, 3);
	_policy_result = 1;
	_usage(fixture, 100, 100, 3);
	_usage(fixture, 100, 100, 3);
	_usage(fixture, 100, 100, 3);
	_usage(fixture, 100, 100, 3);
	_usage(fixture, 100, 100, 4);
	_usage(fixture, 100, 100, 4);
}

/*
 * Falling usage below a 5% step or below the 50% minimum re-arms the
 * threshold, so the next crossing of that step fires again. */
static void _usage_falls(void *fixture)
{
	_usage(fixture, 81, 100, 1);
	_usage(fixture, 40, 100, 1);
	_usage(fixture, 51, 100, 2);
	_usage(fixture, 76, 100, 3);
	_usage(fixture, 61, 100, 3);
	_usage(fixture, 80, 100, 4);
}

/*
 * Resize re-arms at CHECK_MINIMUM; repeating the same size and usage must not
 * run policy again until usage strictly exceeds the re-armed step. */
static void _resize(void *fixture)
{
	_usage(fixture, 81, 100, 1);
	_usage(fixture, 162, 200, 2);
	_usage(fixture, 162, 200, 2);
	_usage(fixture, 162, 400, 2);
	_usage(fixture, 204, 400, 3);
}

/*
 * Failed policy calls double the retry spacing (1, 2, 4, ... skipped events)
 * up to a 256-event cap; a success resets the backoff and a new crossing
 * starts a fresh failure episode. */
static void _backoff(void *fixture)
{
	unsigned limit, i, calls = 1;

	_policy_result = 0;
	_usage(fixture, 51, 100, calls);
	for (limit = 1; limit <= 512; limit *= 2) {
		for (i = 0; i < (limit < 256 ? limit : 256); ++i)
			_usage(fixture, 51, 100, calls);
		_usage(fixture, 51, 100, ++calls);
	}
	_policy_result = 1;
	for (i = 0; i < 256; ++i)
		_usage(fixture, 51, 100, calls);
	_usage(fixture, 51, 100, ++calls);
	_usage(fixture, 51, 100, calls);
	_policy_result = 0;
	_usage(fixture, 56, 100, ++calls);
	_usage(fixture, 56, 100, calls);
	_usage(fixture, 56, 100, ++calls);
}

/*
 * Resizing the LV clears both the recorded failure and the retry delay. */
static void _resize_clears_failure(void *fixture)
{
	_policy_result = 0;
	_usage(fixture, 51, 100, 1);
	_usage(fixture, 51, 100, 1);
	_policy_result = 1;
	_usage(fixture, 102, 200, 2);
	_usage(fixture, 102, 200, 2);
}

/*
 * Malformed status and a missing or non-VDO target must not disable
 * monitoring; a following valid status still works. */
static void _invalid(void *fixture)
{
	const char *bad[] = { "", "garbage", "8:1 normal", "8:1 normal - online online broken 100" };
	unsigned i;

	_usage(fixture, 51, 100, 1);
	for (i = 0; i < DM_ARRAY_SIZE(bad); ++i) {
		_current.status = bad[i];
		_event(fixture, DM_EVENT_TIMEOUT, 1);
	}
	_current.type = "linear";
	_event(fixture, DM_EVENT_TIMEOUT, 1);
	_current.type = NULL;
	_event(fixture, DM_EVENT_TIMEOUT, 1);
	_current.type = "vdo";
	_usage(fixture, 51, 100, 1);
	_usage(fixture, 56, 100, 2);
}

/*
 * A device-error mask runs the policy immediately without re-reading status
 * and without leaving a stale percent from the prior event. */
static void _device_error(void *fixture)
{
	struct dso_state *state = fixture;

	_usage(fixture, 81, 100, 1);
	_event(fixture, DM_EVENT_DEVICE_ERROR | DM_EVENT_TIMEOUT, 2);
	T_ASSERT_EQUAL(_reads, 1);
	T_ASSERT_EQUAL(_creates, 0);
	T_ASSERT_EQUAL(state->percent, 0);
}

/*
 * When policy failed on a device error, the plugin reads a fresh nonblocking
 * status (separate task, no flush) before deciding the retry. */
static void _error_refresh(void *fixture)
{
	struct dso_state *state = fixture;

	_usage(fixture, 10, 100, 0);
	_policy_result = 0;
	_current.status = "stale status must not be parsed";
	_fresh.status = "8:1 normal - online online 81 100";
	_event(fixture, DM_EVENT_DEVICE_ERROR, 1);
	T_ASSERT_EQUAL(_creates, 1);
	T_ASSERT_EQUAL(_runs, 1);
	T_ASSERT_EQUAL(_destroys, 1);
	T_ASSERT_EQUAL(_reads, 2);
	T_ASSERT_EQUAL(state->percent, 81 * DM_PERCENT_1);
}

/*
 * Each step of the status refresh (create, set-uuid, run) is cleaned up on
 * failure without calling the policy. */
static void _refresh_failure(void *fixture)
{
	_policy_result = 0;
	_create_result = 0;
	_event(fixture, DM_EVENT_DEVICE_ERROR, 1);
	T_ASSERT_EQUAL(_destroys, 0);
	_create_result = 1;
	_uuid_result = 0;
	_event(fixture, DM_EVENT_DEVICE_ERROR, 2);
	T_ASSERT_EQUAL(_destroys, 1);
	T_ASSERT_EQUAL(_runs, 0);
	_uuid_result = 1;
	_run_result = 0;
	_event(fixture, DM_EVENT_DEVICE_ERROR, 3);
	T_ASSERT_EQUAL(_destroys, 2);
	T_ASSERT_EQUAL(_reads, 0);
	_policy_result = 1;
	_event(fixture, DM_EVENT_DEVICE_ERROR, 4);
}

/*
 * While the policy child is still running, events neither read status nor
 * start a second command. */
static void _child_running(void *fixture)
{
	struct dso_state *state = fixture;

	state->pid = 123;
	_event(fixture, DM_EVENT_DEVICE_ERROR, 0);
	_event(fixture, DM_EVENT_TIMEOUT, 0);
	T_ASSERT_EQUAL(_waits, 2);
	T_ASSERT_EQUAL(_reads, 0);
	T_ASSERT_EQUAL(state->pid, 123);
	_wait_result = 123;
	_usage(fixture, 51, 100, 1);
	T_ASSERT_EQUAL(state->pid, -1);
}

/*
 * A child that exits with failure or with a signal is reaped so the next
 * event may start a new policy; the two exit modes are exercised by
 * _child_exit_failure and _child_signal. */
static void _child_failure(void *fixture, int status)
{
	struct dso_state *state = fixture;

	_usage(fixture, 10, 100, 0);
	state->pid = 123;
	_wait_result = 123;
	_wait_status = status;
	_usage(fixture, 51, 100, 0);
	T_ASSERT_EQUAL(state->pid, -1);
	_usage(fixture, 51, 100, 1);
	T_ASSERT_EQUAL(_waits, 1);
}
/*
 * Child exited with a non-zero status. */
static void _child_exit_failure(void *fixture) { _child_failure(fixture, 1 << 8); }
/*
 * Child was terminated by SIGTERM. */
static void _child_signal(void *fixture) { _child_failure(fixture, SIGTERM); }

/*
 * A failed fork is not retried until the slot is free: no second fork or
 * policy runs until the previous child is reaped. */
static void _fork_failure(void *fixture)
{
	struct dso_state *state = fixture;

	state->argv[0] = "/test-policy";
	_fork_result = -1;
	_usage(fixture, 51, 100, 0);
	T_ASSERT_EQUAL(_forks, 1);
	T_ASSERT_EQUAL(state->pid, -1);
	_usage(fixture, 51, 100, 0);
	T_ASSERT_EQUAL(_forks, 1);
	_fork_result = 123;
	_usage(fixture, 51, 100, 0);
	T_ASSERT_EQUAL(_forks, 2);
	_event(fixture, DM_EVENT_DEVICE_ERROR, 0);
	T_ASSERT_EQUAL(_forks, 2);
}

void dmeventd_vdo_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_init, _exit_fixture);

	T_ASSERT(ts);
#define TEST(name, desc, fn) register_test(ts, "/dmeventd/vdo/" name, desc, fn)
	TEST("thresholds", "exact boundaries and jumps", _thresholds);
	TEST("full", "full pool is not probed again", _full);
	TEST("full-failure", "failed policy on full pool keeps retrying", _full_failure);
	TEST("usage-falls", "rearm thresholds after usage falls", _usage_falls);
	TEST("resize", "rearm each resized LV", _resize);
	TEST("backoff", "bounded retry spacing and success reset", _backoff);
	TEST("resize-clears-failure", "resize cancels retry delay", _resize_clears_failure);
	TEST("invalid", "invalid status leaves monitoring usable", _invalid);
	TEST("device-error", "error bypasses ordinary status checks", _device_error);
	TEST("error-refresh", "failed policy reads fresh nonblocking status", _error_refresh);
	TEST("refresh-failure", "clean up failed status requests", _refresh_failure);
	TEST("child-running", "defer events until child finishes", _child_running);
	TEST("child-exit-failure", "retry failed child", _child_exit_failure);
	TEST("child-signal", "retry signalled child", _child_signal);
	TEST("fork-failure", "retry fork failure without parallel commands", _fork_failure);
#undef TEST
	dm_list_add(all_tests, &ts->list);
}
