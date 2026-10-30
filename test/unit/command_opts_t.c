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
#include "tools/command_opts.h"

/*
 * defs[] holds the defs of one name.  64 is above the widest name in
 * command-lines.in and below reportformat_ARG, so the private option each
 * def gets in test_many_defs() never collides with the one every def lists.
 */
#define NR_DEFS 64

/* the fold fills one row per command name */
static struct command_name_args names[LVM_COMMAND_COUNT];
static struct command_name cnames[LVM_COMMAND_COUNT];
static struct command defs[NR_DEFS];

static void _reset(void)
{
	memset(names, 0, sizeof(names));
	memset(cnames, 0, sizeof(cnames));
	memset(defs, 0, sizeof(defs));
}

/* add a def for the command name enum cn, holding the nopts given option
 * enums, in slot ndef of the fixture */
static struct command *_cmd_def(const char *name, int cn, int ndef, int nopts,
				const int *opts)
{
	struct command *cmd = &defs[ndef];
	int i;

	cmd->name = name;
	cmd->lvm_command_enum = cn;
	cmd->oo_count = nopts;

	for (i = 0; i < nopts; i++)
		cmd->optional_opt_args[i].opt = opts[i];

	return cmd;
}

/* an option is common to a name only if every one of its defs lists it */
static void test_common_needs_every_def(void *fixture)
{
	const int a[] = { reportformat_ARG, config_ARG };
	const int b[] = { reportformat_ARG, ignorelockingfailure_ARG };

	_reset();
	_cmd_def("lvs", 0, 0, 2, a);
	_cmd_def("lvs", 0, 1, 2, b);

	_command_factor_options(defs, 2, cnames, names);

	T_ASSERT_EQUAL(names[0].variants, 2);

	/* listed by both defs */
	T_ASSERT_EQUAL(names[0].common_options[reportformat_ARG], 1);
	/* listed by only one of them */
	T_ASSERT_EQUAL(names[0].common_options[config_ARG], 0);
	T_ASSERT_EQUAL(names[0].common_options[ignorelockingfailure_ARG], 0);
	/* listed by neither, so not in the union either */
	T_ASSERT_EQUAL(names[0].common_options[mergedconfig_ARG], 0);
	T_ASSERT_EQUAL(names[0].all_options[config_ARG], 1);
	T_ASSERT_EQUAL(names[0].all_options[ignorelockingfailure_ARG], 1);
	T_ASSERT_EQUAL(names[0].all_options[mergedconfig_ARG], 0);
}

/* some real names have defs with disjoint option sets, so nothing is common */
static void test_disjoint_defs_share_nothing(void *fixture)
{
	const int a[] = { reportformat_ARG, config_ARG };
	const int b[] = { name_ARG, devicesfile_ARG };

	_reset();
	_cmd_def("vgsplit", 0, 0, 2, a);
	_cmd_def("vgsplit", 0, 1, 2, b);

	_command_factor_options(defs, 2, cnames, names);

	T_ASSERT_EQUAL(names[0].variants, 2);
	T_ASSERT_EQUAL(names[0].common_options[reportformat_ARG], 0);
	T_ASSERT_EQUAL(names[0].common_options[config_ARG], 0);
	T_ASSERT_EQUAL(names[0].all_options[reportformat_ARG], 1);
	T_ASSERT_EQUAL(names[0].all_options[config_ARG], 1);
	T_ASSERT_EQUAL(names[0].all_options[name_ARG], 1);
	T_ASSERT_EQUAL(names[0].all_options[devicesfile_ARG], 1);
}

/* a name with a single def has every one of that def's options in common */
static void test_single_def(void *fixture)
{
	const int a[] = { reportformat_ARG, config_ARG, ignorelockingfailure_ARG };

	_reset();
	_cmd_def("lvs", 0, 0, 3, a);

	_command_factor_options(defs, 1, cnames, names);

	T_ASSERT_EQUAL(names[0].variants, 1);
	T_ASSERT_EQUAL(names[0].common_options[reportformat_ARG], 1);
	T_ASSERT_EQUAL(names[0].common_options[config_ARG], 1);
	T_ASSERT_EQUAL(names[0].common_options[ignorelockingfailure_ARG], 1);
	T_ASSERT_EQUAL(names[0].common_options[name_ARG], 0);
}

/* one def carrying no optional options at all */
static void test_def_with_no_optional_opts(void *fixture)
{
	_reset();
	_cmd_def("pvck", 0, 0, 0, NULL);
	_cmd_def("pvck", 0, 1, 0, NULL);

	_command_factor_options(defs, 2, cnames, names);

	/* no optional options to be common to, whatever the required ones are */
	T_ASSERT_EQUAL(names[0].variants, 2);
	T_ASSERT_EQUAL(names[0].common_options[reportformat_ARG], 0);
}

/* a variant count wider than any real name; the AND must survive all of
 * them */
