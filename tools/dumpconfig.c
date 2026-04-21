/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
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

#include "tools.h"
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#define EDIT_OP_SET     0
#define EDIT_OP_REMOVE  1

struct edit_spec {
	struct dm_list list;
	char *section;
	char *field;
	char *value;
	int operation; /* SET or REMOVE */
};

static int _parse_edit_spec(struct dm_pool *mem, const char *spec_str, struct edit_spec **result)
{
	char *str, *eq, *path, *value, *slash;
	struct edit_spec *spec;
	size_t path_len;

	if (!(spec = dm_pool_zalloc(mem, sizeof(*spec))))
		return_0;

	/* Make a working copy */
	if (!(str = dm_pool_strdup(mem, spec_str)))
		return_0;

	/* Find the '=' sign */
	if (!(eq = strchr(str, '='))) {
		log_error("Invalid edit specification '%s' - expected 'Section/Field=Value'", spec_str);
		return 0;
	}

	/* Split into path and value */
	*eq = '\0';
	path = str;
	value = eq + 1;

	/* Trim spaces from path */
	while (*path == ' ' || *path == '\t')
		path++;
	path_len = strlen(path);
	while (path_len > 0 && (path[path_len-1] == ' ' || path[path_len-1] == '\t'))
		path[--path_len] = '\0';

	/* Trim spaces from value */
	while (*value == ' ' || *value == '\t')
		value++;
	path_len = strlen(value);
	while (path_len > 0 && (value[path_len-1] == ' ' || value[path_len-1] == '\t'))
		value[--path_len] = '\0';

	/* Find last slash to separate section from field */
	if (!(slash = strrchr(path, '/'))) {
		log_error("Invalid edit specification '%s' - expected 'Section/Field=Value'", spec_str);
		return 0;
	}

	*slash = '\0';
	spec->section = dm_pool_strdup(mem, path);
	spec->field = dm_pool_strdup(mem, slash + 1);

	if (!spec->section || !spec->field)
		return_0;

	/* Determine operation based on value */
	if (strcmp(value, "-") == 0) {
		spec->operation = EDIT_OP_REMOVE;
		spec->value = NULL;
	} else {
		spec->operation = EDIT_OP_SET;
		spec->value = dm_pool_strdup(mem, value);
		if (!spec->value)
			return_0;
	}

	*result = spec;
	return 1;
}

static int _check_local_section(const char *section)
{
	return (strcmp(section, "local") == 0 ||
	        strncmp(section, "local/", 6) == 0);
}

static int _get_vsn(struct cmd_context *cmd, uint16_t *version_int)
{
	const char *vsn;
	unsigned int major, minor, patchlevel;

	if (!(vsn = arg_str_value(cmd, atversion_ARG, NULL)) &&
	    !(vsn = arg_str_value(cmd, sinceversion_ARG, NULL)))
		vsn = LVM_VERSION;

	if (sscanf(vsn, "%u.%u.%u", &major, &minor, &patchlevel) != 3) {
		log_error("Incorrect version format.");
		return 0;
	}

	*version_int = vsn(major, minor, patchlevel);
	return 1;
}

static int _do_def_check(struct config_def_tree_spec *spec,
			 struct dm_config_tree *cft,
			 struct cft_check_handle **cft_check_handle)
{
	struct cft_check_handle *handle;

	if (!(handle = get_config_tree_check_handle(spec->cmd, cft)))
		return 0;

	handle->force_check = 1;
	handle->suppress_messages = 1;

	if (spec->type == CFG_DEF_TREE_DIFF) {
		if (!handle->check_diff)
			handle->skip_if_checked = 0;
		handle->check_diff = 1;
	} else {
		handle->skip_if_checked = 1;
		handle->check_diff = 0;
	}

	handle->ignoreunsupported = spec->ignoreunsupported;
	handle->ignoreadvanced = spec->ignoreadvanced;

	config_def_check(handle);
	*cft_check_handle = handle;

	return 1;
}

static int _merge_config_cascade(struct cmd_context *cmd, struct dm_config_tree *cft_cascaded,
				 struct dm_config_tree **cft_merged)
{
	if (!cft_cascaded)
		return 1;

	if (!*cft_merged && !(*cft_merged = config_open(CONFIG_MERGED_FILES, NULL, 0)))
		return_0;

	if (!_merge_config_cascade(cmd, cft_cascaded->cascade, cft_merged))
		return_0;

