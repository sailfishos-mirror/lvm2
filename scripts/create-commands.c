#include <asm/types.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>
#include <sched.h>
#include <dirent.h>
#include <ctype.h>
#include <getopt.h>

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
struct cmd_context;
struct arg_values;

int yes_no_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int activation_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int cachemode_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int discards_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int mirrorlog_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int size_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int size_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int size_mb_arg_with_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int int_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int int_arg_with_sign_and_percent(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int major_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int minor_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int string_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int tag_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int permission_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int units_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int segtype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int alloc_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int locktype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int readahead_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }

/* also see arg_props */
struct opt_name {
	const char *name;
	int opt_enum;         /* enum from args.h */
	const char short_opt;
	char _padding[7];
	const char *long_opt;
	int val_enum;         /* enum from vals.h */
	uint32_t unused1;
	uint32_t unused2;
};

/* also see val_props */
struct val_name {
	const char *enum_name;
	int val_enum;         /* enum from vals.h */
	int (*fn) (struct cmd_context *cmd, struct arg_values *av); /* unused here */
	const char *name;
	const char *usage;
};

/* create foo_VAL enums */

enum {
#define val(a, b, c, d) a ,
#include "vals.h"
#undef val
};

/* create foo_ARG enums */

enum {
#define arg(a, b, c, d, e, f) a ,
#include "args.h"
#undef arg
};

/* create table of value names, e.g. String, and corresponding enum from vals.h */

static struct val_name val_names[VAL_COUNT + 1] = {
#define val(a, b, c, d) { # a, a, b, c, d },
#include "vals.h"
#undef val
};

/* create table of option names, e.g. --foo, and corresponding enum from args.h */

static struct opt_name opt_names[ARG_COUNT + 1] = {
#define arg(a, b, c, d, e, f) { # a, a, b, "", "--" c, d, e, f },
#include "args.h"
#undef arg
};

#include "command.h"

#define MAX_CMD_NAMES 128
struct cmd_name {
	const char *name;
	const char *desc;
};

/* create table of command names, e.g. vgcreate */

static struct cmd_name cmd_names[MAX_CMD_NAMES] = {
#define xx(a, b, c) { # a , b } ,
#include "commands.h"
#undef xx
};

#define MAX_LINE 1024
#define MAX_LINE_ARGC 256

#define REQUIRED 1
#define OPTIONAL 0

struct oo_line {
	char *name;
	char *line;
};

#define MAX_CMDS 256
int cmd_count;
struct command cmd_array[MAX_CMDS];

struct command common_options; /* for printing common usage */

#define MAX_OO_LINES 256
int oo_line_count;
struct oo_line oo_lines[MAX_OO_LINES];


static void add_optional_opt_line(struct command *cmd, int argc, char *argv[]);

/*
 * modifies buf, replacing the sep characters with \0
 * argv pointers point to positions in buf
 */

static char *split_line(char *buf, int *argc, char **argv, char sep)
{
	char *p = buf, *rp = NULL;
	int i;

	argv[0] = p;

	for (i = 1; i < MAX_LINE_ARGC; i++) {
		p = strchr(buf, sep);
		if (!p)
			break;
		*p = '\0';

		argv[i] = p + 1;
		buf = p + 1;
	}
	*argc = i;

	/* we ended by hitting \0, return the point following that */
	if (!rp)
		rp = strchr(buf, '\0') + 1;

	return rp;
}

/* convert value string, e.g. Number, to foo_VAL enum */

static int val_str_to_num(char *str)
{
	char name[32] = { 0 };
	char *new;
	int i;

	/* compare the name before any suffix like _new or _<lvtype> */

	strncpy(name, str, 31);
	if ((new = strstr(name, "_")))
		*new = '\0';

	for (i = 0; i < VAL_COUNT; i++) {
		if (!val_names[i].name)
			break;
		if (!strncmp(name, val_names[i].name, strlen(val_names[i].name)))
			return val_names[i].val_enum;
	}

	return 0;
}

/* convert "--option" to foo_ARG enum */

static int opt_str_to_num(char *str)
{
	char long_name[32];
	char *p;
	int i;

	/*
	 * --foo_long means there are two args entries
	 * for --foo, one with a short option and one
	 * without, and we want the one without the
	 * short option.
	 */
	if (strstr(str, "_long")) {
		strcpy(long_name, str);
		p = strstr(long_name, "_long");
		*p = '\0';

		for (i = 0; i < ARG_COUNT; i++) {
			if (!opt_names[i].long_opt)
				continue;
			/* skip anything with a short opt */
			if (opt_names[i].short_opt)
				continue;
			if (!strcmp(opt_names[i].long_opt, long_name))
				return opt_names[i].opt_enum;
		}

		printf("Unknown opt str: %s %s\n", str, long_name);
		exit(1);
	}

	for (i = 0; i < ARG_COUNT; i++) {
		if (!opt_names[i].long_opt)
			continue;
		/* These are only selected using --foo_long */
		if (strstr(opt_names[i].name, "_long_ARG"))
			continue;
		if (!strcmp(opt_names[i].long_opt, str))
			return opt_names[i].opt_enum;
	}

	printf("Unknown opt str: \"%s\"\n", str);
	exit(1);
}

static char *val_bits_to_str(int val_bits)
{
	static char buf[128];
	int i;
	int or = 0;

	memset(buf, 0, sizeof(buf));

	for (i = 0; i < VAL_COUNT; i++) {
		if (val_bits & val_enum_to_bit(i)) {
			if (or) strcat(buf, " | ");
			strcat(buf, "val_enum_to_bit(");
			strcat(buf, val_names[i].enum_name);
			strcat(buf, ")");
			or = 1;
		}
	}

	return buf;
}

/*
 * The _<lvtype> and _new suffixes are only used by the command definitions and
 * are not exposed to lvm at large, which uses only the ARG_DEF values.
 */

