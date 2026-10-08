/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * LVM2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This copyrighted material is made available to anyone wishing
 * to use, modify, copy, or redistribute it subject to the terms
 * and conditions of the GNU Lesser General Public License v2.1.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>

#include "units.h"

/*
 * framework.h pulls in libdevmapper.h, which prototypes dm_strncpy(),
 * dm_pool_alloc() and dm_pool_strdup().  command.c has no real dm_pool,
 * so it must call the stand-alone shims from command_standalone.h
 * instead.  Map those three names for this translation unit: the macros
 * have to stay defined over command.c itself, which is where they are
 * used, so units.h comes first to keep them off libdevmapper.h.
 */
#define dm_strncpy standalone_dm_strncpy
#define dm_pool_strdup standalone_dm_pool_strdup
#define dm_pool_alloc standalone_dm_pool_alloc

#include "tools/command_standalone.h"
#include "tools/command.h"

/* command.c's parse errors are reported here rather than on stderr */
#undef log_error
static int _log_count;
static char _log_msg[256];
#define log_error(fmt, args...) \
do { \
	_log_count++; \
	snprintf(_log_msg, sizeof(_log_msg), fmt, ##args); \
} while (0)

/* the fold under test, compiled into this TU like man-generator does it,
 * so the link needs no object from tools/ */
#define MAN_PAGE_GENERATOR
#include "tools/command.c"

static void _reset(void)
{
	cmd_arg_defs[0] = (struct arg_def) { 0 };
	cmd_arg_def_count = 1;
	_log_count = 0;
	_log_msg[0] = '\0';
}

/* a def carrying only what the caller asked for */
static struct arg_def _def(uint64_t val_bits, const char *str,
			   uint32_t flags, uint16_t num)
{
	struct arg_def def = { 0 };

	def.val_bits = val_bits;
	def.str = str;
	def.flags = flags;
	def.num = num;

	return def;
}

/* entry 0 is the all-zero "no value" def, so an empty def never allocates */
static void test_zero_def_is_slot_zero(void *fixture)
{
	struct arg_def def = { 0 };
	struct command cmd = { 0 };

	_reset();
	T_ASSERT_EQUAL(_intern_arg_def(&cmd, &def), 0);
	T_ASSERT_EQUAL(cmd_arg_def_count, 1);
	T_ASSERT_EQUAL(cmd.cmd_flags, 0);
}

/* equal defs share one slot instead of growing the table */
static void test_identical_defs_share_slot(void *fixture)
{
	struct arg_def def = _def(val_enum_to_bit(conststr_VAL),
				  "linear", 0, 0);
	struct command cmd = { 0 };
	uint16_t a, b;

	_reset();
	a = _intern_arg_def(&cmd, &def);
	T_ASSERT_EQUAL(a, 1);
	T_ASSERT_EQUAL(cmd_arg_def_count, 2);

	b = _intern_arg_def(&cmd, &def);
	T_ASSERT_EQUAL(b, a);
	T_ASSERT_EQUAL(cmd_arg_def_count, 2);
}

/* a different value name is a different def */
static void test_distinct_defs_get_distinct_slots(void *fixture)
{
	struct arg_def linear = _def(val_enum_to_bit(conststr_VAL),
				      "linear", 0, 0);
	struct arg_def striped = _def(val_enum_to_bit(conststr_VAL),
				       "striped", 0, 0);
	struct command cmd = { 0 };
	uint16_t a, b;

	_reset();
	a = _intern_arg_def(&cmd, &linear);
	b = _intern_arg_def(&cmd, &striped);
	T_ASSERT_EQUAL(a, 1);
	T_ASSERT_EQUAL(b, 2);
	T_ASSERT_EQUAL(cmd_arg_def_count, 3);
}

/* every field of arg_def is compared: same name, different number */
static void test_num_distinguishes(void *fixture)
{
	struct arg_def one = _def(val_enum_to_bit(constnum_VAL), NULL, 0, 1);
	struct arg_def two = _def(val_enum_to_bit(constnum_VAL), NULL, 0, 2);
	struct command cmd = { 0 };
	uint16_t a, b;

	_reset();
	a = _intern_arg_def(&cmd, &one);
	b = _intern_arg_def(&cmd, &two);
	T_ASSERT_EQUAL(a, 1);
	T_ASSERT_EQUAL(b, 2);
	T_ASSERT_EQUAL(cmd_arg_def_count, 3);
}

/* MAY_REPEAT is part of the identity, so the same def with and without
 * it gets two slots */
static void test_may_repeat_is_distinct(void *fixture)
{
	struct arg_def plain = _def(val_enum_to_bit(conststr_VAL),
				    "foo", 0, 0);
	struct arg_def repeating = _def(val_enum_to_bit(conststr_VAL),
					 "foo", ARG_DEF_FLAG_MAY_REPEAT, 0);
	struct command cmd = { 0 };
	uint16_t a, b;

	_reset();
	a = _intern_arg_def(&cmd, &plain);
	b = _intern_arg_def(&cmd, &repeating);
	T_ASSERT_EQUAL(a, 1);
	T_ASSERT_EQUAL(b, 2);
	T_ASSERT_EQUAL(cmd_arg_def_count, 3);
	T_ASSERT_EQUAL(arg_def_of(a)->flags, 0);
	T_ASSERT_EQUAL(arg_def_of(b)->flags, ARG_DEF_FLAG_MAY_REPEAT);
}

/* interning starts over at slot 1 once the table is reset, and the slot
 * left over from before the reset is not handed out */
static void test_reset_restores_slot_one(void *fixture)
{
	struct arg_def linear = _def(val_enum_to_bit(conststr_VAL),
				      "linear", 0, 0);
	struct arg_def striped = _def(val_enum_to_bit(conststr_VAL),
				       "striped", 0, 0);
	struct command cmd = { 0 };
	uint16_t a;

	_reset();
	a = _intern_arg_def(&cmd, &linear);
	T_ASSERT_EQUAL(a, 1);

	_reset();
	T_ASSERT_EQUAL(_intern_arg_def(&cmd, &striped), 1);
	T_ASSERT_EQUAL(cmd_arg_def_count, 2);
	T_ASSERT_EQUAL(strcmp(arg_def_of(1)->str, "striped"), 0);
}

/* a full table reports the overflow on the command instead of writing
 * past the end */
static void test_overflow_flags_parse_error(void *fixture)
{
	struct arg_def def = _def(val_enum_to_bit(conststr_VAL),
				  "overflow-probe", 0, 0);
	struct command cmd = { 0 };

	_reset();
	memset(cmd_arg_defs, 0, sizeof(cmd_arg_defs));
	cmd_arg_def_count = CMD_ARG_DEF_MAX;

	T_ASSERT_EQUAL(_intern_arg_def(&cmd, &def), 0);
	T_ASSERT(cmd.cmd_flags & CMD_FLAG_PARSE_ERROR);
	T_ASSERT_EQUAL(cmd_arg_def_count, CMD_ARG_DEF_MAX);
	T_ASSERT_EQUAL(_log_count, 1);
	T_ASSERT(strstr(_log_msg, "CMD_ARG_DEF_MAX") != NULL);

	_reset();
}

#define T(path, desc, fn) register_test(ts, "/base/command-intern/" path, desc, fn)

void command_intern_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("zero-def", "an all-zero def is slot 0", test_zero_def_is_slot_zero);
	T("shared-slot", "equal defs share one slot", test_identical_defs_share_slot);
	T("distinct-slots", "a different value name is a different def", test_distinct_defs_get_distinct_slots);
	T("num-distinguishes", "a differing number is a different def", test_num_distinguishes);
	T("may-repeat", "MAY_REPEAT is part of the identity", test_may_repeat_is_distinct);
	T("reset", "the table starts over at slot 1 after a reset", test_reset_restores_slot_one);
	T("overflow", "a full table flags a parse error", test_overflow_flags_parse_error);

	dm_list_add(all_tests, &ts->list);
}