	return merge_config_tree(cmd, *cft_merged, cft_cascaded, CONFIG_MERGE_TYPE_RAW);
}

static int _config_validate(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct cft_check_handle *handle;

	if (!(handle = get_config_tree_check_handle(cmd, cft)))
		return 1; /* FIXME: Currently intentional, may hide real error! */

	handle->force_check = 1;
	handle->skip_if_checked = 1;
	handle->suppress_messages = 0;

	return config_def_check(handle);
}

static struct dm_config_node *_find_or_create_section(struct dm_config_tree *cft,
						       const char *section_path)
{
	struct dm_config_node *cn, *parent, **sibling_ptr;
	char *path, *p, *next;
	char current_path[CFG_PATH_MAX_LEN];

	if (!cft) {
		log_error("Invalid config tree");
		return NULL;
	}

	/* First try to find existing section using the full path */
	if (cft->root && (cn = dm_config_find_node(cft->root, section_path)))
		return cn;

	/* Section doesn't exist, need to create it step by step */
	if (!(path = dm_pool_strdup(cft->mem, section_path)))
		return NULL;

	parent = NULL;  /* Top level has no parent */
	sibling_ptr = &cft->root;  /* Pointer to where first top-level section is stored */
	current_path[0] = '\0';
	p = path;

	while (p && *p) {
		/* Find next component */
		next = strchr(p, '/');
		if (next)
			*next = '\0';

		/* Build current path */
		if (current_path[0])
			snprintf(current_path + strlen(current_path),
				 sizeof(current_path) - strlen(current_path),
				 "/%s", p);
		else
			snprintf(current_path, sizeof(current_path), "%s", p);

		/* Try to find this component */
		cn = cft->root ? dm_config_find_node(cft->root, current_path) : NULL;

		if (!cn) {
			/* Create new section node */
			if (!(cn = dm_config_create_node(cft, p)))
				return NULL;
			cn->parent = parent;
			cn->v = NULL;
			cn->child = NULL;
			cn->sib = NULL;

			/* Link into the tree */
			if (parent) {
				/* Add as child of parent */
				cn->sib = parent->child;
				parent->child = cn;
			} else {
				/* Add as top-level section (sibling) */
				if (*sibling_ptr) {
					/* Find the last sibling and append */
					struct dm_config_node *last = *sibling_ptr;
					while (last->sib)
						last = last->sib;
					last->sib = cn;
				} else {
					/* First top-level section */
					*sibling_ptr = cn;
				}
			}
		}

		/* Move down to this section for next iteration */
		parent = cn;
		sibling_ptr = &cn->child;
		p = next ? next + 1 : NULL;
	}

	return parent;
}

static int _apply_edit_to_section(struct dm_config_tree *cft, struct dm_config_node *section,
				   const char *field, const char *value, int operation)
{
	struct dm_config_node *cn, *prev;

	/* Find the field if it exists */
	prev = NULL;
	for (cn = section->child; cn; prev = cn, cn = cn->sib) {
		if (cn->key && strcmp(cn->key, field) == 0)
			break;
	}

	if (operation == EDIT_OP_REMOVE) {
		if (cn) {
			/* Remove the node */
			if (prev)
				prev->sib = cn->sib;
			else
				section->child = cn->sib;
		}
		return 1;
	}

	/* SET operation */
	if (!cn) {
		/* Create new field node */
		if (!(cn = dm_config_create_node(cft, field)))
			return_0;
		cn->parent = section;
		cn->sib = section->child;
		section->child = cn;
	}

	/* Parse the value by creating a temporary config string */
	{
		char temp_config[4096];
		struct dm_config_tree *temp_cft;
		struct dm_config_node *temp_node;
		int len;

		len = snprintf(temp_config, sizeof(temp_config), "temp_key=%s\n", value);
		if (len >= sizeof(temp_config)) {
			log_error("Value too long for field '%s'", field);
			return 0;
		}

		/* Create temporary config tree and parse */
		if (!(temp_cft = dm_config_create())) {
			log_error("Failed to create temporary config tree");
			return 0;
		}

		if (!dm_config_parse(temp_cft, temp_config, temp_config + len)) {
			log_error("Failed to parse value '%s' for field '%s'", value, field);
			dm_config_destroy(temp_cft);
			return 0;
		}

		/* Find the parsed value */
		if (!(temp_node = dm_config_find_node(temp_cft->root, "temp_key")) || !temp_node->v) {
			log_error("Failed to parse value '%s' for field '%s'", value, field);
			dm_config_destroy(temp_cft);
			return 0;
		}

		/* Clone the node (which includes its value) and use its value */
		{
			struct dm_config_node *cloned;
			if (!(cloned = dm_config_clone_node(cft, temp_node, 0))) {
				log_error("Failed to clone value for field '%s'", field);
				dm_config_destroy(temp_cft);
				return 0;
			}
			cn->v = cloned->v;
		}

		dm_config_destroy(temp_cft);
	}

	return 1;
}

