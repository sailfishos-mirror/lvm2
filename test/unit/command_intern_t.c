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
 * units.h pulls in libdevmapper.h, so include it before
 * command_standalone.h, which maps the dm_strncpy() and dm_pool_*()
 * names to its own shims.
 */
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

/* command.c is compiled into this TU the way man-generator does it, so the
 * link needs no object from tools/ */
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
static struct arg_def _arg_def(uint64_t val_bits, const char *str,
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
	struct arg_def def = _arg_def(val_enum_to_bit(conststr_VAL),
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
	struct arg_def linear = _arg_def(val_enum_to_bit(conststr_VAL),
				          "linear", 0, 0);
	struct arg_def striped = _arg_def(val_enum_to_bit(conststr_VAL),
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
	struct arg_def one = _arg_def(val_enum_to_bit(constnum_VAL), NULL, 0, 1);
	struct arg_def two = _arg_def(val_enum_to_bit(constnum_VAL), NULL, 0, 2);
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
	struct arg_def plain = _arg_def(val_enum_to_bit(conststr_VAL),
				        "foo", 0, 0);
	struct arg_def repeating = _arg_def(val_enum_to_bit(conststr_VAL),
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
	struct arg_def linear = _arg_def(val_enum_to_bit(conststr_VAL),
				          "linear", 0, 0);
	struct arg_def striped = _arg_def(val_enum_to_bit(conststr_VAL),
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
	struct arg_def def = _arg_def(val_enum_to_bit(conststr_VAL),
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

/* the parser rejects --size without ALTERNATIVE_EXTENTS, so make generate
 * fails */
static void test_size_requires_alternate_extents(void *fixture)
{
	struct command cmd = { 0 };
	int takes_arg = 0;
	char opt[] = "--size";

	_reset();
	_create_opt_names_alpha();

	cmd.name = "vgcreate";
	cmd.lvm_command_enum = vgcreate_COMMAND;
	_add_opt_arg(&cmd, opt, &takes_arg, NULL, 1);
	T_ASSERT(cmd.cmd_flags & CMD_FLAG_PARSE_ERROR);
	T_ASSERT_EQUAL(_log_count, 1);
	T_ASSERT(strstr(_log_msg, "vgcreate") != NULL);

	_reset();
	memset(&cmd, 0, sizeof(cmd));
	cmd.name = "lvcreate";
	cmd.lvm_command_enum = lvcreate_COMMAND;
	_add_opt_arg(&cmd, opt, &takes_arg, NULL, 1);
	T_ASSERT_EQUAL(cmd.cmd_flags & CMD_FLAG_PARSE_ERROR, 0);
	T_ASSERT_EQUAL(cmd.ro_count, 1);
	T_ASSERT_EQUAL(cmd.required_opt_args[0].opt, size_ARG);
	T_ASSERT_EQUAL(_log_count, 0);
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
	T("size-requires-alternate-extents", "--size without ALTERNATIVE_EXTENTS is a parse error", test_size_requires_alternate_extents);

	dm_list_add(all_tests, &ts->list);
}
