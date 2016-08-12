/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_COMMAND_H
#define _LVM_COMMAND_H

struct cmd_context;

/* old per-command-name function */
typedef int (*command_fn) (struct cmd_context *cmd, int argc, char **argv);

/* new per-command-line-id functions */
typedef int (*command_line_fn) (struct cmd_context *cmd, int argc, char **argv);

struct command_function {
	int command_line_enum;
	command_line_fn fn;
};

struct command_name {
	const char *name;
	const char *desc; /* general command description from commands.h */
	unsigned int flags;

	/* union of {required,optional}_opt_args for all commands with this name */
	int valid_args[ARG_COUNT];
	int num_args;
};

/*
 * Command defintion
 *
 * A command is defined in terms of a command name,
 * required options (+args), optional options (+args),
 * required positional args, optional positional args.
 *
 * A positional arg always has non-zero pos_arg.def.types.
 * The first positional arg has pos_arg.pos of 1.
 */

/* arg_def flags */
#define ARG_DEF_FLAG_NEW                1 << 0
#define ARG_DEF_FLAG_MAY_REPEAT         1 << 1

/* arg_def lv_types */
enum {
	ARG_DEF_LV_ANY		= 0,
	ARG_DEF_LV_LINEAR	= 1 << 0,
	ARG_DEF_LV_STRIPED	= 1 << 1,
	ARG_DEF_LV_SNAPSHOT	= 1 << 2,
	ARG_DEF_LV_MIRROR	= 1 << 3,
	ARG_DEF_LV_RAID		= 1 << 4,
	ARG_DEF_LV_RAID0	= 1 << 5,
	ARG_DEF_LV_RAID1	= 1 << 6,
	ARG_DEF_LV_RAID4	= 1 << 7,
	ARG_DEF_LV_RAID5	= 1 << 8,
	ARG_DEF_LV_RAID6	= 1 << 9,
	ARG_DEF_LV_RAID10	= 1 << 10,
	ARG_DEF_LV_THIN		= 1 << 11,
	ARG_DEF_LV_THINPOOL	= 1 << 12,
	ARG_DEF_LV_CACHE	= 1 << 13,
	ARG_DEF_LV_CACHEPOOL	= 1 << 14,
	ARG_DEF_LV_LAST		= 1 << 15,
};

static inline int val_bit_is_set(uint64_t val_bits, int val_enum)
{
	return (val_bits & (1 << val_enum)) ? 1 : 0;
}

static inline uint64_t val_enum_to_bit(int val_enum)
{
	return (1ULL << val_enum);
}

/* Description a value that follows an option or exists in a position. */

struct arg_def {
	uint64_t val_bits;   /* bits of x_VAL, can be multiple for pos_arg */
	uint64_t num;        /* a literal number for conststr_VAL */
	const char *str;     /* a literal string for constnum_VAL */
	uint32_t lv_types;   /* ARG_DEF_LV_, for lv_VAL, can be multiple */
	uint32_t flags;      /* ARG_DEF_FLAG_ */
};

/* Description of an option and the value that follows it. */

struct opt_arg {
	int opt;             /* option, e.g. foo_ARG */
	struct arg_def def;  /* defines accepted values */
};

/* Description of a position and the value that exists there. */

struct pos_arg {
	int pos;             /* position, e.g. first is 1 */
	struct arg_def def;  /* defines accepted values */
};

/*
 * CMD_RO_ARGS needs to accomodate a list of options,
 * of which one is required after which the rest are
 * optional.
 */
#define CMD_RO_ARGS 64          /* required opt args */
#define CMD_OO_ARGS 150         /* optional opt args */
#define CMD_RP_ARGS 8           /* required positional args */
#define CMD_OP_ARGS 8           /* optional positional args */

/*
 * one or more from required_opt_args is required,
 * then the rest are optional.
 */
#define CMD_FLAG_ONE_REQUIRED_OPT   1
#define CMD_FLAG_SECONDARY_SYNTAX   2

/* a register of the lvm commands */
struct command {
	const char *name;
	const char *desc; /* specific command description from command-lines.h */
	const char *usage; /* excludes common options like --help, --debug */
	const char *usage_common; /* includes commmon options like --help, --debug */
	const char *command_line_id;
	int command_line_enum; /* <command_line_id>_CMD */

	struct command_name *cname;

	command_fn fn;                      /* old style */
	struct command_function *functions; /* new style */

	unsigned int flags; /* copied from command_name.flags from commands.h */

	unsigned int cmd_flags; /* CMD_FLAG_ */

	/* definitions of opt/pos args */

	/* required args following an --opt */
	struct opt_arg required_opt_args[CMD_RO_ARGS];

	/* optional args following an --opt */
	struct opt_arg optional_opt_args[CMD_OO_ARGS];

	/* required positional args */
	struct pos_arg required_pos_args[CMD_RP_ARGS];

	/* optional positional args */
	struct pos_arg optional_pos_args[CMD_OP_ARGS];

	int ro_count;
	int oo_count;
	int rp_count;
	int op_count;

	/* used for processing current position */
	int pos_count;

	/* struct cmd_rule rules[CMD_RULES]; */
};

#if 0

/*
 * if rule.opt is set, then rule.type specifies if rule.check values
 * are required or invalid.
 *
 * if (arg_is_set(rule.opt) &&
 *     (rule.type & RULE_OPT_INVALID_OPT) && arg_is_set(rule.check.opt)) {
 *     log_error("option %s and option %s cannot be used together");
 * }
 *
 * if (arg_is_set(rule.opt) &&
 *     (rule.type & RULE_OPT_REQUIRES_LV_CHECK) && !lv_check(lv, rule.check.bits, &fail_bits)) {
 *     log_error("LV %s must be %s", lv, lv_check_to_str(fail_bits));
 * }
 *
 * if (arg_is_set(rule.opt) &&
 *     (rule.type & RULE_OPT_INVALID_LV_CHECK) && lv_check(lv, rule.check.bits, &fail_bits)) {
 *     log_error("LV %s must not be %s", lv, lv_check_to_str(fail_bits));
 * }
 *
 */

struct cmd_rule {
	int opt;  /* foo_ARG, or INT_MAX for command def in general */
	int type; /* RULE_ specifies how to require/prohibit check value */

	union {
		int opt;
		int pos;
		uint64_t bits;
	} check;
};
#endif

#endif