static int _apply_edits(struct dm_config_tree *cft, struct dm_list *edits)
{
	struct edit_spec *spec;

	dm_list_iterate_items(spec, edits) {
		struct dm_config_node *section;

		log_verbose("Applying edit: %s/%s = %s",
			    spec->section, spec->field,
			    spec->operation == EDIT_OP_REMOVE ? "-" : spec->value);

		if (!(section = _find_or_create_section(cft, spec->section))) {
			log_error("Failed to create section '%s'", spec->section);
			return 0;
		}

		if (!_apply_edit_to_section(cft, section, spec->field, spec->value, spec->operation)) {
			log_error("Failed to apply edit to '%s/%s'", spec->section, spec->field);
			return 0;
		}
	}

	return 1;
}

static const char *_determine_config_file(struct cmd_context *cmd, struct dm_list *edits)
{
	struct edit_spec *spec;
	int has_local = 0, has_nonlocal = 0;
	static char path[PATH_MAX];

	/* Check if editing local vs non-local sections */
	dm_list_iterate_items(spec, edits) {
		if (_check_local_section(spec->section))
			has_local = 1;
		else
			has_nonlocal = 1;
	}

	if (has_local && has_nonlocal) {
		log_error("Cannot mix local and non-local sections in a single edit operation.");
		return NULL;
	}

	if (has_local) {
		snprintf(path, sizeof(path), "%s/lvmlocal.conf", cmd->system_dir);
		return path;
	}

	snprintf(path, sizeof(path), "%s/lvm.conf", cmd->system_dir);
	return path;
}

static int _create_backup_path(struct cmd_context *cmd, const char *filepath, char *backup_path, size_t len)
{
	char backup_dir[PATH_MAX];
	char *filename;
	time_t now;
	struct tm *tm;
	char timestamp[64];
	char candidate_path[PATH_MAX * 2];
	int counter;
	struct stat st;
	char sys_dir_prefix[PATH_MAX];
	size_t sys_dir_len;

	/* Only backup files in system_dir */
	snprintf(sys_dir_prefix, sizeof(sys_dir_prefix), "%s/", cmd->system_dir);
	sys_dir_len = strlen(sys_dir_prefix);
	if (strncmp(filepath, sys_dir_prefix, sys_dir_len) != 0) {
		backup_path[0] = '\0';
		return 1; /* Not in system_dir, no backup needed */
	}

	/* Create backup directory */
	snprintf(backup_dir, sizeof(backup_dir), "%s/old_conf", cmd->system_dir);
	if (mkdir(backup_dir, 0755) < 0 && errno != EEXIST) {
		log_sys_error("mkdir", backup_dir);
		return 0;
	}

	/* Get filename from path */
	filename = strrchr(filepath, '/');
	filename = filename ? filename + 1 : (char *)filepath;

	/* Create timestamp: YYYYMMDD-HHMMSS */
	now = time(NULL);
	tm = localtime(&now);
	strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm);

	/* Try filenames with counter from 00 to 99 to support multiple updates per second */
	for (counter = 0; counter < 100; counter++) {
		snprintf(candidate_path, sizeof(candidate_path), "%s/%s-%s%02d",
			 backup_dir, filename, timestamp, counter);

		/* Check if this filename already exists */
		if (stat(candidate_path, &st) != 0) {
			/* File doesn't exist, use this path */
			snprintf(backup_path, len, "%s", candidate_path);
			return 1;
		}
	}

	/* All 100 slots for this second are taken - this is extremely unlikely */
	log_error("Failed to create backup path: too many backups in one second");
	return 0;
}

/* Comparator for descending order (newest first) */
static int _compare_backup_files_desc(const void *a, const void *b)
{
	return strcmp(*(const char **)b, *(const char **)a);
}

