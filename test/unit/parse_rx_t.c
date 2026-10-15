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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "units.h"

#include <ctype.h>

/*
 * The regex parser and its AST (struct rx_node) are internal to libdevmapper
 * and not part of the exported ABI, so pull the implementation in directly.
 * This also keeps the AST dump helper below compiled and warning-checked.
 */
#include "libdm/regex/parse_rx.c"

static void *_mem_init(void)
{
	struct dm_pool *mem = dm_pool_create("regex parser test", 1024);
	if (!mem) {
		fprintf(stderr, "out of memory");
		exit(1);
	}

	return mem;
}

static void _mem_exit(void *mem)
{
	dm_pool_destroy(mem);
}

/*
 * Dump a parsed regular expression tree in a human readable form.  Handy when
 * debugging the parser and the optimiser; writes to the given stream so it can
 * be either shown on stdout for eyeballing or captured for testing.
 */
static void _regex_print(FILE *out, struct rx_node *rx, int depth, unsigned show_nodes)
{
	int i, numchars;

	if (rx->left) {
		if (rx->left->type != CHARSET && (show_nodes || (!((rx->type == CAT || rx->type == OR) && rx->left->type == CAT))))
			fprintf(out, "(");

		_regex_print(out, rx->left, depth + 1, show_nodes);

		if (rx->left->type != CHARSET && (show_nodes || (!((rx->type == CAT || rx->type == OR) && rx->left->type == CAT))))
			fprintf(out, ")");
	}

	/* display info about the node */
	switch (rx->type) {
	case CAT:
		break;

	case OR:
		fprintf(out, "|");
		break;

	case STAR:
		fprintf(out, "*");
		break;

	case PLUS:
		fprintf(out, "+");
		break;

	case QUEST:
		fprintf(out, "?");
		break;

	case CHARSET:
		numchars = 0;
		for (i = 0; i < 256; i++)
			if (dm_bit(rx->charset, i) && (isprint(i) || i == HAT_CHAR || i == DOLLAR_CHAR))
				numchars++;
		if (numchars == 97) {
			fprintf(out, ".");
			break;
		}
		if (numchars > 1)
			fprintf(out, "[");
		for (i = 0; i < 256; i++)
			if (dm_bit(rx->charset, i)) {
				if (isprint(i))
					fprintf(out, "%c", (char) i);
				else if (i == HAT_CHAR)
					fprintf(out, "^");
				else if (i == DOLLAR_CHAR)
					fprintf(out, "$");
			}
		if (numchars > 1)
			fprintf(out, "]");
		break;

	default:
		fprintf(out, "Unknown type");
	}

	if (rx->right) {
		if (rx->right->type != CHARSET && (show_nodes || (!(rx->type == CAT && rx->right->type == CAT) && rx->right->right)))
			fprintf(out, "(");
		_regex_print(out, rx->right, depth + 1, show_nodes);
		if (rx->right->type != CHARSET && (show_nodes || (!(rx->type == CAT && rx->right->type == CAT) && rx->right->right)))
			fprintf(out, ")");
	}

	if (!depth)
		fprintf(out, "\n");
}

/* Parse 'pattern' and dump the resulting tree into 'buf'. */
static void _parse_dump(struct dm_pool *mem, const char *pattern,
			unsigned show_nodes, char *buf, size_t size)
{
	struct rx_node *r;
	FILE *f;
	size_t n;

	T_ASSERT(f = tmpfile());

	r = rx_parse_str(mem, pattern);
	T_ASSERT(r != NULL);

	_regex_print(f, r, 0, show_nodes);
	fflush(f);
	rewind(f);

	n = fread(buf, 1, size - 1, f);
	T_ASSERT(!ferror(f));
	buf[n] = '\0';

	fclose(f);
}

static void test_parse_dump(void *fixture)
{
	struct dm_pool *mem = fixture;
	char buf[256];

	_parse_dump(mem, "a", 0, buf, sizeof(buf));
	T_ASSERT_EQUAL(strcmp(buf, "a\n"), 0);

	_parse_dump(mem, "[a-z]", 1, buf, sizeof(buf));
	T_ASSERT(strstr(buf, "a") != NULL);
	T_ASSERT(strstr(buf, "z") != NULL);

	_parse_dump(mem, "a|b", 1, buf, sizeof(buf));
	T_ASSERT(strstr(buf, "a") != NULL);
	T_ASSERT(strstr(buf, "b") != NULL);
	T_ASSERT(strstr(buf, "|") != NULL);
}

#define T(path, desc, fn) register_test(ts, "/base/regex/" path, desc, fn)

void parse_rx_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_mem_init, _mem_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("parse-dump", "dump the parsed regular expression tree", test_parse_dump);

	dm_list_add(all_tests, &ts->list);
}
