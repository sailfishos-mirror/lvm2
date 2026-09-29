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

struct command;
struct command_name_args;

/*
 * Fold the defs in cmds[] into names[], recording for each name how many defs
 * it has, which kinds of args those defs use, the union of their options, and
 * the options common to all of them.  names[] must be zeroed by the caller,
 * and the fold has to run once over it: the first def of a name is recognised
 * by its variants count, so folding the same names[] twice would AND a primed
 * row into itself and report too few common options.
 *
 * A def is filed under the name cmds[ci].lvm_command_enum, which both names[]
 * and the fold's own per-name rows are indexed by, so that enum has to be
 * below nnames, and nnames has to be at most LVM_COMMAND_COUNT, the number of
 * rows the fold holds.  nnames outside (0, LVM_COMMAND_COUNT] is a no-op.
 * Each option value in a def has to be below ARG_COUNT as well, since the
 * fold indexes all_options[] and its bit row by it.  Other than nnames,
 * nothing checks this: a def that names no such row, or carries such an
 * option, indexes past the end of an array.
 */
void command_factor_options(const struct command *cmds, int ncmds,
			    struct command_name_args *names, int nnames);

#endif /* LVM_COMMAND_OPTS_H */
