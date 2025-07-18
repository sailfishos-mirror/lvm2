/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libdm/misc/dmlib.h"

#include <ctype.h>
#include <langinfo.h>
#include <math.h>  /* fabs() */
#include <float.h> /* DBL_EPSILON */
#include <time.h>

/*
 * Internal flags
 */
#define RH_SORT_REQUIRED	0x00000100
#define RH_HEADINGS_PRINTED	0x00000200
#define RH_FIELD_CALC_NEEDED	0x00000400
#define RH_ALREADY_REPORTED	0x00000800

struct selection {
	struct dm_pool *mem;
	struct dm_pool *regex_mem;
	struct selection_node *selection_root;
	int add_new_fields;
};

struct report_group_item;

struct dm_report {
	struct dm_pool *mem;

	/**
	 * Cache the first row allocated so that all rows and fields
	 * can be disposed of in a single dm_pool_free() call.
	 */
	struct row *first_row;

	/* To report all available types */
#define REPORT_TYPES_ALL	UINT32_MAX
	uint32_t report_types;
	const char *output_field_name_prefix;
	const char *field_prefix;
	uint32_t flags;
	const char *separator;

	uint32_t keys_count;

	/* Ordered list of fields needed for this report */
	struct dm_list field_props;

	/* Rows of report data */
	struct dm_list rows;

	/* Array of field definitions */
	const struct dm_report_field_type *fields;
	const char **canonical_field_ids;
	const struct dm_report_object_type *types;

	/* To store caller private data */
	void *private;

	/* Selection handle */
	struct selection *selection;

	/* Null-terminated array of reserved values */
	const struct dm_report_reserved_value *reserved_values;
	struct dm_hash_table *value_cache;

	struct report_group_item *group_item;
};

struct dm_report_group {
	dm_report_group_type_t type;
	struct dm_pool *mem;
	struct dm_list items;
	int indent;
};

struct report_group_item {
	struct dm_list list;
	struct dm_report_group *group;
	struct dm_report *report;
	union store_u {
		uint32_t orig_report_flags;
		uint32_t finished_count;
	} store;
	struct report_group_item *parent;
	unsigned output_done:1;
	unsigned needs_closing:1;
	void *data;
};

/*
 * Internal per-field flags
 */
#define FLD_HIDDEN	0x00001000
#define FLD_SORT_KEY	0x00002000
#define FLD_ASCENDING	0x00004000
#define FLD_DESCENDING	0x00008000
#define FLD_COMPACTED	0x00010000
#define FLD_COMPACT_ONE 0x00020000

struct field_properties {
	struct dm_list list;
	uint32_t field_num;
	uint32_t sort_posn;
	int32_t initial_width;
	int32_t width; /* current width: adjusted by dm_report_object() */
	const struct dm_report_object_type *type;
	uint32_t flags;
	int implicit;
};

/*
 * Report selection
 */
struct op_def {
	const char *string;
	uint32_t flags;
	const char *desc;
};

#define FLD_CMP_MASK		0x0FF00000
#define FLD_CMP_UNCOMPARABLE	0x00100000
#define FLD_CMP_EQUAL		0x00200000
#define FLD_CMP_NOT		0x00400000
#define FLD_CMP_GT		0x00800000
#define FLD_CMP_LT		0x01000000
#define FLD_CMP_REGEX		0x02000000
#define FLD_CMP_NUMBER		0x04000000
#define FLD_CMP_TIME		0x08000000
/*
 * #define FLD_CMP_STRING 0x10000000
 * We could define FLD_CMP_STRING here for completeness here,
 * but it's not needed - we can check operator compatibility with
 * field type by using FLD_CMP_REGEX, FLD_CMP_NUMBER and
 * FLD_CMP_TIME flags only.
 */

/*
 * When defining operators, always define longer one before
 * shorter one if one is a prefix of another!
 * (e.g. =~ comes before =)
*/
static const struct op_def _op_cmp[] = {
	{ "=~", FLD_CMP_REGEX, "Matching regular expression. [regex]" },
	{ "!~", FLD_CMP_REGEX|FLD_CMP_NOT, "Not matching regular expression. [regex]" },
	{ "=", FLD_CMP_EQUAL, "Equal to. [number, size, percent, string, string list, time]" },
	{ "!=", FLD_CMP_NOT|FLD_CMP_EQUAL, "Not equal to. [number, size, percent, string, string_list, time]" },
	{ ">=", FLD_CMP_NUMBER|FLD_CMP_TIME|FLD_CMP_GT|FLD_CMP_EQUAL, "Greater than or equal to. [number, size, percent, time]" },
	{ ">", FLD_CMP_NUMBER|FLD_CMP_TIME|FLD_CMP_GT, "Greater than. [number, size, percent, time]" },
	{ "<=", FLD_CMP_NUMBER|FLD_CMP_TIME|FLD_CMP_LT|FLD_CMP_EQUAL, "Less than or equal to. [number, size, percent, time]" },
	{ "<", FLD_CMP_NUMBER|FLD_CMP_TIME|FLD_CMP_LT, "Less than. [number, size, percent, time]" },
	{ "since", FLD_CMP_TIME|FLD_CMP_GT|FLD_CMP_EQUAL, "Since specified time (same as '>='). [time]" },
	{ "after", FLD_CMP_TIME|FLD_CMP_GT, "After specified time (same as '>'). [time]"},
	{ "until", FLD_CMP_TIME|FLD_CMP_LT|FLD_CMP_EQUAL, "Until specified time (same as '<='). [time]"},
	{ "before", FLD_CMP_TIME|FLD_CMP_LT, "Before specified time (same as '<'). [time]"},
	{ NULL, 0, NULL }
};

#define SEL_MASK		0x000000FF
#define SEL_ITEM		0x00000001
#define SEL_AND 		0x00000002
#define SEL_OR			0x00000004

#define SEL_MODIFIER_MASK	0x00000F00
#define SEL_MODIFIER_NOT	0x00000100

#define SEL_PRECEDENCE_MASK	0x0000F000
#define SEL_PRECEDENCE_PS	0x00001000
#define SEL_PRECEDENCE_PE	0x00002000

#define SEL_LIST_MASK		0x000F0000
#define SEL_LIST_LS		0x00010000
#define SEL_LIST_LE		0x00020000
#define SEL_LIST_SUBSET_LS	0x00040000
#define SEL_LIST_SUBSET_LE	0x00080000

static const struct op_def _op_log[] = {
	{ "&&", SEL_AND, "All fields must match" },
	{ ",", SEL_AND, "All fields must match" },
	{ "||", SEL_OR, "At least one field must match" },
	{ "#", SEL_OR, "At least one field must match" },
	{ "!", SEL_MODIFIER_NOT, "Logical negation" },
	{ "(", SEL_PRECEDENCE_PS, "Left parenthesis" },
	{ ")", SEL_PRECEDENCE_PE, "Right parenthesis" },
	{ "[", SEL_LIST_LS, "List start" },
	{ "]", SEL_LIST_LE, "List end"},
	{ "{", SEL_LIST_SUBSET_LS, "List subset start"},
	{ "}", SEL_LIST_SUBSET_LE, "List subset end"},
	{ NULL,  0, NULL},
};

struct selection_str_list {
	struct dm_str_list str_list;
	struct dm_regex *regex;
	size_t regex_num_patterns;
	unsigned type; /* either SEL_LIST_LS or SEL_LIST_SUBSET_LS with either SEL_AND or SEL_OR */
};

struct field_selection_value {
	union value_u {
		const char *s;
		uint64_t i;
		time_t t;
		double d;
		struct dm_regex *r;
		struct selection_str_list *l;
	} v;
	struct field_selection_value *next;
};

struct field_selection {
	struct field_properties *fp;
	uint32_t flags;
	struct field_selection_value *value;
};

struct selection_node {
	struct dm_list list;
	uint32_t type;
	union selection_u {
		struct field_selection *item;
		struct dm_list set;
	} selection;
};

struct reserved_value_wrapper {
	const char *matched_name;
	const struct dm_report_reserved_value *reserved;
	const void *value;
};

/*
 * Report data field
 */
struct dm_report_field {
	struct dm_list list;
	struct field_properties *props;

	const char *report_string;	/* Formatted ready for display */
	const void *sort_value;		/* Raw value for sorting */
};

struct row {
	struct dm_list list;
	struct dm_report *rh;
	struct dm_list fields;			  /* Fields in display order */
	struct dm_report_field *(*sort_fields)[]; /* Fields in sort order */
	int selected;
	struct dm_report_field *field_sel_status;
};

/*
 * Implicit report types and fields.
 */
#define SPECIAL_REPORT_TYPE 0x80000000
#define SPECIAL_FIELD_SELECTED_ID "selected"
#define SPECIAL_FIELD_HELP_ID "help"
#define SPECIAL_FIELD_HELP_ALT_ID "?"

static void *_null_returning_fn(void *obj __attribute__((unused)))
{
	return NULL;
}

static int _no_report_fn(struct dm_report *rh __attribute__((unused)),
			 struct dm_pool *mem __attribute__((unused)),
			 struct dm_report_field *field __attribute__((unused)),
			 const void *data __attribute__((unused)),
			 void *private __attribute__((unused)))
{
	return 1;
}

static int _selected_disp(struct dm_report *rh,
			  struct dm_pool *mem __attribute__((unused)),
			  struct dm_report_field *field,
			  const void *data,
			  void *private __attribute__((unused)))
{
	const struct row *row = (const struct row *)data;
	return dm_report_field_int(rh, field, &row->selected);
}

static const struct dm_report_object_type _implicit_special_report_types[] = {
	{ SPECIAL_REPORT_TYPE, "Special", "special_", _null_returning_fn },
	{ 0, "", "", NULL }
};

static const struct dm_report_field_type _implicit_special_report_fields[] = {
	{ SPECIAL_REPORT_TYPE, DM_REPORT_FIELD_TYPE_NUMBER | FLD_CMP_UNCOMPARABLE , 0, 8, SPECIAL_FIELD_HELP_ID, "Help", _no_report_fn, "Show help." },
	{ SPECIAL_REPORT_TYPE, DM_REPORT_FIELD_TYPE_NUMBER | FLD_CMP_UNCOMPARABLE , 0, 8, SPECIAL_FIELD_HELP_ALT_ID, "Help", _no_report_fn, "Show help." },
	{ 0, 0, 0, 0, "", "", 0, 0}
};

static const struct dm_report_field_type _implicit_special_report_fields_with_selection[] = {
	{ SPECIAL_REPORT_TYPE, DM_REPORT_FIELD_TYPE_NUMBER, 0, 8, SPECIAL_FIELD_SELECTED_ID, "Selected", _selected_disp, "Set if item passes selection criteria." },
	{ SPECIAL_REPORT_TYPE, DM_REPORT_FIELD_TYPE_NUMBER | FLD_CMP_UNCOMPARABLE , 0, 8, SPECIAL_FIELD_HELP_ID, "Help", _no_report_fn, "Show help." },
	{ SPECIAL_REPORT_TYPE, DM_REPORT_FIELD_TYPE_NUMBER | FLD_CMP_UNCOMPARABLE , 0, 8, SPECIAL_FIELD_HELP_ALT_ID, "Help", _no_report_fn, "Show help." },
	{ 0, 0, 0, 0, "", "", 0, 0}
};

static const struct dm_report_object_type *_implicit_report_types = _implicit_special_report_types;
static const struct dm_report_field_type *_implicit_report_fields = _implicit_special_report_fields;

static const struct dm_report_object_type *_find_type(struct dm_report *rh,
						      uint32_t report_type)
{
	const struct dm_report_object_type *t;

	for (t = _implicit_report_types; t->data_fn; t++)
		if (t->id == report_type)
			return t;

	for (t = rh->types; t->data_fn; t++)
		if (t->id == report_type)
			return t;

	return NULL;
}

/*
 * Data-munging functions to prepare each data type for display and sorting
 */

int dm_report_field_string(struct dm_report *rh,
			   struct dm_report_field *field, const char *const *data)
{
	char *repstr;

	if (!(repstr = dm_pool_strdup(rh->mem, *data))) {
		log_error("dm_report_field_string: dm_pool_strdup failed");
		return 0;
	}

	field->report_string = repstr;
	field->sort_value = (const void *) field->report_string;

	return 1;
}

int dm_report_field_percent(struct dm_report *rh,
			    struct dm_report_field *field,
			    const dm_percent_t *data)
{
	char *repstr;
	uint64_t *sortval;

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("dm_report_field_percent: dm_pool_alloc failed for sort_value.");
		return 0;
	}

	*sortval = (uint64_t)(*data);

	if (*data == DM_PERCENT_INVALID) {
		dm_report_field_set_value(field, "", sortval);
		return 1;
	}

	if (!(repstr = dm_pool_alloc(rh->mem, 8))) {
		dm_pool_free(rh->mem, sortval);
		log_error("dm_report_field_percent: dm_pool_alloc failed for percent report string.");
		return 0;
	}

	if (dm_snprintf(repstr, 7, "%.2f", dm_percent_to_round_float(*data, 2)) < 0) {
		dm_pool_free(rh->mem, sortval);
		log_error("dm_report_field_percent: percentage too large.");
		return 0;
	}

	dm_report_field_set_value(field, repstr, sortval);
	return 1;
}

struct pos_len {
	unsigned pos;
	size_t len;
};

struct str_pos_len {
	const char *str;
	struct pos_len item;
};

struct str_list_sort_value {
	const char *value;
	struct pos_len *items;
};

static int _str_sort_cmp(const void *a, const void *b)
{
	return strcmp(((const struct str_pos_len *) a)->str, ((const struct str_pos_len *) b)->str);
}

#define FIELD_STRING_LIST_DEFAULT_DELIMITER ","

static int _report_field_string_list(struct dm_report *rh,
				     struct dm_report_field *field,
				     const struct dm_list *data,
				     const char *delimiter,
				     int sort_repstr)
{
	static const char _error_msg_prefix[] = "_report_field_string_list: ";
	unsigned int list_size, i, pos;
	struct str_pos_len *arr = NULL;
	struct dm_str_list *sl;
	size_t delimiter_len, repstr_str_len, repstr_size;
	char *repstr = NULL;
	struct pos_len *repstr_extra;
	struct str_list_sort_value *sortval = NULL;
	int r = 0;

	/*
	 * The 'field->report_string' has 2 parts:
	 *
	 *    - string representing the whole string list
	 *      (terminated by '\0' at its end as usual)
	 *
	 *    - extra info beyond the end of the string representing
	 *      position and length of each list item within the
	 *      field->report_string (array of 'struct pos_len')
	 *
	 * We can use the extra info to unambiguously identify list items,
	 * the delimiter is not enough here as it's not assured it won't appear
	 * in list item itself. We will make use of this extra info in case
	 * we need to apply further formatting to the list in dm_report_output
	 * where the pure field->report_string is not enough for printout.
	 *
	 *
	 * The 'field->sort_value' contains a value of type 'struct
	 * str_list_sort_value' ('sortval'). This one has a pointer to the
	 * 'field->report_string' string ('sortval->value') and info
	 * about position and length of each list item within the string
	 * (array of 'struct pos_len').
	 *
	 *
	 * The 'field->report_string' is either in sorted or unsorted form,
	 * depending on 'sort_repstr' arg.
	 *
	 * The 'field->sort_value.items' is always in sorted form because
	 * we need that for effective sorting and selection.
	 *
	 * If 'field->report_string' is sorted, then field->report_string
	 * and field->sort_value.items share the same array of
	 * 'struct pos_len' (because they're both sorted the same way),
	 * otherwise, each one has its own array.
	 *
	 * The very first item in the array of 'struct pos_len' is always
	 * a pair denoting '[list_size,strlen(field->report_string)]'. The
	 * rest of items denote start and length of each item in the list.
         *
	 *
	 * For example, if we have a list with "abc", "xy", "defgh"
	 * as input and delimiter is ",", we end up with either:
	 *
	 *  A) if we don't want the report string sorted ('sort_repstr == 0'):
	 *
	 *    - field->report_string = repstr
	 *
	 *       repstr      repstr_extra
	 *         |             |
	 *         V             V
	 *         abc,xy,defgh\0{[3,12],[0,3],[4,2],[7,5]}
	 *         |____________||________________________|
	 *             string     array of struct pos_len
	 *                        |____||________________|
	 *                        #items      items
	 *
	 *    - field->sort_value = sortval
	 *
	 *       sortval->value = repstr
	 *       sortval->items = {[3,12],[0,3],[7,5],[4,2]}
	 *                         (that is 'abc,defgh,xy')
	 *
	 *
	 *  B) if we want the report string sorted ('sort_repstr == 1'):
	 *
	 *    - field->report_string = repstr
	 *
	 *       repstr     repstr_extra
	 *         |             |
	 *         V             V
	 *         abc,defgh,xy\0{[3,12],[0,3],[4,5],[10,2]}
	 *         |____________||________________________|
	 *             string     array of struct pos_len
	 *                        |____||________________|
	 *                        #items      items
	 *
	 *     - field->sort_value = sortval
	 *
	 *       sortval->value = repstr
	 *       sortval->items = repstr_extra
	 *                       (that is 'abc,defgh,xy')
	 */

	if (!delimiter)
		delimiter = FIELD_STRING_LIST_DEFAULT_DELIMITER;
	delimiter_len = strlen(delimiter);
	list_size = dm_list_size(data);

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(struct str_list_sort_value)))) {
		log_error("%s failed to allocate sort value structure", _error_msg_prefix);
		goto out;
	}

	/* zero items */
	if (list_size == 0) {
		field->report_string = sortval->value = "";
		sortval->items = NULL;
		field->sort_value = sortval;
		return 1;
	}

	/* one item */
	if (list_size == 1) {
		sl = (struct dm_str_list *) dm_list_first(data);

		repstr_str_len = strlen(sl->str);
		repstr_size = repstr_str_len + 1 + (2 * sizeof(struct pos_len));

		if (!(repstr = dm_pool_alloc(rh->mem, repstr_size))) {
			log_error("%s failed to allocate report string structure", _error_msg_prefix);
			goto out;
		}
		repstr_extra = (struct pos_len *) (repstr + repstr_str_len + 1);

		memcpy(repstr, sl->str, repstr_str_len + 1);
		memcpy(repstr_extra, &((struct pos_len) {.pos = 1, .len = repstr_str_len}), sizeof(struct pos_len));
		memcpy(repstr_extra + 1, &((struct pos_len) {.pos = 0, .len = repstr_str_len}), sizeof(struct pos_len));

		sortval->value = field->report_string = repstr;
		sortval->items = repstr_extra;
		field->sort_value = sortval;
		return 1;
	}

	/* more than one item - allocate temporary array for string list items for further processing */
	if (!(arr = dm_malloc(list_size * sizeof(struct str_pos_len)))) {
		log_error("%s failed to allocate temporary array for processing", _error_msg_prefix);
		goto out;
	}

	i = 0;
	repstr_size = 0;
	dm_list_iterate_items(sl, data) {
		arr[i].str = sl->str;
		repstr_size += (arr[i].item.len = strlen(sl->str));
		i++;
	}

	/*
	 * At this point, repstr_size contains sum of lengths of all string list items.
	 * Now, add these to the repstr_size:
	 *
	 * 	--> sum of character count used by all delimiters: + ((list_size - 1) * delimiter_len)
	 *
	 * 	--> '\0' used at the end of the string list: + 1
	 *
	 * 	--> sum of structures used to keep info about pos and length of each string list item:
	 * 	    [0, <list_size>] [<pos1>,<size1>] [<pos2>,<size2>] ...
	 * 	    That is: + ((list_size + 1) * sizeof(struct pos_len))
	 */
	repstr_size += ((list_size - 1) * delimiter_len);
	repstr_str_len = repstr_size;
	repstr_size += 1 + ((list_size + 1) * sizeof(struct pos_len));

	if (sort_repstr)
		qsort(arr, list_size, sizeof(struct str_pos_len), _str_sort_cmp);

	if (!(repstr = dm_pool_alloc(rh->mem, repstr_size))) {
		log_error("%s failed to allocate report string structure", _error_msg_prefix);
		goto out;
	}
	repstr_extra = (struct pos_len *) (repstr + repstr_str_len + 1);

	memcpy(repstr_extra, &(struct pos_len) {.pos = list_size, .len = repstr_str_len}, sizeof(struct pos_len));
	for (i = 0, pos = 0; i < list_size; i++) {
		arr[i].item.pos = pos;

		memcpy(repstr + pos, arr[i].str, arr[i].item.len);
		memcpy(repstr_extra + i + 1, &arr[i].item, sizeof(struct pos_len));

		pos += arr[i].item.len;
		if (i + 1 < list_size) {
			memcpy(repstr + pos, delimiter, delimiter_len);
			pos += delimiter_len;
		}
	}
	*(repstr + pos) = '\0';

	sortval->value = repstr;
	if (sort_repstr)
		sortval->items = repstr_extra;
	else {
		if (!(sortval->items = dm_pool_alloc(rh->mem, (list_size + 1) * sizeof(struct pos_len)))) {
			log_error("%s failed to allocate array of items inside sort value structure",
				  _error_msg_prefix);
			goto out;
		}

		qsort(arr, list_size, sizeof(struct str_pos_len), _str_sort_cmp);

		sortval->items[0] = (struct pos_len) {.pos = list_size, .len = repstr_str_len};
		for (i = 0; i < list_size; i++)
			sortval->items[i+1] = arr[i].item;
	}

	field->report_string = repstr;
	field->sort_value = sortval;
	r = 1;