static uint32_t lv_str_to_types(char *str)
{
	char copy[128] = { 0 };
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	uint32_t types = 0;
	int i;

	strncpy(copy, str, 128);

	split_line(copy, &argc, argv, '_');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		if (!strcmp(name, "linear"))
			types |= ARG_DEF_LV_LINEAR;

		if (!strcmp(name, "striped"))
			types |= ARG_DEF_LV_STRIPED;

		if (!strcmp(name, "snapshot"))
			types |= ARG_DEF_LV_SNAPSHOT;

		if (!strcmp(name, "mirror"))
			types |= ARG_DEF_LV_MIRROR;

		if (!strcmp(name, "thin"))
			types |= ARG_DEF_LV_THIN;

		if (!strcmp(name, "thinpool"))
			types |= ARG_DEF_LV_THINPOOL;

		if (!strcmp(name, "cache"))
			types |= ARG_DEF_LV_CACHE;

		if (!strcmp(name, "cachepool"))
			types |= ARG_DEF_LV_CACHEPOOL;

		if (!strcmp(name, "raid0"))
			types |= ARG_DEF_LV_RAID0;

		if (!strcmp(name, "raid1"))
			types |= ARG_DEF_LV_RAID1;

		if (!strcmp(name, "raid4"))
			types |= ARG_DEF_LV_RAID4;

		if (!strcmp(name, "raid5"))
			types |= ARG_DEF_LV_RAID5;

		if (!strcmp(name, "raid6"))
			types |= ARG_DEF_LV_RAID6;

		if (!strcmp(name, "raid10"))
			types |= ARG_DEF_LV_RAID10;

		if (!strcmp(name, "raid"))
			types |= ARG_DEF_LV_RAID;
	}

	return types;
}

static const char *lv_num_to_str(int num)
{
	switch (num) {
	case ARG_DEF_LV_LINEAR:
		return "linear";
	case ARG_DEF_LV_STRIPED:
		return "striped";
	case ARG_DEF_LV_SNAPSHOT:
		return "snapshot";
	case ARG_DEF_LV_MIRROR:
		return "mirror";
	case ARG_DEF_LV_RAID:
		return "raid";
	case ARG_DEF_LV_RAID0:
		return "raid0";
	case ARG_DEF_LV_RAID1:
		return "raid1";
	case ARG_DEF_LV_RAID4:
		return "raid4";
	case ARG_DEF_LV_RAID5:
		return "raid5";
	case ARG_DEF_LV_RAID6:
		return "raid6";
	case ARG_DEF_LV_RAID10:
		return "raid10";
	case ARG_DEF_LV_THIN:
		return "thin";
	case ARG_DEF_LV_THINPOOL:
		return "thinpool";
	case ARG_DEF_LV_CACHE:
		return "cache";
	case ARG_DEF_LV_CACHEPOOL:
		return "cachepool";
	default:
		printf("lv_num_to_str: unknown LV num: %d\n", num);
		exit(1);
	}
}

static char *lv_types_to_flags(int lv_types)
{
	static char buf_lv_types[128];
	int or = 0;

	memset(buf_lv_types, 0, sizeof(buf_lv_types));

	if (lv_types & ARG_DEF_LV_LINEAR) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_LINEAR");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_STRIPED) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_STRIPED");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_SNAPSHOT) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_SNAPSHOT");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_MIRROR) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_MIRROR");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID0) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID0");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID1) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID1");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID4) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID4");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID5) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID5");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID6) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID6");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_RAID10) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_RAID10");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_THIN) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_THIN");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_THINPOOL) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_THINPOOL");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_CACHE) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_CACHE");
		or = 1;
	}

	if (lv_types & ARG_DEF_LV_CACHEPOOL) {
		if (or) strcat(buf_lv_types, " | ");
		strcat(buf_lv_types, "ARG_DEF_LV_CACHEPOOL");
		or = 1;
	}

	return buf_lv_types;
}

static const char *is_command_name(char *str)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name)
			break;
		if (!strcmp(cmd_names[i].name, str))
			return cmd_names[i].name;
	}
	return NULL;
}

static const char *cmd_name_desc(const char *name)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name)
			break;
		if (!strcmp(cmd_names[i].name, name))
			return cmd_names[i].desc;
	}
	return NULL;
}

static int is_opt_name(char *str)
{
	if (!strncmp(str, "--", 2))
		return 1;

	if ((str[0] == '-') && (str[1] != '-')) {
		printf("Options must be specified in long form: %s\n", str);
		exit(1);
	}

	return 0;
}

/*
 * "Select" as a pos name means that the position
 * can be empty if the --select option is used.
 */

static int is_pos_name(char *str)
{
	if (!strncmp(str, "VG", 2))
		return 1;
	if (!strncmp(str, "LV", 2))
		return 1;
	if (!strncmp(str, "PV", 2))
		return 1;
	if (!strncmp(str, "Tag", 3))
		return 1;
	if (!strncmp(str, "String", 6))
		return 1;
	if (!strncmp(str, "Select", 6))
		return 1;
	return 0;
}

static int is_oo_definition(char *str)
{
	if (!strncmp(str, "OO_", 3))
		return 1;
	return 0;
}

static int is_oo_line(char *str)
{
	if (!strncmp(str, "OO:", 3))
		return 1;
	return 0;
}

static int is_op_line(char *str)
{
	if (!strncmp(str, "OP:", 3))
		return 1;
	return 0;
}

static int is_desc_line(char *str)
{
	if (!strncmp(str, "DESC:", 5))
		return 1;
	return 0;
}

static int is_id_line(char *str)
{
	if (!strncmp(str, "ID:", 3))
		return 1;
	return 0;
}

/*
 * parse str for anything that can appear in a position,
 * like VG, VG|LV, VG|LV_linear|LV_striped, etc
 */

static void set_pos_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = val_str_to_num(name);

		if (!val_enum) {
			printf("Unknown pos arg: %s\n", name);
			exit(1);
		}

		def->val_bits |= val_enum_to_bit(val_enum);

		if ((val_enum == lv_VAL) && strstr(name, "_"))
			def->lv_types = lv_str_to_types(name);

		if (strstr(name, "_new"))
			def->flags |= ARG_DEF_FLAG_NEW;
	}
}

/*
 * parse str for anything that can follow --option
 */

static void set_opt_def(struct command *cmd, char *str, struct arg_def *def)
{
	char *argv[MAX_LINE_ARGC];
	int argc;
	char *name;
	int val_enum;
	int i, j;

	split_line(str, &argc, argv, '|');

	for (i = 0; i < argc; i++) {
		name = argv[i];

		val_enum = val_str_to_num(name);

		if (!val_enum) {
			/* a literal number or string */

			if (isdigit(name[0]))
				val_enum = constnum_VAL;

			else if (isalpha(name[0]))
				val_enum = conststr_VAL;

			else {
				printf("Unknown opt arg: %s\n", name);
				exit(0);
			}
		}


		def->val_bits |= val_enum_to_bit(val_enum);

		if (val_enum == constnum_VAL)
			def->num = (uint64_t)atoi(name);

		if (val_enum == conststr_VAL)
			def->str = strdup(name);

		if (val_enum == lv_VAL) {
			if (strstr(name, "_"))
				def->lv_types = lv_str_to_types(name);
		}

		if ((val_enum == vg_VAL) || (val_enum == lv_VAL) || (val_enum == pv_VAL)) {
			if (strstr(name, "_new"))
				def->flags |= ARG_DEF_FLAG_NEW;
		}
	}
}


