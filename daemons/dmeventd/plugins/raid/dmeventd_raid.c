/*
 * Copyright (C) 2005-2017 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "daemons/dmeventd/plugins/lvm2/dmeventd_lvm.h"
#include "daemons/dmeventd/libdevmapper-event.h"
#include "lib/config/defaults.h"

/* Hold enough elements for the maximum number of RAID images */
#define	RAID_DEVS_ELEMS	((DEFAULT_RAID_MAX_IMAGES + 63) / 64)

enum raid_idle_primary_result {
	RAID_IDLE_PRIMARY_NONE = 0,
	RAID_IDLE_PRIMARY_HANDLED,
	RAID_IDLE_PRIMARY_FAILED,
};

/*
 * RAID monitoring has no periodic timeout.  Repair fresh failures immediately,
 * then retry a still-degraded array when an observed resync completes or a
 * previously dead leg recovers.  The latter also handles a partial replacement
 * whose intermediate resync status was never delivered to the plugin.
 *
 * Events need not change the status.  Do not retry an unchanged failure just
 * because another event arrived.  Conversely, never count or skip events:
 * there may be no further event after the transition that needs repair.
 *
 * Health positions identify legs only within one device-count epoch.  Reset
 * positional state when the count changes; forget each recovered leg as soon
 * as it is observed so a later failure at that position is a fresh failure.
 */
struct dso_state {
	struct dm_pool *mem;
	char cmd_lvconvert[512];
	/*
	 * Devices whose repair has already been attempted and which are
	 * still assumed dead.  A bit is cleared as soon as the device is
	 * observed no longer dead, so that a replacement that fails later is
	 * seen as a new failure again.
	 */
	uint64_t attempted_devs[RAID_DEVS_ELEMS];
	unsigned warned;
	/* Set once per idle primary-source episode after lvconvert is tried. */
	unsigned idle_primary_attempted;
	/* Whether the preceding event reported a complete sync ratio. */
	unsigned in_sync;
	/* Device count the positional state above belongs to; a change
	 * (leg removed/repositioned/added) starts a new epoch. */
	uint32_t dev_count;
};

/* Facts from one status event; only dso_state survives between events. */
struct raid_event {
	uint64_t dead_devs[RAID_DEVS_ELEMS];
	unsigned dead;
	unsigned new_dead;
	unsigned resynced;
	unsigned recovered;
};

DM_EVENT_LOG_FN("raid")

/*
 * If we are converting from non-RAID to RAID (e.g. linear -> raid1) and too
 * many original devices die, such that we cannot continue the "recover"
 * operation, the sync action will go to "idle", the unsynced devs will remain
 * at 'a', and the original devices will NOT SWITCH TO 'D', but will remain at
 * 'A' - hoping to be revived.
 *
 * This is simply the way the kernel works...
 *
 * Two signatures are known (see _lv_raid_has_primary_failure_on_recover in
 * lib/metadata/raid_manip.c): the array is not in-sync yet and a leg is the
 * leading 'a' -- or the kernel gave up on the recover and reports the ratio
 * as complete while a leg has not reached 'A'.  There is no 'D' to key on in
 * either case.
 *
 * dm_get_status_raid decrements a complete ratio for mixed A/a health in
 * idle/recover.  Read the raw ratio from the same validated status for this
 * signature, preserving the distinction between a failed source's "Aa
 * 100/100 idle" and a transient "Aa 99/100 idle".
 */
static int _raid_idle_primary_failure(const struct dm_status_raid *status,
				      const char *params,
				      uint64_t *raw_insync, uint64_t *raw_total)
{
	if (!status->sync_action || strcmp(status->sync_action, "idle"))
		return 0;

	if (strchr(status->dev_health, 'D'))
		return 0;

	if (!strchr(status->dev_health, 'a'))
		return 0;

	if (status->dev_health[0] == 'a')
		return 1;

	if (sscanf(params, "%*s %*u %*s " FMTu64 "/" FMTu64,
		   raw_insync, raw_total) == 2 &&
	    *raw_insync == *raw_total)
		return 1;

	return 0;
}

static int _raid_idle_lvconvert(struct dso_state *state, const char *device)
{
	/* One lvconvert per idle primary-source episode, success or failure. */
	if (state->idle_primary_attempted)
		return 1;

	state->idle_primary_attempted = 1;

	if (!dmeventd_lvm2_run_with_lock(state->cmd_lvconvert)) {
		log_error("Repair of RAID device %s failed.", device);
		return 0;
	}

	return 1;
}