out:
	if (!r && sortval)
		dm_pool_free(rh->mem, sortval);
	dm_free(arr);
	return r;
}

int dm_report_field_string_list(struct dm_report *rh,
				struct dm_report_field *field,
				const struct dm_list *data,
				const char *delimiter)
{
	return _report_field_string_list(rh, field, data, delimiter, 1);
}

int dm_report_field_string_list_unsorted(struct dm_report *rh,
					 struct dm_report_field *field,
					 const struct dm_list *data,
					 const char *delimiter)
{
	/*
	 * The raw value is always sorted, just the string reported is unsorted.
	 * Having the raw value always sorted helps when matching selection list
	 * with selection criteria.
	 */
	return _report_field_string_list(rh, field, data, delimiter, 0);
}

int dm_report_field_int(struct dm_report *rh,
			struct dm_report_field *field, const int *data)
{
	const int value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 13))) {
		log_error("dm_report_field_int: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("dm_report_field_int: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 12, "%d", value) < 0) {
		log_error("dm_report_field_int: int too big: %d", value);
		return 0;
	}

	*sortval = (uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

int dm_report_field_uint32(struct dm_report *rh,
			   struct dm_report_field *field, const uint32_t *data)
{
	const uint32_t value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 12))) {
		log_error("dm_report_field_uint32: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("dm_report_field_uint32: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 11, "%u", value) < 0) {
		log_error("dm_report_field_uint32: uint32 too big: %u", value);
		return 0;
	}

	*sortval = (uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

int dm_report_field_int32(struct dm_report *rh,
			  struct dm_report_field *field, const int32_t *data)
{
	const int32_t value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 13))) {
		log_error("dm_report_field_int32: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("dm_report_field_int32: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 12, "%d", value) < 0) {
		log_error("dm_report_field_int32: int32 too big: %d", value);
		return 0;
	}

	*sortval = (uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

int dm_report_field_uint64(struct dm_report *rh,
			   struct dm_report_field *field, const uint64_t *data)
{
	const uint64_t value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 22))) {
		log_error("dm_report_field_uint64: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("dm_report_field_uint64: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 21, FMTu64 , value) < 0) {
		log_error("dm_report_field_uint64: uint64 too big: %" PRIu64, value);
		return 0;
	}

	*sortval = value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

/*
 * Helper functions for custom report functions
 */
void dm_report_field_set_value(struct dm_report_field *field, const void *value, const void *sortvalue)
{
	field->report_string = (const char *) value;
	field->sort_value = sortvalue ? : value;

	if ((field->sort_value == value) &&
	    (field->props->flags & DM_REPORT_FIELD_TYPE_NUMBER))
		log_warn(INTERNAL_ERROR "Using string as sort value for numerical field.");
}

static const char *_get_field_type_name(unsigned field_type)
{
	switch (field_type) {
		case DM_REPORT_FIELD_TYPE_STRING: return "string";
		case DM_REPORT_FIELD_TYPE_NUMBER: return "number";
		case DM_REPORT_FIELD_TYPE_SIZE: return "size";
		case DM_REPORT_FIELD_TYPE_PERCENT: return "percent";
		case DM_REPORT_FIELD_TYPE_TIME: return "time";
		case DM_REPORT_FIELD_TYPE_STRING_LIST: return "string list";
		default: return "unknown";
	}
}

/*
 * show help message
 */
static size_t _get_longest_field_id_len(const struct dm_report_field_type *fields)
{
	uint32_t f;
	size_t l, id_len = 0;

	for (f = 0; fields[f].report_fn; f++)
		if ((l = strlen(fields[f].id)) > id_len)
			id_len = l;

	return id_len;
}

static void _display_fields_more(struct dm_report *rh,
				 const struct dm_report_field_type *fields,
				 size_t id_len, int display_all_fields_item,
				 int display_field_types)
{
	uint32_t f;
	size_t l;
	const struct dm_report_object_type *type;
	const char *desc, *last_desc = "";

	for (f = 0; fields[f].report_fn; f++)
		if ((l = strlen(fields[f].id)) > id_len)
			id_len = l;

	for (type = rh->types; type->data_fn; type++)
		if ((l = strlen(type->prefix) + 3) > id_len)
			id_len = l;

	for (f = 0; fields[f].report_fn; f++) {
		if (!(type = _find_type(rh, fields[f].type))) {
			log_debug(INTERNAL_ERROR "Field type undefined.");
			continue;
		}
		desc = (type->desc) ? : " ";
		if (desc != last_desc) {
			if (*last_desc)
				log_warn(" ");
			log_warn("%s Fields", desc);
			log_warn("%*.*s", (int) strlen(desc) + 7,
				 (int) strlen(desc) + 7,
				 "-------------------------------------------------------------------------------");
			if (display_all_fields_item && type->id != SPECIAL_REPORT_TYPE)
				log_warn("  %sall%-*s - %s", type->prefix,
					 (int) (id_len - 3 - strlen(type->prefix)), "",
					 "All fields in this section.");
		}
		/* FIXME Add line-wrapping at terminal width (or 80 cols) */
		log_warn("  %-*s - %s%s%s%s%s", (int) id_len, fields[f].id, fields[f].desc,
					      display_field_types ? " [" : "",
					      display_field_types ? fields[f].flags & FLD_CMP_UNCOMPARABLE ? "unselectable " : "" : "",
					      display_field_types ? _get_field_type_name(fields[f].flags & DM_REPORT_FIELD_TYPE_MASK) : "",
					      display_field_types ? "]" : "");
		last_desc = desc;
	}
}

/*
 * show help message
 */
static void _display_fields(struct dm_report *rh, int display_all_fields_item,
			    int display_field_types)
{
	size_t tmp, id_len = 0;

	if ((tmp = _get_longest_field_id_len(_implicit_report_fields)) > id_len)
		id_len = tmp;
	if ((tmp = _get_longest_field_id_len(rh->fields)) > id_len)
		id_len = tmp;

	_display_fields_more(rh, rh->fields, id_len, display_all_fields_item,
			     display_field_types);
	log_warn(" ");
	_display_fields_more(rh, _implicit_report_fields, id_len,
			     display_all_fields_item, display_field_types);

}

/*
 * Initialise report handle
 */
static int _copy_field(struct dm_report *rh, struct field_properties *dest,
		       uint32_t field_num, int implicit)
{
	const struct dm_report_field_type *fields = implicit ? _implicit_report_fields
							     : rh->fields;

	dest->field_num = field_num;
	dest->initial_width = fields[field_num].width;
	dest->width = fields[field_num].width; /* adjusted in _do_report_object() */
	dest->flags = fields[field_num].flags & DM_REPORT_FIELD_MASK;
	dest->implicit = implicit;

	/* set object type method */
	dest->type = _find_type(rh, fields[field_num].type);
	if (!dest->type) {
		log_error("dm_report: field not match: %s",
			  fields[field_num].id);
		return 0;
	}

	return 1;
}

static struct field_properties * _add_field(struct dm_report *rh,
					    uint32_t field_num, int implicit,
					    uint32_t flags)
{
	struct field_properties *fp;

	if (!(fp = dm_pool_zalloc(rh->mem, sizeof(*fp)))) {
		log_error("dm_report: struct field_properties allocation "
			  "failed");
		return NULL;
	}

	if (!_copy_field(rh, fp, field_num, implicit)) {
		stack;
		dm_pool_free(rh->mem, fp);
		return NULL;
	}

	fp->flags |= flags;

	/*
	 * Place hidden fields at the front so dm_list_end() will
	 * tell us when we've reached the last visible field.
	 */
	if (fp->flags & FLD_HIDDEN)
		dm_list_add_h(&rh->field_props, &fp->list);
	else
		dm_list_add(&rh->field_props, &fp->list);

	return fp;
}

static int _get_canonical_field_name(const char *field,
				     size_t flen,
				     char *canonical_field,
				     size_t fcanonical_len,
				     int *differs)
{
	size_t i;
	int diff = 0;

	for (i = 0; *field && flen; field++, flen--) {
		if (*field == '_') {
			diff = 1;
			continue;
		}
		if ((i + 1) >= fcanonical_len) {
			canonical_field[0] = '\0';
			log_error("%s: field name too long.", field);
			return 0;
		}
		canonical_field[i++] = *field;
	}

	canonical_field[i] = '\0';
	if (differs)
		*differs = diff;
	return 1;
}

/*
 * Compare canonical_name1 against canonical_name2 or prefix
 * plus canonical_name2. Canonical name is a name where all
 * superfluous characters are removed (underscores for now).
 * Both names are always null-terminated.
 */
static int _is_same_field(const char *canonical_name1, const char *canonical_name2,
			  const char *prefix, size_t prefix_len)
{
	/* Exact match? */
	if (!strcasecmp(canonical_name1, canonical_name2))
		return 1;

	/* Match including prefix? */
	if (!strncasecmp(prefix, canonical_name1, prefix_len) &&
	    !strcasecmp(canonical_name1 + prefix_len, canonical_name2))
		return 1;

	return 0;
}

/*
 * Check for a report type prefix + "all" match.
 */
static void _all_match_combine(const struct dm_report_object_type *types,
			       unsigned unprefixed_all_matched,
			       const char *field, size_t flen,
			       uint32_t *report_types)
{
	char field_canon[DM_REPORT_FIELD_TYPE_ID_LEN];
	const struct dm_report_object_type *t;
	size_t prefix_len;

	if (!_get_canonical_field_name(field, flen, field_canon, sizeof(field_canon), NULL))
		return;
	flen = strlen(field_canon);

	for (t = types; t->data_fn; t++) {
		prefix_len = strlen(t->prefix) - 1;

		if (!strncasecmp(t->prefix, field_canon, prefix_len) &&
		    ((unprefixed_all_matched && (flen == prefix_len)) ||
		     (!strncasecmp(field_canon + prefix_len, "all", 3) &&
		      (flen == prefix_len + 3))))
			*report_types |= t->id;
	}
}

static uint32_t _all_match(struct dm_report *rh, const char *field, size_t flen)
{
	uint32_t report_types = 0;
	unsigned unprefixed_all_matched = 0;

	if (!strncasecmp(field, "all", 3) && flen == 3) {
		/* If there's no report prefix, match all report types */
		if (!(flen = strlen(rh->field_prefix)))
			return rh->report_types ? : REPORT_TYPES_ALL;

		/* otherwise include all fields beginning with the report prefix. */
		unprefixed_all_matched = 1;
		field = rh->field_prefix;
		report_types = rh->report_types;
	}

	/* Combine all report types that have a matching prefix. */
	_all_match_combine(rh->types, unprefixed_all_matched, field, flen, &report_types);

	return report_types;
}

/*
 * Add all fields with a matching type.
 */
static int _add_all_fields(struct dm_report *rh, uint32_t type)
{
	uint32_t f;

	for (f = 0; rh->fields[f].report_fn; f++)
		if ((rh->fields[f].type & type) && !_add_field(rh, f, 0, 0))
			return 0;

	return 1;
}

static int _get_field(struct dm_report *rh, const char *field, size_t flen,
		      uint32_t *f_ret, int *implicit)
{
	char field_canon[DM_REPORT_FIELD_TYPE_ID_LEN];
	uint32_t f;
	size_t prefix_len;

	if (!flen)
		return 0;

	if (!_get_canonical_field_name(field, flen, field_canon, sizeof(field_canon), NULL))
		return_0;

	prefix_len = strlen(rh->field_prefix) - 1;
	for (f = 0; _implicit_report_fields[f].report_fn; f++) {
		if (_is_same_field(_implicit_report_fields[f].id, field_canon, rh->field_prefix, prefix_len)) {
			*f_ret = f;
			*implicit = 1;
			return 1;
		}
	}

	for (f = 0; rh->fields[f].report_fn; f++) {
		if (_is_same_field(rh->canonical_field_ids[f], field_canon, rh->field_prefix, prefix_len)) {
			*f_ret = f;
			*implicit = 0;
			return 1;
		}
	}

	return 0;
}

static int _field_match(struct dm_report *rh, const char *field, size_t flen,
			unsigned report_type_only)
{
	uint32_t f, type;
	int implicit;

	if (!flen)
		return 0;

	if ((_get_field(rh, field, flen, &f, &implicit))) {
		if (report_type_only) {
			rh->report_types |= implicit ? _implicit_report_fields[f].type
						     : rh->fields[f].type;
			return 1;
		}

		return _add_field(rh, f, implicit, 0) ? 1 : 0;
	}

	if ((type = _all_match(rh, field, flen))) {
		if (report_type_only) {
			rh->report_types |= type;
			return 1;
		}

		return  _add_all_fields(rh, type);
	}

	return 0;
}

static int _add_sort_key(struct dm_report *rh, uint32_t field_num, int implicit,
			 uint32_t flags, unsigned report_type_only)
{
	struct field_properties *fp, *found = NULL;
	const struct dm_report_field_type *fields = implicit ? _implicit_report_fields
							     : rh->fields;

	dm_list_iterate_items(fp, &rh->field_props) {
		if ((fp->implicit == implicit) && (fp->field_num == field_num)) {
			found = fp;
			break;
		}
	}

	if (!found) {
		if (report_type_only)
			rh->report_types |= fields[field_num].type;
		else if (!(found = _add_field(rh, field_num, implicit, FLD_HIDDEN)))
			return_0;
	}

	if (report_type_only)
		return 1;

	if (found->flags & FLD_SORT_KEY) {
		log_warn("dm_report: Ignoring duplicate sort field: %s.",
			 fields[field_num].id);
		return 1;
	}

	found->flags |= FLD_SORT_KEY;
	found->sort_posn = rh->keys_count++;
	found->flags |= flags;

	return 1;
}

static int _key_match(struct dm_report *rh, const char *key, size_t len,
		      unsigned report_type_only)
{
	int implicit;
	uint32_t f;
	uint32_t flags;

	if (!len)
		return 0;

	if (*key == '+') {
		key++;
		len--;
		flags = FLD_ASCENDING;
	} else if (*key == '-') {
		key++;
		len--;
		flags = FLD_DESCENDING;
	} else
		flags = FLD_ASCENDING;

	if (!len) {
		log_error("dm_report: Missing sort field name");
		return 0;
	}

	if (_get_field(rh, key, len, &f, &implicit))
		return _add_sort_key(rh, f, implicit, flags, report_type_only);

	return 0;
}

static int _parse_fields(struct dm_report *rh, const char *format,
			 unsigned report_type_only)
{
	const char *ws;		/* Word start */
	const char *we = format;	/* Word end */

	while (*we) {
		/* Allow consecutive commas */
		while (*we && *we == ',')
			we++;

		/* start of the field name */
		ws = we;
		while (*we && *we != ',')
			we++;

		if (!_field_match(rh, ws, (size_t) (we - ws), report_type_only)) {
			_display_fields(rh, 1, 0);
			log_warn(" ");
			log_error("Unrecognised field: %.*s", (int) (we - ws), ws);
			return 0;
		}
	}

	return 1;
}

static int _parse_keys(struct dm_report *rh, const char *keys,
		       unsigned report_type_only)
{
	const char *ws;		/* Word start */
	const char *we = keys;	/* Word end */

	if (!keys)
		return 1;

	while (*we) {
		/* Allow consecutive commas */
		while (*we && *we == ',')
			we++;
		ws = we;
		while (*we && *we != ',')
			we++;
		if (!_key_match(rh, ws, (size_t) (we - ws), report_type_only)) {
			_display_fields(rh, 1, 0);
			log_warn(" ");
			log_error("dm_report: Unrecognised field: %.*s", (int) (we - ws), ws);
			return 0;
		}
	}

	return 1;
}

static int _contains_reserved_report_type(const struct dm_report_object_type *types)
{
	const struct dm_report_object_type *type, *implicit_type;

	for (implicit_type = _implicit_report_types; implicit_type->data_fn; implicit_type++) {
		for (type = types; type->data_fn; type++) {
			if (implicit_type->id & type->id) {
				log_error(INTERNAL_ERROR "dm_report_init: definition of report "
					  "types given contains reserved identifier");
				return 1;
			}
		}
	}

	return 0;
}

static void _dm_report_init_update_types(struct dm_report *rh, uint32_t *report_types)
{
	const struct dm_report_object_type *type;

	if (!report_types)
		return;

	*report_types = rh->report_types;
	/*
	 * Do not include implicit types as these are not understood by
	 * dm_report_init caller - the caller doesn't know how to check
	 * these types anyway.
	 */
	for (type = _implicit_report_types; type->data_fn; type++)
		*report_types &= ~type->id;
}

static int _help_requested(struct dm_report *rh)
{
	struct field_properties *fp;

	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->implicit &&
		    (!strcmp(_implicit_report_fields[fp->field_num].id, SPECIAL_FIELD_HELP_ID) ||
		     !strcmp(_implicit_report_fields[fp->field_num].id, SPECIAL_FIELD_HELP_ALT_ID)))
			return 1;
	}

	return 0;
}

static int _canonicalize_field_ids(struct dm_report *rh)
{
	size_t registered_field_count = 0, i;
	char canonical_field[DM_REPORT_FIELD_TYPE_ID_LEN];
	char *canonical_field_dup;
	int differs;

	while (*rh->fields[registered_field_count].id)
		registered_field_count++;

	if (!(rh->canonical_field_ids = dm_pool_alloc(rh->mem, registered_field_count * sizeof(const char *)))) {
		log_error("_canonicalize_field_ids: dm_pool_alloc failed");
		return 0;
	}

	for (i = 0; i < registered_field_count; i++) {
		if (!_get_canonical_field_name(rh->fields[i].id, strlen(rh->fields[i].id),
					       canonical_field, sizeof(canonical_field), &differs))
			return_0;

		if (differs) {
			if (!(canonical_field_dup = dm_pool_strdup(rh->mem, canonical_field))) {
				log_error("_canonicalize_field_dup: dm_pool_alloc failed.");
				return 0;
			}
			rh->canonical_field_ids[i] = canonical_field_dup;
		} else
			rh->canonical_field_ids[i] = rh->fields[i].id;
	}

	return 1;
}

struct dm_report *dm_report_init(uint32_t *report_types,
				 const struct dm_report_object_type *types,
				 const struct dm_report_field_type *fields,
				 const char *output_fields,
				 const char *output_separator,
				 uint32_t output_flags,
				 const char *sort_keys,
				 void *private_data)
{
	struct dm_report *rh;
	const struct dm_report_object_type *type;

	if (_contains_reserved_report_type(types))
		return_NULL;

	if (!(rh = dm_zalloc(sizeof(*rh)))) {
		log_error("dm_report_init: dm_malloc failed");
		return NULL;
	}

	/*
	 * rh->report_types is updated in _parse_fields() and _parse_keys()
	 * to contain all types corresponding to the fields specified by
	 * fields or keys.
	 */
	if (report_types)
		rh->report_types = *report_types;

	rh->separator = output_separator;
	rh->fields = fields;
	rh->types = types;
	rh->private = private_data;

	rh->flags |= output_flags & DM_REPORT_OUTPUT_MASK;

	/* With columns_as_rows we must buffer and not align. */
	if (output_flags & DM_REPORT_OUTPUT_COLUMNS_AS_ROWS) {
		if (!(output_flags & DM_REPORT_OUTPUT_BUFFERED))
			rh->flags |= DM_REPORT_OUTPUT_BUFFERED;
		if (output_flags & DM_REPORT_OUTPUT_ALIGNED)
			rh->flags &= ~DM_REPORT_OUTPUT_ALIGNED;
	}

	if (output_flags & DM_REPORT_OUTPUT_BUFFERED)
		rh->flags |= RH_SORT_REQUIRED;

	rh->flags |= RH_FIELD_CALC_NEEDED;

	dm_list_init(&rh->field_props);
	dm_list_init(&rh->rows);

	if ((type = _find_type(rh, rh->report_types)) && type->prefix)
		rh->field_prefix = type->prefix;
	else
		rh->field_prefix = "";

	if (!(rh->mem = dm_pool_create("report", 10 * 1024))) {
		log_error("dm_report_init: allocation of memory pool failed");
		dm_free(rh);
		return NULL;
	}

	if (!_canonicalize_field_ids(rh)) {
		dm_report_free(rh);
		return NULL;
	}

	/*
	 * To keep the code needed to add the "all" field to a minimum, we parse
	 * the field lists twice.  The first time we only update the report type.
	 * FIXME Use one pass instead and expand the "all" field afterwards.
	 */
	if (!_parse_fields(rh, output_fields, 1) ||
	    !_parse_keys(rh, sort_keys, 1)) {
		dm_report_free(rh);
		return NULL;
	}

	/* Generate list of fields for output based on format string & flags */
	if (!_parse_fields(rh, output_fields, 0) ||
	    !_parse_keys(rh, sort_keys, 0)) {
		dm_report_free(rh);
		return NULL;
	}

	/*
	 * Return updated types value for further compatibility check by caller.
	 */
	_dm_report_init_update_types(rh, report_types);

	if (_help_requested(rh)) {
		_display_fields(rh, 1, 0);
		log_warn(" ");
		rh->flags |= RH_ALREADY_REPORTED;
	}

	return rh;
}

void dm_report_free(struct dm_report *rh)
{
	if (rh->selection) {
		dm_pool_destroy(rh->selection->mem);
		if (rh->selection->regex_mem)
			dm_pool_destroy(rh->selection->regex_mem);
	}
	if (rh->value_cache)
		dm_hash_destroy(rh->value_cache);
	dm_pool_destroy(rh->mem);
	dm_free(rh);
}

