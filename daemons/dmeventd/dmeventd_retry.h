/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
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

#ifndef DMEVENTD_RETRY_H
#define DMEVENTD_RETRY_H

#include "lib/misc/lib.h"

#define DMEVENTD_POLICY_MAX_FAILS 256U /* ~42 min between retries at 10s */

/*
 * TODO: a permanently failing policy command may retry forever at the
 * DMEVENTD_POLICY_MAX_FAILS cadence.  Consider giving up once backoff has
 * saturated several times (snapshot/thin/VDO plugins).
 */

struct dmeventd_policy_retry {
	unsigned fails;
	unsigned max_fails;
};

/*
 * Power-of-2 backoff between failed external policy commands (thin, VDO,
 * snapshot plugins).  Returns 1 when this event should skip running policy.
 */
static inline int dmeventd_policy_retry_should_postpone(struct dmeventd_policy_retry *retry,
							const char *what)
{
	if (!retry->fails) {
		retry->max_fails = 1;
		return 0;
	}

	if (retry->fails++ <= retry->max_fails) {
		log_debug("Postponing frequently failing %s (%u <= %u).",
			  what, retry->fails - 1, retry->max_fails);
		return 1;
	}

	if (retry->max_fails < DMEVENTD_POLICY_MAX_FAILS)
		retry->max_fails <<= 1;
	retry->fails = 1;

	return 0;
}

static inline void dmeventd_policy_retry_after_success(struct dmeventd_policy_retry *retry)
{
	retry->fails = 0;
	retry->max_fails = 1;
}

#endif