/*
 * Cleanup backup directory to keep only the N most recent backup files.
 * Files are sorted by name in descending order (newest first) and files
 * beyond the limit are deleted.
 */
static void _cleanup_backup_directory(const char *backup_dir, const char *file_prefix, int max_keep)
{
	struct dirent *entry;
	DIR *dp;
	char **files = NULL;
	size_t count = 0;
	size_t capacity = 256;
	size_t i;
	size_t prefix_len;
	char full_path[PATH_MAX];

	if (!(dp = opendir(backup_dir)))
		return;

	if (!(files = malloc(capacity * sizeof(char *)))) {
		closedir(dp);
		return;
	}

	prefix_len = strlen(file_prefix);

	/* Collect all backup files for this config file */
	while ((entry = readdir(dp))) {
		if (strncmp(entry->d_name, file_prefix, prefix_len) == 0) {
			if (count >= capacity) {
				char **new_files;
				capacity *= 2;
				if (!(new_files = realloc(files, capacity * sizeof(char *)))) {
					/* Cleanup and return on allocation failure */
					for (i = 0; i < count; i++)
						free(files[i]);
					free(files);
					closedir(dp);
					return;
				}
				files = new_files;
			}
			files[count++] = strdup(entry->d_name);
		}
	}
	closedir(dp);

	/* Sort in descending order (newest first) */
	qsort(files, count, sizeof(char *), _compare_backup_files_desc);

	/* Delete files beyond the limit */
	for (i = max_keep; i < count; i++) {
		snprintf(full_path, sizeof(full_path), "%s/%s", backup_dir, files[i]);
		if (unlink(full_path) == 0)
			log_verbose("Removed old backup: %s", full_path);
		else
			log_sys_debug("unlink", full_path);
	}

	/* Cleanup memory */
	for (i = 0; i < count; i++)
		free(files[i]);
	free(files);
}

static int _move_to_backup(struct cmd_context *cmd, const char *source_path, const char *original_filepath)
{
	char backup_path[PATH_MAX * 2];
	char backup_dir[PATH_MAX];
	char *filename;
	char file_prefix[PATH_MAX];

	/* TODO: skip backup if -A0 or lvm.conf config_file_backup_limit=0 */

	if (!_create_backup_path(cmd, original_filepath, backup_path, sizeof(backup_path)))
		return 0;

	if (!backup_path[0]) {
		/* No backup needed - just remove the file */
		unlink(source_path);
		return 1;
	}

	/* Move file to backup location */
	if (rename(source_path, backup_path) < 0) {
		log_sys_error("rename", source_path);
		return 0;
	}

	log_print("Previous config backed up to %s", backup_path);

	/* Cleanup old backup files to keep only the most recent 100 */
	snprintf(backup_dir, sizeof(backup_dir), "%s/old_conf", cmd->system_dir);

	/* Get filename from original path to use as prefix for cleanup */
	filename = strrchr(original_filepath, '/');
	filename = filename ? filename + 1 : (char *)original_filepath;

	/* Create prefix: filename followed by hyphen (e.g., "lvm.conf-") */
	snprintf(file_prefix, sizeof(file_prefix), "%s-", filename);

	_cleanup_backup_directory(backup_dir, file_prefix, 100);

	return 1;
}


/*
 * Atomically write config to destination file using RENAME_EXCHANGE.
 * This ensures the file is never left in a partially written state.
 */