static void _raid_reset_epoch(struct dso_state *state,
			      const struct dm_status_raid *status,
			      const char *device)
{
	if (status->dev_count == state->dev_count)
		return;

	log_debug("RAID %s device count changed from %u to %u,"
		  " resetting repair state.", device,
		  state->dev_count, status->dev_count);
	state->dev_count = status->dev_count;
	memset(state->attempted_devs, 0, sizeof(state->attempted_devs));
	state->warned = 0;
	state->idle_primary_attempted = 0;
	state->in_sync = 0;
}

static void _raid_collect_dead(const struct dm_status_raid *status,
			       struct dso_state *state,
			       struct raid_event *event,
			       const char *device)
{
	const char *d;
	uint32_t dev;

	for (d = status->dev_health; (d = strchr(d, 'D')); d++) {
		dev = (uint32_t)(d - status->dev_health);
		event->dead_devs[dev / 64] |= UINT64_C(1) << (dev % 64);
		event->dead = 1;

		if (!(state->attempted_devs[dev / 64] & (UINT64_C(1) << (dev % 64)))) {
			event->new_dead = 1;
			log_warn("WARNING: Device #%u of %s array, %s, has failed.",
				 dev, status->raid_type, device);
		}
	}
}

static void _raid_forget_recovered(struct dso_state *state, struct raid_event *event)
{
	unsigned i;

	for (i = 0; i < RAID_DEVS_ELEMS; i++) {
		if (state->attempted_devs[i] & ~event->dead_devs[i])
			event->recovered = 1;
		state->attempted_devs[i] &= event->dead_devs[i];
	}
}

/* True when a completed resync or a recovered leg warrants retrying repair. */
static int _raid_has_repair_transition(const struct dso_state *state,
				       const struct raid_event *event)
{
	if (!state->in_sync)
		return 0;

	return event->resynced || event->recovered;
}

static int _raid_run_repair(struct dso_state *state,
			    const struct raid_event *event, const char *device)
{
	unsigned i;

	for (i = 0; i < RAID_DEVS_ELEMS; i++)
		state->attempted_devs[i] |= event->dead_devs[i];

	if (!dmeventd_lvm2_run_with_lock(state->cmd_lvconvert)) {
		log_error("Repair of RAID device %s failed.", device);
		return 0;
	}

	return 1;
}

/*
 * Idle primary-source failure during linear->raid recover (see
 * _lv_raid_has_primary_failure_on_recover in lib/metadata/raid_manip.c).
 */
static enum raid_idle_primary_result _raid_handle_idle_primary(struct dso_state *state,
							       const struct dm_status_raid *status,
							       const char *params,
							       const char *device)
{
	uint64_t raw_insync, raw_total;

	if (!_raid_idle_primary_failure(status, params, &raw_insync, &raw_total))
		return RAID_IDLE_PRIMARY_NONE;

	log_error("Primary sources for new RAID, %s, have failed.", device);

	/* Run lvconvert once for this idle episode: it reports the
	 * condition and the recovery options (it refuses the repair
	 * on purpose).  A still-idle repeat event carries no new
	 * information; once a source is revived the array leaves the
	 * idle state again. */
	if (!_raid_idle_lvconvert(state, device))
		return RAID_IDLE_PRIMARY_FAILED;

	return RAID_IDLE_PRIMARY_HANDLED;
}

/*
 * Degraded-array repair policy after idle handling.  Returns 1 when lvconvert
 * should run, 0 when this event is finished without repair.
 */
static int _raid_should_run_repair(struct dso_state *state,
				   const struct raid_event *event,
				   const struct dm_status_raid *status,
				   const char *device)
{
	state->idle_primary_attempted = 0;

	if (!event->dead) {
		if (status->insync_regions == status->total_regions) {
			state->warned = 0;
			log_info("%s array, %s, is now in-sync.",
				 status->raid_type, device);
		}
		return 0;
	}

	if (!state->warned && status->insync_regions < status->total_regions) {
		state->warned = 1;
		log_warn("WARNING: Waiting for resynchronization to finish "
			 "before initiating repair on RAID device %s.", device);
	}

	if (event->new_dead) {
		log_debug("RAID %s: new device failure, running repair.", device);
		return 1;
	}

	if (!_raid_has_repair_transition(state, event)) {
		log_debug("RAID %s: no repair transition (in_sync=%u resynced=%d "
			  "recovered=%d).", device, state->in_sync,
			  event->resynced, event->recovered);
		return 0;
	}

	log_debug("RAID %s: retry repair after resync or leg recovery.", device);

	/* TODO: an already-attempted dead leg is retried only after an observed
	 * resync completion or a leg recovery.  If lvconvert's replacement dies
	 * before any intermediate event, that leg stays dead and un-repaired for
	 * the whole epoch.  Future enhancement: allow a bounded number of
	 * retries for a leg that remains dead, without letting repeated
	 * identical statuses run lvconvert every time. */

	return 1;
}

