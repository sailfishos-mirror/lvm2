/*
 * Copyright (C) 2010 Red Hat, Inc. All rights reserved.
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

#include <unistd.h>

#include "units.h"
#include "libdm/libdevmapper.h"

static void *_mem_init(void)
{
	struct dm_pool *mem = dm_pool_create("config test", 1024);
	if (!mem) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	return mem;
}

static void _mem_exit(void *mem)
{
	dm_pool_destroy(mem);
}

static const char *conf =
	"id = \"yada-yada\"\n"
	"seqno = 15\n"
	"status = [\"READ\", \"WRITE\"]\n"
	"flags = []\n"
	"extent_size = 8192\n"
	"physical_volumes {\n"
	"    pv0 {\n"
	"        id = \"abcd-efgh\"\n"
	"    }\n"
	"    pv1 {\n"
	"        id = \"bbcd-efgh\"\n"
	"    }\n"
	"    pv2 {\n"
	"        id = \"cbcd-efgh\"\n"
	"    }\n"
	"}\n";

static const char *overlay =
	"id = \"yoda-soda\"\n"
	"flags = [\"FOO\"]\n"
	"physical_volumes {\n"
	"    pv1 {\n"
	"        id = \"hgfe-dcba\"\n"
	"    }\n"
	"    pv3 {\n"
	"        id = \"dbcd-efgh\"\n"
	"    }\n"
	"}\n";

static void test_parse(void *fixture)
{
	struct dm_config_tree *tree = dm_config_from_string(conf);
	const struct dm_config_value *value;

	T_ASSERT((long) tree);
	T_ASSERT(dm_config_has_node(tree->root, "id"));
	T_ASSERT(dm_config_has_node(tree->root, "physical_volumes"));
	T_ASSERT(dm_config_has_node(tree->root, "physical_volumes/pv0"));
	T_ASSERT(dm_config_has_node(tree->root, "physical_volumes/pv0/id"));

	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "id", "foo"), "yada-yada"));
	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "idt", "foo"), "foo"));

	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "physical_volumes/pv0/bb", "foo"), "foo"));
	T_ASSERT(!strcmp(dm_config_find_str(tree->root, "physical_volumes/pv0/id", "foo"), "abcd-efgh"));

	T_ASSERT(!dm_config_get_uint32(tree->root, "id", NULL));
	T_ASSERT(dm_config_get_uint32(tree->root, "extent_size", NULL));

	/* FIXME: Currently everything parses as a list, even if it's not */
	// T_ASSERT(!dm_config_get_list(tree->root, "id", NULL));
	// T_ASSERT(!dm_config_get_list(tree->root, "extent_size", NULL));

	T_ASSERT(dm_config_get_list(tree->root, "flags", &value));
	T_ASSERT(value->next == NULL); /* an empty list */
	T_ASSERT(dm_config_get_list(tree->root, "status", &value));
	T_ASSERT(value->next != NULL); /* a non-empty list */

	dm_config_destroy(tree);
}

static void test_clone(void *fixture)
{
	struct dm_config_tree *tree = dm_config_from_string(conf);
	struct dm_config_node *n;
	const struct dm_config_value *value;

	T_ASSERT(tree);

	n = dm_config_clone_node(tree, tree->root, 1);

	T_ASSERT(n);

	/* Check that the nodes are actually distinct. */
	T_ASSERT(n != tree->root);
	T_ASSERT(n->sib != tree->root->sib);
	T_ASSERT(dm_config_find_node(n, "physical_volumes") != NULL);
	T_ASSERT(dm_config_find_node(tree->root, "physical_volumes") != NULL);
	T_ASSERT(dm_config_find_node(n, "physical_volumes") != dm_config_find_node(tree->root, "physical_volumes"));

	T_ASSERT(dm_config_has_node(n, "id"));
	T_ASSERT(dm_config_has_node(n, "physical_volumes"));
	T_ASSERT(dm_config_has_node(n, "physical_volumes/pv0"));
	T_ASSERT(dm_config_has_node(n, "physical_volumes/pv0/id"));

	T_ASSERT(!strcmp(dm_config_find_str(n, "id", "foo"), "yada-yada"));
	T_ASSERT(!strcmp(dm_config_find_str(n, "idt", "foo"), "foo"));

	T_ASSERT(!strcmp(dm_config_find_str(n, "physical_volumes/pv0/bb", "foo"), "foo"));
	T_ASSERT(!strcmp(dm_config_find_str(n, "physical_volumes/pv0/id", "foo"), "abcd-efgh"));

	T_ASSERT(!dm_config_get_uint32(n, "id", NULL));
	T_ASSERT(dm_config_get_uint32(n, "extent_size", NULL));

	/* FIXME: Currently everything parses as a list, even if it's not */
	// T_ASSERT(!dm_config_get_list(tree->root, "id", NULL));
	// T_ASSERT(!dm_config_get_list(tree->root, "extent_size", NULL));

	T_ASSERT(dm_config_get_list(n, "flags", &value));
	T_ASSERT(value->next == NULL); /* an empty list */
	T_ASSERT(dm_config_get_list(n, "status", &value));
	T_ASSERT(value->next != NULL); /* a non-empty list */

	dm_config_destroy(tree);
}