/*
 * OO_FOO: --opt1 ...
 *
 * oo->name = "OO_FOO";
 * oo->line = "--opt1 ...";
 */

static void add_oo_definition_line(const char *name, const char *line)
{
	struct oo_line *oo;
	char *colon;
	char *start;

	oo = &oo_lines[oo_line_count++];
	oo->name = strdup(name);

	if ((colon = strstr(oo->name, ":")))
		*colon = '\0';
	else {
		printf("invalid OO definition\n");
		exit(1);
	}

	start = strstr(line, ":") + 2;
	oo->line = strdup(start);
}

/* when OO_FOO: continues on multiple lines */

static void append_oo_definition_line(const char *new_line)
{
	struct oo_line *oo;
	char *old_line;
	char *line;
	int len;

	oo = &oo_lines[oo_line_count-1];

	old_line = oo->line;

	/* +2 = 1 space between old and new + 1 terminating \0 */
	len = strlen(old_line) + strlen(new_line) + 2;
	line = malloc(len);
	memset(line, 0, len);

	strcat(line, old_line);
	strcat(line, " ");
	strcat(line, new_line);

	free(oo->line);
	oo->line = line;
}

char *get_oo_line(char *str)
{
	char *name;
	char *end;
	char str2[64];
	int i;

	strcpy(str2, str);
	if ((end = strstr(str2, ":")))
		*end = '\0';
	if ((end = strstr(str2, ",")))
		*end = '\0';

	for (i = 0; i < oo_line_count; i++) {
		name = oo_lines[i].name;
		if (!strcmp(name, str2))
			return oo_lines[i].line;
	}
	return NULL;
}

/* add optional_opt_args entries when OO_FOO appears on OO: line */

static void include_optional_opt_args(struct command *cmd, char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = get_oo_line(str))) {
		printf("No OO line found for %s\n", str);
		exit(1);
	}

	if (!(line = strdup(oo_line)))
		exit(1); 

	split_line(line, &line_argc, line_argv, ' ');
	add_optional_opt_line(cmd, line_argc, line_argv);
	free(line);
}

static void add_opt_arg(struct command *cmd, char *str, int *takes_arg, int required)
{
	char *comma;
	int opt;

	/* opt_arg.opt set here */
	/* opt_arg.def will be set in update_prev_opt_arg() if needed */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	/*
	 * Work around nasty hack where --uuid is used for both uuid_ARG
	 * and uuidstr_ARG.  The input uses --uuidstr, where an actual
	 * command uses --uuid string.
	 */
	if (!strcmp(str, "--uuidstr")) {
		opt = uuidstr_ARG;
		goto skip;
	}

	opt = opt_str_to_num(str);
skip:
	if (required)
		cmd->required_opt_args[cmd->ro_count++].opt = opt;
	else
		cmd->optional_opt_args[cmd->oo_count++].opt = opt;

	*takes_arg = opt_names[opt].val_enum ? 1 : 0;
}

static void update_prev_opt_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };
	char *comma;

	if (str[0] == '-') {
		printf("Option %s must be followed by an arg.\n", str);
		exit(1);
	}

	/* opt_arg.def set here */
	/* opt_arg.opt was previously set in add_opt_arg() when --foo was read */

	if ((comma = strstr(str, ",")))
		*comma = '\0';

	set_opt_def(cmd, str, &def);

	if (required)
		cmd->required_opt_args[cmd->ro_count-1].def = def;
	else
		cmd->optional_opt_args[cmd->oo_count-1].def = def;
}

static void add_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def def = { 0 };

	/* pos_arg.pos and pos_arg.def are set here */

	set_pos_def(cmd, str, &def);

	if (required) {
		cmd->required_pos_args[cmd->rp_count].pos = cmd->pos_count++;
		cmd->required_pos_args[cmd->rp_count].def = def;
		cmd->rp_count++;
	} else {
		cmd->optional_pos_args[cmd->op_count].pos = cmd->pos_count++;;
		cmd->optional_pos_args[cmd->op_count].def = def;
		cmd->op_count++;
	}
}

/* process something that follows a pos arg, which is not a new pos arg */

static void update_prev_pos_arg(struct command *cmd, char *str, int required)
{
	struct arg_def *def;

	/* a previous pos_arg.def is modified here */

	if (required)
		def = &cmd->required_pos_args[cmd->rp_count-1].def;
	else
		def = &cmd->optional_pos_args[cmd->op_count-1].def;

	if (!strcmp(str, "..."))
		def->flags |= ARG_DEF_FLAG_MAY_REPEAT;
	else {
		printf("Unknown pos arg: %s\n", str);
		exit(1);
	}
}

/* process what follows OO:, which are optional opt args */

static void add_optional_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OO:", 3))
			continue;
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, OPTIONAL);
		else if (!strncmp(argv[i], "OO_", 3))
			include_optional_opt_args(cmd, argv[i]);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], OPTIONAL);
		else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);
	}
}

/* process what follows OP:, which are optional pos args */

static void add_optional_pos_line(struct command *cmd, int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++) {
		if (!i && !strncmp(argv[i], "OP:", 3))
			continue;
		if (is_pos_name(argv[i]))
			add_pos_arg(cmd, argv[i], OPTIONAL);
		else
			update_prev_pos_arg(cmd, argv[i], OPTIONAL);
	}
}

/* add required opt args from OO_FOO definition */

static void add_required_opt_line(struct command *cmd, int argc, char *argv[])
{
	int takes_arg;
	int i;

	for (i = 0; i < argc; i++) {
		if (is_opt_name(argv[i]))
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
		else if (takes_arg)
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
		else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);
	}
}

/* add to required_opt_args when OO_FOO appears on required line */
 
static void include_required_opt_args(struct command *cmd, char *str)
{
	char *oo_line;
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;

	if (!(oo_line = get_oo_line(str))) {
		printf("No OO line found for %s\n", str);
		exit(1);
	}

	if (!(line = strdup(oo_line)))
		exit(1); 

	split_line(line, &line_argc, line_argv, ' ');
	add_required_opt_line(cmd, line_argc, line_argv);
	free(line);
}

/* process what follows command_name, which are required opt/pos args */