static int _atomic_write_config(struct cmd_context *cmd,
				 struct dm_config_tree *cft,
				 struct dm_config_tree *long_cft,
				 struct config_def_tree_spec *tree_spec,
				 const char *dest_path)
{
	char temp_path[PATH_MAX];
	char dir_path[PATH_MAX];
	char *last_slash;
	struct stat st;
	int exists;
	int ret;

	/* Check if destination exists */
	exists = (stat(dest_path, &st) == 0);

	/* Create temp file in same directory as destination */
	strncpy(dir_path, dest_path, sizeof(dir_path) - 1);
	dir_path[sizeof(dir_path) - 1] = '\0';
	last_slash = strrchr(dir_path, '/');
	if (last_slash)
		*last_slash = '\0';
	else
		strcpy(dir_path, ".");

	snprintf(temp_path, sizeof(temp_path), "%s/.lvm-config-XXXXXX", dir_path);

	/* Create temp file with mkstemp for safety */
	{
		int fd = mkstemp(temp_path);
		if (fd < 0) {
			log_sys_error("mkstemp", temp_path);
			return 0;
		}
		close(fd);
	}

	/* Write config to temp file */
	if (long_cft)
		ret = config_write(long_cft, tree_spec, temp_path, 0, NULL);
	else
		ret = config_write(cft, tree_spec, temp_path, 0, NULL);

	if (!ret) {
		log_error("Failed to write config to temp file %s", temp_path);
		unlink(temp_path);
		return 0;
	}

	if (exists) {
		/* Atomically exchange temp file with destination */
		if (_renameat2(AT_FDCWD, temp_path, AT_FDCWD, dest_path, RENAME_EXCHANGE) < 0) {
			if (errno == ENOSYS || errno == EINVAL) {
				log_debug("renameat2 error, trying rename");
				if (rename(temp_path, dest_path) < 0) {
					log_sys_error("rename", temp_path);
					unlink(temp_path);
					return 0;
				}
			} else {
				log_sys_error("renameat2", temp_path);
				unlink(temp_path);
				return 0;
			}
		} else {
			/* Successfully exchanged - temp_path now contains old file */
			/* Move old file to backup */
			if (!_move_to_backup(cmd, temp_path, dest_path)) {
				/* Backup failed, but new file is in place - just remove temp */
				log_warn("Failed to create backup, removing old file");
				unlink(temp_path);
			}
		}
	} else {
		/* Destination doesn't exist, just rename temp to dest */
		if (rename(temp_path, dest_path) < 0) {
			log_sys_error("rename", temp_path);
			unlink(temp_path);
			return 0;
		}
	}

	return 1;
}

int editconfig_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = arg_str_value(cmd, file_ARG, NULL);
	const char *input_file = arg_str_value(cmd, input_ARG, NULL);
	const char *output_file = arg_str_value(cmd, output_ARG, NULL);
	struct config_def_tree_spec tree_spec = {0};
	struct dm_config_tree *cft = NULL;
	struct cft_check_handle *cft_check_handle = NULL;
	struct dm_list edits;
	struct edit_spec *spec;
	const char *edit_str;
	const char *config_file;
	struct arg_value_group_list *group;
	struct dm_config_tree *long_cft = NULL;

	tree_spec.cmd = cmd;
	dm_list_init(&edits);

	/* Parse all edit specifications */
	dm_list_iterate_items(group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(group->arg_values, edit_ARG))
			continue;

		edit_str = grouped_arg_str_value(group->arg_values, edit_ARG, NULL);
		if (!edit_str) {
			log_error("Failed to get edit specification");
			return EINVALID_CMD_LINE;
		}

		if (!_parse_edit_spec(cmd->mem, edit_str, &spec)) {
			log_error("Failed to parse edit specification: %s", edit_str);
			return EINVALID_CMD_LINE;
		}
		dm_list_add(&edits, &spec->list);
	}

	if (input_file)
		config_file = input_file;
	else if (file)
		config_file = file;
	else {
		if (!(config_file = _determine_config_file(cmd, &edits)))
			return EINVALID_CMD_LINE;
	}

	/* Read or create config tree */
	if (access(config_file, F_OK) == 0) {
		/* File exists, read it */
		log_verbose("Reading existing config from %s", config_file);
		if (!(cft = config_file_open_and_read(config_file, CONFIG_FILE, cmd))) {
			log_error("Failed to read config file %s", config_file);
			return ECMD_FAILED;
		}
	} else {
		/* File doesn't exist, create new config tree */
		log_verbose("Creating new config file %s", config_file);
		if (!(cft = dm_config_create())) {
			log_error("Failed to create config tree");
			return ECMD_FAILED;
		}
		/* cft->root starts as NULL - sections will be added as siblings */
	}

	/* Apply edits */
	if (!_apply_edits(cft, &edits)) {
		config_destroy(cft);
		return ECMD_FAILED;
	}

	/* Determine output destination and write method */
	int use_atomic_write = 0;
	char lvm_conf_path[PATH_MAX];
	char lvmlocal_conf_path[PATH_MAX];

	snprintf(lvm_conf_path, sizeof(lvm_conf_path), "%s/lvm.conf", cmd->system_dir);
	snprintf(lvmlocal_conf_path, sizeof(lvmlocal_conf_path), "%s/lvmlocal.conf", cmd->system_dir);

	if (output_file) {
		/* Check if output_file is standard lvm.conf or lvmlocal.conf */
		if (strcmp(output_file, lvm_conf_path) == 0 ||
		    strcmp(output_file, lvmlocal_conf_path) == 0) {
			/* Use atomic write for standard system config files */
			file = output_file;
			use_atomic_write = 1;
		} else {
			file = output_file;
		}
	} else if (!input_file) {
		/* Editing in place - use atomic write with exchange */
		file = config_file;
		use_atomic_write = 1;
	} else {
		/* input without output - write to stdout */
		file = NULL;
	}

	/* Set up tree spec options */
	if (arg_is_set(cmd, withspaces_ARG))
		tree_spec.withspaces = 1;

	if (arg_is_set(cmd, long_ARG)) {
		/* Long mode: show all fields, defaults commented, edits uncommented */
		tree_spec.withcomments = 1;
		tree_spec.type = CFG_DEF_TREE_FULL;
		tree_spec.current_cft = cft;

		/* Validate the config tree and check for diffs from defaults */
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			config_destroy(cft);
			return ECMD_FAILED;
		}
		/* Force diff checking to populate CFG_DIFF flags */
		cft_check_handle->check_diff = 1;
		cft_check_handle->skip_if_checked = 0;
		config_def_check(cft_check_handle);
		tree_spec.check_status = cft_check_handle->status;

		/* Create merged tree with defaults */
		if (!(long_cft = config_def_create_tree(&tree_spec))) {
			log_error("Failed to create output config tree");
			config_destroy(cft);
			return ECMD_FAILED;
		}
	}

	/*
	 * Write the config
	 * - atomically for in-place edits of existing config file
	 * - non-atomically when writing to a custom --output file
	 */

	if (use_atomic_write) {
		if (!_atomic_write_config(cmd, cft, long_cft, &tree_spec, file)) {
			log_error("Failed to write config to %s", file);
			if (long_cft)
				dm_config_destroy(long_cft);
			config_destroy(cft);
			return ECMD_FAILED;
		}
	} else {
		if (long_cft) {
			/* Long mode with merged tree */
			if (!config_write(long_cft, &tree_spec, file, 0, NULL)) {
				log_error("Failed to write config%s%s", file ? " to " : "", file ? file : "");
				dm_config_destroy(long_cft);
				config_destroy(cft);
				return ECMD_FAILED;
			}
		} else {
			/* Short mode: write only edited values */
			if (!config_write(cft, &tree_spec, file, 0, NULL)) {
				log_error("Failed to write config%s%s", file ? " to " : "", file ? file : "");
				config_destroy(cft);
				return ECMD_FAILED;
			}
		}
	}

	if (long_cft)
		dm_config_destroy(long_cft);
	config_destroy(cft);
	if (file)
		log_print("Configuration written to %s", file);
	return ECMD_PROCESSED;
}