static void test_cascade(void *fixture)
{
	struct dm_config_tree *t1 = dm_config_from_string(conf),
		              *t2 = dm_config_from_string(overlay),
			      *tree;

	T_ASSERT(t1);
	T_ASSERT(t2);

	tree = dm_config_insert_cascaded_tree(t2, t1);

	T_ASSERT(tree);

	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "id", "foo"), "yoda-soda"));
	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "idt", "foo"), "foo"));

	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "physical_volumes/pv0/bb", "foo"), "foo"));
	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "physical_volumes/pv1/id", "foo"), "hgfe-dcba"));
	T_ASSERT(!strcmp(dm_config_tree_find_str(tree, "physical_volumes/pv3/id", "foo"), "dbcd-efgh"));

	dm_config_destroy(t1);
	dm_config_destroy(t2);
}

/*
 * _eat_space() scans a comment to the next '\n' bounded by the end of the
 * buffer, so an unterminated final comment is the case where that bound is
 * actually load-bearing.  All of these must parse, and identically.
 */
static void test_comments(void *fixture)
{
	static const struct {
		const char *conf;
		const char *expect;
	} cases[] = {
		/* Trailing comment terminated by a newline. */
		{ "id = \"a\"\n# trailing\n", "a" },
		/* Same, with no trailing newline: the scan runs into the end of the
		 * buffer and must stop there rather than past it. */
		{ "id = \"a\"\n# unterminated", "a" },
		/* Comment on the same line as the final value, no newline. */
		{ "id = \"a\" # same line", "a" },
		/* '#' starts a comment only where a token may begin; inside a value
		 * it is an ordinary character. */
		{ "id = \"a#b\" # real comment\n", "a#b" },
		/* Blank and whitespace-only lines interleaved with comments. */
		{ "\n# one\n\n   \n# two\nid = \"b\"\n", "b" },
		/* CRLF: '\r' is whitespace, so the comment ends at the '\n'. */
		{ "id = \"c\"\r\n# comment\r\n", "c" },
	};
	struct dm_config_tree *tree;
	unsigned i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		tree = dm_config_from_string(cases[i].conf);
		T_ASSERT((long) tree);
		T_ASSERT(!strcmp(dm_config_find_str(tree->root, "id", "foo"),
				 cases[i].expect));
		dm_config_destroy(tree);
	}

	/* A config with no value in it is a parse error, comments included.
	 * Pinned here so the comment-skipping path cannot change it. */
	T_ASSERT(!dm_config_from_string("# only a comment\n"));
	T_ASSERT(!dm_config_from_string("# only a comment, no newline"));
	T_ASSERT(!dm_config_from_string(""));
}

/*
 * A NUL ends the parse: it delimits the config buffer, which is cut at the
 * first one, so nothing from there on is part of the config.  A NUL is not a
 * byte of any config we write, so it only reaches the parser in malformed or
 * deliberately hostile input, but where it appears it bounds the parse.
 *
 * dm_config_from_string() cannot express this: it stops at the NUL, so the
 * explicit-length entry point is needed to put one inside the buffer.
 */