static void add_required_line(struct command *cmd, int argc, char *argv[])
{
	int i;
	int takes_arg;
	int prev_was_opt = 0, prev_was_pos = 0;

	/* argv[0] is command name */

	for (i = 1; i < argc; i++) {
		if (is_opt_name(argv[i])) {
			add_opt_arg(cmd, argv[i], &takes_arg, REQUIRED);
			prev_was_opt = 1;
			prev_was_pos = 0;
		} else if (prev_was_opt && takes_arg) {
			update_prev_opt_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 0;
		} else if (is_pos_name(argv[i])) {
			add_pos_arg(cmd, argv[i], REQUIRED);
			prev_was_opt = 0;
			prev_was_pos = 1;
		} else if (!strncmp(argv[i], "OO_", 3)) {
			cmd->cmd_flags |= CMD_FLAG_ONE_REQUIRED_OPT;
			include_required_opt_args(cmd, argv[i]);
		} else if (prev_was_pos) {
			update_prev_pos_arg(cmd, argv[i], REQUIRED);
		} else
			printf("Can't parse argc %d argv %s prev %s\n",
				i, argv[i], argv[i-1]);

	}
}

static void print_def(struct arg_def *def, int usage)
{
	int val_enum;
	int sep = 0;
	int i;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL)
				printf("%s", def->str);

			else if (val_enum == constnum_VAL)
				printf("%llu", (unsigned long long)def->num);

			else {
				if (sep) printf("|");

				if (!usage || !val_names[val_enum].usage)
					printf("%s", val_names[val_enum].name);
				else
					printf("%s", val_names[val_enum].usage);

				sep = 1;
			}

			if (val_enum == lv_VAL && def->lv_types) {
				for (i = 0; i < 32; i++) {
					if (def->lv_types & (1 << i))
						printf("_%s", lv_num_to_str(1 << i));
				}
			}

			if ((val_enum == pv_VAL) || (val_enum == vg_VAL) || (val_enum == lv_VAL)) {
				if (def->flags & ARG_DEF_FLAG_NEW)
					printf("_new");
			}
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

void print_expanded(void)
{
	struct command *cmd;
	int onereq;
	int i, ro, rp, oo, op;

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];
		printf("%s", cmd->name);

		onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;

		if (cmd->ro_count) {
			if (onereq)
				printf(" (");

			for (ro = 0; ro < cmd->ro_count; ro++) {
				if (ro && onereq)
					printf(",");
				printf(" %s", opt_names[cmd->required_opt_args[ro].opt].long_opt);
				if (cmd->required_opt_args[ro].def.val_bits) {
					printf(" ");
					print_def(&cmd->required_opt_args[ro].def, 0);
				}
			}
			if (onereq)
				printf(" )");
		}

		if (cmd->rp_count) {
			for (rp = 0; rp < cmd->rp_count; rp++) {
				if (cmd->required_pos_args[rp].def.val_bits) {
					printf(" ");
					print_def(&cmd->required_pos_args[rp].def, 0);
				}
			}
		}

		if (cmd->oo_count) {
			printf("\n");
			printf("OO:");
			for (oo = 0; oo < cmd->oo_count; oo++) {
				if (oo)
					printf(",");
				printf(" %s", opt_names[cmd->optional_opt_args[oo].opt].long_opt);
				if (cmd->optional_opt_args[oo].def.val_bits) {
					printf(" ");
					print_def(&cmd->optional_opt_args[oo].def, 0);
				}
			}
		}

		if (cmd->op_count) {
			printf("\n");
			printf("OP:");
			for (op = 0; op < cmd->op_count; op++) {
				if (cmd->optional_pos_args[op].def.val_bits) {
					printf(" ");
					print_def(&cmd->optional_pos_args[op].def, 0);
				}
			}
		}

		printf("\n\n");
	}
}

static int opt_arg_matches(struct opt_arg *oa1, struct opt_arg *oa2)
{
	if (oa1->opt != oa2->opt)
		return 0;

	/* FIXME: some cases may need more specific val_bits checks */
	if (oa1->def.val_bits != oa2->def.val_bits)
		return 0;

	if (oa1->def.str && oa2->def.str && strcmp(oa1->def.str, oa2->def.str))
		return 0;

	if (oa1->def.num != oa2->def.num)
		return 0;

	/*
	 * Do NOT compare lv_types because we are checking if two
	 * command lines are ambiguous before the LV type is known.
	 */

	return 1;
}

static int pos_arg_matches(struct pos_arg *pa1, struct pos_arg *pa2)
{
	if (pa1->pos != pa2->pos)
		return 0;

	/* FIXME: some cases may need more specific val_bits checks */
	if (pa1->def.val_bits != pa2->def.val_bits)
		return 0;

	if (pa1->def.str && pa2->def.str && strcmp(pa1->def.str, pa2->def.str))
		return 0;

	if (pa1->def.num != pa2->def.num)
		return 0;

	/*
	 * Do NOT compare lv_types because we are checking if two
	 * command lines are ambiguous before the LV type is known.
	 */

	return 1;
}

static const char *opt_to_enum_str(int opt)
{
	return opt_names[opt].name;
}

static char *flags_to_str(int flags)
{
	static char buf_flags[32];

	memset(buf_flags, 0, sizeof(buf_flags));

	if (flags & ARG_DEF_FLAG_MAY_REPEAT)
		strcat(buf_flags, "ARG_DEF_FLAG_MAY_REPEAT");
	if (flags & ARG_DEF_FLAG_NEW)
		strcat(buf_flags, "ARG_DEF_FLAG_NEW");

	return buf_flags;
}

void print_command_count(void)
{
	struct command *cmd;
	int i, j;

	printf("/* Do not edit. This file is generated by scripts/create-commands */\n");
	printf("/* using command definitions from scripts/command-lines.in */\n");
	printf("#define COMMAND_COUNT %d\n", cmd_count);

	printf("enum {\n");
	printf("\tno_CMD,\n");  /* enum value 0 is not used */

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		if (!cmd->command_line_id) {
			printf("Missing ID: at %d\n", i);
			exit(1);
		}

		for (j = 0; j < i; j++) {
			if (!strcmp(cmd->command_line_id, cmd_array[j].command_line_id))
				goto next;
		}

		printf("\t%s_CMD,\n", cmd->command_line_id);
	next:
		;
	}
	printf("\tCOMMAND_ID_COUNT,\n");
	printf("};\n");
}

static int is_common_opt(int opt)
{
	int oo;

	for (oo = 0; oo < common_options.oo_count; oo++) {
		if (common_options.optional_opt_args[oo].opt == opt)
			return 1;
	}
	return 0;
}

/*
 * For certain commands (esp commands like lvcreate with many variants), common
 * options should not be printed for every variation, but once for all.  The
 * list of commands this applies to is fixed for now but could be encoded in
 * command-lines.in.
 *
 * The common options are defined in OO_USAGE_COMMON.  Those options
 * are skipped when creating the usage strings for each variation of
 * these commands.  Instead they are set in the usage_common string.
 */

