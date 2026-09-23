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

#ifndef DMEVENTD_PERCENT_H
#define DMEVENTD_PERCENT_H

#include "lib/misc/lib.h"

/* Above DM_PERCENT_100: skip extension until COW total_sectors changes. */
#define DMEVENTD_PERCENT_DISARMED (DM_PERCENT_100 + 1)

/*
 * After a successful snapshot extension, arm the next inclusive CHECK_STEP
 * boundary (50%, 55%, ...).  Below 100% usage the armed value is capped at
 * DM_PERCENT_100 - 1 so a later event at exactly 100% still runs extension
 * once.  After success at 100%, disarm until a size change re-arms checking.
 */
static inline void dmeventd_advance_percent_check_after_action(dm_percent_t percent,
							       dm_percent_t *check,
							       dm_percent_t step)
{
	*check = (percent / step) * step + step;
	if (*check >= DM_PERCENT_100)
		*check = (percent >= DM_PERCENT_100) ? DMEVENTD_PERCENT_DISARMED :
		    DM_PERCENT_100 - 1;
}

#endif
