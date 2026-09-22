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
#define process_event raid_test_process_event
#define register_device raid_test_register_device
#define unregister_device raid_test_unregister_device
#include "daemons/dmeventd/libdevmapper-event.h"

/* Use the real plugin and status parser, with only command execution mocked. */
#undef DM_EVENT_LOG_FN
#define DM_EVENT_LOG_FN(subsys)
#include "daemons/dmeventd/plugins/raid/dmeventd_raid.c"

static unsigned _repairs;
static int _repair_result;

int dmeventd_lvm2_init(void) { return 1; }
void dmeventd_lvm2_exit(void) {}
void dmeventd_lvm2_lock(void) {}
void dmeventd_lvm2_unlock(void) {}
int dmeventd_lvm2_command(struct dm_pool *mem, char *buffer, size_t size,
			const char *cmd, const char *device) { return 1; }
int dmeventd_lvm2_run(const char *cmd)
{
	_repairs++;
	return _repair_result;
}

static void *_init(void)
{
	struct dm_pool *mem = dm_pool_create("raid event test", 1024);
	struct dso_state *state;

	T_ASSERT(mem);
	state = dm_pool_zalloc(mem, sizeof(*state));
	T_ASSERT(state);
	state->mem = mem;
	_repairs = 0;
	_repair_result = 1;
	return state;
}

static void _fixture_exit(void *fixture)
{
	struct dso_state *state = fixture;
	dm_pool_destroy(state->mem);
}

static void _event(void *fixture, const char *status, unsigned repairs)
{
	_process_raid_event(fixture, status, "test-raid");
	T_ASSERT_EQUAL(_repairs, repairs);
}

/*
 * Repeated events with the same failed-source status must not run the policy
 * again, and a successful no-op policy must not re-arm the attempt set either. */
static void _unchanged(void *fixture)
{
	_repair_result = 0;
	_event(fixture, "raid1 2 AD 100/100 idle 0", 1);
	_event(fixture, "raid1 2 AD 100/100 idle 0", 1);
	_repair_result = 1; /* A successful policy no-op must not loop either. */
	_event(fixture, "raid1 2 AA 100/100 idle 0", 1);
	_event(fixture, "raid1 2 AD 100/100 idle 0", 2);
	_event(fixture, "raid1 2 AD 100/100 idle 0", 2);
}

/*
 * A replacement that is still resyncing is not retried; only a completed
 * replacement that leaves another leg dead triggers the next repair, and the
 * fresh repair is itself suppressed on the following identical event. */
static void _partial_repair(void *fixture)
{
	_event(fixture, "raid1 3 ADD 100/100 idle 0", 1);
	_event(fixture, "raid1 3 AaD 10/100 recover 0", 1);
	_event(fixture, "raid1 3 AaD 80/100 recover 0", 1);
	_event(fixture, "raid1 3 AAD 100/100 idle 0", 2);
	_event(fixture, "raid1 3 AAD 100/100 idle 0", 2);
	/* The replacement fails while another leg is still dead. */
	_event(fixture, "raid1 3 ADD 10/100 recover 0", 3);
	_event(fixture, "raid1 3 ADD 100/100 idle 0", 4);
	_event(fixture, "raid1 3 ADD 100/100 idle 0", 4);
}

/*
 * When the kernel coalesces recovery into a single complete status, the
 * repair still runs exactly once and duplicate events stay quiet. */
static void _coalesced_repair(void *fixture)
{
	_event(fixture, "raid1 3 ADD 100/100 idle 0", 1);
	/* No intermediate 'a' or incomplete ratio was delivered. */
	_event(fixture, "raid1 3 AAD 100/100 idle 0", 2);
	_event(fixture, "raid1 3 AAD 100/100 idle 0", 2);
}

/*
 * A fresh failing leg is repaired immediately while a recovery is still in
 * progress, and a changed device count invalidates the positional attempts. */
static void _new_failure(void *fixture)
{
	_event(fixture, "raid1 3 ADA 20/100 recover 0", 1);
	_event(fixture, "raid1 3 ADA 20/100 recover 0", 1);
	_event(fixture, "raid1 3 ADD 20/100 recover 0", 2);
	/* Changed leg count invalidates positional attempts. */
	_event(fixture, "raid1 2 AD 20/100 recover 0", 3);
}

/*
 * The libdm parser normalizes a full raw ratio; with the dedicated legs
 * failed the plugin detects the primary failure from the idle full-ratio
 * signature, and leaving the episode permits later detections. */
static void _idle_primary(void *fixture)
{
	_repair_result = 0;
	/* libdm normalizes the full raw ratio to 99/100. */
	_event(fixture, "raid1 2 Aa 100/100 idle 0", 1);
	_event(fixture, "raid1 2 Aa 100/100 idle 0", 1);
	/* Leaving the episode permits a later failed-source report. */
	_event(fixture, "raid1 2 Aa 40/100 recover 0", 1);
	_event(fixture, "raid1 2 Aa 100/100 idle 0", 2);
	_event(fixture, "raid1 2 AA 100/100 idle 0", 2);
	_event(fixture, "raid1 2 aA 40/100 idle 0", 3);
	_event(fixture, "raid1 2 aA 40/100 idle 0", 3);
}

/*
 * An incomplete sync in an idle snapshot with the same parsed ratio must not
 * be mistaken for a failed source. */
static void _idle_incomplete(void *fixture)
{
	_event(fixture, "raid1 2 Aa 99/100 idle 0", 0);
	_event(fixture, "raid1 2 Aa 100/100 recover 0", 0);
	_event(fixture, "raid1 2 AA 100/100 idle 0", 0);
	_event(fixture, "raid1 3 Aaa 100/100 idle 0", 1);
	_event(fixture, "raid1 3 Aaa 100/100 idle 0", 1);
}

void dmeventd_raid_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_init, _fixture_exit);

	T_ASSERT(ts);
	register_test(ts, "/dmeventd/raid/unchanged", "suppress duplicate repairs", _unchanged);
	register_test(ts, "/dmeventd/raid/partial", "finish partial replacement", _partial_repair);
	register_test(ts, "/dmeventd/raid/coalesced", "observe only completed replacement", _coalesced_repair);
	register_test(ts, "/dmeventd/raid/new-failure", "repair fresh failures immediately", _new_failure);
	register_test(ts, "/dmeventd/raid/idle-primary", "detect normalized primary failure", _idle_primary);
	register_test(ts, "/dmeventd/raid/idle-incomplete", "exclude incomplete idle snapshot", _idle_incomplete);
	dm_list_add(all_tests, &ts->list);
}