void print_usage(struct command *cmd, int skip_required)
{
	int onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;
	int i, sep, ro, rp, oo, op;

	if (skip_required)
		goto oo_count;

	printf("\"%s", cmd->name);

	if (cmd->ro_count) {
		if (onereq)
			printf(" (");
		for (ro = 0; ro < cmd->ro_count; ro++) {
			if (ro && onereq)
				printf(",");
			printf(" %s", opt_names[cmd->required_opt_args[ro].opt].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def(&cmd->required_opt_args[ro].def, 1);
			}
		}
		if (onereq)
			printf(" )");
	}

	if (cmd->rp_count) {
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def(&cmd->required_pos_args[rp].def, 1);
			}
		}
	}

	printf("\"");

 oo_count:
	if (!cmd->oo_count)
		goto op_count;

	sep = 0;

	if (cmd->oo_count) {
		for (oo = 0; oo < cmd->oo_count; oo++) {
			/* skip common opts which are in the usage_common string */
			if ((cmd != &common_options) && is_common_opt(cmd->optional_opt_args[oo].opt))
				continue;

			if (!sep) {
				printf("\n");
				printf("\" [");
			}

			if (sep)
				printf(",");

			printf(" %s", opt_names[cmd->optional_opt_args[oo].opt].long_opt);
			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}
	}

	if (sep)
		printf(" ]\"");

 op_count:
	if (!cmd->op_count)
		goto done;

	printf("\n");
	printf("\" [");

	if (cmd->op_count) {
		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				print_def(&cmd->optional_pos_args[op].def, 1);
			}
		}
	}

	printf(" ]\"");

 done:
	printf(";\n");
}

static void print_val_man(const char *str)
{
	char *line;
	char *line_argv[MAX_LINE_ARGC];
	int line_argc;
	int i;

	if (!strcmp(str, "Number") ||
	    !strcmp(str, "String") ||
	    !strncmp(str, "VG", 2) ||
	    !strncmp(str, "LV", 2) ||
	    !strncmp(str, "PV", 2) ||
	    !strcmp(str, "Tag")) {
		printf("\\fI%s\\fP", str);
		return;
	}

	if (strstr(str, "Number[") || strstr(str, "]Number")) {
		for (i = 0; i < strlen(str); i++) {
			if (str[i] == 'N')
				printf("\\fI");
			if (str[i] == 'r') {
				printf("%c", str[i]);
				printf("\\fP");
				continue;
			}
			printf("%c", str[i]);
		}
		return;
	}

	if (strstr(str, "|")) {
		line = strdup(str);
		split_line(line, &line_argc, line_argv, '|');
		for (i = 0; i < line_argc; i++) {
			if (i)
				printf("|");
			if (strstr(line_argv[i], "Number"))
				printf("\\fI%s\\fP", line_argv[i]);
			else
				printf("\\fB%s\\fP", line_argv[i]);
		}
		return;
	}

	printf("\\fB%s\\fP", str);
}

static void print_def_man(struct arg_def *def, int usage)
{
	int val_enum;
	int sep = 0;
	int i;

	for (val_enum = 0; val_enum < VAL_COUNT; val_enum++) {
		if (def->val_bits & val_enum_to_bit(val_enum)) {

			if (val_enum == conststr_VAL) {
				printf("\\fB");
				printf("%s", def->str);
				printf("\\fP");
			}

			else if (val_enum == constnum_VAL) {
				printf("\\fB");
				printf("ll%u", (unsigned long long)def->num);
				printf("\\fP");
			}

			else {
				if (sep) printf("|");

				if (!usage || !val_names[val_enum].usage) {
					printf("\\fI");
					printf("%s", val_names[val_enum].name);
					printf("\\fP");
				} else {
					print_val_man(val_names[val_enum].usage);
				}

				sep = 1;
			}

			if (val_enum == lv_VAL && def->lv_types) {
				printf("\\fI");
				for (i = 0; i < 32; i++) {
					if (def->lv_types & (1 << i))
						printf("_%s", lv_num_to_str(1 << i));
				}
				printf("\\fP");
			}

			if ((val_enum == pv_VAL) || (val_enum == vg_VAL) || (val_enum == lv_VAL)) {
				if (def->flags & ARG_DEF_FLAG_NEW) {
					printf("\\fI");
					printf("_new");
					printf("\\fP");
				}
			}
		}
	}

	if (def->flags & ARG_DEF_FLAG_MAY_REPEAT)
		printf(" ...");
}