static void test_nul_ends_parse(void *fixture)
{
	static const char no_nul[] = "id = \"a\"\n# comment\nother = \"b\"\n";
	static const char nul_in_comment[] = "id = \"a\"\n# comment\0hidden\nother = \"b\"\n";
	static const char nul_in_value[] = "id = \"a\"\0other = \"b\"\n";
	struct dm_config_tree *tree;

	/* Control: with no NUL, the key after the comment is parsed. */
	tree = dm_config_create();
	T_ASSERT(tree);
	T_ASSERT(dm_config_parse_without_dup_node_check(tree, no_nul,
						       no_nul + sizeof(no_nul) - 1));
	T_ASSERT(dm_config_find_str(tree->root, "other", NULL));
	dm_config_destroy(tree);

	/* A NUL inside a comment stops the parse there, as it always has. */
	tree = dm_config_create();
	T_ASSERT(tree);
	T_ASSERT(dm_config_parse_without_dup_node_check(tree, nul_in_comment,
						       nul_in_comment + sizeof(nul_in_comment) - 1));
	T_ASSERT(dm_config_find_str(tree->root, "id", NULL));
	T_ASSERT(!dm_config_find_str(tree->root, "other", NULL));
	dm_config_destroy(tree);

	/* And a NUL outside a comment does the same. */
	tree = dm_config_create();
	T_ASSERT(tree);
	T_ASSERT(dm_config_parse_without_dup_node_check(tree, nul_in_value,
						       nul_in_value + sizeof(nul_in_value) - 1));
	T_ASSERT(dm_config_find_str(tree->root, "id", NULL));
	T_ASSERT(!dm_config_find_str(tree->root, "other", NULL));
	dm_config_destroy(tree);
}

static int _dup_stderr(void)
{
	return dup(fileno(stderr));
}

static void _restore_stderr(int saved)
{
	fflush(stderr);
	dup2(saved, fileno(stderr));
	close(saved);
}

/*
 * Run one parse and return what it logged, which means redirecting the whole
 * process's stderr to a temp file while it runs.  The line number of a parse
 * error is only visible through log_error(), so the assertion below has to
 * intercept it.  The parse result itself is not enough: a comment-skipping bug
 * that drops the trailing newline still yields a correct tree, it just reports
 * the wrong line to the user.
 */
static void _capture_parse_error(const char *conf_text, char *buf, size_t buflen)
{
	/* Relative, so it lands in the per-run TESTDIR the harness creates,
	 * as the other mkstemp() callers in this suite do. */
	char tmpl[] = "unit-test-XXXXXX";
	int errfd = fileno(stderr);
	int tfd, saved;
	ssize_t n;

	*buf = 0;

	tfd = mkstemp(tmpl);
	T_ASSERT(tfd >= 0);

	saved = _dup_stderr();
	T_ASSERT(saved >= 0);
	fflush(stderr);
	T_ASSERT(dup2(tfd, errfd) >= 0);

	/*
	 * Nothing between here and _restore_stderr() below may assert:
	 * test_fail() longjmps out, which would leave stderr pointing at the
	 * temp file for the rest of the run, and leave the temp file behind.
	 */
	dm_config_from_string(conf_text);
	_restore_stderr(saved);

	lseek(tfd, 0, SEEK_SET);
	n = read(tfd, buf, buflen - 1);
	if (n < 0)
		n = 0;
	buf[n] = 0;

	close(tfd);
	unlink(tmpl);
}

/* Long enough for "Parse error at byte <n> (line <n>): <reason>\n". */
#define PARSE_ERR_LEN 256

static void test_comment_line_numbers(void *fixture)
{
	char buf[PARSE_ERR_LEN];

	/* A trailing comment whose newline must still be counted.  The error is
	 * reported at line 4, not 3, because the final newline is consumed
	 * before the unexpected end of input is seen. */
	_capture_parse_error("id = \"a\"\n# comment\nbad\n", buf, sizeof(buf));
	T_ASSERT(strstr(buf, "line 4") != NULL);

	/* Several comment lines: every one of them advances the count. */
	_capture_parse_error("# one\n# two\n# three\nbad\n", buf, sizeof(buf));
	T_ASSERT(strstr(buf, "line 5") != NULL);

	/* Comment on the same line as a value: the newline is still counted. */
	_capture_parse_error("id = \"a\" # trailing\nbad\n", buf, sizeof(buf));
	T_ASSERT(strstr(buf, "line 3") != NULL);
}

#define T(path, desc, fn) register_test(ts, "/metadata/config/" path, desc, fn)

void config_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_mem_init, _mem_exit);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("parse", "parsing various", test_parse);
	T("comments", "comment and end-of-buffer handling", test_comments);
	T("comment-lines", "line numbering across comments", test_comment_line_numbers);
	T("comment-nul", "a NUL ends the parse, even inside a comment", test_nul_ends_parse);
	T("clone", "duplicating a config tree", test_clone);
	T("cascade", "cascade", test_cascade);

	dm_list_add(all_tests, &ts->list);
}