int dumpconfig(struct cmd_context *cmd, int argc, char **argv)
{
	const char *file = arg_str_value(cmd, file_ARG, NULL);
	const char *type = arg_str_value(cmd, configtype_ARG, arg_is_set(cmd, list_ARG) ? "list" : "current");
	struct config_def_tree_spec tree_spec = {0};
	struct dm_config_tree *cft = NULL;
	struct cft_check_handle *cft_check_handle = NULL;
	struct profile *profile = NULL;
	int r = ECMD_PROCESSED;

	tree_spec.cmd = cmd;

	if (arg_is_set(cmd, configtype_ARG) && arg_is_set(cmd, validate_ARG)) {
		log_error("Only one of --type and --validate permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, atversion_ARG)) {
		if (arg_is_set(cmd, sinceversion_ARG)) {
			log_error("Only one of --atversion and --sinceversion permitted.");
			return EINVALID_CMD_LINE;
		}

		if (!arg_is_set(cmd, configtype_ARG) && !arg_is_set(cmd, list_ARG)) {
			log_error("--atversion requires --type or --list");
			return EINVALID_CMD_LINE;
		}
	} else if (arg_is_set(cmd, sinceversion_ARG)) {
		if (!arg_is_set(cmd, configtype_ARG) || strcmp(type, "new")) {
			log_error("--sinceversion requires --type new");
			return EINVALID_CMD_LINE;
		}
	}

	if (arg_is_set(cmd, ignoreadvanced_ARG))
		tree_spec.ignoreadvanced = 1;

	if (arg_is_set(cmd, ignoreunsupported_ARG)) {
		if (arg_is_set(cmd, showunsupported_ARG)) {
			log_error("Only one of --ignoreunsupported and --showunsupported permitted.");
			return EINVALID_CMD_LINE;
		}
		tree_spec.ignoreunsupported = 1;
	} else if (arg_is_set(cmd, showunsupported_ARG)) {
		tree_spec.ignoreunsupported = 0;
	} else if (strcmp(type, "current") && strcmp(type, "diff")) {
		/*
		 * By default hide unsupported settings
		 * for all display types except "current"
		 * and "diff".
		 */
		tree_spec.ignoreunsupported = 1;
	}

	if (strcmp(type, "current") && strcmp(type, "diff")) {
		/*
		 * By default hide deprecated settings
		 * for all display types except "current"
		 * and "diff" unless --showdeprecated is set.
		 *
		 * N.B. Deprecated settings are visible if
		 * --atversion is used with a version that
		 * is lower than the version in which the
		 * setting was deprecated.
		 */
		if (!arg_is_set(cmd, showdeprecated_ARG))
			tree_spec.ignoredeprecated = 1;
	}

	if (arg_is_set(cmd, ignorelocal_ARG))
		tree_spec.ignorelocal = 1;

	if (!strcmp(type, "current") || !strcmp(type, "full")) {
		if (arg_is_set(cmd, atversion_ARG)) {
			log_error("--atversion has no effect with --type %s", type);
			return EINVALID_CMD_LINE;
		}

		if ((arg_is_set(cmd, ignoreunsupported_ARG) ||
		    arg_is_set(cmd, ignoreadvanced_ARG)) &&
		    !strcmp(type, "current")) {
			/* FIXME: allow these even for --type current */
			log_error("--ignoreadvanced and --ignoreunsupported has "
				  "no effect with --type current");
			return EINVALID_CMD_LINE;
		}
	} else if (arg_is_set(cmd, mergedconfig_ARG)) {
		log_error("--mergedconfig has no effect without --type current or --type full");
		return EINVALID_CMD_LINE;
	}

	if (!_get_vsn(cmd, &tree_spec.version))
		return EINVALID_CMD_LINE;

	/*
	 * The profile specified by --profile cmd arg is like --commandprofile,
	 * but it is used just for dumping the profile content and not for
	 * application.
	 */
	if (arg_is_set(cmd, profile_ARG) &&
	    (!(profile = add_profile(cmd, arg_str_value(cmd, profile_ARG, NULL), CONFIG_PROFILE_COMMAND)) ||
	    !override_config_tree_from_profile(cmd, profile))) {
		log_error("Failed to load profile %s.", arg_str_value(cmd, profile_ARG, NULL));
		return ECMD_FAILED;
	}

	/*
	 * Set the 'cft' to work with based on whether we need the plain
	 * config tree or merged config tree cascade if --mergedconfig is used.
	 */
	if ((arg_is_set(cmd, mergedconfig_ARG) || !strcmp(type, "full") || !strcmp(type, "diff")) && cmd->cft->cascade) {
		if (!_merge_config_cascade(cmd, cmd->cft, &cft)) {
			log_error("Failed to merge configuration.");
			r = ECMD_FAILED;
			goto out;
		}
	} else
		cft = cmd->cft;
	tree_spec.current_cft = cft;

	if (arg_is_set(cmd, validate_ARG)) {
		if (_config_validate(cmd, cft)) {
			log_print("LVM configuration valid.");
			goto out;
		} else {
			log_error("LVM configuration invalid.");
			r = ECMD_FAILED;
			goto out;
		}
	}

	if (!strcmp(type, "list")) {
		tree_spec.listmode = 1;
		tree_spec.type = CFG_DEF_TREE_LIST;
		/* list type does not require status check */
	} else if (!strcmp(type, "full")) {
		tree_spec.type = CFG_DEF_TREE_FULL;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	} else if (!strcmp(type, "current")) {
		tree_spec.type = CFG_DEF_TREE_CURRENT;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "missing")) {
		tree_spec.type = CFG_DEF_TREE_MISSING;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "default")) {
		tree_spec.type = CFG_DEF_TREE_DEFAULT;
		/* default type does not require check status */
	}
	else if (!strcmp(type, "diff")) {
		tree_spec.type = CFG_DEF_TREE_DIFF;
		if (!_do_def_check(&tree_spec, cft, &cft_check_handle)) {
			r = ECMD_FAILED;
			goto_out;
		}
	}
	else if (!strcmp(type, "new")) {
		tree_spec.type = arg_is_set(cmd, sinceversion_ARG) ? CFG_DEF_TREE_NEW_SINCE
								  : CFG_DEF_TREE_NEW;
		/* new type does not require check status */
	}
	else if (!strcmp(type, "profilable")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE;
		/* profilable type does not require check status */
	}
	else if (!strcmp(type, "profilable-command")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE_CMD;
		/* profilable-command type does not require check status */
	}
	else if (!strcmp(type, "profilable-metadata")) {
		tree_spec.type = CFG_DEF_TREE_PROFILABLE_MDA;
		/* profilable-metadata  type does not require check status */
	}
	else {
		log_error("Incorrect type of configuration specified. "
			  "Expected one of: current, default, diff, full, list, missing, "
			  "new, profilable, profilable-command, profilable-metadata.");
		r = EINVALID_CMD_LINE;
		goto out;
	}

	if (arg_is_set(cmd, withsummary_ARG))
		tree_spec.withsummary = 1;

	/*
	 *  This is for backwards compatibility with the original prehistoric
	 *  'dumpcofig --list' when the --typeconfig was not supported yet.
	 */
	if (arg_is_set(cmd, list_ARG) && !arg_is_set(cmd, configtype_ARG))
		tree_spec.withsummary = 1;

	if (arg_is_set(cmd, withcomments_ARG))
		tree_spec.withcomments = 1;

	if (arg_is_set(cmd, unconfigured_ARG))
		tree_spec.unconfigured = 1;

	if (arg_is_set(cmd, withversions_ARG))
		tree_spec.withversions = 1;

	if (arg_is_set(cmd, withgeneralpreamble_ARG))
		tree_spec.withgeneralpreamble = 1;

	if (arg_is_set(cmd, withlocalpreamble_ARG))
		tree_spec.withlocalpreamble = 1;

	if (arg_is_set(cmd, withspaces_ARG))
		tree_spec.withspaces = 1;

	if (arg_is_set(cmd, valuesonly_ARG))
		tree_spec.valuesonly = 1;

	if (arg_is_set(cmd, list_ARG))
		tree_spec.listmode = 1;

	if (tree_spec.listmode) {
		if (arg_is_set(cmd, withcomments_ARG)) {
			log_error("--withcomments has no effect with --type list or --list");
			r = EINVALID_CMD_LINE;
			goto out;
		}
		if (arg_is_set(cmd, withlocalpreamble_ARG)) {
			log_error("--withlocalpreamble has no effect with --type list or --list");
			r = EINVALID_CMD_LINE;
			goto out;
		}
		if (arg_is_set(cmd, withgeneralpreamble_ARG)) {
			log_error("--withgeneralpreamble has no effect with --type list or --list");
			r = EINVALID_CMD_LINE;
			goto out;
		}
		if (arg_is_set(cmd, valuesonly_ARG)) {
			log_error("--valuesonly has no effect with --type list or --list.");
			r = EINVALID_CMD_LINE;
			goto out;
		}
	}

	if (cft_check_handle)
		tree_spec.check_status = cft_check_handle->status;

	if ((tree_spec.type != CFG_DEF_TREE_CURRENT) &&
	    (tree_spec.type != CFG_DEF_TREE_DIFF) &&
	    !(cft = config_def_create_tree(&tree_spec))) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (!config_write(cft, &tree_spec, file, argc, argv)) {
		stack;
		r = ECMD_FAILED;
	}
out:
	if (tree_spec.current_cft && (tree_spec.current_cft != cft) &&
	    (tree_spec.current_cft != cmd->cft))
		/*
		 * This happens in case of CFG_DEF_TREE_FULL where we
		 * have merged explicitly defined config trees and also
		 * we have used default tree.
		 */
		dm_config_destroy(tree_spec.current_cft);

	if (cft && (cft != cmd->cft))
		dm_config_destroy(cft);
	else if (profile)
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_COMMAND);

	/*
	 * The cmd->cft (the "current" tree) is destroyed
	 * together with cmd context destroy...
	 */

	return r;
}

int config(struct cmd_context *cmd, int argc, char **argv)
{
	return dumpconfig(cmd, argc, argv);
}

int lvmconfig(struct cmd_context *cmd, int argc, char **argv)
{
	return dumpconfig(cmd, argc, argv);
}
