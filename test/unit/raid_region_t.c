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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA.
 */

#include "units.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/lv.h"
#include "lib/metadata/metadata-exported.h"
#include "lib/metadata/vg.h"

#include <stdint.h>

/*
 * Exercise region-size bounds with a minimal LV fixture because adjustment
 * paths log using the LV and its VG command context.
 */

struct raid_region_fixture {
	struct cmd_context cmd;
	struct volume_group vg;
	struct logical_volume lv;
	struct dm_pool *mem;
};

static void *_fixture_init(void)
{
	struct raid_region_fixture *f = calloc(1, sizeof(*f));

	if (!f) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	if (!(f->mem = dm_pool_create("raid region test", 1024))) {
		fprintf(stderr, "out of memory\n");
		free(f);
		exit(1);
	}

	f->cmd.mem = f->mem;
	f->cmd.current_settings.unit_type = 'h';
	f->vg.cmd = &f->cmd;
	f->vg.name = "test-vg";
	f->lv.vg = &f->vg;
	f->lv.name = "test-lv";

	return f;
}

static void _fixture_exit(void *fixture)
{
	struct raid_region_fixture *f = fixture;

	dm_pool_destroy(f->mem);
	free(f);
}

static struct logical_volume *_fixture_lv(void *fixture)
{
	struct raid_region_fixture *f = fixture;

	return &f->lv;
}

static void test_no_doubling_needed(void *fixture)
{
	uint32_t r = raid_ensure_min_region_size(_fixture_lv(fixture), 100, 1000);

	T_ASSERT_EQUAL(r, 1000);
}

static void test_doubling_within_bounds(void *fixture)
{
	/* min_region_size = (1 << 30) / (1 << 21) = 512 */
	uint32_t r = raid_ensure_min_region_size(_fixture_lv(fixture), 1ULL << 30, 1);

	T_ASSERT_EQUAL(r, 512);
}

static void test_zero_region_size(void *fixture)
{
	uint32_t r = raid_ensure_min_region_size(_fixture_lv(fixture), 100, 0);

	T_ASSERT_EQUAL(r, 0);
}

static void test_above_half_uint32_max(void *fixture)
{
	uint32_t r = raid_ensure_min_region_size(_fixture_lv(fixture), 100, UINT32_MAX);

	T_ASSERT_EQUAL(r, UINT32_MAX);
}

#define T(path, desc, fn) register_test(ts, "/raid/region/" path, desc, fn)

void raid_region_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_fixture_init, _fixture_exit);

	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("no-doubling", "region size already meets minimum", test_no_doubling_needed);
	T("doubling", "region size doubles within bounds", test_doubling_within_bounds);
	T("zero", "zero region size stays zero", test_zero_region_size);
	T("above-half-max", "region size above UINT32_MAX/2 is not doubled", test_above_half_uint32_max);

	dm_list_add(all_tests, &ts->list);
}