static char *_toupperstr(char *str)
{
	char *u = str;

	do
		*u = toupper(*u);
	while (*u++);

	return str;
}

int dm_report_set_output_field_name_prefix(struct dm_report *rh, const char *output_field_name_prefix)
{
	char *prefix;

	if (!(prefix = dm_pool_strdup(rh->mem, output_field_name_prefix))) {
		log_error("dm_report_set_output_field_name_prefix: dm_pool_strdup failed");
		return 0;
	}

	rh->output_field_name_prefix = _toupperstr(prefix);
	
	return 1;
}

/*
 * Create a row of data for an object
 */
static void *_report_get_field_data(struct dm_report *rh,
				    struct field_properties *fp, void *object)
{
	const struct dm_report_field_type *fields = fp->implicit ? _implicit_report_fields
								 : rh->fields;
	char *ret;

	if (!object) {
		log_error(INTERNAL_ERROR "_report_get_field_data: missing object.");
		return NULL;
	}

	if (!(ret = fp->type->data_fn(object)))
		return NULL;

	return (void *)(ret + fields[fp->field_num].offset);
}

static void *_report_get_implicit_field_data(struct dm_report *rh __attribute__((unused)),
					     struct field_properties *fp, struct row *row)
{
	if (!strcmp(_implicit_report_fields[fp->field_num].id, SPECIAL_FIELD_SELECTED_ID))
		return row;

	return NULL;
}

static int _dbl_equal(double d1, double d2)
{
	return fabs(d1 - d2) < DBL_EPSILON;
}

static int _dbl_greater(double d1, double d2)
{
	return (d1 > d2) && !_dbl_equal(d1, d2);
}

static int _dbl_less(double d1, double d2)
{
	return (d1 < d2) && !_dbl_equal(d1, d2);
}

static int _dbl_greater_or_equal(double d1, double d2)
{
	return _dbl_greater(d1, d2) || _dbl_equal(d1, d2);
}

static int _dbl_less_or_equal(double d1, double d2)
{
	return _dbl_less(d1, d2) || _dbl_equal(d1, d2);
}

#define _uint64 *(const uint64_t *)
#define _uint64arr(var,index) ((const uint64_t *)(var))[(index)]
#define _str (const char *)
#define _dbl *(const double *)
#define _dblarr(var,index) ((const double *)(var))[(index)]

static int _do_check_value_is_strictly_reserved(unsigned type, const void *res_val, int res_range,
						const void *val, struct field_selection *fs)
{
	int sel_range = fs ? fs->value->next != NULL : 0;

	switch (type & DM_REPORT_FIELD_TYPE_MASK) {
		case DM_REPORT_FIELD_TYPE_NUMBER:
			if (res_range && sel_range) {
				/* both reserved value and selection value are ranges */
				if (((_uint64 val >= _uint64arr(res_val,0)) && (_uint64 val <= _uint64arr(res_val,1))) ||
				    (fs && ((fs->value->v.i == _uint64arr(res_val,0)) && (fs->value->next->v.i == _uint64arr(res_val,1)))))
					return 1;
			} else if (res_range) {
				/* only reserved value is a range */
				if (((_uint64 val >= _uint64arr(res_val,0)) && (_uint64 val <= _uint64arr(res_val,1))) ||
				    (fs && ((fs->value->v.i >= _uint64arr(res_val,0)) && (fs->value->v.i <= _uint64arr(res_val,1)))))
					return 1;
			} else if (sel_range) {
				/* only selection value is a range */
				if (((_uint64 val >= _uint64 res_val) && (_uint64 val <= _uint64 res_val)) ||
				    (fs && ((fs->value->v.i >= _uint64 res_val) && (fs->value->next->v.i <= _uint64 res_val))))
					return 1;
			} else {
				/* neither selection value nor reserved value is a range */
				if ((_uint64 val == _uint64 res_val) ||
				    (fs && (fs->value->v.i == _uint64 res_val)))
					return 1;
			}
			break;

		case DM_REPORT_FIELD_TYPE_STRING:
			/* there are no ranges for string type yet */
			if ((!strcmp(_str val, _str res_val)) ||
			    (fs && (!strcmp(fs->value->v.s, _str res_val))))
				return 1;
			break;

		case DM_REPORT_FIELD_TYPE_SIZE:
			if (res_range && sel_range) {
				/* both reserved value and selection value are ranges */
				if ((_dbl_greater_or_equal(_dbl val, _dblarr(res_val,0)) && _dbl_less_or_equal(_dbl val, _dblarr(res_val,1))) ||
				    (fs && (_dbl_equal(fs->value->v.d, _dblarr(res_val,0)) && (_dbl_equal(fs->value->next->v.d, _dblarr(res_val,1))))))
					return 1;
			} else if (res_range) {
				/* only reserved value is a range */
				if ((_dbl_greater_or_equal(_dbl val, _dblarr(res_val,0)) && _dbl_less_or_equal(_dbl val, _dblarr(res_val,1))) ||
				    (fs && (_dbl_greater_or_equal(fs->value->v.d, _dblarr(res_val,0)) && _dbl_less_or_equal(fs->value->v.d, _dblarr(res_val,1)))))
					return 1;
			} else if (sel_range) {
				/* only selection value is a range */
				if ((_dbl_greater_or_equal(_dbl val, _dbl res_val) && (_dbl_less_or_equal(_dbl val, _dbl res_val))) ||
				    (fs && (_dbl_greater_or_equal(fs->value->v.d, _dbl res_val) && _dbl_less_or_equal(fs->value->next->v.d, _dbl res_val))))
					return 1;
			} else {
				/* neither selection value nor reserved value is a range */
				if ((_dbl_equal(_dbl val, _dbl res_val)) ||
				    (fs && (_dbl_equal(fs->value->v.d, _dbl res_val))))
					return 1;
			}
			break;

		case DM_REPORT_FIELD_TYPE_STRING_LIST:
			/* FIXME Add comparison for string list */
			break;
		case DM_REPORT_FIELD_TYPE_TIME:
			/* FIXME Add comparison for time */
			break;
	}

	return 0;
}

/*
 * Used to check whether a value of certain type used in selection is reserved.
 */
static int _check_value_is_strictly_reserved(struct dm_report *rh, uint32_t field_num, unsigned type,
					     const void *val, struct field_selection *fs)
{
	const struct dm_report_reserved_value *iter = rh->reserved_values;
	const struct dm_report_field_reserved_value *frv;
	int res_range;

	if (!iter)
		return 0;

	while (iter->value) {
		/* Only check strict reserved values, not the weaker form ("named" reserved value). */
		if (!(iter->type & DM_REPORT_FIELD_RESERVED_VALUE_NAMED)) {
			res_range = iter->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE;
			if ((iter->type & DM_REPORT_FIELD_TYPE_MASK) == DM_REPORT_FIELD_TYPE_NONE) {
				frv = (const struct dm_report_field_reserved_value *) iter->value;
				if (frv->field_num == field_num && _do_check_value_is_strictly_reserved(type, frv->value, res_range, val, fs))
					return 1;
			} else if (iter->type & type && _do_check_value_is_strictly_reserved(type, iter->value, res_range, val, fs))
				return 1;
		}
		iter++;
	}

	return 0;
}

static int _cmp_field_int(struct dm_report *rh, uint32_t field_num, const char *field_id,
			  uint64_t val, struct field_selection *fs)
{
	int range = fs->value->next != NULL;
	const uint64_t sel1 = fs->value->v.i;
	const uint64_t sel2 = range ? fs->value->next->v.i : 0;

	switch(fs->flags & FLD_CMP_MASK) {
		case FLD_CMP_EQUAL:
			return range ? ((val >= sel1) && (val <= sel2)) : val == sel1;

		case FLD_CMP_NOT|FLD_CMP_EQUAL:
			return range ? !((val >= sel1) && (val <= sel2)) : val != sel1;

		case FLD_CMP_NUMBER|FLD_CMP_GT:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_NUMBER, &val, fs))
				return 0;
			return range ? val > sel2 : val > sel1;

		case FLD_CMP_NUMBER|FLD_CMP_GT|FLD_CMP_EQUAL:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_NUMBER, &val, fs))
				return 0;
			return val >= sel1;

		case FLD_CMP_NUMBER|FLD_CMP_LT:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_NUMBER, &val, fs))
				return 0;
			return val < sel1;

		case FLD_CMP_NUMBER|FLD_CMP_LT|FLD_CMP_EQUAL:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_NUMBER, &val, fs))
				return 0;
			return range ? val <= sel2 : val <= sel1;

		default:
			log_error(INTERNAL_ERROR "_cmp_field_int: unsupported number "
				  "comparison type for field %s", field_id);
	}

	return 0;
}

static int _cmp_field_double(struct dm_report *rh, uint32_t field_num, const char *field_id,
			     double val, struct field_selection *fs)
{
	int range = fs->value->next != NULL;
	double sel1 = fs->value->v.d;
	double sel2 = range ? fs->value->next->v.d : 0;

	switch(fs->flags & FLD_CMP_MASK) {
		case FLD_CMP_EQUAL:
			return range ? (_dbl_greater_or_equal(val, sel1) && _dbl_less_or_equal(val, sel2))
				     : _dbl_equal(val, sel1);

		case FLD_CMP_NOT|FLD_CMP_EQUAL:
			return range ? !(_dbl_greater_or_equal(val, sel1) && _dbl_less_or_equal(val, sel2))
				     : !_dbl_equal(val, sel1);

		case FLD_CMP_NUMBER|FLD_CMP_GT:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_SIZE, &val, fs))
				return 0;
			return range ? _dbl_greater(val, sel2)
				     : _dbl_greater(val, sel1);

		case FLD_CMP_NUMBER|FLD_CMP_GT|FLD_CMP_EQUAL:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_SIZE, &val, fs))
				return 0;
			return _dbl_greater_or_equal(val, sel1);

		case FLD_CMP_NUMBER|FLD_CMP_LT:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_SIZE, &val, fs))
				return 0;
			return _dbl_less(val, sel1);

		case FLD_CMP_NUMBER|FLD_CMP_LT|FLD_CMP_EQUAL:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_SIZE, &val, fs))
				return 0;
			return range ? _dbl_less_or_equal(val, sel2) : _dbl_less_or_equal(val, sel1); 

		default:
			log_error(INTERNAL_ERROR "_cmp_field_double: unsupported number "
				  "comparison type for selection field %s", field_id);
	}

	return 0;
}

static int _cmp_field_string(struct dm_report *rh __attribute__((unused)),
			     uint32_t field_num, const char *field_id,
			     const char *val, struct field_selection *fs)
{
	const char *sel = fs->value->v.s;

	switch (fs->flags & FLD_CMP_MASK) {
		case FLD_CMP_EQUAL:
			return !strcmp(val, sel);
		case FLD_CMP_NOT|FLD_CMP_EQUAL:
			return strcmp(val, sel);
		default:
			log_error(INTERNAL_ERROR "_cmp_field_string: unsupported string "
				  "comparison type for selection field %s", field_id);
	}

	return 0;
}

static int _cmp_field_time(struct dm_report *rh,
			   uint32_t field_num, const char *field_id,
			   time_t val, struct field_selection *fs)
{
	int range = fs->value->next != NULL;
	time_t sel1 = fs->value->v.t;
	time_t sel2 = range ? fs->value->next->v.t : 0;

	switch(fs->flags & FLD_CMP_MASK) {
		case FLD_CMP_EQUAL:
			return range ? ((val >= sel1) && (val <= sel2)) : val == sel1;
		case FLD_CMP_NOT|FLD_CMP_EQUAL:
			return range ? ((val >= sel1) && (val <= sel2)) : val != sel1;
		case FLD_CMP_TIME|FLD_CMP_GT:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_TIME, &val, fs))
				return 0;
			return range ? val > sel2 : val > sel1;
		case FLD_CMP_TIME|FLD_CMP_GT|FLD_CMP_EQUAL:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_TIME, &val, fs))
				return 0;
			return val >= sel1;
		case FLD_CMP_TIME|FLD_CMP_LT:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_TIME, &val, fs))
				return 0;
			return val < sel1;
		case FLD_CMP_TIME|FLD_CMP_LT|FLD_CMP_EQUAL:
			if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_TIME, &val, fs))
				return 0;
			return range ? val <= sel2 : val <= sel1;
		default:
			log_error(INTERNAL_ERROR "_cmp_field_time: unsupported time "
				  "comparison type for field %s", field_id);
	}

	return 0;
}

static int _str_list_item_match_regex(const struct str_list_sort_value *val, unsigned int i, struct dm_regex *regex)
{
	struct pos_len *item = val->items + i;
	char *s = (char *) (val->value + item->pos);
	char c = s[item->len];
	int r;

	/*
	 * The val->items contains the whole string list in the form of a single string,
	 * where each item is delimited by a delimiter.
	 *
	 * The item->pos + item->len pair then points to the exact item within the val->items.
	 *
	 * The dm_regex_match accepts a string, not the pos + len pair, so we need to adapt here:
	 * replace the delimiter with '\0' temporarily so the item is a proper string.
	 */
	s[item->len] = '\0';
	r = dm_regex_match(regex, s);
	s[item->len] = c;

	return r;
}

static size_t _bitset_count_set(dm_bitset_t bs)
{
	size_t i, size = bs[0]/DM_BITS_PER_INT + 1;
	size_t count = 0;

	for (i = 1; i <= size; i++)
		count += hweight32(bs[i]);

	return count;
}

/* Matches if all items from selection string list match list value strictly 1:1. */
static int _cmp_field_string_list_strict_regex_all(const struct dm_report *rh,
						   const struct str_list_sort_value *val,
						   const struct selection_str_list *sel)
{
	unsigned int i;
	dm_bitset_t bs;
	int r;

	if (!val->items)
		return (sel->regex_num_patterns == 1) && dm_regex_match(sel->regex, "") >= 0;

	if (!(bs = dm_bitset_create(rh->selection->mem, sel->regex_num_patterns))) {
		log_error("Failed to create bitset for regex match counter.");
		return 0;
	}

	for (i = 1; i <= val->items[0].pos; i++) {
		if ((r = _str_list_item_match_regex(val, i, sel->regex)) < 0) {
			r = 0;
			goto out;
		}
		dm_bit_set(bs, r);
	}

	r = _bitset_count_set(bs) == sel->regex_num_patterns;
out:
	dm_pool_free(rh->selection->mem, bs);
	return r;
}

/* Matches if all items from selection string list match list value strictly 1:1. */
static int _cmp_field_string_list_strict_all(const struct dm_report *rh,
					     const struct str_list_sort_value *val,
					     const struct selection_str_list *sel)
{
	unsigned int sel_list_size = dm_list_size(&sel->str_list.list);
	struct dm_str_list *sel_item;
	unsigned int i = 1;

	if (!val->items) {
		if (sel_list_size == 1) {
			/* match blank string list with selection defined as blank string only */
			sel_item = dm_list_item(dm_list_first(&sel->str_list.list), struct dm_str_list);
			return !strcmp(sel_item->str, "");
		}
		return 0;
	}

	/* if item count differs, it's clear the lists do not match */
	if (val->items[0].pos != sel_list_size)
		return 0;

	/* both lists are sorted so they either match 1:1 or not */
	dm_list_iterate_items(sel_item, &sel->str_list.list) {
		if ((strlen(sel_item->str) != val->items[i].len) ||
		    strncmp(sel_item->str, val->value + val->items[i].pos, val->items[i].len))
			return 0;
		i++;
	}

	return 1;
}

/* Matches if all items from selection string list match a subset of list value. */
static int _cmp_field_string_list_subset_regex_all(const struct dm_report *rh,
						   const struct str_list_sort_value *val,
						   const struct selection_str_list *sel)
{
	dm_bitset_t bs;
	unsigned int i;
	int r;

	if (!val->items)
		return (sel->regex_num_patterns == 1) && dm_regex_match(sel->regex, "") >= 0;

	if (!(bs = dm_bitset_create(rh->selection->mem, sel->regex_num_patterns))) {
		log_error("Failed to create bitset for regex match counter.");
		return 0;
	}

	for (i = 1; i <= val->items[0].pos; i++) {
		if ((r = _str_list_item_match_regex(val, i, sel->regex)) < 0)
			continue;
		dm_bit_set(bs, r);
	}

	r = _bitset_count_set(bs) == sel->regex_num_patterns;
	dm_pool_free(rh->selection->mem, bs);
	return r;
}

/* Matches if all items from selection string list match a subset of list value. */
static int _cmp_field_string_list_subset_all(const struct dm_report *rh __attribute__((unused)),
					     const struct str_list_sort_value *val,
					     const struct selection_str_list *sel)
{
	unsigned int sel_list_size = dm_list_size(&sel->str_list.list);
	struct dm_str_list *sel_item;
	unsigned int i, last_found = 1;
	int r = 0;

	if (!val->items) {
		if (sel_list_size == 1) {
			/* match blank string list with selection defined as blank string only */
			sel_item = dm_list_item(dm_list_first(&sel->str_list.list), struct dm_str_list);
			return !strcmp(sel_item->str, "");
		}
		return 0;
	}

	/* check selection is a subset of the value */
	dm_list_iterate_items(sel_item, &sel->str_list.list) {
		r = 0;
		for (i = last_found; i <= val->items[0].pos; i++) {
			if ((strlen(sel_item->str) == val->items[i].len) &&
			    !strncmp(sel_item->str, val->value + val->items[i].pos, val->items[i].len)) {
				last_found = i;
				r = 1;
			}
		}
		if (!r)
			break;
	}

	return r;
}

/* Matches if any item from selection string list matches list value. */
static int _cmp_field_string_list_subset_regex_any(const struct dm_report *rh __attribute__((unused)),
						   const struct str_list_sort_value *val,
						   const struct selection_str_list *sel)
{
	unsigned int i;

	if (!val->items)
		return dm_regex_match(sel->regex, "") >= 0;

	for (i = 1; i <= val->items[0].pos; i++) {
		if (_str_list_item_match_regex(val, i, sel->regex) >= 0)
			return 1;
	}
	return 0;
}

/* Matches if any item from selection string list matches list value. */
static int _cmp_field_string_list_subset_any(const struct dm_report *rh __attribute__((unused)),
					     const struct str_list_sort_value *val,
					     const struct selection_str_list *sel)
{
	struct dm_str_list *sel_item;
	unsigned int i;

	/* match blank string list with selection that contains blank string */
	if (!val->items) {
		dm_list_iterate_items(sel_item, &sel->str_list.list) {
			if (!strcmp(sel_item->str, ""))
				return 1;
		}
		return 0;
	}

	dm_list_iterate_items(sel_item, &sel->str_list.list) {
		/*
		 * TODO: Optimize this so we don't need to compare the whole lists' content.
		 *       Make use of the fact that the lists are sorted!
		 */
		for (i = 1; i <= val->items[0].pos; i++) {
			if ((strlen(sel_item->str) == val->items[i].len) &&
			    !strncmp(sel_item->str, val->value + val->items[i].pos, val->items[i].len))
				return 1;
		}
	}

	return 0;
}

/* Matches if all items from list value can be matched by any item from selection list. */
static int _cmp_field_string_list_strict_regex_any(const struct dm_report *rh __attribute__((unused)),
						   const struct str_list_sort_value *val,
						   const struct selection_str_list *sel)
{
	unsigned int i;

	if (!val->items)
		return dm_regex_match(sel->regex, "") >= 0;

	for (i = 1; i <= val->items[0].pos; i++) {
		if (_str_list_item_match_regex(val, i, sel->regex) < 0)
			return 0;
	}

	return 1;
}

/* Matches if all items from list value can be matched by any item from selection list. */
static int _cmp_field_string_list_strict_any(const struct dm_report *rh __attribute__((unused)),
					     const struct str_list_sort_value *val,
					     const struct selection_str_list *sel)
{
	struct dm_str_list *sel_item;
	unsigned int i;
	int match;

	/* match blank string list with selection that contains blank string */
	if (!val->items) {
		dm_list_iterate_items(sel_item, &sel->str_list.list) {
			if (!strcmp(sel_item->str, ""))
				return 1;
		}
		return 0;
	}

	for (i = 1; i <= val->items[0].pos; i++) {
		match = 0;
		dm_list_iterate_items(sel_item, &sel->str_list.list) {
			if ((strlen(sel_item->str) == val->items[i].len) &&
			    !strncmp(sel_item->str, val->value + val->items[i].pos, val->items[i].len)) {
				match = 1;
				break;
			}
		}
		if (!match)
			return 0;
	}

	return 1;
}

static int _cmp_field_string_list(struct dm_report *rh,
				  uint32_t field_num, const char *field_id,
				  const struct str_list_sort_value *val,
				  struct field_selection *fs)
{
	const struct selection_str_list *sel = fs->value->v.l;
	int subset, r;

	switch (sel->type & SEL_LIST_MASK) {
		case SEL_LIST_LS:
			subset = 0;
			break;
		case SEL_LIST_SUBSET_LS:
			subset = 1;
			break;
		default:
			log_error(INTERNAL_ERROR "_cmp_field_string_list: unknown list type");
			return 0;
	}

