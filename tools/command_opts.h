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

#ifndef LVM_COMMAND_OPTS_H
#define LVM_COMMAND_OPTS_H

#include <string.h>

#include "command.h"

/* bits held by one uint64_t word, and the words needed for one bit per option
 * in args.h */
#define ARG_WORD_BITS 64
#define ARG_WORDS ((ARG_COUNT + ARG_WORD_BITS - 1) / ARG_WORD_BITS)

static inline void _command_opt_set(uint64_t *row, int opt)
{
	row[opt / ARG_WORD_BITS] |= 1ULL << (opt % ARG_WORD_BITS);
}

static inline int _command_opt_is_set(const uint64_t *row, int opt)
{
	return (row[opt / ARG_WORD_BITS] >> (opt % ARG_WORD_BITS)) & 1;
}

/*
 * Find the options common to all the defs of each command name.
 *
 * The defs, the per-name flags and the per-name output are passed in rather
 * than read from commands[] and command_names_args[], so this can be
 * exercised by a unit test over a small fixture.  A def is filed under the
 * name cmds[ci].lvm_command_enum, which both names[] and the fold's own
 * per-name rows are indexed by; cnames[] is indexed by the same enum and
 * names[] must have LVM_COMMAND_COUNT entries.
 *
 * names[] must be zeroed by the caller.  The intersection rows are primed
 * afresh on every call, so no row is ever ANDed into itself, but the counts
 * and unions in names[] accumulate, so a caller must fold a given def set
 * only once.
 */
static void _command_factor_options(const struct command *cmds, int ncmds,
				    const struct command_name *cnames,
				    struct command_name_args *names)
{
	int cn, ci, oo, ro, w, opt_enum;
	int def_cn;	/* name the current def belongs to, distinct from the cn index */
	uint64_t common[LVM_COMMAND_COUNT][ARG_WORDS] = { { 0 } };
	uint64_t def_opts[ARG_WORDS];	/* optional opts of the def being folded in */
	const struct command *cmd;

	/*
	 * Each def belongs to exactly one name, so a single pass over cmds[]
	 * serves every name.  The union of options goes straight into
	 * names[].all_options; only the optional-option intersection uses
	 * common[][] bit rows (AND across defs).
	 *
	 * A row is filled from the first def of its name, and the later defs
	 * AND their own sets into it.  A name with no def keeps the zero it
	 * started with, so it has no option common to its zero defs.  A slot
	 * of cmds[] that no def filled is zero and belongs to enum 0, so it
	 * contributes an empty set to that row.  A single command run parses
	 * the defs of one name only, so most names have no def here.
	 */
	for (ci = 0; ci < ncmds; ci++) {
		cmd = &cmds[ci];
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
			_command_opt_set(def_opts, opt_enum);
		}

		/*
		 * The first def of a name seeds the row and the later defs AND
		 * their own sets into it.  names[] is zeroed by the caller, so
		 * the first def of a name is the one that brings its variants
		 * to 1.
		 */
		if (names[def_cn].variants == 1)
			memcpy(common[def_cn], def_opts, sizeof(def_opts));
		else
			for (w = 0; w < ARG_WORDS; ++w)
				common[def_cn][w] &= def_opts[w];
	}

	/*
	 * Only the ARG_COUNT bits are written out.  A name with no def was
	 * never written to, so its rows are still zero.
	 */
	for (cn = 0; cn < LVM_COMMAND_COUNT; cn++)
		for (opt_enum = 0; opt_enum < ARG_COUNT; opt_enum++)
			names[cn].common_options[opt_enum] =
				_command_opt_is_set(common[cn], opt_enum);
}

#endif /* LVM_COMMAND_OPTS_H */