void print_cmd_man(struct command *cmd, int skip_required)
{
	int onereq = (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT) ? 1 : 0;
	int i, sep, ro, rp, oo, op;

	if (skip_required)
		goto oo_count;

	printf("\\fB%s\\fP", cmd->name);

	if (!onereq)
		goto ro_normal;

	/*
	 * one required option in a set, print as:
	 * ( -a|--a,
	 *   -b|--b,
	 *      --c,
	 *      --d )
	 *
	 * First loop through ro prints those with short opts,
	 * and the second loop prints those without short opts.
	 */

	if (cmd->ro_count) {
		printf("\n");
		printf(".RS 4\n");
		printf("(");

		sep = 0;

		for (ro = 0; ro < cmd->ro_count; ro++) {
			if (!opt_names[cmd->required_opt_args[ro].opt].short_opt)
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			if (opt_names[cmd->required_opt_args[ro].opt].short_opt) {
				printf(" \\fB-%c\\fP|\\fB%s\\fP",
				       opt_names[cmd->required_opt_args[ro].opt].short_opt,
				       opt_names[cmd->required_opt_args[ro].opt].long_opt);
			} else {
				printf("   ");
				printf(" \\fB%s\\fP", opt_names[cmd->required_opt_args[ro].opt].long_opt);
			}

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep = 1;
		}

		for (ro = 0; ro < cmd->ro_count; ro++) {
			if (opt_names[cmd->required_opt_args[ro].opt].short_opt)
				continue;

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			printf("   ");
			printf(" \\fB%s\\fP", opt_names[cmd->required_opt_args[ro].opt].long_opt);

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}

			sep = 1;
		}

		printf(" )\n");
		printf(".RE\n");
	}

	if (cmd->rp_count) {
		printf(".RS 4\n");
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_pos_args[rp].def, 1);
			}
		}

		printf("\n");
		printf(".RE\n");
	} else {
		/* printf("\n"); */
	}

	printf(".br\n");
	goto oo_count;

 ro_normal:

	/*
	 * all are required options, print as:
	 * -a|--a, -b|--b
	 */

	if (cmd->ro_count) {
		for (ro = 0; ro < cmd->ro_count; ro++) {
			if (opt_names[cmd->required_opt_args[ro].opt].short_opt) {
				printf(" \\fB-%c\\fP|\\fB%s\\fP",
				       opt_names[cmd->required_opt_args[ro].opt].short_opt,
				       opt_names[cmd->required_opt_args[ro].opt].long_opt);
			} else {
				printf(" \\fB%s\\fP", opt_names[cmd->required_opt_args[ro].opt].long_opt);
			}

			if (cmd->required_opt_args[ro].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_opt_args[ro].def, 1);
			}
		}
	}

	if (cmd->rp_count) {
		for (rp = 0; rp < cmd->rp_count; rp++) {
			if (cmd->required_pos_args[rp].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->required_pos_args[rp].def, 1);
			}
		}

		printf("\n");
	} else {
		printf("\n");
	}

	printf(".br\n");

 oo_count:
	if (!cmd->oo_count)
		goto op_count;

	sep = 0;

	printf(".br\n");

	if (cmd->oo_count) {

		for (oo = 0; oo < cmd->oo_count; oo++) {
			/* skip common opts which are in the usage_common string */
			if ((cmd != &common_options) && is_common_opt(cmd->optional_opt_args[oo].opt))
				continue;

			if (!opt_names[cmd->optional_opt_args[oo].opt].short_opt)
				continue;

			if (!sep) {
				printf(".RS 4\n");
				printf("[");
			}

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			printf(" \\fB-%c\\fP|\\fB%s\\fP",
				opt_names[cmd->optional_opt_args[oo].opt].short_opt,
				opt_names[cmd->optional_opt_args[oo].opt].long_opt);

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}

		for (oo = 0; oo < cmd->oo_count; oo++) {
			/* skip common opts which are in the usage_common string */
			if ((cmd != &common_options) && is_common_opt(cmd->optional_opt_args[oo].opt))
				continue;

			if (opt_names[cmd->optional_opt_args[oo].opt].short_opt)
				continue;

			if (!sep) {
				printf(".RS 4\n");
				printf("[");
			}

			if (sep) {
				printf(",");
				printf("\n.br\n");
				printf(" ");
			}

			/* space alignment without short opt */
			printf("   ");

			printf(" \\fB%s\\fP", opt_names[cmd->optional_opt_args[oo].opt].long_opt);

			if (cmd->optional_opt_args[oo].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_opt_args[oo].def, 1);
			}
			sep = 1;
		}
	}

	if (sep) {
		printf(" ]\n");
		printf(".RE\n");
		printf(".br\n");
	}

 op_count:
	if (!cmd->op_count)
		goto done;

	printf(".RS 4\n");
	printf("[");

	if (cmd->op_count) {
		for (op = 0; op < cmd->op_count; op++) {
			if (cmd->optional_pos_args[op].def.val_bits) {
				printf(" ");
				print_def_man(&cmd->optional_pos_args[op].def, 1);
			}
		}
	}

	printf("]\n");
	printf(".RE\n");

 done:
	printf("\n");
}

#define DESC_LINE 256

void print_desc_man(const char *desc)
{
	char buf[DESC_LINE] = {0};
	int di = 0;
	int bi = 0;

	for (di = 0; di < strlen(desc); di++) {
		if (desc[di] == '\0')
			break;
		if (desc[di] == '\n')
			continue;

		if (!strncmp(&desc[di], "DESC:", 5)) {
			if (bi) {
				printf("%s\n", buf);
				printf(".br\n");
				memset(buf, 0, sizeof(buf));
				bi = 0;
			}
			di += 5;
			continue;
		}

		if (!bi && desc[di] == ' ')
			continue;

		buf[bi++] = desc[di];

		if (bi == (DESC_LINE - 1))
			break;
	}

	if (bi) {
		printf("%s\n", buf);
		printf(".br\n");
	}
}

void print_command_man(void)
{
	struct command *cmd;
	const char *last_cmd_name = NULL;
	const char *desc;
	int i, j, ro, rp, oo, op;

	include_optional_opt_args(&common_options, "OO_USAGE_COMMON");

	printf(".TH LVM_ALL 8\n");

	for (i = 0; i < cmd_count; i++) {

		cmd = &cmd_array[i];

		if (!last_cmd_name || strcmp(last_cmd_name, cmd->name)) {
			printf(".SH NAME\n");
			printf(".\n");
			if ((desc = cmd_name_desc(cmd->name)))
				printf("%s - %s\n", cmd->name, desc);
			else
				printf("%s\n", cmd->name);
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
			printf(".SH SYNOPSIS\n");
			printf(".br\n");
			printf(".P\n");
			printf(".\n");
			last_cmd_name = cmd->name;
		}

		if (cmd->desc) {
			print_desc_man(cmd->desc);
			printf(".P\n");
		}

		print_cmd_man(cmd, 0);

		if ((i == (cmd_count - 1)) || strcmp(cmd->name, cmd_array[i+1].name)) {
			printf("Common options:\n");
			printf(".\n");
			print_cmd_man(&common_options, 1);
		}

		printf("\n");
		continue;
	}
}