	switch (sel->type & SEL_MASK) {
		case SEL_AND:
			r = subset ? sel->regex ? _cmp_field_string_list_subset_regex_all(rh, val, sel)
						: _cmp_field_string_list_subset_all(rh, val, sel)
				   : sel->regex ? _cmp_field_string_list_strict_regex_all(rh, val, sel)
				   		: _cmp_field_string_list_strict_all(rh, val, sel);
			break;
		case SEL_OR:
			r = subset ? sel->regex ? _cmp_field_string_list_subset_regex_any(rh, val, sel)
						: _cmp_field_string_list_subset_any(rh, val, sel)
				   : sel->regex ? _cmp_field_string_list_strict_regex_any(rh, val, sel)
				   		: _cmp_field_string_list_strict_any(rh, val, sel);
			break;
		default:
			log_error(INTERNAL_ERROR "_cmp_field_string_list: unsupported string "
				  "list type found, expecting either AND or OR list for "
				  "selection field %s", field_id);
			return 0;
	}

	return fs->flags & FLD_CMP_NOT ? !r : r;
}

static int _cmp_field_regex(const char *s, struct field_selection *fs)
{
	int match = dm_regex_match(fs->value->v.r, s) >= 0;
	return fs->flags & FLD_CMP_NOT ? !match : match;
}

static int _compare_selection_field(struct dm_report *rh,
				    struct dm_report_field *f,
				    struct field_selection *fs)
{
	const struct dm_report_field_type *fields = f->props->implicit ? _implicit_report_fields
								       : rh->fields;
	const char *field_id = fields[f->props->field_num].id;
	int r = 0;

	if (!f->sort_value) {
		log_error("_compare_selection_field: field without value :%d",
			  f->props->field_num);
		return 0;
	}

	if (fs->flags & FLD_CMP_REGEX)
		switch (f->props->flags & DM_REPORT_FIELD_TYPE_MASK) {
			case DM_REPORT_FIELD_TYPE_STRING:
					r = _cmp_field_regex((const char *) f->sort_value, fs);
					break;
			case DM_REPORT_FIELD_TYPE_STRING_LIST:
					r = _cmp_field_string_list(rh, f->props->field_num, field_id, (const struct str_list_sort_value *) f->sort_value, fs);
					break;
			default:
				log_error(INTERNAL_ERROR "_compare_selection_field: regex: incorrect type %" PRIu32 " for field %s",
					  f->props->flags & DM_REPORT_FIELD_TYPE_MASK, field_id);
		}
	else {
		switch(f->props->flags & DM_REPORT_FIELD_TYPE_MASK) {
			case DM_REPORT_FIELD_TYPE_PERCENT:
				/*
				 * Check against real percent values only.
				 * That means DM_PERCENT_0 <= percent <= DM_PERCENT_100.
				 */
				if (*(const uint64_t *) f->sort_value > DM_PERCENT_100)
					return 0;
				/* fall through */
			case DM_REPORT_FIELD_TYPE_NUMBER:
				r = _cmp_field_int(rh, f->props->field_num, field_id, *(const uint64_t *) f->sort_value, fs);
				break;
			case DM_REPORT_FIELD_TYPE_SIZE:
				r = _cmp_field_double(rh, f->props->field_num, field_id, *(const double *) f->sort_value, fs);
				break;
			case DM_REPORT_FIELD_TYPE_STRING:
				r = _cmp_field_string(rh, f->props->field_num, field_id, (const char *) f->sort_value, fs);
				break;
			case DM_REPORT_FIELD_TYPE_STRING_LIST:
				r = _cmp_field_string_list(rh, f->props->field_num, field_id, (const struct str_list_sort_value *) f->sort_value, fs);
				break;
			case DM_REPORT_FIELD_TYPE_TIME:
				r = _cmp_field_time(rh, f->props->field_num, field_id, *(const time_t *) f->sort_value, fs);
				break;
			default:
				log_error(INTERNAL_ERROR "_compare_selection_field: incorrect type %" PRIu32 " for field %s",
					  f->props->flags & DM_REPORT_FIELD_TYPE_MASK, field_id);
		}
	}

	return r;
}

static int _check_selection(struct dm_report *rh, struct selection_node *sn,
			    struct dm_list *fields)
{
	int r;
	struct selection_node *iter_n;
	struct dm_report_field *f;

	switch (sn->type & SEL_MASK) {
		case SEL_ITEM:
			r = 1;
			dm_list_iterate_items(f, fields) {
				if (sn->selection.item->fp != f->props)
					continue;
				if (!_compare_selection_field(rh, f, sn->selection.item))
					r = 0;
			}
			break;
		case SEL_OR:
			r = 0;
			dm_list_iterate_items(iter_n, &sn->selection.set)
				if ((r |= _check_selection(rh, iter_n, fields)))
					break;
			break;
		case SEL_AND:
			r = 1;
			dm_list_iterate_items(iter_n, &sn->selection.set)
				if (!(r &= _check_selection(rh, iter_n, fields)))
					break;
			break;
		default:
			log_error("Unsupported selection type");
			return 0;
	}

	return (sn->type & SEL_MODIFIER_NOT) ? !r : r;
}

static int _check_report_selection(struct dm_report *rh, struct dm_list *fields)
{
	if (!rh->selection || !rh->selection->selection_root)
		return 1;

	return _check_selection(rh, rh->selection->selection_root, fields);
}

static int _do_report_object(struct dm_report *rh, void *object, int do_output, int *selected)
{
	const struct dm_report_field_type *fields;
	struct field_properties *fp;
	struct row *row = NULL;
	struct dm_report_field *field;
	void *data = NULL;
	int r = 0;

	if (!rh) {
		log_error(INTERNAL_ERROR "_do_report_object: dm_report handler is NULL.");
		return 0;
	}

	if (!do_output && !selected) {
		log_error(INTERNAL_ERROR "_do_report_object: output not requested and "
					 "selected output variable is NULL too.");
		return 0;
	}

	if (rh->flags & RH_ALREADY_REPORTED)
		return 1;

	if (!(row = dm_pool_zalloc(rh->mem, sizeof(*row)))) {
		log_error("_do_report_object: struct row allocation failed");
		return 0;
	}

	if (!rh->first_row)
		rh->first_row = row;

	row->rh = rh;

	if ((rh->flags & RH_SORT_REQUIRED) &&
	    !(row->sort_fields =
		dm_pool_zalloc(rh->mem, sizeof(struct dm_report_field *) *
			       rh->keys_count))) {
		log_error("_do_report_object: "
			  "row sort value structure allocation failed");
		goto out;
	}

	dm_list_init(&row->fields);
	row->selected = 1;

	/* For each field to be displayed, call its report_fn */
	dm_list_iterate_items(fp, &rh->field_props) {
		if (!(field = dm_pool_zalloc(rh->mem, sizeof(*field)))) {
			log_error("_do_report_object: "
				  "struct dm_report_field allocation failed");
			goto out;
		}

		if (fp->implicit) {
			fields = _implicit_report_fields;
			if (!strcmp(fields[fp->field_num].id, SPECIAL_FIELD_SELECTED_ID))
				row->field_sel_status = field;
		} else
			fields = rh->fields;

		field->props = fp;

		data = fp->implicit ? _report_get_implicit_field_data(rh, fp, row)
				    : _report_get_field_data(rh, fp, object);
		if (!data) {
			log_error("_do_report_object: "
				  "no data assigned to field %s",
				  fields[fp->field_num].id);
			goto out;
		}

		if (!fields[fp->field_num].report_fn(rh, rh->mem,
							 field, data,
							 rh->private)) {
			log_error("_do_report_object: "
				  "report function failed for field %s",
				  fields[fp->field_num].id);
			goto out;
		}

		dm_list_add(&row->fields, &field->list);
	}

	r = 1;

	if (!_check_report_selection(rh, &row->fields)) {
		row->selected = 0;

		/*
		 * If the row is not selected, we still keep it for output if either:
		 *   - we're displaying special "selected" field in the row,
		 *   - or the report is supposed to be on output multiple times
		 *     where each output can have a new selection defined.
		 */
		if (!row->field_sel_status && !(rh->flags & DM_REPORT_OUTPUT_MULTIPLE_TIMES))
			goto out;

		if (row->field_sel_status) {
			/*
			 * If field with id "selected" is reported,
			 * report the row although it does not pass
			 * the selection criteria.
			 * The "selected" field reports the result
			 * of the selection.
			 */
			_implicit_report_fields[row->field_sel_status->props->field_num].report_fn(rh,
							rh->mem, row->field_sel_status, row, rh->private);
			/*
			 * If the "selected" field is not displayed, e.g.
			 * because it is part of the sort field list,
			 * skip the display of the row as usual unless
			 * we plan to do the output multiple times.
			 */
			if ((row->field_sel_status->props->flags & FLD_HIDDEN) &&
			    !(rh->flags & DM_REPORT_OUTPUT_MULTIPLE_TIMES))
				goto out;
		}
	}

	if (!do_output)
		goto out;

	dm_list_add(&rh->rows, &row->list);

	if (!(rh->flags & DM_REPORT_OUTPUT_BUFFERED))
		return dm_report_output(rh);
out:
	if (selected)
		*selected = row->selected;
	if (!do_output || !r)
		dm_pool_free(rh->mem, row);
	return r;
}

static int _do_report_compact_fields(struct dm_report *rh, int global)
{
	struct dm_report_field *field;
	struct field_properties *fp;
	struct row *row;

	if (!rh) {
		log_error("dm_report_enable_compact_output: dm report handler is NULL.");
		return 0;
	}

	if (!(rh->flags & DM_REPORT_OUTPUT_BUFFERED) ||
	      dm_list_empty(&rh->rows))
		return 1;

	/*
	 * At first, mark all fields with FLD_HIDDEN flag.
	 * Also, mark field with FLD_COMPACTED flag, but only
	 * the ones that didn't have FLD_HIDDEN set before.
	 * This prevents losing the original FLD_HIDDEN flag
	 * in next step...
	 */
	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->flags & FLD_HIDDEN)
			continue;
		if (global || (fp->flags & FLD_COMPACT_ONE))
			fp->flags |= (FLD_COMPACTED | FLD_HIDDEN);
	}

	/*
	 * ...check each field in a row and if its report value
	 * is not empty, drop the FLD_COMPACTED and FLD_HIDDEN
	 * flag if FLD_COMPACTED flag is set. It's important
	 * to keep FLD_HIDDEN flag for the fields that were
	 * already marked with FLD_HIDDEN before - these don't
	 * have FLD_COMPACTED set - check this condition!
	 */
	dm_list_iterate_items(row, &rh->rows) {
		dm_list_iterate_items(field, &row->fields) {
			if ((field->report_string && *field->report_string) &&
			     field->props->flags & FLD_COMPACTED)
					field->props->flags &= ~(FLD_COMPACTED | FLD_HIDDEN);
			}
	}

	/*
	 * The fields left with FLD_COMPACTED and FLD_HIDDEN flag are
	 * the ones which have blank value in all rows. The FLD_HIDDEN
	 * will cause such field to not be reported on output at all.
	 */

	return 1;
}

int dm_report_compact_fields(struct dm_report *rh)
{
	return _do_report_compact_fields(rh, 1);
}

static int _field_to_compact_match(struct dm_report *rh, const char *field, size_t flen)
{
	struct field_properties *fp;
	uint32_t f;
	int implicit;

	if ((_get_field(rh, field, flen, &f, &implicit))) {
		dm_list_iterate_items(fp, &rh->field_props) {
			if ((fp->implicit == implicit) && (fp->field_num == f)) {
				fp->flags |= FLD_COMPACT_ONE;
				break;
			}
		}
		return 1;
	}

	return 0;
}

static int _parse_fields_to_compact(struct dm_report *rh, const char *fields)
{
	const char *ws;		  /* Word start */
	const char *we = fields;  /* Word end */

	if (!fields)
		return 1;

	while (*we) {
		while (*we && *we == ',')
			we++;
		ws = we;
		while (*we && *we != ',')
			we++;
		if (!_field_to_compact_match(rh, ws, (size_t) (we - ws))) {
			log_error("dm_report: Unrecognized field: %.*s", (int) (we - ws), ws);
			return 0;
		}
	}

	return 1;
}

int dm_report_compact_given_fields(struct dm_report *rh, const char *fields)
{
	if (!_parse_fields_to_compact(rh, fields))
		return_0;

	return _do_report_compact_fields(rh, 0);
}

int dm_report_object(struct dm_report *rh, void *object)
{
	return _do_report_object(rh, object, 1, NULL);
}

int dm_report_object_is_selected(struct dm_report *rh, void *object, int do_output, int *selected)
{
	return _do_report_object(rh, object, do_output, selected);
}

/*
 * Selection parsing
 */

/*
 * Other tokens (FIELD, VALUE, STRING, NUMBER, REGEX)
 *     FIELD := <strings of alphabet, number and '_'>
 *     VALUE := NUMBER | STRING
 *     REGEX := <strings quoted by '"', '\'', '(', '{', '[' or unquoted>
 *     NUMBER := <strings of [0-9]> (because sort_value is unsigned)
 *     STRING := <strings quoted by '"', '\'' or unquoted>
 */

static const char * _skip_space(const char *s)
{
	while (*s && isspace(*s))
		s++;
	return s;
}

static int _tok_op(const struct op_def *t, const char *s, const char **end,
		   uint32_t expect)
{
	size_t len;

	s = _skip_space(s);

	for (; t->string; t++) {
		if (expect && !(t->flags & expect))
			continue;

		len = strlen(t->string);
		if (!strncmp(s, t->string, len)) {
			if (end)
				*end = s + len;
			return t->flags;
		}
	}

	if (end)
		*end = s;
	return 0;
}

static int _tok_op_log(const char *s, const char **end, uint32_t expect)
{
	return _tok_op(_op_log, s, end, expect);
}

static int _tok_op_cmp(const char *s, const char **end)
{
	return _tok_op(_op_cmp, s, end, 0);
}

static char _get_and_skip_quote_char(char const **s)
{
	char c = 0;

	if (**s == '"' || **s == '\'') {
		c = **s;
		(*s)++;
	}

	return c;
}

 /*
  *
  * Input:
  *   s             - a pointer to the parsed string
  * Output:
  *   begin         - a pointer to the beginning of the token
  *   end           - a pointer to the end of the token + 1
  *                   or undefined if return value is NULL
  *   return value  - a starting point of the next parsing or
  *                   NULL if 's' doesn't match with token type
  *                   (the parsing should be terminated)
  */
static const char *_tok_value_number(const char *s,
				     const char **begin, const char **end)

{
	int is_float = 0;

	*begin = s;
	while ((!is_float && (*s == '.') && ++is_float) || isdigit(*s))
		s++;
	*end = s;

	if (*begin == *end)
		return NULL;

	return s;
}

/*
 * Input:
 *   s               - a pointer to the parsed string
 *   endchar         - terminating character
 *   end_op_flags    - terminating operator flags (see _op_log)
 *                     (if endchar is non-zero then endflags is ignored)
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 *   end_op_flag_hit - the flag from endflags hit during parsing
 *   return value    - a starting point of the next parsing
 */
static const char *_tok_value_string(const char *s,
				     const char **begin, const char **end,
				     const char endchar, uint32_t end_op_flags,
				     uint32_t *end_op_flag_hit)
{
	uint32_t flag_hit = 0;

	*begin = s;

	/*
	 * If endchar is defined, scan the string till
	 * the endchar or the end of string is hit.
	 * This is in case the string is quoted and we
	 * know exact character that is the stopper.
	 */
	if (endchar) {
		while (*s && *s != endchar)
			s++;
		if (*s != endchar) {
			log_error("Missing end quote.");
			return NULL;
		}
		*end = s;
		s++;
	} else {
		/*
		 * If endchar is not defined then endchar is/are the
		 * operator/s as defined by 'endflags' arg or space char.
		 * This is in case the string is not quoted and
		 * we don't know which character is the exact stopper.
		 */
		while (*s) {
			if ((flag_hit = _tok_op(_op_log, s, NULL, end_op_flags)) || *s == ' ')
				break;
			s++;
		}
		*end = s;
		/*
		 * If we hit one of the strings as defined by 'endflags'
		 * and if 'endflag_hit' arg is provided, save the exact
		 * string flag that was hit.
		 */
		if (end_op_flag_hit)
			*end_op_flag_hit = flag_hit;
	}

	return s;
}

static const char *_reserved_name(struct dm_report *rh,
				  const struct dm_report_reserved_value *reserved,
				  const struct dm_report_field_reserved_value *frv,
				  uint32_t field_num, const char *s, size_t len)
{
	dm_report_reserved_handler handler;
	const char *canonical_name = NULL;
	const char **name;
	char *tmp_s;
	char c;
	int r;

	name = reserved->names;
	while (*name) {
		if ((strlen(*name) == len) && !strncmp(*name, s, len))
			return *name;
		name++;
	}

	if (reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_FUZZY_NAMES) {
		handler = (dm_report_reserved_handler) (frv ? frv->value : reserved->value);
		c = s[len];
		tmp_s = (char *) s;
		tmp_s[len] = '\0';
		if ((r = handler(rh, rh->selection->mem, field_num,
				 DM_REPORT_RESERVED_PARSE_FUZZY_NAME,
				 tmp_s, (const void **) &canonical_name)) <= 0) {
			if (r == -1)
				log_error(INTERNAL_ERROR "%s reserved value handler for field %s has missing "
					  "implementation of DM_REPORT_RESERVED_PARSE_FUZZY_NAME action",
					  (reserved->type & DM_REPORT_FIELD_TYPE_MASK) ? "type-specific" : "field-specific",
					   rh->fields[field_num].id);
			else
				log_error("Error occurred while processing %s reserved value handler for field %s",
					  (reserved->type & DM_REPORT_FIELD_TYPE_MASK) ? "type-specific" : "field-specific",
					   rh->fields[field_num].id);
		}
		tmp_s[len] = c;
		if (r && canonical_name)
			return canonical_name;
	}

	return NULL;
}

/*
 * Used to replace a string representation of the reserved value
 * found in selection with the exact reserved value of certain type.
 */
static const char *_get_reserved(struct dm_report *rh, unsigned type,
				 uint32_t field_num, int implicit,
				 const char *s, const char **begin, const char **end,
				 struct reserved_value_wrapper *rvw)
{
	const struct dm_report_reserved_value *iter = implicit ? NULL : rh->reserved_values;
	const struct dm_report_field_reserved_value *frv;
	const char *tmp_begin = NULL, *tmp_end = NULL, *tmp_s = s;
	const char *name = NULL;
	char c;

	rvw->reserved = NULL;

	if (!iter)
		return s;

	c = _get_and_skip_quote_char(&tmp_s);
	if (!(tmp_s = _tok_value_string(tmp_s, &tmp_begin, &tmp_end, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL)))
		return s;

	while (iter->value) {
		if (!(iter->type & DM_REPORT_FIELD_TYPE_MASK)) {
			/* DM_REPORT_FIELD_TYPE_NONE - per-field reserved value */
			frv = (const struct dm_report_field_reserved_value *) iter->value;
			if ((frv->field_num == field_num) && (name = _reserved_name(rh, iter, frv, field_num,
										    tmp_begin, tmp_end - tmp_begin)))
				break;
		} else if (iter->type & type) {
			/* DM_REPORT_FIELD_TYPE_* - per-type reserved value */
			if ((name = _reserved_name(rh, iter, NULL, field_num,
						   tmp_begin, tmp_end - tmp_begin)))
				break;
		}
		iter++;
	}

	if (name) {
		/* found! */
		*begin = tmp_begin;
		*end = tmp_end;
		s = tmp_s;
		rvw->reserved = iter;
		rvw->matched_name = name;
	}

	return s;
}

float dm_percent_to_float(dm_percent_t percent)
{
	/* Add 0.f to prevent returning -0.00 */
	return (float) percent / DM_PERCENT_1 + 0.f;
}

float dm_percent_to_round_float(dm_percent_t percent, unsigned digits)
{
	const float power10[] = {
		1.f, .1f, .01f, .001f, .0001f, .00001f, .000001f,
		.0000001f, .00000001f, .000000001f,
		.0000000001f
	};
	float r;
	float f = dm_percent_to_float(percent);

	if (digits >= DM_ARRAY_SIZE(power10))
		digits = DM_ARRAY_SIZE(power10) - 1; /* no better precision */

	r = DM_PERCENT_1 * power10[digits];

	if ((percent < r) && (percent > DM_PERCENT_0))
		f = power10[digits];
	else if ((percent > (DM_PERCENT_100 - r)) && (percent < DM_PERCENT_100))
		f = (float) (DM_PERCENT_100 - r) / DM_PERCENT_1;

	return f;
}

dm_percent_t dm_make_percent(uint64_t numerator, uint64_t denominator)
{
	dm_percent_t percent;

	if (!denominator)
		return DM_PERCENT_100; /* FIXME? */
	if (!numerator)
		return DM_PERCENT_0;
	if (numerator == denominator)
		return DM_PERCENT_100;
	switch (percent = DM_PERCENT_100 * ((double) numerator / (double) denominator)) {
		case DM_PERCENT_100:
			return DM_PERCENT_100 - 1;
		case DM_PERCENT_0:
			return DM_PERCENT_0 + 1;
		default:
			return percent;
	}
}

int dm_report_value_cache_set(struct dm_report *rh, const char *name, const void *data)
{
	if (!rh->value_cache && (!(rh->value_cache = dm_hash_create(64)))) {
		log_error("Failed to create cache for values used during reporting.");
		return 0;
	}

	return dm_hash_insert(rh->value_cache, name, (void *) data);
}

const void *dm_report_value_cache_get(struct dm_report *rh, const char *name)
{
	return (rh->value_cache) ? dm_hash_lookup(rh->value_cache, name) : NULL;
}

/*
 * Used to check whether the reserved_values definition passed to
 * dm_report_init_with_selection contains only supported reserved value types.
 */