static int _process_raid_event(struct dso_state *state, const char *params, const char *device)
{
	struct raid_event event = { 0 };
	struct dm_status_raid *status;
	int r = 1;

	if (!dm_get_status_raid(state->mem, params, &status)) {
		log_error("Failed to process status line for %s.", device);
		return 0;
	}

	/* The bitmap is sized for the kernel's maximum, but never trust a
	 * status that reports more: indexing would run out of bounds. */
	if (status->dev_count > RAID_DEVS_ELEMS * 64) {
		log_error("Unexpected RAID device count %u for %s.",
			  status->dev_count, device);
		r = 0;
		goto out;
	}

	/* New dev_count: leg indexes changed; forget positional repair state. */
	_raid_reset_epoch(state, status, device);

	/* Snapshot 'D' legs; new_dead when this index was not yet attempted. */
	_raid_collect_dead(status, state, &event, device);

	/* Leg no longer 'D': drop attempted bit (later failure there is fresh). */
	_raid_forget_recovered(state, &event);

	/* Was incomplete last event and is complete now -> may retry repair. */
	event.resynced = !state->in_sync &&
	    (status->insync_regions == status->total_regions);
	state->in_sync = (status->insync_regions == status->total_regions);

	/* Idle linear->raid recover with failed primary: one lvconvert per episode. */
	switch (_raid_handle_idle_primary(state, status, params, device)) {
	case RAID_IDLE_PRIMARY_HANDLED:
		goto out;
	case RAID_IDLE_PRIMARY_FAILED:
		r = 0;
		goto_out;
	default:
		break;
	}

	/* Fresh 'D', or resync/recover transition on a still-dead array. */
	if (!_raid_should_run_repair(state, &event, status, device))
		goto_out;

	/* Mark dead legs attempted and run lvconvert --repair --use-policies. */
	if (!_raid_run_repair(state, &event, device)) {
		stack;
		r = 0;
	}

out:
	dm_pool_free(state->mem, status);

	return r;
}

void process_event(struct dm_task *dmt,
		   enum dm_event_mask evmask __attribute__((unused)),
		   void **user)
{
	struct dso_state *state = *user;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;
	const char *device = dm_task_get_name(dmt);

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);

		if (!target_type) {
			log_info("%s mapping lost.", device);
			continue;
		}

		if (strcmp(target_type, "raid")) {
			log_info("%s has non-raid portion.", device);
			continue;
		}

		if (!_process_raid_event(state, params, device))
			log_error("Failed to process event for %s.",
				  device);
	} while (next);
}

int register_device(const char *device_name,
		    const char *uuid __attribute__((unused)),
		    int major __attribute__((unused)),
		    int minor __attribute__((unused)),
		    void **user)
{
	struct dso_state *state;

	if (!dmeventd_lvm2_init_with_pool("raid_state", state))
		goto_bad;

	if (!dmeventd_lvm2_command(state->mem, state->cmd_lvconvert, sizeof(state->cmd_lvconvert),
				   "lvconvert --repair --use-policies", device_name))
		goto_bad;

	*user = state;

	log_info("Monitoring RAID device %s for events.", device_name);

	return 1;
bad:
	log_error("Failed to monitor RAID %s.", device_name);

	if (state)
		dmeventd_lvm2_exit_with_pool(state);

	return 0;
}

int unregister_device(const char *device_name,
		      const char *uuid __attribute__((unused)),
		      int major __attribute__((unused)),
		      int minor __attribute__((unused)),
		      void **user)
{
	struct dso_state *state = *user;

	dmeventd_lvm2_exit_with_pool(state);
	log_info("No longer monitoring RAID device %s for events.",
		 device_name);

	return 1;
}