void print_command_struct(int only_usage)
{
	struct command *cmd;
	int i, j, ro, rp, oo, op;

	include_optional_opt_args(&common_options, "OO_USAGE_COMMON");

	printf("/* Do not edit. This file is generated by scripts/create-commands */\n");
	printf("/* using command definitions from scripts/command-lines.in */\n");
	printf("\n");

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		if (only_usage) {
			print_usage(cmd, 0);
			print_usage(&common_options, 1);
			printf("\n");
			continue;
		}

		printf("commands[%d].name = \"%s\";\n", i, cmd->name);
		printf("commands[%d].command_line_id = \"%s\";\n", i, cmd->command_line_id);
		printf("commands[%d].command_line_enum = %s_CMD;\n", i, cmd->command_line_id);
		printf("commands[%d].fn = %s;\n", i, cmd->name);
		printf("commands[%d].ro_count = %d;\n", i, cmd->ro_count);
		printf("commands[%d].rp_count = %d;\n", i, cmd->rp_count);
		printf("commands[%d].oo_count = %d;\n", i, cmd->oo_count);
		printf("commands[%d].op_count = %d;\n", i, cmd->op_count);

		if (cmd->cmd_flags & CMD_FLAG_ONE_REQUIRED_OPT)
			printf("commands[%d].cmd_flags = CMD_FLAG_ONE_REQUIRED_OPT;\n", i);

		printf("commands[%d].desc = \"%s\";\n", i, cmd->desc ?: "");
		printf("commands[%d].usage = ", i);
		print_usage(cmd, 0);

		if (cmd->oo_count) {
			printf("commands[%d].usage_common = ", i);
			print_usage(&common_options, 1);
		} else {
			printf("commands[%d].usage_common = \"NULL\";\n", i);
		}

		if (cmd->ro_count) {
			for (ro = 0; ro < cmd->ro_count; ro++) {
				printf("commands[%d].required_opt_args[%d].opt = %s;\n",
					i, ro, opt_to_enum_str(cmd->required_opt_args[ro].opt));

				if (!cmd->required_opt_args[ro].def.val_bits)
					continue;

				printf("commands[%d].required_opt_args[%d].def.val_bits = %s;\n",
					i, ro, val_bits_to_str(cmd->required_opt_args[ro].def.val_bits));

				if (cmd->required_opt_args[ro].def.lv_types)
					printf("commands[%d].required_opt_args[%d].def.lv_types = %s;\n",
						i, ro, lv_types_to_flags(cmd->required_opt_args[ro].def.lv_types));

				if (cmd->required_opt_args[ro].def.flags)
					printf("commands[%d].required_opt_args[%d].def.flags = %s;\n",
						i, ro, flags_to_str(cmd->required_opt_args[ro].def.flags));

				if (val_bit_is_set(cmd->required_opt_args[ro].def.val_bits, constnum_VAL))
					printf("commands[%d].required_opt_args[%d].def.num = %d;\n",
						i, ro, cmd->required_opt_args[ro].def.num);

				if (val_bit_is_set(cmd->required_opt_args[ro].def.val_bits, conststr_VAL))
					printf("commands[%d].required_opt_args[%d].def.str = \"%s\";\n",
						i, ro, cmd->required_opt_args[ro].def.str ?: "NULL");
			}
		}

		if (cmd->rp_count) {
			for (rp = 0; rp < cmd->rp_count; rp++) {
				printf("commands[%d].required_pos_args[%d].pos = %d;\n",
					i, rp, cmd->required_pos_args[rp].pos);

				if (!cmd->required_pos_args[rp].def.val_bits)
					continue;

				printf("commands[%d].required_pos_args[%d].def.val_bits = %s;\n",
					i, rp, val_bits_to_str(cmd->required_pos_args[rp].def.val_bits));

				if (cmd->required_pos_args[rp].def.lv_types)
					printf("commands[%d].required_pos_args[%d].def.lv_types = %s;\n",
						i, rp, lv_types_to_flags(cmd->required_pos_args[rp].def.lv_types));

				if (cmd->required_pos_args[rp].def.flags)
					printf("commands[%d].required_pos_args[%d].def.flags = %s;\n",
						i, rp, flags_to_str(cmd->required_pos_args[rp].def.flags));

				if (val_bit_is_set(cmd->required_pos_args[rp].def.val_bits, constnum_VAL))
					printf("commands[%d].required_pos_args[%d].def.num = %d;\n",
						i, rp, cmd->required_pos_args[rp].def.num);

				if (val_bit_is_set(cmd->required_pos_args[rp].def.val_bits, conststr_VAL))
					printf("commands[%d].required_pos_args[%d].def.str = \"%s\";\n",
						i, rp, cmd->required_pos_args[rp].def.str ?: "NULL");
			}
		}

		if (cmd->oo_count) {
			for (oo = 0; oo < cmd->oo_count; oo++) {
				printf("commands[%d].optional_opt_args[%d].opt = %s;\n",
					i, oo, opt_to_enum_str(cmd->optional_opt_args[oo].opt));

				if (!cmd->optional_opt_args[oo].def.val_bits)
					continue;

				printf("commands[%d].optional_opt_args[%d].def.val_bits = %s;\n",
					i, oo, val_bits_to_str(cmd->optional_opt_args[oo].def.val_bits));

				if (cmd->optional_opt_args[oo].def.lv_types)
					printf("commands[%d].optional_opt_args[%d].def.lv_types = %s;\n",
						i, oo, lv_types_to_flags(cmd->optional_opt_args[oo].def.lv_types));

				if (cmd->optional_opt_args[oo].def.flags)
					printf("commands[%d].optional_opt_args[%d].def.flags = %s;\n",
						i, oo, flags_to_str(cmd->optional_opt_args[oo].def.flags));

				if (val_bit_is_set(cmd->optional_opt_args[oo].def.val_bits, constnum_VAL)) 
					printf("commands[%d].optional_opt_args[%d].def.num = %d;\n",
						i, oo, cmd->optional_opt_args[oo].def.num);

				if (val_bit_is_set(cmd->optional_opt_args[oo].def.val_bits, conststr_VAL))
					printf("commands[%d].optional_opt_args[%d].def.str = \"%s\";\n",
						i, oo, cmd->optional_opt_args[oo].def.str ?: "NULL");
			}
		}

		if (cmd->op_count) {
			for (op = 0; op < cmd->op_count; op++) {
				printf("commands[%d].optional_pos_args[%d].pos = %d;\n",
					i, op, cmd->optional_pos_args[op].pos);

				if (!cmd->optional_pos_args[op].def.val_bits)
					continue;

				printf("commands[%d].optional_pos_args[%d].def.val_bits = %s;\n",
					i, op, val_bits_to_str(cmd->optional_pos_args[op].def.val_bits));

				if (cmd->optional_pos_args[op].def.lv_types)
					printf("commands[%d].optional_pos_args[%d].def.lv_types = %s;\n",
						i, op, lv_types_to_flags(cmd->optional_pos_args[op].def.lv_types));

				if (cmd->optional_pos_args[op].def.flags)
					printf("commands[%d].optional_pos_args[%d].def.flags = %s;\n",
						i, op, flags_to_str(cmd->optional_pos_args[op].def.flags));

				if (val_bit_is_set(cmd->optional_pos_args[op].def.val_bits, constnum_VAL))
					printf("commands[%d].optional_pos_args[%d].def.num = %d;\n",
						i, op, cmd->optional_pos_args[op].def.num);

				if (val_bit_is_set(cmd->optional_pos_args[op].def.val_bits, conststr_VAL))
					printf("commands[%d].optional_pos_args[%d].def.str = \"%s\";\n",
						i, op, cmd->optional_pos_args[op].def.str ?: "NULL");
			}
		}

		printf("\n");
	}
}

struct cmd_pair {
	int i, j;
};