static int _check_reserved_values_supported(const struct dm_report_field_type fields[],
					    const struct dm_report_reserved_value reserved_values[])
{
	const struct dm_report_reserved_value *iter;
	const struct dm_report_field_reserved_value *field_res;
	const struct dm_report_field_type *field;
	const uint32_t supported_reserved_types =  DM_REPORT_FIELD_TYPE_NUMBER |
						   DM_REPORT_FIELD_TYPE_SIZE |
						   DM_REPORT_FIELD_TYPE_PERCENT |
						   DM_REPORT_FIELD_TYPE_STRING |
						   DM_REPORT_FIELD_TYPE_TIME;
	const uint32_t supported_reserved_types_with_range =  DM_REPORT_FIELD_RESERVED_VALUE_RANGE |
							      DM_REPORT_FIELD_TYPE_NUMBER |
							      DM_REPORT_FIELD_TYPE_SIZE |
							      DM_REPORT_FIELD_TYPE_PERCENT |
							      DM_REPORT_FIELD_TYPE_TIME;


	if (!reserved_values)
		return 1;

	iter = reserved_values;

	while (iter->value) {
		if (iter->type & DM_REPORT_FIELD_TYPE_MASK) {
			if (!(iter->type & supported_reserved_types) ||
			    ((iter->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE) &&
			     !(iter->type & (supported_reserved_types_with_range &
					     ~DM_REPORT_FIELD_RESERVED_VALUE_RANGE)))) {
				log_error(INTERNAL_ERROR "_check_reserved_values_supported: "
					  "global reserved value for type 0x%x not supported",
					   iter->type);
				return 0;
			}
		} else {
			field_res = (const struct dm_report_field_reserved_value *) iter->value;
			field = &fields[field_res->field_num];
			if (!(field->flags & supported_reserved_types) ||
			    ((field->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE) &&
			     !(field->type & (supported_reserved_types_with_range &
					     ~DM_REPORT_FIELD_RESERVED_VALUE_RANGE)))) {
				log_error(INTERNAL_ERROR "_check_reserved_values_supported: "
					  "field-specific reserved value of type 0x%x for "
					  "field %s not supported",
					   field->flags & DM_REPORT_FIELD_TYPE_MASK, field->id);
				return 0;
			}
		}
		iter++;
	}

	return 1;
}

/*
 * Input:
 *   ft              - field type for which the value is parsed
 *   s               - a pointer to the parsed string
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 *   flags           - parsing flags
 */
static const char *_tok_value_regex(struct dm_report *rh,
				    const struct dm_report_field_type *ft,
				    const char *s, const char **begin,
				    const char **end, uint32_t *flags)
{
	char c;

	s = _skip_space(s);

	if (!*s) {
		log_error("Regular expression expected for selection field %s", ft->id);
		return NULL;
	}

	switch (*s) {
		case '(': c = ')'; break;
		case '{': c = '}'; break;
		case '[': c = ']'; break;
		case '"': /* fall through */
		case '\'': c = *s; break;
		default:  c = 0;
	}

	if (!(s = _tok_value_string(c ? s + 1 : s, begin, end, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL))) {
		log_error("Failed to parse regex value for selection field %s.", ft->id);
		return NULL;
	}

	*flags |= DM_REPORT_FIELD_TYPE_STRING;
	return s;
}

static int _str_list_item_cmp(const void *a, const void *b)
{
	const struct dm_str_list * const *item_a = (const struct dm_str_list * const *) a;
	const struct dm_str_list * const *item_b = (const struct dm_str_list * const *) b;

	return strcmp((*item_a)->str, (*item_b)->str);
}

static int _add_item_to_string_list(struct dm_pool *mem, const char *begin,
				    const char *end, struct dm_list *list)
{
	struct dm_str_list *item;

	if (!(item = dm_pool_zalloc(mem, sizeof(*item))) ||
	    !(item->str = begin == end ? "" : dm_pool_strndup(mem, begin, end - begin))) {
		log_error("_add_item_to_string_list: memory allocation failed for string list item");
		return 0;
	}
	dm_list_add(list, &item->list);

	return 1;
}

/*
 * Input:
 *   ft              - field type for which the value is parsed
 *   mem             - memory pool to allocate from
 *   s               - a pointer to the parsed string
 * Output:
 *   begin           - a pointer to the beginning of the token (whole list)
 *   end             - a pointer to the end of the token + 1 (whole list)
 *   sel_str_list    - the list of strings parsed
 */
static const char *_tok_value_string_list(const struct dm_report_field_type *ft,
					  struct dm_pool *mem, const char *s,
					  const char **begin, const char **end,
					  struct selection_str_list **sel_str_list,
					  uint32_t *flags)
{
	static const char _str_list_item_parsing_failed[] = "Failed to parse string list value "
							    "for selection field %s.";
	struct selection_str_list *ssl = NULL;
	struct dm_str_list *item;
	const char *begin_item = NULL, *end_item = NULL, *tmp;
	uint32_t op_flags, end_op_flag_expected, end_op_flag_hit = 0;
	struct dm_str_list **arr;
	size_t list_size;
	unsigned int i;
	int list_end = 0;
	char c;

	if (!(ssl = dm_pool_zalloc(mem, sizeof(*ssl)))) {
		log_error("_tok_value_string_list: memory allocation failed for selection list.");
		goto bad;
	}
	dm_list_init(&ssl->str_list.list);
	*begin = s;

	if (!(op_flags = _tok_op_log(s, &tmp, SEL_LIST_LS | SEL_LIST_SUBSET_LS))) {
		/* Only one item - SEL_LIST_{SUBSET_}LS and SEL_LIST_{SUBSET_}LE not used */
		c = _get_and_skip_quote_char(&s);
		if (!(s = _tok_value_string(s, &begin_item, &end_item, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL))) {
			log_error(_str_list_item_parsing_failed, ft->id);
			goto bad;
		}
		if (!_add_item_to_string_list(mem, begin_item, end_item, &ssl->str_list.list))
			goto_bad;
		ssl->type = SEL_OR | SEL_LIST_SUBSET_LS;
		goto out;
	}

	/* More than one item - items enclosed in SEL_LIST_LS and SEL_LIST_LE
	 * or SEL_LIST_SUBSET_LS and SEL_LIST_SUBSET_LE.
	 * Each element is terminated by AND or OR operator or 'list end'.
	 * The first operator hit is then the one allowed for the whole list,
	 * no mixing allowed!
	 */

	/* Are we using [] or {} for the list? */
	end_op_flag_expected = (op_flags == SEL_LIST_LS) ? SEL_LIST_LE : SEL_LIST_SUBSET_LE;

	op_flags = SEL_LIST_LE | SEL_LIST_SUBSET_LE | SEL_AND | SEL_OR;
	s++;
	while (*s) {
		s = _skip_space(s);
		c = _get_and_skip_quote_char(&s);
		if (!(s = _tok_value_string(s, &begin_item, &end_item, c, op_flags, NULL))) {
			log_error(_str_list_item_parsing_failed, ft->id);
			goto bad;
		}
		s = _skip_space(s);

		if (!(end_op_flag_hit = _tok_op_log(s, &tmp, op_flags))) {
			log_error("Invalid operator in selection list.");
			goto bad;
		}

		if (end_op_flag_hit & (SEL_LIST_LE | SEL_LIST_SUBSET_LE)) {
			list_end = 1;
			if (end_op_flag_hit != end_op_flag_expected) {
				for (i = 0; _op_log[i].string; i++)
					if (_op_log[i].flags == end_op_flag_expected)
						break;
				log_error("List ended with incorrect character, "
					  "expecting \'%s\'.", _op_log[i].string);
				goto bad;
			}
		}

		if (ssl->type) {
			if (!list_end && !(ssl->type & end_op_flag_hit)) {
				log_error("Only one type of logical operator allowed "
					  "in selection list at a time.");
				goto bad;
			}
		} else {
			if (list_end)
				ssl->type = end_op_flag_expected == SEL_LIST_LE ? SEL_AND : SEL_OR;
			else
				ssl->type = end_op_flag_hit;
		}

		if (!_add_item_to_string_list(mem, begin_item, end_item, &ssl->str_list.list))
			goto_bad;

		s = tmp;

		if (list_end)
			break;
	}

	if (!(end_op_flag_hit & (SEL_LIST_LE | SEL_LIST_SUBSET_LE))) {
		log_error("Missing list end for selection field %s", ft->id);
		goto bad;
	}

	/* Store information whether [] or {} was used. */
	if (end_op_flag_expected == SEL_LIST_LE)
		ssl->type |= SEL_LIST_LS;
	else
		ssl->type |= SEL_LIST_SUBSET_LS;

	if (!(list_size = dm_list_size(&ssl->str_list.list))) {
		log_error(INTERNAL_ERROR "_tok_value_string_list: list has no items");
		goto bad;
	} else if (list_size == 1)
		goto out;

	if (*flags & FLD_CMP_REGEX)
		/* No need to sort the list for regex. */
		goto out;

	/* Sort the list. */
	if (!(arr = dm_malloc(sizeof(item) * list_size))) {
		log_error("_tok_value_string_list: memory allocation failed for sort array");
		goto bad;
	}

	i = 0;
	dm_list_iterate_items(item, &ssl->str_list.list)
		arr[i++] = item;
	qsort(arr, list_size, sizeof(item), _str_list_item_cmp);
	dm_list_init(&ssl->str_list.list);
	for (i = 0; i < list_size; i++)
		dm_list_add(&ssl->str_list.list, &arr[i]->list);

	dm_free(arr);
out:
	*end = s;
        if (sel_str_list)
		*sel_str_list = ssl;

	return s;
bad:
	*end = s;
	if (ssl)
		dm_pool_free(mem, ssl);
        if (sel_str_list)
		*sel_str_list = NULL;
	return s;
}

struct time_value {
	int range;
	time_t t1;
	time_t t2;
};

static const char _out_of_range_msg[] = "Field selection value %s out of supported range for field %s.";

/*
 * Standard formatted date and time - ISO8601.
 *
 * date time timezone
 *
 * date:
 * YYYY-MM-DD (or shortly YYYYMMDD)
 * YYYY-MM (shortly YYYYMM), auto DD=1
 * YYYY, auto MM=01 and DD=01
 *
 * time:
 * hh:mm:ss (or shortly hhmmss)
 * hh:mm (or shortly hhmm), auto ss=0
 * hh (or shortly hh), auto mm=0, auto ss=0
 *
 * timezone:
 * +hh:mm or -hh:mm (or shortly +hhmm or -hhmm)
 * +hh or -hh
*/

#define DELIM_DATE '-'
#define DELIM_TIME ':'

static const int _days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int _is_leap_year(long year)
{
	return (((year % 4==0) && (year % 100 != 0)) || (year % 400 == 0));
}

static int _get_days_in_month(long month, long year)
{
	return (month == 2 && _is_leap_year(year)) ? _days_in_month[month-1] + 1
						   : _days_in_month[month-1];
}

typedef enum {
	RANGE_NONE,
	RANGE_SECOND,
	RANGE_MINUTE,
	RANGE_HOUR,
	RANGE_DAY,
	RANGE_MONTH,
	RANGE_YEAR
} time_range_t;

static char *_get_date(char *str, struct tm *tm, time_range_t *range)
{
	time_range_t tmp_range = RANGE_NONE;
	long n1, n2 = -1, n3 = -1;
	char *s = str, *end;
	size_t len = 0;

	if (!isdigit(*s))
		/* we need a year at least */
		return NULL;

	n1 = strtol(s, &end, 10);
	if (*end == DELIM_DATE) {
		len += (4 - (end - s)); /* diff in length from standard YYYY */
		s = end + 1;
		if (isdigit(*s)) {
			n2 = strtol(s, &end, 10);
			len += (2 - (end - s)); /* diff in length from standard MM */
			if (*end == DELIM_DATE) {
				s = end + 1;
				n3 = strtol(s, &end, 10);
				len += (2 - (end - s)); /* diff in length from standard DD */
			}
		}
	}

	len = len + end - str;

	/* variations from standard YYYY-MM-DD */
	if (n3 == -1) {
		if (n2 == -1) {
			if (len == 4) {
				/* YYYY */
				tmp_range = RANGE_YEAR;
				n3 = n2 = 1;
			} else if (len == 6) {
				/* YYYYMM */
				tmp_range = RANGE_MONTH;
				n3 = 1;
				n2 = n1 % 100;
				n1 = n1 / 100;
			} else if (len == 8) {
				tmp_range = RANGE_DAY;
				/* YYYYMMDD */
				n3 = n1 % 100;
				n2 = (n1 / 100) % 100;
				n1 = n1 / 10000;
			} else
                                goto_bad;
		} else {
			if (len == 7) {
				tmp_range = RANGE_MONTH;
				/* YYYY-MM */
				n3 = 1;
			} else
				goto_bad;
		}
	}

	if (n2 < 1 || n2 > 12) {
		log_error("Specified month out of range.");
		return NULL;
	}

	if (n3 < 1 || n3 > _get_days_in_month(n2, n1)) {
		log_error("Specified day out of range.");
		return NULL;
	}

	if (tmp_range == RANGE_NONE)
		tmp_range = RANGE_DAY;

	tm->tm_year = n1 - 1900;
	tm->tm_mon = n2 - 1;
	tm->tm_mday = n3;
	*range = tmp_range;

	return (char *) _skip_space(end);

bad:
	log_error("Incorrect date format.");

	return NULL;
}

static char *_get_time(char *str, struct tm *tm, time_range_t *range)
{
	time_range_t tmp_range = RANGE_NONE;
	long n1, n2 = -1, n3 = -1;
	char *s = str, *end;
	size_t len = 0;

	if (!isdigit(*s)) {
		/* time is not compulsory */
		tm->tm_hour = tm->tm_min = tm->tm_sec = 0;
		return (char *) _skip_space(s);
	}

	n1 = strtol(s, &end, 10);
	if (*end == DELIM_TIME) {
		len += (2 - (end - s)); /* diff in length from standard HH */
		s = end + 1;
		if (isdigit(*s)) {
			n2 = strtol(s, &end, 10);
			len += (2 - (end - s)); /* diff in length from standard MM */
			if (*end == DELIM_TIME) {
				s = end + 1;
				n3 = strtol(s, &end, 10);
				len += (2 - (end - s)); /* diff in length from standard SS */
			}
		}
	}

	len = len + end - str;

	/* variations from standard HH:MM:SS */
	if (n3 == -1) {
		if (n2 == -1) {
			if (len == 2) {
				/* HH */
				tmp_range = RANGE_HOUR;
				n3 = n2 = 0;
			} else if (len == 4) {
				/* HHMM */
				tmp_range = RANGE_MINUTE;
				n3 = 0;
				n2 = n1 % 100;
				n1 = n1 / 100;
			} else if (len == 6) {
				/* HHMMSS */
				tmp_range = RANGE_SECOND;
				n3 = n1 % 100;
				n2 = (n1 / 100) % 100;
				n1 = n1 / 10000;
			} else
				goto_bad;
		} else {
			if (len == 5) {
				/* HH:MM */
				tmp_range = RANGE_MINUTE;
				n3 = 0;
			} else
				goto_bad;
		}
	}

	if (n1 < 0 || n1 > 23) {
		log_error("Specified hours out of range.");
		return NULL;
	}

	if (n2 < 0 || n2 > 60) {
		log_error("Specified minutes out of range.");
		return NULL;
	}

	if (n3 < 0 || n3 > 60) {
		log_error("Specified seconds out of range.");
		return NULL;
	}

	/* Just time without exact date is incomplete! */
	if (*range != RANGE_DAY) {
		log_error("Full date specification needed.");
		return NULL;
	}

	tm->tm_hour = n1;
	tm->tm_min = n2;
	tm->tm_sec = n3;
	*range = tmp_range;

	return (char *) _skip_space(end);

bad:
	log_error("Incorrect time format.");

	return NULL;
}

/* The offset is always an absolute offset against GMT! */
static char *_get_tz(char *str, int *tz_supplied, int *offset)
{
	long n1, n2 = -1;
	char *s = str, *end;
	int sign = 1; /* +HH:MM by default */
	size_t len = 0;

	*tz_supplied = 0;
	*offset = 0;

	if (!isdigit(*s)) {
		if (*s == '+')  {
			sign = 1;
			s = s + 1;
		} else if (*s == '-') {
			sign = -1;
			s = s + 1;
		} else
			return (char *) _skip_space(s);
	}

	n1 = strtol(s, &end, 10);
	if (*end == DELIM_TIME) {
		len = (2 - (end - s)); /* diff in length from standard HH */
		s = end + 1;
		if (isdigit(*s)) {
			n2 = strtol(s, &end, 10);
			len = (2 - (end - s)); /* diff in length from standard MM */
		}
	}

	len = len + end - s;

	/* variations from standard HH:MM */
	if (n2 == -1) {
		if (len == 2) {
			/* HH */
			n2 = 0;
		} else if (len == 4) {
			/* HHMM */
			n2 = n1 % 100;
			n1 = n1 / 100;
		} else
			return NULL;
	}

	if (n2 < 0 || n2 > 60)
		return NULL;

	if (n1 < 0 || n1 > 14)
		return NULL;

	/* timezone offset in seconds */
	*offset = sign * ((n1 * 3600) + (n2 * 60));
	*tz_supplied = 1;
	return (char *) _skip_space(end);
}

static int _local_tz_offset(time_t t_local)
{
	struct tm tm_gmt;
	time_t t_gmt;

	gmtime_r(&t_local, &tm_gmt);

	if ((t_gmt = mktime(&tm_gmt)) < 0)
		return 0;

	/*
	 * gmtime returns time that is adjusted
	 * for DST.Subtract this adjustment back
	 * to give us proper *absolute* offset
	 * for our local timezone.
	 */
	if (tm_gmt.tm_isdst)
		t_gmt -= 3600;

	return (int)(t_local - t_gmt);
}

static void _get_final_time(time_range_t range, struct tm *tm,
			    int tz_supplied, int offset,
			    struct time_value *tval)
{

	struct tm tm_up = *tm;

	switch (range) {
		case RANGE_SECOND:
			if (tm_up.tm_sec < 59) {
				tm_up.tm_sec += 1;
				break;
			}
			/* fall through */
		case RANGE_MINUTE:
			if (tm_up.tm_min < 59) {
				tm_up.tm_min += 1;
				break;
			}
			/* fall through */
		case RANGE_HOUR:
			if (tm_up.tm_hour < 23) {
				tm_up.tm_hour += 1;
				break;
			}
			/* fall through */
		case RANGE_DAY:
			if (tm_up.tm_mday < _get_days_in_month(tm_up.tm_mon, tm_up.tm_year)) {
				tm_up.tm_mday += 1;
				break;
			}
			/* fall through */
		case RANGE_MONTH:
			if (tm_up.tm_mon < 11) {
				tm_up.tm_mon += 1;
				break;
			}
			/* fall through */
		case RANGE_YEAR:
			tm_up.tm_year += 1;
			break;
		case RANGE_NONE:
			/* nothing to do here */
			break;
	}

	tval->range = (range != RANGE_NONE);
	tval->t1 = mktime(tm);
	tval->t2 = mktime(&tm_up) - 1;

	if (tz_supplied) {
		/*
		 * The 'offset' is with respect to the GMT.
		 * Calculate what the offset is with respect
		 * to our local timezone and adjust times
		 * so they represent time in our local timezone.
		 */
		offset -= _local_tz_offset(tval->t1);
		tval->t1 -= offset;
		tval->t2 -= offset;
	}
}

static int _parse_formatted_date_time(char *str, struct time_value *tval)
{
	time_range_t range = RANGE_NONE;
	struct tm tm = {0};
	int gmt_offset;
	int tz_supplied;

	tm.tm_year = tm.tm_mday = tm.tm_mon = -1;
	tm.tm_hour = tm.tm_min = tm.tm_sec = -1;
	tm.tm_isdst = tm.tm_wday = tm.tm_yday = -1;

	if (!(str = _get_date(str, &tm, &range)))
		return 0;

	if (!(str = _get_time(str, &tm, &range)))
		return 0;

	if (!(str = _get_tz(str, &tz_supplied, &gmt_offset)))
		return 0;

	if (*str)
		return 0;

	_get_final_time(range, &tm, tz_supplied, gmt_offset, tval);

	return 1;
}

static const char *_tok_value_time(const struct dm_report_field_type *ft,
				   struct dm_pool *mem, const char *s,
				   const char **begin, const char **end,
				   struct time_value *tval)
{
	char *time_str = NULL;
	const char *r = NULL;
	uint64_t t;
	char c;

	s = _skip_space(s);

	if (*s == '@') {
		/* Absolute time value in number of seconds since epoch. */
		if (!(s = _tok_value_number(s+1, begin, end)))
			goto_out;

		if (!(time_str = dm_pool_strndup(mem, *begin, *end - *begin))) {
			log_error("_tok_value_time: dm_pool_strndup failed");
			goto out;
		}

		errno = 0;
		if (((t = strtoull(time_str, NULL, 10)) == ULLONG_MAX) && errno == ERANGE) {
			log_error(_out_of_range_msg, time_str, ft->id);
			goto out;
		}

		tval->range = 0;
		tval->t1 = (time_t) t;
		tval->t2 = 0;
		r = s;
	} else {
		c = _get_and_skip_quote_char(&s);
		if (!(s = _tok_value_string(s, begin, end, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL)))
			goto_out;

		if (!(time_str = dm_pool_strndup(mem, *begin, *end - *begin))) {
			log_error("tok_value_time: dm_pool_strndup failed");
			goto out;
		}

		if (!_parse_formatted_date_time(time_str, tval))
			goto_out;
		r = s;
	}
out:
	if (time_str)
		dm_pool_free(mem, time_str);
	return r;
}

