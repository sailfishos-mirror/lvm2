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
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/config/config.h"

/*
 * Removing a tree from the middle of the config cascade must only unlink
 * that tree.  The cascade head must stay in place, otherwise a later
 * lookup or cleanup can no longer find trees in front of the removed one.
 */

struct cascade_fixture {
	struct cmd_context cmd;
	struct dm_config_tree *file;
	struct dm_config_tree *mda;
	struct dm_config_tree *command;
	struct dm_config_tree *string;
};

static void *_fixture_init(void)
{
	struct cascade_fixture *f = calloc(1, sizeof(*f));

	if (!f) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	return f;
}

static void _fixture_exit(void *fixture)
{
	free(fixture);
}

static struct dm_config_tree *_open_tree(config_source_t source, const char *filename)
{
	struct dm_config_tree *cft = config_open(source, filename, 0);

	T_ASSERT(cft);

	return cft;
}

static void _build_cascade(struct cascade_fixture *f)
{
	f->file = _open_tree(CONFIG_FILE, "lvm.conf");
	f->mda = _open_tree(CONFIG_PROFILE_METADATA, "mda.profile");
	f->command = _open_tree(CONFIG_PROFILE_COMMAND, "command.profile");
	f->string = _open_tree(CONFIG_STRING, NULL);

	dm_config_insert_cascaded_tree(f->mda, f->file);
	dm_config_insert_cascaded_tree(f->command, f->mda);
	dm_config_insert_cascaded_tree(f->string, f->command);

	f->cmd.cft = f->string;
}

static void _destroy_cascade(struct cascade_fixture *f)
{
	dm_config_destroy(f->string);
	dm_config_destroy(f->command);
	dm_config_destroy(f->mda);
	dm_config_destroy(f->file);
	memset(f, 0, sizeof(*f));
}

static void test_remove_third_preserves_head(void *fixture)
{
	struct cascade_fixture *f = fixture;
	struct dm_config_tree *removed;

	_build_cascade(f);

	removed = remove_config_tree_by_source(&f->cmd, CONFIG_PROFILE_METADATA);

	T_ASSERT(removed == f->mda);
	T_ASSERT(f->cmd.cft == f->string);
	T_ASSERT(f->string->cascade == f->command);
	T_ASSERT(f->command->cascade == f->file);
	T_ASSERT(f->mda->cascade == NULL);

	_destroy_cascade(f);
}

static void test_remove_second_preserves_head(void *fixture)
{
	struct cascade_fixture *f = fixture;
	struct dm_config_tree *removed;

	_build_cascade(f);

	removed = remove_config_tree_by_source(&f->cmd, CONFIG_PROFILE_COMMAND);

	T_ASSERT(removed == f->command);
	T_ASSERT(f->cmd.cft == f->string);
	T_ASSERT(f->string->cascade == f->mda);
	T_ASSERT(f->mda->cascade == f->file);
	T_ASSERT(f->command->cascade == NULL);

	_destroy_cascade(f);
}

static void test_remove_head(void *fixture)
{
	struct cascade_fixture *f = fixture;
	struct dm_config_tree *removed;

	_build_cascade(f);

	removed = remove_config_tree_by_source(&f->cmd, CONFIG_STRING);

	T_ASSERT(removed == f->string);
	T_ASSERT(f->cmd.cft == f->command);
	T_ASSERT(f->string->cascade == NULL);
	T_ASSERT(f->command->cascade == f->mda);
	T_ASSERT(f->mda->cascade == f->file);

	_destroy_cascade(f);
}

static void test_remove_missing(void *fixture)
{
	struct cascade_fixture *f = fixture;

	_build_cascade(f);

	T_ASSERT(remove_config_tree_by_source(&f->cmd, CONFIG_MERGED_FILES) == NULL);
	T_ASSERT(f->cmd.cft == f->string);
	T_ASSERT(f->string->cascade == f->command);
	T_ASSERT(f->command->cascade == f->mda);
	T_ASSERT(f->mda->cascade == f->file);

	_destroy_cascade(f);
}

#define T(path, desc, fn) register_test(ts, "/config/cascade/" path, desc, fn)

void config_cascade_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_fixture_init, _fixture_exit);

	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("remove-third", "removing a third cascade tree keeps the head", test_remove_third_preserves_head);
	T("remove-second", "removing a second cascade tree keeps the head", test_remove_second_preserves_head);
	T("remove-head", "removing the head advances the cascade", test_remove_head);
	T("remove-missing", "removing an absent tree leaves the cascade intact", test_remove_missing);

	dm_list_add(all_tests, &ts->list);
}