static void print_ambiguous(void)
{
	struct command *cmd, *dup;
	struct cmd_pair dups[64] = { 0 };
	int found = 0;
	int i, j, f, ro, rp;

	for (i = 0; i < cmd_count; i++) {
		cmd = &cmd_array[i];

		for (j = 0; j < cmd_count; j++) {
			dup = &cmd_array[j];

			if (i == j)
				continue;
			if (strcmp(cmd->name, dup->name))
				continue;
			if (cmd->ro_count != dup->ro_count)
				continue;
			if (cmd->rp_count != dup->rp_count)
				continue;

			for (ro = 0; ro < cmd->ro_count; ro++) {
				if (!opt_arg_matches(&cmd->required_opt_args[ro],
						     &dup->required_opt_args[ro]))
					goto next;
			}

			for (rp = 0; rp < cmd->rp_count; rp++) {
				if (!pos_arg_matches(&cmd->required_pos_args[rp],
						     &dup->required_pos_args[rp]))
					goto next;
			}

			for (f = 0; f < found; f++) {
				if ((dups[f].i == j) && (dups[f].j == i))
					goto next;
			}

			printf("Ambiguous commands %d and %d:\n", i, j);
			print_usage(cmd, 0);
			print_usage(dup, 0);
			printf("\n");

			dups[found].i = i;
			dups[found].j = j;
			found++;
next:
			;
		}
	}
}

void print_command_list(void)
{
	int i;

	for (i = 0; i < MAX_CMD_NAMES; i++) {
		if (!cmd_names[i].name) {
			printf("found %d command names\n", i);
			break;
		}
		printf("%s\n", cmd_names[i].name);
	}
}

void print_option_list(void)
{
	int i;

	for (i = 0; i < ARG_COUNT; i++)
		printf("%d %s %s %c (%d)\n",
			opt_names[i].opt_enum, opt_names[i].name,
			opt_names[i].long_opt, opt_names[i].short_opt ?: ' ',
			opt_names[i].short_opt ? opt_names[i].short_opt : 0);
}

static void print_help(int argc, char *argv[])
{
	printf("%s --output struct|count|usage|expanded <filename>\n", argv[0]);
	printf("\n");
	printf("struct:    print C structures.\n");
	printf("usage:     print usage format.\n");
	printf("expanded:  print expanded input format.\n");
	printf("count:     print #define COMMAND_COUNT <Number>\n");
	printf("ambiguous: print commands differing only by LV types\n");
}

int main(int argc, char *argv[])
{
	char *outputformat = NULL;
	char *inputfile = NULL;
	FILE *file;
	struct command *cmd;
	char line[MAX_LINE];
	char line_orig[MAX_LINE];
	const char *name;
	char *line_argv[MAX_LINE_ARGC];
	char *n;
	int line_argc;
	int prev_was_oo_def = 0;
	int prev_was_oo = 0;
	int prev_was_op = 0;

	if (argc < 2) {
		print_help(argc, argv);
		exit(EXIT_FAILURE);
	}

	if (!strcmp(argv[1], "debug")) {
		print_command_list();
		print_option_list();
		return 0;
	}

	static struct option long_options[] = {
		{"help",      no_argument,       0, 'h' },
		{"output",    required_argument, 0, 'o' },
		{0, 0, 0, 0 }
	};

        while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "ho:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case '0':
			break;
		case 'h':
                        print_help(argc, argv);
			exit(EXIT_SUCCESS);
		case 'o':
			outputformat = strdup(optarg);
			break;
		}
	}

	if (optind < argc)
		inputfile = argv[optind];
	else {
		printf("Missing filename.\n");
		return 0;
	}

	if (!(file = fopen(inputfile, "r"))) {
		printf("Cannot open %s\n", argv[1]);
		return -1;
	}

	while (fgets(line, MAX_LINE, file)) {
		if (line[0] == '#')
			continue;
		if (line[0] == '\n')
			continue;
		if (line[0] == '-' && line[1] == '-' && line[2] == '-')
			continue;

		if ((n = strchr(line, '\n')))
			*n = '\0';

		memcpy(line_orig, line, sizeof(line));
		split_line(line, &line_argc, line_argv, ' ');

		if (!line_argc)
			continue;

		/* command ... */
		if ((name = is_command_name(line_argv[0]))) {
			if (cmd_count >= MAX_CMDS) {
				printf("MAX_CMDS too small\n");
				return -1;
			}
			cmd = &cmd_array[cmd_count++];
			cmd->name = name;
			cmd->pos_count = 1;
			add_required_line(cmd, line_argc, line_argv);

			/* Every cmd gets the OO_ALL options */
			include_optional_opt_args(cmd, "OO_ALL:");
			continue;
		}

		if (is_desc_line(line_argv[0])) {
			char *desc = strdup(line_orig);
			if (cmd->desc) {
				cmd->desc = realloc((char *)cmd->desc, strlen(cmd->desc) + strlen(desc) + 2);
				strcat((char *)cmd->desc, "  ");
				strcat((char *)cmd->desc, desc);
				free(desc);
			} else
				cmd->desc = desc;
			continue;
		}

		if (is_id_line(line_argv[0])) {
			cmd->command_line_id = strdup(line_argv[1]);
			continue;
		}

		/* OO_FOO: ... */
		if (is_oo_definition(line_argv[0])) {
			add_oo_definition_line(line_argv[0], line_orig);
			prev_was_oo_def = 1;
			prev_was_oo = 0;
			prev_was_op = 0;
			continue;
		}

		/* OO: ... */
		if (is_oo_line(line_argv[0])) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 1;
			prev_was_op = 0;
			continue;
		}

		/* OP: ... */
		if (is_op_line(line_argv[0])) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			prev_was_oo_def = 0;
			prev_was_oo = 0;
			prev_was_op = 1;
			continue;
		}

		/* handle OO_FOO:, OO:, OP: continuing on multiple lines */

		if (prev_was_oo_def) {
			append_oo_definition_line(line_orig);
			continue;
		}

		if (prev_was_oo) {
			add_optional_opt_line(cmd, line_argc, line_argv);
			continue;
		}

		if (prev_was_op) {
			add_optional_pos_line(cmd, line_argc, line_argv);
			continue;
		}
	}

	fclose(file);

	if (!outputformat)
		print_command_struct(1);
	else if (!strcmp(outputformat, "struct"))
		print_command_struct(0);
	else if (!strcmp(outputformat, "count"))
		print_command_count();
	else if (!strcmp(outputformat, "usage"))
		print_command_struct(1);
	else if (!strcmp(outputformat, "expanded"))
		print_expanded();
	else if (!strcmp(outputformat, "ambiguous"))
		print_ambiguous();
	else if (!strcmp(outputformat, "man"))
		print_command_man();
	else
		print_help(argc, argv);
}