/*
 * Input:
 *   ft              - field type for which the value is parsed
 *   s               - a pointer to the parsed string
 *   mem             - memory pool to allocate from
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 *   flags           - parsing flags
 *   custom          - custom data specific to token type
 *                     (e.g. size unit factor)
 */
static const char *_tok_value(struct dm_report *rh,
			      const struct dm_report_field_type *ft,
			      uint32_t field_num, int implicit,
			      const char *s,
			      const char **begin, const char **end,
			      uint32_t *flags,
			      struct reserved_value_wrapper *rvw,
			      struct dm_pool *mem, void *custom)
{
	int expected_type = ft->flags & DM_REPORT_FIELD_TYPE_MASK;
	struct selection_str_list **str_list;
	struct time_value *tval;
	uint64_t *factor;
	const char *tmp;
	char c;

	s = _skip_space(s);

	/* recognize possible reserved value (but not in a regex) */
	if (!(*flags & FLD_CMP_REGEX))
		s = _get_reserved(rh, expected_type, field_num, implicit, s, begin, end, rvw);

	if (rvw->reserved) {
		/*
		 * FLD_CMP_NUMBER shares operators with FLD_CMP_TIME,
		 * so adjust flags here based on expected type.
		 */
		if (expected_type == DM_REPORT_FIELD_TYPE_TIME)
			*flags &= ~FLD_CMP_NUMBER;
		else if (expected_type == DM_REPORT_FIELD_TYPE_NUMBER)
			*flags &= ~FLD_CMP_TIME;
		*flags |= expected_type;

		/* if we matched a reserved value, skip further processing of this token */
		return s;
	}

	switch (expected_type) {

		case DM_REPORT_FIELD_TYPE_STRING:
			if (*flags & FLD_CMP_REGEX) {
				if (!(s = _tok_value_regex(rh, ft, s, begin, end, flags)))
					return NULL;
			} else {
				c = _get_and_skip_quote_char(&s);
				if (!(s = _tok_value_string(s, begin, end, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL))) {
					log_error("Failed to parse string value "
						  "for selection field %s.", ft->id);
					return NULL;
				}
			}
			*flags |= DM_REPORT_FIELD_TYPE_STRING;
			break;

		case DM_REPORT_FIELD_TYPE_STRING_LIST:
			if (!(str_list = (struct selection_str_list **) custom))
				goto_bad;

			s = _tok_value_string_list(ft, mem, s, begin, end, str_list, flags);
			if (!(*str_list)) {
				log_error("Failed to parse string list value "
					  "for selection field %s.", ft->id);
				return NULL;
			}
			*flags |= DM_REPORT_FIELD_TYPE_STRING_LIST;
			break;

		case DM_REPORT_FIELD_TYPE_NUMBER:
			/* fall through */
		case DM_REPORT_FIELD_TYPE_SIZE:
			/* fall through */
		case DM_REPORT_FIELD_TYPE_PERCENT:
			if (!(s = _tok_value_number(s, begin, end))) {
				log_error("Failed to parse numeric value "
					  "for selection field %s.", ft->id);
				return NULL;
			}

			if (*s == DM_PERCENT_CHAR) {
				s++;
				c = DM_PERCENT_CHAR;
				if (expected_type != DM_REPORT_FIELD_TYPE_PERCENT) {
					log_error("Found percent value but %s value "
						  "expected for selection field %s.",
						  expected_type == DM_REPORT_FIELD_TYPE_NUMBER ?
							"numeric" : "size", ft->id);
					return NULL;
				}
			} else {
				if (!(factor = (uint64_t *) custom))
					goto_bad;

				if ((*factor = dm_units_to_factor(s, &c, 0, &tmp))) {
					s = tmp;
					if (expected_type != DM_REPORT_FIELD_TYPE_SIZE) {
						log_error("Found size unit specifier "
							  "but %s value expected for "
							  "selection field %s.",
							  expected_type == DM_REPORT_FIELD_TYPE_NUMBER ?
							  "numeric" : "percent", ft->id);
						return NULL;
					}
				} else if (expected_type == DM_REPORT_FIELD_TYPE_SIZE) {
					/*
					 * If size unit is not defined in the selection
					 * and the type expected is size, use use 'm'
					 * (1 MiB) for the unit by default. This is the
					 * same behaviour as seen in lvcreate -L <size>.
					 */
					*factor = 1024*1024;
				}
			}

			*flags |= expected_type;
			/*
			 * FLD_CMP_NUMBER shares operators with FLD_CMP_TIME,
			 * but we have NUMBER here, so remove FLD_CMP_TIME.
			 */
			*flags &= ~FLD_CMP_TIME;
			break;

		case DM_REPORT_FIELD_TYPE_TIME:
			if (!(tval = (struct time_value *) custom))
				goto_bad;

			if (!(s = _tok_value_time(ft, mem, s, begin, end, tval))) {
				log_error("Failed to parse time value "
					  "for selection field %s.", ft->id);
				return NULL;
			}

			*flags |= DM_REPORT_FIELD_TYPE_TIME;
			/*
			 * FLD_CMP_TIME shares operators with FLD_CMP_NUMBER,
			 * but we have TIME here, so remove FLD_CMP_NUMBER.
			 */
			*flags &= ~FLD_CMP_NUMBER;
			break;
	}

	return s;
bad:
	log_error(INTERNAL_ERROR "_tok_value: Forbidden NULL custom parameter detected.");

	return NULL;
}

/*
 * Input:
 *   s               - a pointer to the parsed string
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 */
static const char *_tok_field_name(const char *s,
				    const char **begin, const char **end)
{
	char c;
	s = _skip_space(s);

	*begin = s;
	while ((c = *s) &&
	       (isalnum(c) || c == '_' || c == '-'))
		s++;
	*end = s;

	if (*begin == *end)
		return NULL;

	return s;
}

static int _get_reserved_value(struct dm_report *rh, uint32_t field_num,
			       struct reserved_value_wrapper *rvw)
{
	const void *tmp_value;
	dm_report_reserved_handler handler;
	int r;

	if (!rvw->reserved) {
		rvw->value = NULL;
		return 1;
	}

	if (rvw->reserved->type & DM_REPORT_FIELD_TYPE_MASK)
		/* type reserved value */
		tmp_value = rvw->reserved->value;
	else
		/* per-field reserved value */
		tmp_value = ((const struct dm_report_field_reserved_value *) rvw->reserved->value)->value;

	if (rvw->reserved->type & (DM_REPORT_FIELD_RESERVED_VALUE_DYNAMIC_VALUE | DM_REPORT_FIELD_RESERVED_VALUE_FUZZY_NAMES)) {
		handler = (dm_report_reserved_handler) tmp_value;
		if ((r = handler(rh, rh->selection->mem, field_num,
				 DM_REPORT_RESERVED_GET_DYNAMIC_VALUE,
				 rvw->matched_name, &tmp_value)) <= 0) {
			if (r == -1)
				log_error(INTERNAL_ERROR "%s reserved value handler for field %s has missing"
					  "implementation of DM_REPORT_RESERVED_GET_DYNAMIC_VALUE action",
					  (rvw->reserved->type) & DM_REPORT_FIELD_TYPE_MASK ? "type-specific" : "field-specific",
					  rh->fields[field_num].id);
			else
				log_error("Error occurred while processing %s reserved value handler for field %s",
					  (rvw->reserved->type) & DM_REPORT_FIELD_TYPE_MASK ? "type-specific" : "field-specific",
					  rh->fields[field_num].id);
			return 0;
		}
	}

	rvw->value = tmp_value;
	return 1;
}

static struct dm_regex *_selection_regex_create(struct selection *selection, const char * const *patterns,
						unsigned num_patterns)
{
	if (!selection->regex_mem) {
		if (!(selection->regex_mem = dm_pool_create("report selection regex", 32 * 1024))) {
			log_error("Failed to create report selection regex memory pool.");
			return NULL;
		}
	}

	return dm_regex_create(selection->regex_mem, patterns, num_patterns);
}

static struct field_selection *_create_field_selection(struct dm_report *rh,
						       uint32_t field_num,
						       int implicit,
						       const char *v,
						       size_t len,
						       uint32_t flags,
						       struct reserved_value_wrapper *rvw,
						       void *custom)
{
	const struct dm_report_field_type *fields = implicit ? _implicit_report_fields
							     : rh->fields;
	struct field_properties *fp, *found = NULL;
	struct field_selection *fs;
	const char *field_id;
	struct time_value *tval;
	uint64_t factor;
	char *s;
	const char *s_arr_single[2] = { 0 };
	const char **s_arr;
	size_t s_arr_size;
	struct dm_str_list *sl;
	size_t i;

	dm_list_iterate_items(fp, &rh->field_props) {
		if ((fp->implicit == implicit) && (fp->field_num == field_num)) {
			found = fp;
			break;
		}
	}

	/* The field is neither used in display options nor sort keys. */
	if (!found) {
		if (rh->selection->add_new_fields) {
			if (!(found = _add_field(rh, field_num, implicit, FLD_HIDDEN)))
				return NULL;
			rh->report_types |= fields[field_num].type;
		} else {
			log_error("Unable to create selection with field \'%s\' "
				  "which is not included in current report.",
				  implicit ? _implicit_report_fields[field_num].id
					   : rh->fields[field_num].id);
			return NULL;
		}
	}

	field_id = fields[found->field_num].id;

	if (!(found->flags & flags & DM_REPORT_FIELD_TYPE_MASK)) {
		log_error("dm_report: incompatible comparison "
			  "type for selection field %s", field_id);
		return NULL;
	}

	/* set up selection */
	if (!(fs = dm_pool_zalloc(rh->selection->mem, sizeof(struct field_selection)))) {
		log_error("dm_report: struct field_selection "
			  "allocation failed for selection field %s", field_id);
		return NULL;
	}

	if (!(fs->value = dm_pool_zalloc(rh->selection->mem, sizeof(struct field_selection_value)))) {
		stack;
		goto error_field_id;
	}

	if (((rvw->reserved && (rvw->reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE)) ||
	     (((flags & DM_REPORT_FIELD_TYPE_MASK) == DM_REPORT_FIELD_TYPE_TIME) &&
	      custom && ((struct time_value *) custom)->range))
		 &&
	    !(fs->value->next = dm_pool_zalloc(rh->selection->mem, sizeof(struct field_selection_value)))) {
		stack;
		goto error_field_id;
	}

	fs->fp = found;
	fs->flags = flags;

	if (!_get_reserved_value(rh, field_num, rvw)) {
		log_error("dm_report: could not get reserved value "
			  "while processing selection field %s", field_id);
		goto error;
	}

	/* store comparison operand */
	if (flags & FLD_CMP_REGEX) {
		/* REGEX */
		switch (flags & DM_REPORT_FIELD_TYPE_MASK) {
			case DM_REPORT_FIELD_TYPE_STRING:
				if (!(s = malloc(len + 1))) {
					log_error("dm_report: malloc failed to store "
						  "regex value for selection field %s", field_id);
					goto error;
				}
				memcpy(s, v, len);
				s[len] = '\0';
				s_arr_single[0] = s;

				fs->value->v.r = _selection_regex_create(rh->selection, s_arr_single, 1);
				free(s);
				if (!fs->value->v.r) {
					log_error("dm_report: failed to create regex "
						  "matcher for selection field %s", field_id);
					goto error;
				}
				break;
			case DM_REPORT_FIELD_TYPE_STRING_LIST:
				if (!custom)
					goto bad;
				fs->value->v.l = *((struct selection_str_list **) custom);

				if (!(s_arr_size = dm_list_size(&fs->value->v.l->str_list.list)))
					break;

				if (!(s_arr = calloc(s_arr_size, sizeof(char *)))) {
					log_error("dm_report: malloc failed for regex array "
						  "for selection field %s", field_id);
					goto error;
				}
				i = 0;
				dm_list_iterate_items(sl, &fs->value->v.l->str_list.list)
					s_arr[i++] = sl->str;

				fs->value->v.l->regex = _selection_regex_create(rh->selection, s_arr, s_arr_size);
				fs->value->v.l->regex_num_patterns = s_arr_size;
				free(s_arr);
				if (!fs->value->v.l->regex) {
					log_error("dm_report: failed to create regex "
						  "matcher for selection field %s", field_id);
					goto error;
				}
				break;
			default:
				log_error(INTERNAL_ERROR "_create_field_selection: regex: incorrect type %" PRIu32 " for field %s",
					  flags & DM_REPORT_FIELD_TYPE_MASK, field_id);
				goto error;
		}
	} else {
		/* STRING, NUMBER, SIZE, PERCENT, STRING_LIST, TIME */
		if (!(s = dm_pool_strndup(rh->selection->mem, v, len))) {
			log_error("dm_report: dm_pool_strndup for value "
				  "of selection field %s", field_id);
			goto error;
		}

		switch (flags & DM_REPORT_FIELD_TYPE_MASK) {
			case DM_REPORT_FIELD_TYPE_STRING:
				if (rvw->value) {
					fs->value->v.s = (const char *) rvw->value;
					if (rvw->reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE)
						fs->value->next->v.s = (((const char * const *) rvw->value)[1]);
					dm_pool_free(rh->selection->mem, s);
				} else {
					fs->value->v.s = s;
					if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_STRING, fs->value->v.s, NULL)) {
						log_error("String value %s found in selection is reserved.", fs->value->v.s);
						goto error;
					}
				}
				break;
			case DM_REPORT_FIELD_TYPE_NUMBER:
				if (rvw->value) {
					fs->value->v.i = *(const uint64_t *) rvw->value;
					if (rvw->reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE)
						fs->value->next->v.i = (((const uint64_t *) rvw->value)[1]);
				} else {
					errno = 0;
					if (((fs->value->v.i = strtoull(s, NULL, 10)) == ULLONG_MAX) &&
						 (errno == ERANGE)) {
						log_error(_out_of_range_msg, s, field_id);
						goto error;
					}
					if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_NUMBER, &fs->value->v.i, NULL)) {
						log_error("Numeric value %" PRIu64 " found in selection is reserved.", fs->value->v.i);
						goto error;
					}
				}
				dm_pool_free(rh->selection->mem, s);
				break;
			case DM_REPORT_FIELD_TYPE_SIZE:
				if (rvw->value) {
					fs->value->v.d = *(const double *) rvw->value;
					if (rvw->reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE)
						fs->value->next->v.d = (((const double *) rvw->value)[1]);
				} else {
					errno = 0;
					fs->value->v.d = strtod(s, NULL);
					if (errno == ERANGE) {
						log_error(_out_of_range_msg, s, field_id);
						goto error;
					}
					if (custom && (factor = *((const uint64_t *)custom)))
						fs->value->v.d *= factor;
					fs->value->v.d /= 512; /* store size in sectors! */
					if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_SIZE, &fs->value->v.d, NULL)) {
						log_error("Size value %f found in selection is reserved.", fs->value->v.d);
						goto error;
					}
				}
				dm_pool_free(rh->selection->mem, s);
				break;
			case DM_REPORT_FIELD_TYPE_PERCENT:
				if (rvw->value) {
					fs->value->v.i = *(const uint64_t *) rvw->value;
					if (rvw->reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE)
						fs->value->next->v.i = (((const uint64_t *) rvw->value)[1]);
				} else {
					errno = 0;
					fs->value->v.d = strtod(s, NULL);
					if ((errno == ERANGE) || (fs->value->v.d < 0) || (fs->value->v.d > 100)) {
						log_error(_out_of_range_msg, s, field_id);
						goto error;
					}

					fs->value->v.i = (dm_percent_t) (DM_PERCENT_1 * fs->value->v.d);

					if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_PERCENT, &fs->value->v.i, NULL)) {
						log_error("Percent value %s found in selection is reserved.", s);
						goto error;
					}
				}
				break;
			case DM_REPORT_FIELD_TYPE_STRING_LIST:
				if (!custom)
                                        goto_bad;
				fs->value->v.l = *(struct selection_str_list **)custom;
				if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_STRING_LIST, fs->value->v.l, NULL)) {
					log_error("String list value found in selection is reserved.");
					goto error;
				}
				break;
			case DM_REPORT_FIELD_TYPE_TIME:
				if (rvw->value) {
					fs->value->v.t = *(const time_t *) rvw->value;
					if (rvw->reserved->type & DM_REPORT_FIELD_RESERVED_VALUE_RANGE)
						fs->value->next->v.t = (((const time_t *) rvw->value)[1]);
				} else {
					if (!(tval = (struct time_value *) custom))
						goto_bad;
					fs->value->v.t = tval->t1;
					if (tval->range)
						fs->value->next->v.t = tval->t2;
					if (_check_value_is_strictly_reserved(rh, field_num, DM_REPORT_FIELD_TYPE_TIME, &fs->value->v.t, NULL)) {
						log_error("Time value found in selection is reserved.");
						goto error;
					}
				}
				break;
			default:
				log_error(INTERNAL_ERROR "_create_field_selection: incorrect type %" PRIu32 " for field %s",
					  flags & DM_REPORT_FIELD_TYPE_MASK, field_id);
				goto error;
		}
	}

	return fs;
error_field_id:
	log_error("dm_report: struct field_selection_value allocation failed for selection field %s",
		  field_id);
	goto error;
bad:
	log_error(INTERNAL_ERROR "_create_field_selection: Forbidden NULL custom detected.");
error:
	dm_pool_free(rh->selection->mem, fs);

	return NULL;
}

static struct selection_node *_alloc_selection_node(struct dm_pool *mem, uint32_t type)
{
	struct selection_node *sn;

	if (!(sn = dm_pool_zalloc(mem, sizeof(struct selection_node)))) {
		log_error("dm_report: struct selection_node allocation failed");
		return NULL;
	}

	dm_list_init(&sn->list);
	sn->type = type;
	if (!(type & SEL_ITEM))
		dm_list_init(&sn->selection.set);

	return sn;
}

static void _display_selection_help(struct dm_report *rh)
{
	static const char _grow_object_failed_msg[] = "_display_selection_help: dm_pool_grow_object failed";
	const struct op_def *t;
	const struct dm_report_reserved_value *rv;
	size_t len_all, len_final = 0;
	const char **rvs;
	char *rvs_all;

	log_warn("Selection operands");
	log_warn("------------------");
	log_warn("  field               - Reporting field.");
	log_warn("  number              - Non-negative integer value.");
	log_warn("  size                - Floating point value with units, 'm' unit used by default if not specified.");
	log_warn("  percent             - Non-negative integer with or without %% suffix.");
	log_warn("  string              - Characters quoted by \' or \" or unquoted.");
	log_warn("  string list         - Strings enclosed by [ ] or { } and elements delimited by either");
	log_warn("                        \"all items must match\" or \"at least one item must match\" operator.");
	log_warn("  regular expression  - Characters quoted by \' or \" or unquoted.");
	log_warn(" ");
	if (rh->reserved_values) {
		log_warn("Reserved values");
		log_warn("---------------");

		for (rv = rh->reserved_values; rv->type; rv++) {
			for (len_all = 0, rvs = rv->names; *rvs; rvs++)
				len_all += strlen(*rvs) + 2;
			if (len_all > len_final)
				len_final = len_all;
		}

		for (rv = rh->reserved_values; rv->type; rv++) {
			if (!dm_pool_begin_object(rh->mem, 256)) {
				log_error("_display_selection_help: dm_pool_begin_object failed");
				break;
			}
			for (rvs = rv->names; *rvs; rvs++) {
				if (((rvs != rv->names) && !dm_pool_grow_object(rh->mem, ", ", 2)) ||
				    !dm_pool_grow_object(rh->mem, *rvs, strlen(*rvs))) {
					log_error(_grow_object_failed_msg);
					goto out_reserved_values;
				}
			}
			if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
				log_error(_grow_object_failed_msg);
				goto out_reserved_values;
			}
			rvs_all = dm_pool_end_object(rh->mem);

			log_warn("  %-*s - %s [%s]", (int) len_final, rvs_all, rv->description,
						     _get_field_type_name(rv->type));
			dm_pool_free(rh->mem, rvs_all);
		}
		log_warn(" ");
	}
out_reserved_values:
	log_warn("Selection operators");
	log_warn("-------------------");
	log_warn("  Comparison operators:");
	t = _op_cmp;
	for (; t->string; t++)
		log_warn("    %6s  - %s", t->string, t->desc);
	log_warn(" ");
	log_warn("  Logical and grouping operators:");
	t = _op_log;
	for (; t->string; t++)
		log_warn("    %4s  - %s", t->string, t->desc);
	log_warn(" ");
}

static void _parse_syntax_error(const char *s)
{
	log_error("Selection syntax error at '%s'.", s);
	log_error("Use \'help\' for selection to get more help.");
}

/*
 * Selection parser
 *
 * _parse_* functions
 *
 *   Input:
 *     s             - a pointer to the parsed string
 *   Output:
 *     next          - a pointer used for next _parse_*'s input,
 *                     next == s if return value is NULL
 *     return value  - a filter node pointer,
 *                     NULL if s doesn't match
 */

/*
 * SELECTION := FIELD_NAME OP_CMP STRING |
 *              FIELD_NAME OP_CMP NUMBER  |
 *              FIELD_NAME OP_REGEX REGEX
 */