static void test_many_defs(void *fixture)
{
	int opts[2] = { reportformat_ARG, 0 };
	int i;

	_reset();

	/* every def carries reportformat_ARG and one private option */
	for (i = 0; i < NR_DEFS; i++) {
		opts[1] = i + 1;	/* an opt enum nothing else lists */
		_cmd_def("lvcreate", 0, i, 2, opts);
	}

	_command_factor_options(defs, NR_DEFS, cnames, names);

	T_ASSERT_EQUAL(names[0].variants, NR_DEFS);
	/* survived all of them */
	T_ASSERT_EQUAL(names[0].common_options[reportformat_ARG], 1);
	/* each private option belongs to exactly one def, but every one of
	 * them is in the union */
	for (i = 0; i < NR_DEFS; i++) {
		T_ASSERT_EQUAL(names[0].common_options[i + 1], 0);
		T_ASSERT_EQUAL(names[0].all_options[i + 1], 1);
	}
}

/* each name is folded independently of the others in the same pass */
static void test_names_are_independent(void *fixture)
{
	const int a[] = { reportformat_ARG, config_ARG };
	const int b[] = { reportformat_ARG, config_ARG };
	const int c[] = { name_ARG };

	_reset();
	_cmd_def("lvs", 0, 0, 2, a);
	_cmd_def("lvs", 0, 1, 2, b);
	_cmd_def("vgsplit", 1, 2, 1, c);
	_cmd_def("vgs", 2, 3, 0, a);

	_command_factor_options(defs, 4, cnames, names);

	/* lvs: both defs agree, so both options are common */
	T_ASSERT_EQUAL(names[0].variants, 2);
	T_ASSERT_EQUAL(names[0].common_options[reportformat_ARG], 1);
	T_ASSERT_EQUAL(names[0].common_options[config_ARG], 1);

	/* vgsplit: one def, so its single option is common */
	T_ASSERT_EQUAL(names[1].variants, 1);
	T_ASSERT_EQUAL(names[1].common_options[name_ARG], 1);
	T_ASSERT_EQUAL(names[1].common_options[reportformat_ARG], 0);

	/* vgs: a def with no optional options has none in common */
	T_ASSERT_EQUAL(names[2].variants, 1);
	T_ASSERT_EQUAL(names[2].common_options[reportformat_ARG], 0);

	/* name 3 has no defs at all: nothing is common to all of no defs */
	T_ASSERT_EQUAL(names[3].variants, 0);
	T_ASSERT_EQUAL(names[3].common_options[reportformat_ARG], 0);
	T_ASSERT_EQUAL(names[3].common_options[config_ARG], 0);
}

/* required options go in the union, and record that the name uses them */
static void test_required_opts(void *fixture)
{
	struct command *cmd;

	_reset();
	cnames[0].flags = ALTERNATIVE_EXTENTS;
	cmd = _cmd_def("lvcreate", 0, 0, 0, NULL);
	cmd->ro_count = 2;
	cmd->required_opt_args[0].opt = name_ARG;
	cmd->required_opt_args[1].opt = size_ARG;

	_command_factor_options(defs, 1, cnames, names);

	T_ASSERT_EQUAL(names[0].variant_has_ro, 1);
	T_ASSERT_EQUAL(names[0].all_options[name_ARG], 1);
	/* the name has ALTERNATIVE_EXTENTS, so --extents is accepted in place
	 * of --size */
	T_ASSERT_EQUAL(names[0].all_options[extents_ARG], 1);
	/* a required option is never common: the fold only sees optional ones */
	T_ASSERT_EQUAL(names[0].common_options[name_ARG], 0);
	T_ASSERT_EQUAL(names[0].common_options[extents_ARG], 0);
}

/* which kinds of args a name's defs use */
static void test_variant_flags(void *fixture)
{
	const int a[] = { reportformat_ARG };
	struct command *cmd;

	_reset();
	cmd = _cmd_def("lvcreate", 0, 0, 1, a);
	cmd->rp_count = 1;
	cmd->op_count = 1;

	_command_factor_options(defs, 1, cnames, names);

	T_ASSERT_EQUAL(names[0].variant_has_oo, 1);
	T_ASSERT_EQUAL(names[0].variant_has_rp, 1);
	T_ASSERT_EQUAL(names[0].variant_has_op, 1);
	T_ASSERT_EQUAL(names[0].variant_has_ro, 0);
}

#define T(path, desc, fn) register_test(ts, "/base/command-opts/" path, desc, fn)

void command_opts_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("common-needs-every-def", "an option is common only if every def lists it", test_common_needs_every_def);
	T("disjoint-defs", "defs with disjoint options share nothing", test_disjoint_defs_share_nothing);
	T("single-def", "a single def has all its options in common", test_single_def);
	T("no-optional-opts", "a def with no optional options", test_def_with_no_optional_opts);
	T("many-defs", "the AND survives NR_DEFS-scale variant count", test_many_defs);
	T("names-independent", "each name folds independently", test_names_are_independent);
	T("required-opts", "required options join the union", test_required_opts);
	T("variant-flags", "which kinds of args a name uses", test_variant_flags);

	dm_list_add(all_tests, &ts->list);
}
