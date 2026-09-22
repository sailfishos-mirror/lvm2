/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * Shared strict greater-than pool step expectations for thin and VDO unit
 * tests (>50%, >55%, ... per the production plugins).
 */

#ifndef DMEVENTD_UNIT_POOL_SCENARIOS_H
#define DMEVENTD_UNIT_POOL_SCENARIOS_H

#define DMEVENTD_UNIT_POOL_STRICT_THRESHOLD_STEPS 12

static const unsigned dmeventd_unit_pool_strict_threshold_used[
	DMEVENTD_UNIT_POOL_STRICT_THRESHOLD_STEPS] = {
	0, 49, 50, 50, 54, 55, 56, 81, 81, 95, 99, 100
};

static const unsigned dmeventd_unit_pool_strict_threshold_calls[
	DMEVENTD_UNIT_POOL_STRICT_THRESHOLD_STEPS] = {
	0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 3, 4
};

#endif