static struct selection_node *_parse_selection(struct dm_report *rh,
					       const char *s,
					       const char **next)
{
	struct field_selection *fs;
	struct selection_node *sn;
	const char *ws, *we; /* field name */
	const char *vs = NULL, *ve = NULL; /* value */
	const char *last;
	uint32_t flags, field_num;
	int implicit;
	const struct dm_report_field_type *ft;
	struct selection_str_list *str_list;
	struct reserved_value_wrapper rvw = {0};
	struct time_value tval;
	uint64_t factor;
	void *custom = NULL;
	char *tmp;
	char c;

	/* get field name */
	if (!(last = _tok_field_name(s, &ws, &we))) {
		log_error("Expecting field name");
		goto bad;
	}

	/* check if the field with given name exists */
	if (!_get_field(rh, ws, (size_t) (we - ws), &field_num, &implicit)) {
		c = we[0];
		tmp = (char *) we;
		tmp[0] = '\0';
		_display_fields(rh, 0, 1);
		log_warn(" ");
		log_error("Unrecognised selection field: %s", ws);
		tmp[0] = c;
		goto bad;
	}

	if (implicit) {
		ft = &_implicit_report_fields[field_num];
		if (ft->flags & FLD_CMP_UNCOMPARABLE) {
			c = we[0];
			tmp = (char *) we;
			tmp[0] = '\0';
			_display_fields(rh, 0, 1);
			log_warn(" ");
			log_error("Selection field is uncomparable: %s.", ws);
			tmp[0] = c;
			goto bad;
		}
	} else
		ft = &rh->fields[field_num];

	/* get comparison operator */
	if (!(flags = _tok_op_cmp(we, &last))) {
		_display_selection_help(rh);
		log_error("Unrecognised comparison operator: %s", we);
		goto bad;
	}
	if (!last) {
		_display_selection_help(rh);
		log_error("Missing value after operator");
		goto bad;
	}

	/* check we can use the operator with the field */
	if (flags & FLD_CMP_REGEX) {
		if (!(ft->flags & (DM_REPORT_FIELD_TYPE_STRING |
				   DM_REPORT_FIELD_TYPE_STRING_LIST))) {
			_display_selection_help(rh);
			log_error("Operator can be used only with string or string list fields: %s", ws);
			goto bad;
		}
	} else if (flags & FLD_CMP_NUMBER) {
		if (!(ft->flags & (DM_REPORT_FIELD_TYPE_NUMBER |
				   DM_REPORT_FIELD_TYPE_SIZE |
				   DM_REPORT_FIELD_TYPE_PERCENT |
				   DM_REPORT_FIELD_TYPE_TIME))) {
			_display_selection_help(rh);
			log_error("Operator can be used only with number, size, time or percent fields: %s", ws);
			goto bad;
		}
	} else if (flags & FLD_CMP_TIME) {
		if (!(ft->flags & DM_REPORT_FIELD_TYPE_TIME)) {
			_display_selection_help(rh);
			log_error("Operator can be used only with time fields: %s", ws);
			goto bad;
		}
	}

	/* assign custom structures to hold extra information for specific value types */
	if (ft->flags == DM_REPORT_FIELD_TYPE_SIZE ||
	    ft->flags == DM_REPORT_FIELD_TYPE_NUMBER ||
	    ft->flags == DM_REPORT_FIELD_TYPE_PERCENT)
		custom = &factor;
	else if (ft->flags & DM_REPORT_FIELD_TYPE_TIME)
		custom = &tval;
	else if (ft->flags == DM_REPORT_FIELD_TYPE_STRING_LIST)
		custom = &str_list;
	else
		custom = NULL;

	/* get value to compare with */
	if (!(last = _tok_value(rh, ft, field_num, implicit,
				last, &vs, &ve, &flags,
				&rvw, rh->selection->mem, custom)))
		goto_bad;

	*next = _skip_space(last);

	/* create selection */
	if (!(fs = _create_field_selection(rh, field_num, implicit, vs, (size_t) (ve - vs), flags, &rvw, custom)))
		return_NULL;

	/* create selection node */
	if (!(sn = _alloc_selection_node(rh->selection->mem, SEL_ITEM)))
		return_NULL;

	/* add selection to selection node */
	sn->selection.item = fs;

	return sn;
bad:
	_parse_syntax_error(s);
	*next = s;
	return NULL;
}

static struct selection_node *_parse_or_ex(struct dm_report *rh,
					   const char *s,
					   const char **next,
					   struct selection_node *or_sn);

static struct selection_node *_parse_ex(struct dm_report *rh,
					const char *s,
					const char **next)
{
	static const char _pe_expected_msg[] = "Syntax error: right parenthesis expected at \'%s\'";
	struct selection_node *sn = NULL;
	uint32_t t;
	const char *tmp = NULL;

	t = _tok_op_log(s, next, SEL_MODIFIER_NOT | SEL_PRECEDENCE_PS);
	if (t == SEL_MODIFIER_NOT) {
		/* '!' '(' EXPRESSION ')' */
		if (!_tok_op_log(*next, &tmp, SEL_PRECEDENCE_PS)) {
			log_error("Syntax error: left parenthesis expected at \'%s\'", *next);
			goto error;
		}
		if (!(sn = _parse_or_ex(rh, tmp, next, NULL)))
			goto error;
		sn->type |= SEL_MODIFIER_NOT;
		if (!_tok_op_log(*next, &tmp, SEL_PRECEDENCE_PE)) {
			log_error(_pe_expected_msg, *next);
			goto error;
		}
		*next = tmp;
	} else if (t == SEL_PRECEDENCE_PS) {
		/* '(' EXPRESSION ')' */
		if (!(sn = _parse_or_ex(rh, *next, &tmp, NULL)))
			goto error;
		if (!_tok_op_log(tmp, next, SEL_PRECEDENCE_PE)) {
			log_error(_pe_expected_msg, *next);
			goto error;
		}
	} else if ((s = _skip_space(s))) {
		/* SELECTION */
		sn = _parse_selection(rh, s, next);
	} else {
		sn = NULL;
		*next = s;
	}

	return sn;
error:
	*next = s;
	return NULL;
}

/* AND_EXPRESSION := EX (AND_OP AND_EXPRESSION) */
static struct selection_node *_parse_and_ex(struct dm_report *rh,
					    const char *s,
					    const char **next,
					    struct selection_node *and_sn)
{
	struct selection_node *n;
	const char *tmp = NULL;

	n = _parse_ex(rh, s, next);
	if (!n)
		goto error;

	if (!_tok_op_log(*next, &tmp, SEL_AND)) {
		if (!and_sn)
			return n;
		dm_list_add(&and_sn->selection.set, &n->list);
		return and_sn;
	}

	if (!and_sn) {
		if (!(and_sn = _alloc_selection_node(rh->selection->mem, SEL_AND)))
			goto error;
	}
	dm_list_add(&and_sn->selection.set, &n->list);

	return _parse_and_ex(rh, tmp, next, and_sn);
error:
	*next = s;
	return NULL;
}

/* OR_EXPRESSION := AND_EXPRESSION (OR_OP OR_EXPRESSION) */
static struct selection_node *_parse_or_ex(struct dm_report *rh,
					   const char *s,
					   const char **next,
					   struct selection_node *or_sn)
{
	struct selection_node *n;
	const char *tmp = NULL;

	n = _parse_and_ex(rh, s, next, NULL);
	if (!n)
		goto error;

	if (!_tok_op_log(*next, &tmp, SEL_OR)) {
		if (!or_sn)
			return n;
		dm_list_add(&or_sn->selection.set, &n->list);
		return or_sn;
	}

	if (!or_sn) {
		if (!(or_sn = _alloc_selection_node(rh->selection->mem, SEL_OR)))
			goto error;
	}
	dm_list_add(&or_sn->selection.set, &n->list);

	return _parse_or_ex(rh, tmp, next, or_sn);
error:
	*next = s;
	return NULL;
}

static int _alloc_rh_selection(struct dm_report *rh)
{
	if (!(rh->selection = dm_pool_zalloc(rh->mem, sizeof(struct selection))) ||
	    !(rh->selection->mem = dm_pool_create("report selection", 1024))) {
		log_error("Failed to allocate report selection structure.");
		if (rh->selection)
			dm_pool_free(rh->mem, rh->selection);
		return 0;
	}

	return 1;
}

#define SPECIAL_SELECTION_ALL "all"

static int _report_set_selection(struct dm_report *rh, const char *selection, int add_new_fields)
{
	struct selection_node *root = NULL;
	const char *fin = NULL, *next;

	if (rh->selection) {
		if (rh->selection->selection_root)
			/* Trash any previous selection. */
			dm_pool_free(rh->selection->mem, rh->selection->selection_root);
		rh->selection->selection_root = NULL;
	} else {
		if (!_alloc_rh_selection(rh))
			goto_bad;
	}

	if (!selection || !selection[0] || !strcasecmp(selection, SPECIAL_SELECTION_ALL))
		return 1;

	rh->selection->add_new_fields = add_new_fields;

	if (!(root = _alloc_selection_node(rh->selection->mem, SEL_OR)))
		return 0;

	if (!_parse_or_ex(rh, selection, &fin, root) || !fin)
		goto_bad;

	next = _skip_space(fin);
	if (*next) {
		log_error("Expecting logical operator");
		_parse_syntax_error(next);
		goto bad;
	}

	rh->selection->selection_root = root;
	return 1;
bad:
	dm_pool_free(rh->selection->mem, root);
	return 0;
}

static void _reset_field_props(struct dm_report *rh)
{
	struct field_properties *fp;
	dm_list_iterate_items(fp, &rh->field_props)
		fp->width = fp->initial_width;
	rh->flags |= RH_FIELD_CALC_NEEDED;
}

int dm_report_set_selection(struct dm_report *rh, const char *selection)
{
	struct row *row;

	if (!_report_set_selection(rh, selection, 0))
		return_0;

	_reset_field_props(rh);

	dm_list_iterate_items(row, &rh->rows) {
		row->selected = _check_report_selection(rh, &row->fields);
		if (row->field_sel_status)
			_implicit_report_fields[row->field_sel_status->props->field_num].report_fn(rh,
							rh->mem, row->field_sel_status, row, rh->private);
	}

	return 1;
}

struct dm_report *dm_report_init_with_selection(uint32_t *report_types,
						const struct dm_report_object_type *types,
						const struct dm_report_field_type *fields,
						const char *output_fields,
						const char *output_separator,
						uint32_t output_flags,
						const char *sort_keys,
						const char *selection,
						const struct dm_report_reserved_value reserved_values[],
						void *private_data)
{
	struct dm_report *rh;

	_implicit_report_fields = _implicit_special_report_fields_with_selection;

	if (!(rh = dm_report_init(report_types, types, fields, output_fields,
			output_separator, output_flags, sort_keys, private_data)))
		return NULL;

	if (!selection || !selection[0]) {
		rh->selection = NULL;
		return rh;
	}

	if (!_check_reserved_values_supported(fields, reserved_values)) {
		log_error(INTERNAL_ERROR "dm_report_init_with_selection: "
			  "trying to register unsupported reserved value type, "
			  "skipping report selection");
		return rh;
	}
	rh->reserved_values = reserved_values;

	if (!strcasecmp(selection, SPECIAL_FIELD_HELP_ID) ||
	    !strcmp(selection, SPECIAL_FIELD_HELP_ALT_ID)) {
		_display_fields(rh, 0, 1);
		log_warn(" ");
		_display_selection_help(rh);
		rh->flags |= RH_ALREADY_REPORTED;
		return rh;
	}

	if (!_report_set_selection(rh, selection, 1))
		goto_bad;

	_dm_report_init_update_types(rh, report_types);

	return rh;
bad:
	dm_report_free(rh);
	return NULL;
}

/*
 * Print row of headings
 */
static int _report_headings(struct dm_report *rh)
{
	const struct dm_report_field_type *fields;
	struct field_properties *fp;
	const char *heading;
	char *buf = NULL;
	size_t buf_size = 0;

	rh->flags |= RH_HEADINGS_PRINTED;

	if (!(rh->flags & DM_REPORT_OUTPUT_HEADINGS))
		return 1;

	if (!dm_pool_begin_object(rh->mem, 128)) {
		log_error("dm_report: "
			  "dm_pool_begin_object failed for headings");
		return 0;
	}

	dm_list_iterate_items(fp, &rh->field_props) {
		if ((int) buf_size < fp->width)
			buf_size = (size_t) fp->width;
	}
	/* Including trailing '\0'! */
	buf_size++;

	if (!(buf = dm_malloc(buf_size))) {
		log_error("dm_report: Could not allocate memory for heading buffer.");
		goto bad;
	}

	/* First heading line */
	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->flags & FLD_HIDDEN)
			continue;

		fields = fp->implicit ? _implicit_report_fields : rh->fields;

		heading = rh->flags & DM_REPORT_OUTPUT_FIELD_IDS_IN_HEADINGS ?
				fields[fp->field_num].id : fields[fp->field_num].heading;

		if (rh->flags & DM_REPORT_OUTPUT_ALIGNED) {
			if (dm_snprintf(buf, buf_size, "%-*.*s",
					 fp->width, fp->width, heading) < 0) {
				log_error("dm_report: snprintf heading failed");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, buf, fp->width)) {
				log_error("dm_report: Failed to generate report headings for printing");
				goto bad;
			}
		} else if (!dm_pool_grow_object(rh->mem, heading, 0)) {
			log_error("dm_report: Failed to generate report headings for printing");
			goto bad;
		}

		if (!dm_list_end(&rh->field_props, &fp->list))
			if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
				log_error("dm_report: Failed to generate report headings for printing");
				goto bad;
			}
	}
	if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
		log_error("dm_report: Failed to generate report headings for printing");
		goto bad;
	}

	/* print all headings */
	heading = (char *) dm_pool_end_object(rh->mem);
	log_print("%s", heading);

	dm_pool_free(rh->mem, (void *)heading);
	dm_free(buf);

	return 1;

      bad:
	dm_free(buf);
	dm_pool_abandon_object(rh->mem);
	return 0;
}

static int _should_display_row(struct row *row)
{
	return row->field_sel_status || row->selected;
}

static void _recalculate_fields(struct dm_report *rh)
{
	struct row *row;
	struct dm_report_field *field;
	int len;
	int id_len;

	dm_list_iterate_items(row, &rh->rows) {
		dm_list_iterate_items(field, &row->fields) {
			if ((rh->flags & RH_SORT_REQUIRED) &&
			    (field->props->flags & FLD_SORT_KEY)) {
				(*row->sort_fields)[field->props->sort_posn] = field;
			}

			if (_should_display_row(row)) {
				len = (int) strlen(field->report_string);
				if ((len > field->props->width))
					field->props->width = len;

			}

			if (rh->flags & DM_REPORT_OUTPUT_FIELD_IDS_IN_HEADINGS) {
				id_len = (int) strlen(rh->fields[field->props->field_num].id);
				if (field->props->width < id_len)
					field->props->width = id_len;
			}
		}
	}

	rh->flags &= ~RH_FIELD_CALC_NEEDED;
}

int dm_report_column_headings(struct dm_report *rh)
{
	/* Columns-as-rows does not use _report_headings. */
	if (rh->flags & DM_REPORT_OUTPUT_COLUMNS_AS_ROWS)
		return 1;

	if (rh->flags & RH_FIELD_CALC_NEEDED)
		_recalculate_fields(rh);

	return _report_headings(rh);
}

/*
 * Sort rows of data
 */
static int _row_compare(const void *a, const void *b)
{
	const struct row *rowa = *(const struct row * const *) a;
	const struct row *rowb = *(const struct row * const *) b;
	const struct dm_report_field *sfa, *sfb;
	uint32_t cnt;

	for (cnt = 0; cnt < rowa->rh->keys_count; cnt++) {
		sfa = (*rowa->sort_fields)[cnt];
		sfb = (*rowb->sort_fields)[cnt];
		if (sfa->props->flags &
		    ((DM_REPORT_FIELD_TYPE_NUMBER) |
		     (DM_REPORT_FIELD_TYPE_SIZE) |
		     (DM_REPORT_FIELD_TYPE_TIME))) {
			const uint64_t numa =
			    *(const uint64_t *) sfa->sort_value;
			const uint64_t numb =
			    *(const uint64_t *) sfb->sort_value;

			if (numa == numb)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (numa > numb) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (numa < numb) ? 1 : -1;
			}
		} else {
			/* DM_REPORT_FIELD_TYPE_STRING
			 * DM_REPORT_FIELD_TYPE_STRING_LIST */
			const char *stra = (const char *) sfa->sort_value;
			const char *strb = (const char *) sfb->sort_value;
			int cmp = strcmp(stra, strb);

			if (!cmp)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (cmp > 0) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (cmp < 0) ? 1 : -1;
			}
		}
	}

	return 0;		/* Identical */
}

static int _sort_rows(struct dm_report *rh)
{
	struct row *(*rows)[];
	uint32_t count = 0;
	struct row *row;
	size_t cnt_rows;

	if (!(cnt_rows = dm_list_size(&rh->rows)))
		return 1; /* nothing to sort */

	if (!(rows = dm_pool_alloc(rh->mem, sizeof(**rows) * cnt_rows))) {
		log_error("dm_report: sort array allocation failed");
		return 0;
	}

	dm_list_iterate_items(row, &rh->rows)
		(*rows)[count++] = row;

	qsort(rows, count, sizeof(**rows), _row_compare);

	dm_list_init(&rh->rows);

	while (count > 0)
		dm_list_add_h(&rh->rows, &(*rows)[--count]->list);

	return 1;
}

#define STANDARD_QUOTE		"\'"
#define STANDARD_PAIR		"="

#define JSON_INDENT_UNIT       4
#define JSON_SPACE             " "
#define JSON_QUOTE             "\""
#define JSON_PAIR              ":"
#define JSON_SEPARATOR         ","
#define JSON_OBJECT_START      "{"
#define JSON_OBJECT_END        "}"
#define JSON_ARRAY_START       "["
#define JSON_ARRAY_END         "]"
#define JSON_ESCAPE_CHAR       "\\"
#define JSON_NULL              "null"

#define UNABLE_TO_EXTEND_OUTPUT_LINE_MSG "dm_report: Unable to extend output line"

static int _is_basic_report(struct dm_report *rh)
{
	return rh->group_item &&
	       (rh->group_item->group->type == DM_REPORT_GROUP_BASIC);
}

static int _is_json_std_report(struct dm_report *rh)
{
	return rh->group_item &&
	       rh->group_item->group->type == DM_REPORT_GROUP_JSON_STD;
}

static int _is_json_report(struct dm_report *rh)
{
	return rh->group_item &&
	       (rh->group_item->group->type == DM_REPORT_GROUP_JSON ||
		rh->group_item->group->type == DM_REPORT_GROUP_JSON_STD);
}

static int _is_pure_numeric_field(struct dm_report_field *field)
{
	return field->props->flags & (DM_REPORT_FIELD_TYPE_NUMBER | DM_REPORT_FIELD_TYPE_PERCENT);
}

static const char *_get_field_id(struct dm_report *rh, struct dm_report_field *field)
{
	const struct dm_report_field_type *fields = field->props->implicit ? _implicit_report_fields
									   : rh->fields;

	return fields[field->props->field_num].id;
}

static int _output_field_basic_fmt(struct dm_report *rh, struct dm_report_field *field)
{
	char buf_local[8192];
	char *field_id;
	int32_t width;
	uint32_t align;
	char *buf = NULL;
	size_t buf_size = 0;

	if (rh->flags & DM_REPORT_OUTPUT_FIELD_NAME_PREFIX) {
		buf_size = strlen(_get_field_id(rh, field)) + 1;
		if (buf_size >= sizeof(buf_local)) {
			/* for field names our buf_local should be enough */
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}

		field_id = buf_local;
		memcpy(field_id, _get_field_id(rh, field), buf_size);

		if (!dm_pool_grow_object(rh->mem, rh->output_field_name_prefix, 0)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}

		if (!dm_pool_grow_object(rh->mem, _toupperstr(field_id), 0)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}

		if (!dm_pool_grow_object(rh->mem, STANDARD_PAIR, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}

		if (!(rh->flags & DM_REPORT_OUTPUT_FIELD_UNQUOTED) &&
		    !dm_pool_grow_object(rh->mem, STANDARD_QUOTE, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}
	}

	if (rh->flags & DM_REPORT_OUTPUT_ALIGNED) {
		if (!(align = field->props->flags & DM_REPORT_FIELD_ALIGN_MASK))
			align = ((field->props->flags & DM_REPORT_FIELD_TYPE_NUMBER) ||
				 (field->props->flags & DM_REPORT_FIELD_TYPE_SIZE)) ?
				DM_REPORT_FIELD_ALIGN_RIGHT : DM_REPORT_FIELD_ALIGN_LEFT;

		width = field->props->width;

		/* Including trailing '\0'! */
		buf_size = width + 1;
		if (buf_size < sizeof(buf_local))
			/* Use local buffer on stack for smaller strings */
			buf = buf_local;
		else if (!(buf = dm_malloc(buf_size))) {
			log_error("dm_report: Could not allocate memory for output line buffer.");
			return 0;
		}

		if (align & DM_REPORT_FIELD_ALIGN_LEFT) {
			if (dm_snprintf(buf, buf_size, "%-*.*s",
					 width, width, field->report_string) < 0) {
				log_error("dm_report: left-aligned snprintf() failed");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, buf, width)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				goto bad;
			}
		} else if (align & DM_REPORT_FIELD_ALIGN_RIGHT) {
			if (dm_snprintf(buf, buf_size, "%*.*s",
					 width, width, field->report_string) < 0) {
				log_error("dm_report: right-aligned snprintf() failed");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, buf, width)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				goto bad;
			}
		}
	} else {
		if (!dm_pool_grow_object(rh->mem, field->report_string, 0)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}
	}

	if (rh->flags & DM_REPORT_OUTPUT_FIELD_NAME_PREFIX) {
		if (!(rh->flags & DM_REPORT_OUTPUT_FIELD_UNQUOTED)) {
			if (!dm_pool_grow_object(rh->mem, STANDARD_QUOTE, 1)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				goto bad;
			}
		}
	}

	if (buf != buf_local)
		dm_free(buf);
	return 1;
bad:
	if (buf != buf_local)
		dm_free(buf);
	return 0;
}

