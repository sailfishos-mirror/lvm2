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

#include "command.h"
#include "command_opts.h"

/* bits held by one uint64_t word, and the words needed for one bit per option
 * in args.h */
#define ARG_WORD_BITS 64
#define ARG_WORDS ((ARG_COUNT + ARG_WORD_BITS - 1) / ARG_WORD_BITS)

/*
 * Find the options common to all the defs of each command name.
 *
 * The defs and the per-name output are passed in rather than read from
 * commands[] and command_names_args[], so this can be exercised by a unit
 * test over a small fixture.  names[] must be zeroed by the caller; the
 * counts here are added to whatever is already there.
 */
void command_factor_options(const struct command *cmds, int ncmds,
			    struct command_name_args *names, int nnames)
{
	int cn, ci, oo, ro, w, opt_enum;
	int def_cn;	/* name the current def belongs to, distinct from the cn index */
	uint64_t common[LVM_COMMAND_COUNT][ARG_WORDS] = { { 0 } };
	uint64_t def_opts[ARG_WORDS];	/* optional opts of the def being folded in */
	const struct command *cmd;

	if (nnames <= 0 || nnames > LVM_COMMAND_COUNT)
		return;

	/*
	 * Each def belongs to exactly one name, so a single pass over cmds[]
	 * serves every name.  The union of options goes straight into
	 * names[].all_options; only the optional-option intersection uses
	 * common[][] bit rows (AND across defs).
	 *
	 * A row is filled from the first def of its name, and the later defs
	 * AND their own sets into it.  A name with no def is never written
	 * to, so it keeps the zero it started with: it has no option common
	 * to all of its zero defs.  A single command run parses the defs of
	 * one name only, so most names have no def here.
	 */
	for (ci = 0; ci < ncmds; ci++) {
		cmd = &cmds[ci];

		/*
		 * names[] and the row of common[][] are both indexed by the
		 * def's own enum, so it has to name a row that exists: see
		 * the range the header asks for.  define_commands() only
		 * stores an enum it matched against command_names[], so no
		 * def out of the real tree carries one.
		 */
		def_cn = cmd->lvm_command_enum;

		names[def_cn].variants++;

		if (cmd->ro_count || cmd->any_ro_count)
			names[def_cn].variant_has_ro = 1;
		if (cmd->rp_count)
			names[def_cn].variant_has_rp = 1;
		if (cmd->oo_count)
			names[def_cn].variant_has_oo = 1;
		if (cmd->op_count)
			names[def_cn].variant_has_op = 1;

		for (ro = 0; ro < cmd->ro_count + cmd->any_ro_count; ro++) {
			names[def_cn].all_options[cmd->required_opt_args[ro].opt] = 1;

			if ((cmd->required_opt_args[ro].opt == size_ARG) && !strncmp(cmd->name, "lv", 2))
				names[def_cn].all_options[extents_ARG] = 1;
		}

		memset(def_opts, 0, sizeof(def_opts));
		for (oo = 0; oo < cmd->oo_count; oo++) {
			opt_enum = cmd->optional_opt_args[oo].opt;

			names[def_cn].all_options[opt_enum] = 1;
			def_opts[opt_enum / ARG_WORD_BITS] |= 1ULL << (opt_enum % ARG_WORD_BITS);
		}

		if (names[def_cn].variants == 1)	/* first def of this name in cmds[] order */
			memcpy(common[def_cn], def_opts, sizeof(def_opts));
		else
			for (w = 0; w < ARG_WORDS; ++w)
				common[def_cn][w] &= def_opts[w];
	}

	/*
	 * Only the ARG_COUNT bits are written out.  A name with no def was
	 * never written to, so its rows are still zero.
	 */
	for (cn = 0; cn < nnames; cn++)
		for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++)
			names[cn].common_options[opt_enum] =
				(common[cn][opt_enum / ARG_WORD_BITS] >>
				 (opt_enum % ARG_WORD_BITS)) & 1;
}