static int _safe_repstr_output(struct dm_report *rh, const char *repstr, size_t len)
{
	const char *p_repstr;
	const char *repstr_end = len ? repstr + len : repstr + strlen(repstr);

	/* Escape any JSON_QUOTE that may appear in reported string. */
	while (1) {
		if (!(p_repstr = memchr(repstr, JSON_QUOTE[0], repstr_end - repstr)))
			break;

		if (p_repstr > repstr) {
			if (!dm_pool_grow_object(rh->mem, repstr, p_repstr - repstr)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				return 0;
			}
		}
		if (!dm_pool_grow_object(rh->mem, JSON_ESCAPE_CHAR, 1) ||
		    !dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}
		repstr = p_repstr + 1;
	}

	if (!dm_pool_grow_object(rh->mem, repstr, repstr_end - repstr)) {
		log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
		return 0;
	}

	return 1;
}

static int _output_field_json_fmt(struct dm_report *rh, struct dm_report_field *field)
{
	const char *repstr;
	size_t list_size, i;
	struct pos_len *pos_len;

	if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1) ||
	    !dm_pool_grow_object(rh->mem, _get_field_id(rh, field),  0) ||
	    !dm_pool_grow_object(rh->mem, JSON_QUOTE, 1) ||
	    !dm_pool_grow_object(rh->mem, JSON_PAIR, 1)) {
		log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
		return 0;
	}

	if (field->props->flags & DM_REPORT_FIELD_TYPE_STRING_LIST) {
		if (!_is_json_std_report(rh)) {

			/* string list in JSON - report whole list as simple string in quotes */

			if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				return 0;
			}

			if (!_safe_repstr_output(rh, field->report_string, 0))
				return_0;

			if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				return 0;
			}

			return 1;
		}

		/* string list in JSON_STD - report list as proper JSON array */

		if (!dm_pool_grow_object(rh->mem, JSON_ARRAY_START, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}

		if (*field->report_string != 0) {
			pos_len = (struct pos_len *) (field->report_string +
				  ((struct str_list_sort_value *) field->sort_value)->items[0].len + 1);
			list_size = pos_len->pos;
		} else
			list_size = 0;

		for (i = 0; i < list_size; i++) {
			pos_len++;

			if (i != 0) {
				if (!dm_pool_grow_object(rh->mem, JSON_SEPARATOR, 1)) {
					log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
					return 0;
				}
			}

			if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				return 0;
			}

			if (!_safe_repstr_output(rh, field->report_string + pos_len->pos, pos_len->len))
				return_0;

			if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				return 0;
			}
		}

		if (!dm_pool_grow_object(rh->mem, JSON_ARRAY_END, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}

		return 1;
	}

	/* all other types than string list - handle both JSON and JSON_STD */

	if (!(_is_json_std_report(rh) && _is_pure_numeric_field(field))) {
		if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}
	}

	if (_is_json_std_report(rh) && _is_pure_numeric_field(field) && !*field->report_string)
		repstr = JSON_NULL;
	else
		repstr = field->report_string;

	if (!_safe_repstr_output(rh, repstr, 0))
		return_0;

	if (!(_is_json_std_report(rh) && _is_pure_numeric_field(field))) {
		if (!dm_pool_grow_object(rh->mem, JSON_QUOTE, 1)) {
			log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
			return 0;
		}
	}

	return 1;
}

/*
 * Produce report output
 */
static int _output_field(struct dm_report *rh, struct dm_report_field *field)
{
	return _is_json_report(rh) ? _output_field_json_fmt(rh, field)
				   : _output_field_basic_fmt(rh, field);
}

static void _destroy_rows(struct dm_report *rh)
{
	/*
	 * free the first row allocated to this report: since this is a
	 * pool allocation this will also free all subsequently allocated
	 * rows from the report and any associated string data.
	 */
	if (rh->first_row)
		dm_pool_free(rh->mem, rh->first_row);
	rh->first_row = NULL;
	dm_list_init(&rh->rows);

	/* Reset field widths to original values. */
	_reset_field_props(rh);
}

static int _output_as_rows(struct dm_report *rh)
{
	const struct dm_report_field_type *fields;
	struct field_properties *fp;
	struct dm_report_field *field;
	struct row *row;
	const char *heading;

	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->flags & FLD_HIDDEN) {
			dm_list_iterate_items(row, &rh->rows) {
				if (dm_list_empty(&row->fields))
					continue;
				field = dm_list_item(dm_list_first(&row->fields), struct dm_report_field);
				dm_list_del(&field->list);
			}
			continue;
		}

		fields = fp->implicit ? _implicit_report_fields : rh->fields;

		if (!dm_pool_begin_object(rh->mem, 512)) {
			log_error("dm_report: Unable to allocate output line");
			return 0;
		}

		if ((rh->flags & DM_REPORT_OUTPUT_HEADINGS)) {
			heading = rh->flags & DM_REPORT_OUTPUT_FIELD_IDS_IN_HEADINGS ?
				fields[fp->field_num].id : fields[fp->field_num].heading;

			if (!dm_pool_grow_object(rh->mem, heading, 0)) {
				log_error("dm_report: Failed to extend row for field name");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
				log_error("dm_report: Failed to extend row with separator");
				goto bad;
			}
		}

		dm_list_iterate_items(row, &rh->rows) {
			if ((field = dm_list_item(dm_list_first(&row->fields), struct dm_report_field))) {
				if (!_output_field(rh, field))
					goto bad;
				dm_list_del(&field->list);
			}

			if (!dm_list_end(&rh->rows, &row->list))
				if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
					log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
					goto bad;
				}
		}

		if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
			log_error("dm_report: Failed to terminate row");
			goto bad;
		}
		log_print("%s", (char *) dm_pool_end_object(rh->mem));
	}

	_destroy_rows(rh);

	return 1;

      bad:
	dm_pool_abandon_object(rh->mem);
	return 0;
}

static struct dm_list *_get_last_displayed_rowh(struct dm_report *rh)
{
	struct dm_list *rowh;
	struct row *row;

	/*
	 * We need to find 'last displayed row', not just 'last row'.
	 *
	 * This is because the report may be marked with
	 * DM_REPORT_OUTPUT_MULTIPLE_TIMES flag. In that case, the report
	 * may be used more than once and with different selection
	 * criteria each time. Therefore, such report may also contain
	 * rows which we do not display on output with current selection
	 * criteria.
	 */
	for (rowh = dm_list_last(&rh->rows); rowh; rowh = dm_list_prev(&rh->rows, rowh)) {
		row = dm_list_item(rowh, struct row);
		if (_should_display_row(row))
			return rowh;
	}

	return NULL;
}

static int _output_as_columns(struct dm_report *rh)
{
	struct dm_list *fh, *rowh, *ftmp, *rtmp;
	struct row *row = NULL;
	struct dm_report_field *field;
	struct dm_list *last_rowh;
	int do_field_delim;
	int is_json_report = _is_json_report(rh);
	char *line;

	/* If headings not printed yet, calculate field widths and print them */
	if (!(rh->flags & RH_HEADINGS_PRINTED))
		_report_headings(rh);

	/* Print and clear buffer */
	last_rowh = _get_last_displayed_rowh(rh);
	dm_list_iterate_safe(rowh, rtmp, &rh->rows) {
		row = dm_list_item(rowh, struct row);

		if (!_should_display_row(row))
			continue;

		if (!dm_pool_begin_object(rh->mem, 512)) {
			log_error("dm_report: Unable to allocate output line");
			return 0;
		}

		if (is_json_report) {
			if (!dm_pool_grow_object(rh->mem, JSON_OBJECT_START, 0)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				goto bad;
			}
		}

		do_field_delim = 0;

		dm_list_iterate_safe(fh, ftmp, &row->fields) {
			field = dm_list_item(fh, struct dm_report_field);
			if (field->props->flags & FLD_HIDDEN)
				continue;

			if (do_field_delim) {
				if (is_json_report) {
					if (!dm_pool_grow_object(rh->mem, JSON_SEPARATOR, 0) ||
					    !dm_pool_grow_object(rh->mem, JSON_SPACE, 0)) {
						log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
						goto bad;
					}
				} else {
					if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
						log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
						goto bad;
					}
				}
			} else
				do_field_delim = 1;

			if (!_output_field(rh, field))
				goto bad;

			if (!(rh->flags & DM_REPORT_OUTPUT_MULTIPLE_TIMES))
				dm_list_del(&field->list);
		}

		if (is_json_report) {
			if (!dm_pool_grow_object(rh->mem, JSON_OBJECT_END, 0)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				goto bad;
			}
			if (rowh != last_rowh &&
			    !dm_pool_grow_object(rh->mem, JSON_SEPARATOR, 0)) {
				log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
				goto bad;
			}
		}

		if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
			log_error("dm_report: Unable to terminate output line");
			goto bad;
		}

		line = (char *) dm_pool_end_object(rh->mem);
		log_print("%*s", rh->group_item ? rh->group_item->group->indent + (int) strlen(line) : 0, line);
		if (!(rh->flags & DM_REPORT_OUTPUT_MULTIPLE_TIMES))
			dm_list_del(&row->list);
	}

	if (!(rh->flags & DM_REPORT_OUTPUT_MULTIPLE_TIMES))
		_destroy_rows(rh);

	return 1;

      bad:
	dm_pool_abandon_object(rh->mem);
	return 0;
}

int dm_report_is_empty(struct dm_report *rh)
{
	return dm_list_empty(&rh->rows) ? 1 : 0;
}

static struct report_group_item *_get_topmost_report_group_item(struct dm_report_group *group)
{
	struct report_group_item *item;

	if (group && !dm_list_empty(&group->items))
		item = dm_list_item(dm_list_first(&group->items), struct report_group_item);
	else
		item = NULL;

	return item;
}

static void _json_output_start(struct dm_report_group *group)
{
	if (!group->indent) {
		log_print(JSON_OBJECT_START);
		group->indent += JSON_INDENT_UNIT;
	}
}

static int _json_output_array_start(struct dm_pool *mem, struct report_group_item *item)
{
	const char *name = (const char *) item->data;
	char *output;

	if (!dm_pool_begin_object(mem, 32)) {
		log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
		return 0;
	}

	if (!dm_pool_grow_object(mem, JSON_QUOTE, 1) ||
	    !dm_pool_grow_object(mem, name, 0) ||
	    !dm_pool_grow_object(mem, JSON_QUOTE JSON_PAIR JSON_SPACE JSON_ARRAY_START, 0) ||
	    !dm_pool_grow_object(mem, "\0", 1) ||
	    !(output = dm_pool_end_object(mem))) {
		log_error(UNABLE_TO_EXTEND_OUTPUT_LINE_MSG);
		goto bad;
	}

	if (item->parent->store.finished_count > 0)
		log_print("%*s", item->group->indent + (int) sizeof(JSON_SEPARATOR) - 1, JSON_SEPARATOR);

	if (item->parent->parent && item->parent->data) {
		log_print("%*s", item->group->indent + (int) sizeof(JSON_OBJECT_START) - 1, JSON_OBJECT_START);
		item->group->indent += JSON_INDENT_UNIT;
	}

	log_print("%*s", item->group->indent + (int) strlen(output), output);
	item->group->indent += JSON_INDENT_UNIT;

	dm_pool_free(mem, output);
	return 1;
bad:
	dm_pool_abandon_object(mem);
	return 0;
}

static int _prepare_json_report_output(struct dm_report *rh)
{
	_json_output_start(rh->group_item->group);

	if (rh->group_item->output_done && dm_list_empty(&rh->rows))
		return 1;

	/*
	 * If this report is in JSON group, it must be at the
	 * top of the stack of reports so the output from
	 * different reports do not interleave with each other.
	 */
	if (_get_topmost_report_group_item(rh->group_item->group) != rh->group_item) {
		log_error("dm_report: dm_report_output: interleaved reports detected for JSON output");
		return 0;
	}

	if (rh->group_item->needs_closing) {
		log_error("dm_report: dm_report_output: unfinished JSON output detected");
		return 0;
	}

	if (!_json_output_array_start(rh->mem, rh->group_item))
		return_0;

	rh->group_item->needs_closing = 1;
	return 1;
}

static int _print_basic_report_header(struct dm_report *rh)
{
	const char *report_name = (const char *) rh->group_item->data;
	size_t len = strlen(report_name);
	char *underline;

	if (!(underline = dm_pool_zalloc(rh->mem, len + 1)))
		return_0;

	memset(underline, '=', len);

	if (rh->group_item->parent->store.finished_count > 0)
		log_print("%s", "");
	log_print("%s", report_name);
	log_print("%s", underline);

	dm_pool_free(rh->mem, underline);
	return 1;
}

int dm_report_output(struct dm_report *rh)
{
	int r = 0;

	if (_is_json_report(rh) &&
	    !_prepare_json_report_output(rh))
		return_0;

	if (dm_list_empty(&rh->rows)) {
		r = 1;
		goto out;
	}

	if (rh->flags & RH_FIELD_CALC_NEEDED)
		_recalculate_fields(rh);

	if ((rh->flags & RH_SORT_REQUIRED))
		_sort_rows(rh);

	if (_is_basic_report(rh) && !_print_basic_report_header(rh))
		goto_out;

	if ((rh->flags & DM_REPORT_OUTPUT_COLUMNS_AS_ROWS))
		r = _output_as_rows(rh);
	else
		r = _output_as_columns(rh);
out:
	if (r && rh->group_item)
		rh->group_item->output_done = 1;
	return r;
}

void dm_report_destroy_rows(struct dm_report *rh)
{
	_destroy_rows(rh);
}

struct dm_report_group *dm_report_group_create(dm_report_group_type_t type, void *data)
{
	struct dm_report_group *group;
	struct dm_pool *mem;
	struct report_group_item *item;

	if (type == DM_REPORT_GROUP_JSON_STD) {
		const char * radixchar = nl_langinfo(RADIXCHAR);
		if (radixchar && strcmp(radixchar, ".")) {
			log_error("dm_report: incompatible locale used for DM_REPORT_GROUP_JSON_STD, "
				  "radix character is '%s', expected '.'", radixchar);
			return NULL;
		}
	}

	if (!(mem = dm_pool_create("report_group", 1024))) {
		log_error("dm_report: dm_report_init_group: failed to allocate mem pool");
		return NULL;
	}

	if (!(group = dm_pool_zalloc(mem, sizeof(*group)))) {
		log_error("dm_report: failed to allocate report group structure");
		goto bad;
	}

	group->mem = mem;
	group->type = type;
	dm_list_init(&group->items);

	if (!(item = dm_pool_zalloc(mem, sizeof(*item)))) {
		log_error("dm_report: failed to allocate root report group item");
		goto bad;
	}

	dm_list_add_h(&group->items, &item->list);

	return group;
bad:
	dm_pool_destroy(mem);
	return NULL;
}

static int _report_group_push_single(struct report_group_item *item, void *data)
{
	struct report_group_item *item_iter;
	unsigned count = 0;

	dm_list_iterate_items(item_iter, &item->group->items) {
		if (item_iter->report)
			count++;
	}

	if (count > 1) {
		log_error("dm_report: unable to add more than one report "
			  "to current report group");
		return 0;
	}

	return 1;
}

static int _report_group_push_basic(struct report_group_item *item, const char *name)
{
	if (item->report) {
		if (!(item->report->flags & DM_REPORT_OUTPUT_BUFFERED))
			item->report->flags &= ~(DM_REPORT_OUTPUT_MULTIPLE_TIMES);
	} else {
		if (!name && item->parent->store.finished_count > 0)
			log_print("%s", "");
	}

	return 1;
}

static int _report_group_push_json(struct report_group_item *item, const char *name)
{
	if (name && !(item->data = dm_pool_strdup(item->group->mem, name))) {
		log_error("dm_report: failed to duplicate json item name");
		return 0;
	}

	if (item->report) {
		item->report->flags &= ~(DM_REPORT_OUTPUT_ALIGNED |
					 DM_REPORT_OUTPUT_HEADINGS |
					 DM_REPORT_OUTPUT_COLUMNS_AS_ROWS);
		item->report->flags |= DM_REPORT_OUTPUT_BUFFERED;
	} else {
		_json_output_start(item->group);
		if (name) {
			if (!_json_output_array_start(item->group->mem, item))
				return_0;
		} else {
			if (!item->parent->parent) {
				log_error("dm_report: can't use unnamed object at top level of JSON output");
				return 0;
			}
			if (item->parent->store.finished_count > 0)
				log_print("%*s", item->group->indent + (int) sizeof(JSON_SEPARATOR) - 1, JSON_SEPARATOR);
			log_print("%*s", item->group->indent + (int) sizeof(JSON_OBJECT_START) - 1, JSON_OBJECT_START);
			item->group->indent += JSON_INDENT_UNIT;
		}

		item->output_done = 1;
		item->needs_closing = 1;
	}

	return 1;
}

int dm_report_group_push(struct dm_report_group *group, struct dm_report *report, void *data)
{
	struct report_group_item *item, *tmp_item;

	if (!group)
		return 1;

	if (!(item = dm_pool_zalloc(group->mem, sizeof(*item)))) {
		log_error("dm_report: dm_report_group_push: group item allocation failed");
		return 0;
	}

	if ((item->report = report)) {
		item->store.orig_report_flags = report->flags;
		report->group_item = item;
	}

	item->group = group;
	item->data = data;

	dm_list_iterate_items(tmp_item, &group->items) {
		if (!tmp_item->report) {
			item->parent = tmp_item;
			break;
		}
	}

	dm_list_add_h(&group->items, &item->list);

	switch (group->type) {
		case DM_REPORT_GROUP_SINGLE:
			if (!_report_group_push_single(item, data))
				goto_bad;
			break;
		case DM_REPORT_GROUP_BASIC:
			if (!_report_group_push_basic(item, data))
				goto_bad;
			break;
		case DM_REPORT_GROUP_JSON:
		case DM_REPORT_GROUP_JSON_STD:
			if (!_report_group_push_json(item, data))
				goto_bad;
			break;
		default:
			goto_bad;
	}

	return 1;
bad:
	dm_list_del(&item->list);
	dm_pool_free(group->mem, item);
	return 0;
}

static int _report_group_pop_single(struct report_group_item *item)
{
	return 1;
}

static int _report_group_pop_basic(struct report_group_item *item)
{
	return 1;
}

static int _report_group_pop_json(struct report_group_item *item)
{
	if (item->output_done && item->needs_closing) {
		if (item->data) {
			item->group->indent -= JSON_INDENT_UNIT;
			log_print("%*s", item->group->indent + (int) sizeof(JSON_ARRAY_END) - 1, JSON_ARRAY_END);
		}
		if (item->parent->data && item->parent->parent) {
			item->group->indent -= JSON_INDENT_UNIT;
			log_print("%*s", item->group->indent + (int) sizeof(JSON_OBJECT_END) - 1, JSON_OBJECT_END);
		}
		item->needs_closing = 0;
	}

	return 1;
}

int dm_report_group_pop(struct dm_report_group *group)
{
	struct report_group_item *item;

	if (!group)
		return 1;

	if (!(item = _get_topmost_report_group_item(group))) {
		log_error("dm_report: dm_report_group_pop: group has no items");
		return 0;
	}

	switch (group->type) {
		case DM_REPORT_GROUP_SINGLE:
			if (!_report_group_pop_single(item))
				return_0;
			break;
		case DM_REPORT_GROUP_BASIC:
			if (!_report_group_pop_basic(item))
				return_0;
			break;
		case DM_REPORT_GROUP_JSON:
		case DM_REPORT_GROUP_JSON_STD:
			if (!_report_group_pop_json(item))
				return_0;
			break;
		default:
			return 0;
        }

	dm_list_del(&item->list);

	if (item->report) {
		item->report->flags = item->store.orig_report_flags;
		item->report->group_item = NULL;
	}

	if (item->parent)
		item->parent->store.finished_count++;

	dm_pool_free(group->mem, item);
	return 1;
}

int dm_report_group_output_and_pop_all(struct dm_report_group *group)
{
	struct report_group_item *item, *tmp_item;

	dm_list_iterate_items_safe(item, tmp_item, &group->items) {
		if (!item->parent) {
			item->store.finished_count = 0;
			continue;
		}
		if (item->report && !dm_report_output(item->report))
			return_0;
		if (!dm_report_group_pop(group))
			return_0;
	}

	if (group->type == DM_REPORT_GROUP_JSON || group->type == DM_REPORT_GROUP_JSON_STD) {
		_json_output_start(group);
		log_print(JSON_OBJECT_END);
		group->indent -= JSON_INDENT_UNIT;
	}

	return 1;
}

int dm_report_group_destroy(struct dm_report_group *group)
{
	int r = 1;

	if (!group)
		return 1;

	if (!dm_report_group_output_and_pop_all(group))
		r = 0;

	dm_pool_destroy(group->mem);
	return r;
}
