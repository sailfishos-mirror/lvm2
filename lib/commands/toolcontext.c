/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/metadata.h"
#include "lib/config/defaults.h"
#include "lib/misc/lvm-string.h"
#include "lib/activate/activate.h"
#include "lib/filters/filter.h"
#include "lib/label/label.h"
#include "lib/label/hints.h"
#include "lib/misc/lvm-file.h"
#include "lib/format_text/format-text.h"
#include "lib/display/display.h"
#include "lib/mm/memlock.h"
#include "lib/datastruct/str_list.h"
#include "lib/metadata/segtype.h"
#include "lib/cache/lvmcache.h"
#include "lib/format_text/archiver.h"
#include "lib/lvmpolld/lvmpolld-client.h"
#include "lib/device/device_id.h"

#include <locale.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>

#ifdef APP_MACHINEID_SUPPORT
#include <systemd/sd-id128.h>
#endif

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif

#ifdef __linux__
#  include <malloc.h>
#endif

static const size_t _linebuffer_size = 4096;

/*
 * Copy the input string, removing invalid characters.
 */
const char *system_id_from_string(struct cmd_context *cmd, const char *str)
{
	char *system_id;

	if (!str || !*str) {
		log_warn("WARNING: Empty system ID supplied.");
		return "";
	}

	if (!(system_id = dm_pool_zalloc(cmd->libmem, strlen(str) + 1))) {
		log_warn("WARNING: Failed to allocate system ID.");
		return NULL;
	}

	copy_systemid_chars(str, system_id);

	if (!*system_id) {
		log_warn("WARNING: Invalid system ID format: %s", str);
		return NULL;
	}

	if (!strncmp(system_id, "localhost", 9)) {
		log_warn("WARNING: System ID may not begin with the string \"localhost\".");
		return NULL;
	}

	return system_id;
}

static const char *_read_system_id_from_file(struct cmd_context *cmd, const char *file)
{
	char *line = NULL;
	size_t line_size;
	char *start, *end;
	const char *system_id = NULL;
	FILE *fp;

	if (!file || !strlen(file) || !file[0])
		return_NULL;

	if (!(fp = fopen(file, "r"))) {
		log_warn("WARNING: %s: fopen failed: %s", file, strerror(errno));
		return NULL;
	}

	while (getline(&line, &line_size, fp) > 0) {
		start = line;

		/* Ignore leading whitespace */
		while (*start && isspace(*start))
			start++;

		/* Ignore rest of line after # */
		if (!*start || *start == '#')
			continue;

		if (system_id && *system_id) {
			log_warn("WARNING: Ignoring extra line(s) in system ID file %s.", file);
			break;
		}

		/* Remove any comments from end of line */
		for (end = start; *end; end++)
			if (*end == '#') {
				*end = '\0';
				break;
			}

		system_id = system_id_from_string(cmd, start);
	}

	free(line);

	if (fclose(fp))
		stack;

	return system_id;
}

/* systemd-id128 new produced: f64406832c2140e8ac5422d1089aae03 */
#define LVM_APPLICATION_ID SD_ID128_MAKE(f6,44,06,83,2c,21,40,e8,ac,54,22,d1,08,9a,ae,03)

static const char *_system_id_from_source(struct cmd_context *cmd, const char *source)
{
	char buf[PATH_MAX];
	const char *file;
	const char *etc_str;
	const char *str;
	const char *system_id = NULL;

	if (!strcasecmp(source, "uname")) {
		if (cmd->hostname)
			system_id = system_id_from_string(cmd, cmd->hostname);
		goto out;
	}

	/* lvm.conf and lvmlocal.conf are merged into one config tree */
	if (!strcasecmp(source, "lvmlocal")) {
		if ((str = find_config_tree_str(cmd, local_system_id_CFG, NULL)))
			system_id = system_id_from_string(cmd, str);
		goto out;
	}

#ifdef APP_MACHINEID_SUPPORT
	if (!strcasecmp(source, "appmachineid")) {
		sd_id128_t id = { 0 };

		if (sd_id128_get_machine_app_specific(LVM_APPLICATION_ID, &id) != 0)
			log_warn("WARNING: sd_id128_get_machine_app_specific() failed %s (%d).",
				 strerror(errno), errno);

		if (dm_snprintf(buf, PATH_MAX, SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id)) < 0)
			stack;
		system_id = system_id_from_string(cmd, buf);
		goto out;
	}
#endif

	if (!strcasecmp(source, "machineid") || !strcasecmp(source, "machine-id")) {
		etc_str = find_config_tree_str(cmd, global_etc_CFG, NULL);
		if (dm_snprintf(buf, sizeof(buf), "%s/machine-id", etc_str) != -1)
			system_id = _read_system_id_from_file(cmd, buf);
		goto out;
	}

	if (!strcasecmp(source, "file")) {
		file = find_config_tree_str(cmd, global_system_id_file_CFG, NULL);
		system_id = _read_system_id_from_file(cmd, file);
		goto out;
	}

	log_warn("WARNING: Unrecognised system_id_source \"%s\".", source);

out:
	return system_id;
}

static int _get_env_vars(struct cmd_context *cmd)
{
	const char *e;

	/* Set to "" to avoid using any system directory */
	if ((e = getenv("LVM_SYSTEM_DIR"))) {
		if (dm_snprintf(cmd->system_dir, sizeof(cmd->system_dir),
				 "%s", e) < 0) {
			log_error("LVM_SYSTEM_DIR environment variable "
				  "is too long.");
			return 0;
		}
	}

	if (strcmp((getenv("LVM_RUN_BY_DMEVENTD") ? : "0"), "1") == 0)
		init_run_by_dmeventd(cmd);

	return 1;
}

static void _get_sysfs_dir(struct cmd_context *cmd, char *buf, size_t buf_size)
{
	static char proc_mounts[PATH_MAX];
	static char *split[4], buffer[PATH_MAX + 16];
	FILE *fp;
	char *sys_mnt = NULL;

	*buf = '\0';

	if (!*cmd->proc_dir) {
		log_debug("No proc filesystem found: skipping sysfs detection");
		return;
	}

	if (dm_snprintf(proc_mounts, sizeof(proc_mounts),
			 "%s/mounts", cmd->proc_dir) < 0) {
		log_error("Failed to create /proc/mounts string for sysfs detection");
		return;
	}

	if (!(fp = fopen(proc_mounts, "r"))) {
		log_sys_error("_get_sysfs_dir fopen", proc_mounts);
		return;
	}

	while (fgets(buffer, sizeof(buffer), fp)) {
		if (dm_split_words(buffer, 4, 0, split) == 4 &&
		    !strcmp(split[2], "sysfs")) {
			sys_mnt = split[1];
			break;
		}
	}

	if (fclose(fp))
		log_sys_error("fclose", proc_mounts);

	if (!sys_mnt) {
		log_error("Failed to find sysfs mount point");
		return;
	}

	dm_strncpy(buf, sys_mnt, buf_size);
}

static uint32_t _parse_debug_fields(struct cmd_context *cmd, int cfg, const char *cfgname)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	uint32_t debug_fields = 0;

	if (!(cn = find_config_tree_array(cmd, cfg, NULL))) {
		log_error(INTERNAL_ERROR "Unable to find configuration for log/%s.", cfgname);
		return 0;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_verbose("log/%s contains a value which is not a string.  Ignoring.", cfgname);
			continue;
		}

		if (!strcasecmp(cv->v.str, "all"))
			return 0;

		if (!strcasecmp(cv->v.str, "time"))
			debug_fields |= LOG_DEBUG_FIELD_TIME;

		else if (!strcasecmp(cv->v.str, "command"))
			debug_fields |= LOG_DEBUG_FIELD_COMMAND;

		else if (!strcasecmp(cv->v.str, "fileline"))
			debug_fields |= LOG_DEBUG_FIELD_FILELINE;

		else if (!strcasecmp(cv->v.str, "message"))
			debug_fields |= LOG_DEBUG_FIELD_MESSAGE;

		else
			log_verbose("Unrecognised value for log/%s: %s", cfgname, cv->v.str);
	}

	return debug_fields;
}

static int _parse_debug_classes(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int debug_classes = 0;

	if (!(cn = find_config_tree_array(cmd, log_debug_classes_CFG, NULL))) {
		log_error(INTERNAL_ERROR "Unable to find configuration for log/debug_classes.");
		return -1;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_verbose("log/debug_classes contains a value "
				    "which is not a string.  Ignoring.");
			continue;
		}

		if (!strcasecmp(cv->v.str, "all"))
			return -1;

		if (!strcasecmp(cv->v.str, "memory"))
			debug_classes |= LOG_CLASS_MEM;
		else if (!strcasecmp(cv->v.str, "devices"))
			debug_classes |= LOG_CLASS_DEVS;
		else if (!strcasecmp(cv->v.str, "activation"))
			debug_classes |= LOG_CLASS_ACTIVATION;
		else if (!strcasecmp(cv->v.str, "allocation"))
			debug_classes |= LOG_CLASS_ALLOC;
		else if (!strcasecmp(cv->v.str, "metadata"))
			debug_classes |= LOG_CLASS_METADATA;
		else if (!strcasecmp(cv->v.str, "cache"))
			debug_classes |= LOG_CLASS_CACHE;
		else if (!strcasecmp(cv->v.str, "locking"))
			debug_classes |= LOG_CLASS_LOCKING;
		else if (!strcasecmp(cv->v.str, "lvmpolld"))
			debug_classes |= LOG_CLASS_LVMPOLLD;
		else if (!strcasecmp(cv->v.str, "dbus"))
			debug_classes |= LOG_CLASS_DBUS;
		else if (!strcasecmp(cv->v.str, "io"))
			debug_classes |= LOG_CLASS_IO;
		else
			log_verbose("Unrecognised value for log/debug_classes: %s", cv->v.str);
	}

	return debug_classes;
}

static uint32_t _parse_log_journal(struct cmd_context *cmd, int cfg, const char *cfgname)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	uint32_t fields = 0;
	uint32_t val;

	if (!(cn = find_config_tree_array(cmd, cfg, NULL))) {
		log_debug("Unable to find configuration for log/%s.", cfgname);
		return 0;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_verbose("log/%s contains a value which is not a string.  Ignoring.", cfgname);
			continue;
		}

		if ((val = log_journal_str_to_val(cv->v.str)))
			fields |= val;
		else
			log_verbose("Unrecognised value for log/%s: %s", cfgname, cv->v.str);
	}

	return fields;
}

static void _init_logging(struct cmd_context *cmd)
{
	int append = 1;
	time_t t;

	const char *log_file;
	char timebuf[26];

	/* Syslog */
	cmd->default_settings.syslog = find_config_tree_bool(cmd, log_syslog_CFG, NULL);
	if (cmd->default_settings.syslog)
		init_syslog(1, DEFAULT_LOG_FACILITY);
	else
		fin_syslog();

	/* Debug level for log file output */
	cmd->default_settings.debug = find_config_tree_int(cmd, log_level_CFG, NULL);
	init_debug(cmd->default_settings.debug);

	/*
	 * Suppress all non-essential stdout?
	 * -qq can override the default of 0 to 1 later.
	 * Once set to 1, there is no facility to change it back to 0.
	 */
	cmd->default_settings.silent = silent_mode() ? :
	    find_config_tree_bool(cmd, log_silent_CFG, NULL);
	init_silent(cmd->default_settings.silent);

	/* Verbose level for tty output */
	cmd->default_settings.verbose = find_config_tree_int(cmd, log_verbose_CFG, NULL);
	init_verbose(cmd->default_settings.verbose + VERBOSE_BASE_LEVEL);

	/* Log message formatting */
	init_indent(find_config_tree_bool(cmd, log_indent_CFG, NULL));
	init_abort_on_internal_errors(find_config_tree_bool(cmd, global_abort_on_internal_errors_CFG, NULL));

	cmd->default_settings.msg_prefix = find_config_tree_str_allow_empty(cmd, log_prefix_CFG, NULL);
	init_msg_prefix(cmd->default_settings.msg_prefix);

	/* so that file and verbose output have a command prefix */
	init_log_command(0, 0);

	/* Test mode */
	cmd->default_settings.test =
	    find_config_tree_bool(cmd, global_test_CFG, NULL);
	init_test(cmd->default_settings.test);

	/* Settings for logging to file */
	if (find_config_tree_bool(cmd, log_overwrite_CFG, NULL))
		append = 0;

	log_file = find_config_tree_str(cmd, log_file_CFG, NULL);

	if (log_file) {
		fin_log();
		init_log_file(log_file, append);
	}

	init_log_while_suspended(find_config_tree_bool(cmd, log_activation_CFG, NULL));

	cmd->default_settings.debug_classes = _parse_debug_classes(cmd);
	log_debug("Setting log debug classes to %d", cmd->default_settings.debug_classes);
	init_debug_classes_logged(cmd->default_settings.debug_classes);

	init_debug_file_fields(_parse_debug_fields(cmd, log_debug_file_fields_CFG, "debug_file_fields"));
	init_debug_output_fields(_parse_debug_fields(cmd, log_debug_output_fields_CFG, "debug_output_fields"));

	cmd->default_settings.journal = _parse_log_journal(cmd, log_journal_CFG, "journal");
	init_log_journal(cmd->default_settings.journal);

	t = time(NULL);
	ctime_r(&t, &timebuf[0]);
	timebuf[24] = '\0';
	log_verbose("Logging initialised at %s", timebuf);

	/* Tell device-mapper about our logging */
#ifdef DEVMAPPER_SUPPORT
	if (!dm_log_is_non_default())
		dm_log_with_errno_init(print_log_libdm);
#endif
	reset_log_duplicated();
	reset_lvm_errno(1);
}

static int _check_disable_udev(const char *msg)
{
	if (getenv("DM_DISABLE_UDEV")) {
		log_very_verbose("DM_DISABLE_UDEV environment variable set.");
		log_very_verbose("Overriding configuration to use udev_rules=0, udev_sync=0, verify_udev_operations=1.");
		log_very_verbose("LVM will %s.", msg);
		return 1;
	}

	return 0;
}

static int _check_config_by_source(struct cmd_context *cmd, config_source_t source)
{
	struct dm_config_tree *cft;
	struct cft_check_handle *handle;

	if (!(cft = get_config_tree_by_source(cmd, source)) ||
	    !(handle = get_config_tree_check_handle(cmd, cft)))
		return 1;

	return config_def_check(handle);
}

static int _check_config(struct cmd_context *cmd)
{
	int abort_on_error;

	if (!find_config_tree_bool(cmd, config_checks_CFG, NULL))
		return 1;

	abort_on_error = find_config_tree_bool(cmd, config_abort_on_errors_CFG, NULL);

	if ((!_check_config_by_source(cmd, CONFIG_STRING) ||
	    !_check_config_by_source(cmd, CONFIG_MERGED_FILES) ||
	    !_check_config_by_source(cmd, CONFIG_FILE)) &&
	    abort_on_error) {
		log_error("LVM_ configuration invalid.");
		return 0;
	}

	return 1;
}

static const char *_set_time_format(struct cmd_context *cmd)
{
	/* Compared to strftime, we do not allow "newline" character - the %n in format. */
	static const char _allowed_format_chars[] = "aAbBcCdDeFGghHIjklmMpPrRsStTuUVwWxXyYzZ%";
	static const char _allowed_alternative_format_chars_e[] = "cCxXyY";
	static const char _allowed_alternative_format_chars_o[] = "deHImMSuUVwWy";
	const char *chars_to_check;
	const char *tf = find_config_tree_str(cmd, report_time_format_CFG, NULL);
	const char *p_fmt;
	size_t i;
	char c;

	if (!*tf) {
		log_error("Configured time format is empty string.");
		goto bad;
	} else {
		p_fmt = tf;
		while ((c = *p_fmt)) {
			if (c == '%') {
				c = *++p_fmt;
				if (c == 'E') {
					c = *++p_fmt;
					chars_to_check = _allowed_alternative_format_chars_e;
				} else if (c == 'O') {
					c = *++p_fmt;
					chars_to_check = _allowed_alternative_format_chars_o;
				} else
					chars_to_check = _allowed_format_chars;

				for (i = 0; chars_to_check[i]; i++) {
					if (c == chars_to_check[i])
						break;
				}
				if (!chars_to_check[i])
					goto_bad;
			}
			else if (isprint(c))
				p_fmt++;
			else {
				log_error("Configured time format contains non-printable characters.");
				goto bad;
			}
		}
	}

	return tf;
bad:
	log_error("Invalid time format \"%s\" supplied.", tf);
	return NULL;
}

int process_profilable_config(struct cmd_context *cmd)
{
	const char *units;

	if (!(cmd->default_settings.unit_factor =
	      dm_units_to_factor(units = find_config_tree_str(cmd, global_units_CFG, NULL),
				 &cmd->default_settings.unit_type, 1, NULL))) {
		log_error("Unrecognised configuration setting for global/units: %s", units);
		return 0;
	}

	cmd->si_unit_consistency = find_config_tree_bool(cmd, global_si_unit_consistency_CFG, NULL);
	cmd->report_binary_values_as_numeric = find_config_tree_bool(cmd, report_binary_values_as_numeric_CFG, NULL);
	cmd->report_mark_hidden_devices = find_config_tree_bool(cmd, report_mark_hidden_devices_CFG, NULL);
	cmd->default_settings.suffix = find_config_tree_bool(cmd, global_suffix_CFG, NULL);
	cmd->report_list_item_separator = find_config_tree_str(cmd, report_list_item_separator_CFG, NULL);
	if (!(cmd->time_format = _set_time_format(cmd)))
		return 0;

	return 1;
}

static int _init_system_id(struct cmd_context *cmd)
{
	const char *source, *system_id;
	int local_set = 0;

	cmd->system_id = NULL;
	cmd->unknown_system_id = 0;

	system_id = find_config_tree_str_allow_empty(cmd, local_system_id_CFG, NULL);
	if (system_id && *system_id)
		local_set = 1;

	source = find_config_tree_str(cmd, global_system_id_source_CFG, NULL);
	if (!source)
		source = "none";

	/* Defining local system_id but not using it is probably a config mistake. */
	if (local_set && strcmp(source, "lvmlocal"))
		log_warn("WARNING: local/system_id is set, so should global/system_id_source be \"lvmlocal\" not \"%s\"?", source);

	if (!strcmp(source, "none"))
		return 1;

	if ((system_id = _system_id_from_source(cmd, source)) && *system_id) {
		cmd->system_id = system_id;
		return 1;
	}

	/*
	 * The source failed to resolve a system_id.  In this case allow
	 * VGs with no system_id to be accessed, but not VGs with a system_id.
	 */
	log_warn("WARNING: No system ID found from system_id_source %s.", source);
	cmd->unknown_system_id = 1;

	return 1;
}

static void _init_device_ids_refresh(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int check_product_uuid = 0;
	int check_hostname = 0;
	char path[PATH_MAX];
	char uuid[128] = { 0 };

	cmd->device_ids_check_product_uuid = 0;
	cmd->device_ids_check_hostname = 0;

	if (!find_config_tree_bool(cmd, devices_device_ids_refresh_CFG, NULL))
		return;
	if (!(cn = find_config_tree_array(cmd, devices_device_ids_refresh_checks_CFG, NULL)))
		return;

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING)
			continue;
		if (!strcmp(cv->v.str, "product_uuid"))
			check_product_uuid = 1;
		if (!strcmp(cv->v.str, "hostname"))
			check_hostname = 1;
	}

	if (check_product_uuid) {
		const char *sysfs_dir = cmd->device_id_sysfs_dir ?: dm_sysfs_dir();
		if (dm_snprintf(path, sizeof(path), "%sdevices/virtual/dmi/id/product_uuid", sysfs_dir) < 0)
			return;
		if (get_sysfs_value(path, uuid, sizeof(uuid), 0) && uuid[0])
			cmd->product_uuid = dm_pool_strdup(cmd->libmem, uuid);;
		if (cmd->product_uuid)
			cmd->device_ids_check_product_uuid = 1;
	}

	if (check_hostname && cmd->hostname)
		cmd->device_ids_check_hostname = 1;
}

static int _process_config(struct cmd_context *cmd)
{
	mode_t old_umask;
	const char *dev_ext_info_src = NULL;
	const char *read_ahead, *validate_metadata;
	struct stat st;
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	int64_t pv_min_kb;
	int udev_disabled = 0;
	char sysfs_dir[PATH_MAX];

	if (!_check_config(cmd))
		return_0;

	/* umask */
	cmd->default_settings.umask = find_config_tree_int(cmd, global_umask_CFG, NULL);

	if ((old_umask = umask((mode_t) cmd->default_settings.umask)) !=
	    (mode_t) cmd->default_settings.umask)
		log_verbose("Set umask from %04o to %04o",
                            old_umask, cmd->default_settings.umask);

	/* dev dir */
	if (dm_snprintf(cmd->dev_dir, sizeof(cmd->dev_dir), "%s/",
			 find_config_tree_str(cmd, devices_dir_CFG, NULL)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}
#ifdef DEVMAPPER_SUPPORT
	dm_set_dev_dir(cmd->dev_dir);

	if (!dm_set_uuid_prefix(UUID_PREFIX))
		return_0;
#endif
	cmd->device_id_sysfs_dir = find_config_tree_str(cmd, devices_device_id_sysfs_dir_CFG, NULL);

	dev_ext_info_src = find_config_tree_str(cmd, devices_external_device_info_source_CFG, NULL);

	if (dev_ext_info_src &&
	    strcmp(dev_ext_info_src, "none") &&
	    strcmp(dev_ext_info_src, "udev")) {
		log_warn("WARNING: Unknown external device info source, using none.");
		dev_ext_info_src = NULL;
	}

	if (dev_ext_info_src && !strcmp(dev_ext_info_src, "udev")) {
		if (udev_init_library_context()) {
			init_external_device_info_source(DEV_EXT_UDEV);
		} else {
			log_warn("WARNING: Failed to init udev for external device info, using none.");
			dev_ext_info_src = NULL;
		}
	}

	if (!dev_ext_info_src || !strcmp(dev_ext_info_src, "none"))
		init_external_device_info_source(DEV_EXT_NONE);

	/* proc dir */
	if (dm_snprintf(cmd->proc_dir, sizeof(cmd->proc_dir), "%s",
			 find_config_tree_str(cmd, global_proc_CFG, NULL)) < 0) {
		log_error("Device directory given in config file too long");
		return 0;
	}

	if (*cmd->proc_dir && !dir_exists(cmd->proc_dir)) {
		log_warn("WARNING: proc dir %s not found - some checks will be bypassed.",
			 cmd->proc_dir);
		cmd->proc_dir[0] = '\0';
	}

	_get_sysfs_dir(cmd, sysfs_dir, sizeof(sysfs_dir));
	dm_set_sysfs_dir(sysfs_dir);

	/* activation? */
	cmd->default_settings.activation = find_config_tree_bool(cmd, global_activation_CFG, NULL);
	set_activation(cmd->default_settings.activation, 0);

	cmd->auto_set_activation_skip = find_config_tree_bool(cmd, activation_auto_set_activation_skip_CFG, NULL);

	read_ahead = find_config_tree_str(cmd, activation_readahead_CFG, NULL);
	if (!strcasecmp(read_ahead, "auto"))
		cmd->default_settings.read_ahead = DM_READ_AHEAD_AUTO;
	else if (!strcasecmp(read_ahead, "none"))
		cmd->default_settings.read_ahead = DM_READ_AHEAD_NONE;
	else {
		log_error("Invalid readahead specification");
		return 0;
	}

	cmd->vg_write_validates_vg = 1;
	if ((validate_metadata = find_config_tree_str(cmd, config_validate_metadata_CFG, NULL))) {
		if (!strcasecmp(validate_metadata, "none"))
			cmd->vg_write_validates_vg = 0;
		else if (strcasecmp(validate_metadata, "full"))
			log_warn("WARNING: Ignoring unknown validate_metadata setting: %s.",
				 validate_metadata);
	}

	/*
	 * If udev is disabled using DM_DISABLE_UDEV environment
	 * variable, override existing config and hardcode these:
	 *   - udev_rules = 0
	 *   - udev_sync = 0
	 *   - udev_fallback = 1
	 */
	udev_disabled = _check_disable_udev("manage logical volume symlinks in device directory");

	cmd->default_settings.udev_rules = udev_disabled ? 0 :
		find_config_tree_bool(cmd, activation_udev_rules_CFG, NULL);

	cmd->default_settings.udev_sync = udev_disabled ? 0 :
		find_config_tree_bool(cmd, activation_udev_sync_CFG, NULL);

	/*
	 * Set udev_fallback lazily on first use since it requires
	 * checking DM driver version which is an extra ioctl!
	 * This also prevents unnecessary use of mapper/control.
	 * If udev is disabled globally, set fallback mode immediately.
	 */
	cmd->default_settings.udev_fallback = udev_disabled ? 1 : -1;

	cmd->default_settings.issue_discards = find_config_tree_bool(cmd, devices_issue_discards_CFG, NULL);

	init_retry_deactivation(find_config_tree_bool(cmd, activation_retry_deactivation_CFG, NULL));

	init_activation_checks(find_config_tree_bool(cmd, activation_checks_CFG, NULL));

	cmd->use_linear_target = find_config_tree_bool(cmd, activation_use_linear_target_CFG, NULL);

	cmd->stripe_filler = find_config_tree_str(cmd, activation_missing_stripe_filler_CFG, NULL);

	/* FIXME Missing error code checks from the stats, not log_warn?, notify if setting overridden, delay message/check till it is actually used (eg consider if lvm shell - file could appear later after this check)? */
	if (!strcmp(cmd->stripe_filler, "/dev/ioerror") &&
	    stat(cmd->stripe_filler, &st))
		cmd->stripe_filler = "error";
	else if (strcmp(cmd->stripe_filler, "error") &&
		 strcmp(cmd->stripe_filler, "zero")) {
		if (stat(cmd->stripe_filler, &st)) {
			log_warn("WARNING: activation/missing_stripe_filler = \"%s\"."
				 "is invalid,", cmd->stripe_filler);
			log_warn("         stat failed: %s", strerror(errno));
			log_warn("Falling back to \"error\" missing_stripe_filler.");
			cmd->stripe_filler = "error";
		} else if (!S_ISBLK(st.st_mode)) {
			log_warn("WARNING: activation/missing_stripe_filler = \"%s\"."
				 "is not a block device.", cmd->stripe_filler);
			log_warn("Falling back to \"error\" missing_stripe_filler.");
			cmd->stripe_filler = "error";
		}
	}

	if ((cn = find_config_tree_array(cmd, activation_mlock_filter_CFG, NULL)))
		for (cv = cn->v; cv; cv = cv->next) 
			if ((cv->type != DM_CFG_STRING) || !cv->v.str[0]) 
				log_error("Ignoring invalid activation/mlock_filter entry in config file");

	cmd->metadata_read_only = find_config_tree_bool(cmd, global_metadata_read_only_CFG, NULL);

	pv_min_kb = find_config_tree_int64(cmd, devices_pv_min_size_CFG, NULL);
	if (pv_min_kb < PV_MIN_SIZE_KB) {
		log_warn("Ignoring too small pv_min_size %" PRId64 "KB, using default %dKB.",
			 pv_min_kb, PV_MIN_SIZE_KB);
		pv_min_kb = PV_MIN_SIZE_KB;
	}
	/* LVM stores sizes internally in units of 512-byte sectors. */
	init_pv_min_size((uint64_t)pv_min_kb * (1024 >> SECTOR_SHIFT));

	cmd->check_pv_dev_sizes = find_config_tree_bool(cmd, metadata_check_pv_device_sizes_CFG, NULL);
	cmd->event_activation = find_config_tree_bool(cmd, global_event_activation_CFG, NULL);

	if (!process_profilable_config(cmd))
		return_0;

	if (find_config_tree_bool(cmd, report_two_word_unknown_device_CFG, NULL))
		init_unknown_device_name("unknown device");

	if (!_init_system_id(cmd))
		return_0;

	_init_device_ids_refresh(cmd);

	init_io_memory_size(find_config_tree_int(cmd, global_io_memory_size_CFG, NULL));

	return 1;
}

static int _set_tag(struct cmd_context *cmd, const char *tag)
{
	log_very_verbose("Setting host tag: %s", dm_pool_strdup(cmd->libmem, tag));

	if (!str_list_add(cmd->libmem, &cmd->tags, tag)) {
		log_error("_set_tag: str_list_add %s failed", tag);
		return 0;
	}

	return 1;
}

static int _check_host_filters(struct cmd_context *cmd, const struct dm_config_node *hn,
			       int *passes)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;

	*passes = 1;

	for (cn = hn; cn; cn = cn->sib) {
		if (!cn->v)
			continue;
		if (!strcmp(cn->key, "host_list")) {
			*passes = 0;
			if (cn->v->type == DM_CFG_EMPTY_ARRAY)
				continue;
			for (cv = cn->v; cv; cv = cv->next) {
				if (cv->type != DM_CFG_STRING) {
					log_error("Invalid hostname string "
						  "for tag %s", cn->key);
					return 0;
				}
				if (!strcmp(cv->v.str, cmd->hostname)) {
					*passes = 1;
					return 1;
				}
			}
		}
		if (!strcmp(cn->key, "host_filter")) {
			log_error("host_filter not supported yet");
			return 0;
		}
	}

	return 1;
}

static int _init_tags(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	const struct dm_config_node *tn, *cn;
	const char *tag;
	int passes;

	/* Access tags section directly */
	if (!(tn = find_config_node(cmd, cft, tags_CFG_SECTION)) || !tn->child)
		return 1;

	/* NB hosttags 0 when already 1 intentionally does not delete the tag */
	if (!cmd->hosttags && find_config_bool(cmd, cft, tags_hosttags_CFG)) {
		/* FIXME Strip out invalid chars: only A-Za-z0-9_+.- */
		if (!_set_tag(cmd, cmd->hostname))
			return_0;
		cmd->hosttags = 1;
	}

	for (cn = tn->child; cn; cn = cn->sib) {
		if (cn->v)
			continue;
		tag = cn->key;
		if (*tag == '@')
			tag++;
		if (!validate_name(tag)) {
			log_error("Invalid tag in config file: %s", cn->key);
			return 0;
		}
		if (cn->child) {
			passes = 0;
			if (!_check_host_filters(cmd, cn->child, &passes))
				return_0;
			if (!passes)
				continue;
		}
		if (!_set_tag(cmd, tag))
			return_0;
	}

	return 1;
}

static int _load_config_file(struct cmd_context *cmd, const char *tag, int local)
{
	static char config_file[PATH_MAX] = "";
	const char *filler = "";
	struct config_tree_list *cfl;

	if (*tag)
		filler = "_";
	else if (local) {
		filler = "";
		tag = "local";
	}

	if (dm_snprintf(config_file, sizeof(config_file), "%s/lvm%s%s.conf",
			 cmd->system_dir, filler, tag) < 0) {
		log_error("LVM_SYSTEM_DIR or tag was too long");
		return 0;
	}

	if (!(cfl = dm_pool_alloc(cmd->libmem, sizeof(*cfl)))) {
		log_error("config_tree_list allocation failed");
		return 0;
	}

	if (!(cfl->cft = config_file_open_and_read(config_file, CONFIG_FILE, cmd)))
		return_0;

	dm_list_add(&cmd->config_files, &cfl->list);

	if (*tag) {
		if (!_init_tags(cmd, cfl->cft))
			return_0;
	} else
		/* Use temporary copy of lvm.conf while loading other files */
		cmd->cft = cfl->cft;

	return 1;
}

/*
 * Find and read lvm.conf.
 */
static int _init_lvm_conf(struct cmd_context *cmd)
{
	/* No config file if LVM_SYSTEM_DIR is empty */
	if (!*cmd->system_dir) {
		if (!(cmd->cft = config_open(CONFIG_FILE, NULL, 0))) {
			log_error("Failed to create config tree");
			return 0;
		}
		return 1;
	}

	if (!_load_config_file(cmd, "", 0))
		return_0;

	return 1;
}

/* Read any additional config files */
static int _init_tag_configs(struct cmd_context *cmd)
{
	struct dm_str_list *sl;

	/* Tag list may grow while inside this loop */
	dm_list_iterate_items(sl, &cmd->tags) {
		if (!_load_config_file(cmd, sl->str, 0))
			return_0;
	}

	return 1;
}

static int _init_profiles(struct cmd_context *cmd)
{
	const char *dir;

	if (!(dir = find_config_tree_str(cmd, config_profile_dir_CFG, NULL)))
		return_0;

	if (!cmd->profile_params) {
		if (!(cmd->profile_params = dm_pool_zalloc(cmd->libmem, sizeof(*cmd->profile_params)))) {
			log_error("profile_params alloc failed");
			return 0;
		}
		dm_list_init(&cmd->profile_params->profiles_to_load);
		dm_list_init(&cmd->profile_params->profiles);
	}

	if (!(dm_strncpy(cmd->profile_params->dir, dir, sizeof(cmd->profile_params->dir)))) {
		log_error("_init_profiles: dm_strncpy failed");
		return 0;
	}

	return 1;
}

static struct dm_config_tree *_merge_config_files(struct cmd_context *cmd, struct dm_config_tree *cft)
{
	struct config_tree_list *cfl;

	/* Replace temporary duplicate copy of lvm.conf */
	if (cft->root) {
		if (!(cft = config_open(CONFIG_MERGED_FILES, NULL, 0))) {
			log_error("Failed to create config tree");
			return 0;
		}
	}

	dm_list_iterate_items(cfl, &cmd->config_files) {
		/* Merge all config trees into cmd->cft using merge/tag rules */
		if (!merge_config_tree(cmd, cft, cfl->cft, CONFIG_MERGE_TYPE_TAGS))
			return_0;
	}

	return cft;
}

static void _destroy_tags(struct cmd_context *cmd)
{
	struct dm_list *slh, *slht;

	dm_list_iterate_safe(slh, slht, &cmd->tags) {
		dm_list_del(slh);
	}
}

int config_files_changed(struct cmd_context *cmd)
{
	struct config_tree_list *cfl;

	dm_list_iterate_items(cfl, &cmd->config_files) {
		if (config_file_changed(cfl->cft))
			return 1;
	}

	return 0;
}

static void _destroy_config(struct cmd_context *cmd)
{
	struct config_tree_list *cfl;
	struct dm_config_tree *cft;
	struct profile *profile, *tmp_profile;

	/*
	 * Configuration cascade:
	 * CONFIG_STRING -> CONFIG_PROFILE -> CONFIG_FILE/CONFIG_MERGED_FILES
	 */

	/* CONFIG_FILE/CONFIG_MERGED_FILES */
	if ((cft = remove_config_tree_by_source(cmd, CONFIG_MERGED_FILES)))
		config_destroy(cft);
	else if ((cft = remove_config_tree_by_source(cmd, CONFIG_FILE))) {
		dm_list_iterate_items(cfl, &cmd->config_files) {
			if (cfl->cft == cft)
				dm_list_del(&cfl->list);
		}
		config_destroy(cft);
	}

	dm_list_iterate_items(cfl, &cmd->config_files)
		config_destroy(cfl->cft);
	dm_list_init(&cmd->config_files);

	/* CONFIG_PROFILE */
	if (cmd->profile_params) {
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_COMMAND);
		remove_config_tree_by_source(cmd, CONFIG_PROFILE_METADATA);
		/*
		 * Destroy config trees for any loaded profiles and
		 * move these profiles to profile_to_load list.
		 * Whenever these profiles are referenced later,
		 * they will get loaded again automatically.
		 */
		dm_list_iterate_items_safe(profile, tmp_profile, &cmd->profile_params->profiles) {
			if (cmd->is_interactive && (profile == cmd->profile_params->shell_profile))
				continue;

			config_destroy(profile->cft);
			profile->cft = NULL;
			dm_list_move(&cmd->profile_params->profiles_to_load, &profile->list);
		}
	}

	/* CONFIG_STRING */
	if ((cft = remove_config_tree_by_source(cmd, CONFIG_STRING)))
		config_destroy(cft);

	if (cmd->cft)
		log_error(INTERNAL_ERROR "_destroy_config: "
			  "cmd config tree not destroyed fully");
}

static int _init_dev_cache(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	size_t len, udev_dir_len = strlen(DM_UDEV_DEV_DIR);
	int len_diff;
	int device_list_from_udev;

	if (!dev_cache_init(cmd))
		return_0;

	if ((device_list_from_udev = find_config_tree_bool(cmd, devices_obtain_device_list_from_udev_CFG, NULL))) {
		if (!udev_init_library_context())
			device_list_from_udev = 0;
	}

	init_obtain_device_list_from_udev(device_list_from_udev);

	if (!(cn = find_config_tree_array(cmd, devices_scan_CFG, NULL))) {
		log_error(INTERNAL_ERROR "Unable to find configuration for devices/scan.");
		return 0;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_error("Invalid string in config file: "
				  "devices/scan");
			return 0;
		}

		if (device_list_from_udev) {
			len = strlen(cv->v.str);

			/*
			 * DM_UDEV_DEV_DIR always has '/' at its end.
			 * If the item in the conf does not have it, be sure
			 * to make the right comparison without the '/' char!
			 */
			len_diff = len && cv->v.str[len - 1] != '/' ?
					udev_dir_len - 1 != len :
					udev_dir_len != len;

			if (len_diff || strncmp(DM_UDEV_DEV_DIR, cv->v.str, len)) {
				log_very_verbose("Non standard udev dir %s, resetting "
						 "devices/obtain_device_list_from_udev.",
						 cv->v.str);
				device_list_from_udev = 0;
				init_obtain_device_list_from_udev(0);
			}
		}

		if (!dev_cache_add_dir(cv->v.str)) {
			log_error("Failed to add %s to internal device cache",
				  cv->v.str);
			return 0;
		}
	}

	return 1;
}

#define MAX_FILTERS 10

static struct dev_filter *_init_filter_chain(struct cmd_context *cmd)
{
	int nr_filt = 0;
	const struct dm_config_node *cn;
	struct dev_filter *filters[MAX_FILTERS] = { 0 };
	struct dev_filter *composite;

	/*
	 * Filters listed in order: top one gets applied first.
	 * Failure to initialise some filters is not fatal.
	 * Update MAX_FILTERS definition above when adding new filters.
	 */

	/* global regex filter. Optional. */
	if ((cn = find_config_tree_node(cmd, devices_global_filter_CFG, NULL))) {
		if (!(filters[nr_filt] = regex_filter_create(cn->v, 0, 1))) {
			log_error("Failed to create global regex device filter");
			goto bad;
		}
		nr_filt++;
	}

	/* regex filter. Optional. */
	if ((cn = find_config_tree_node(cmd, devices_filter_CFG, NULL))) {
		if (!(filters[nr_filt] = regex_filter_create(cn->v, 1, 0))) {
			log_error("Failed to create regex device filter");
			goto bad;
		}
		nr_filt++;
	}

	/* device type filter. Required. */
	if (!(filters[nr_filt] = lvm_type_filter_create(cmd->dev_types))) {
		log_error("Failed to create lvm type filter");
		goto bad;
	}
	nr_filt++;

	/* filter based on the device_ids saved in the devices file */
	if (!(filters[nr_filt] = deviceid_filter_create(cmd))) {
		log_error("Failed to create deviceid device filter");
		goto bad;
	}
	nr_filt++;

	/*
	 * sysfs filter. Only available on 2.6 kernels.  Non-critical.
	 * Eliminates unavailable devices.
	 * TODO: this may be unnecessary now with device ids
	 * (currently not used for devs match to device id using sysfs)
	 */
	if (find_config_tree_bool(cmd, devices_sysfs_scan_CFG, NULL)) {
		if ((filters[nr_filt] = sysfs_filter_create(dm_sysfs_dir())))
			nr_filt++;
	}

	/* usable device filter. Required. */
	if (!(filters[nr_filt] = usable_filter_create(cmd, cmd->dev_types))) {
		log_error("Failed to create usable device filter");
		goto bad;
	}
	nr_filt++;

	/* mpath component filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, devices_multipath_component_detection_CFG, NULL)) {
		if ((filters[nr_filt] = mpath_filter_create(cmd->dev_types)))
			nr_filt++;
	}

	/* partitioned device filter. Required. */
	if (!(filters[nr_filt] = partitioned_filter_create(cmd->dev_types))) {
		log_error("Failed to create partitioned device filter");
		goto bad;
	}
	nr_filt++;

	/* signature filter. Required. */
	if (!(filters[nr_filt] = signature_filter_create(cmd->dev_types))) {
		log_error("Failed to create signature device filter");
		goto bad;
	}
	nr_filt++;

	/* md component filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, devices_md_component_detection_CFG, NULL)) {
		init_md_filtering(1);
		if ((filters[nr_filt] = md_filter_create(cmd, cmd->dev_types)))
			nr_filt++;
	}

	/* firmware raid filter. Optional, non-critical. */
	if (find_config_tree_bool(cmd, devices_fw_raid_component_detection_CFG, NULL)) {
		init_fwraid_filtering(1);
		if ((filters[nr_filt] = fwraid_filter_create(cmd->dev_types)))
			nr_filt++;
	}

	if (!(composite = composite_filter_create(nr_filt, filters)))
		goto_bad;

	return composite;

bad:
	while (--nr_filt >= 0)
		 filters[nr_filt]->destroy(filters[nr_filt]);

	return NULL;
}

/*
 *   cmd->filter == 
 *     persistent(cache) filter -> sysfs filter -> internal filter -> global regex filter ->
 *     regex_filter -> type filter -> usable device filter ->
 *     mpath component filter -> partitioned filter -> md component filter -> fw raid filter
 *
 */
int init_filters(struct cmd_context *cmd, unsigned load_persistent_cache)
{
	struct dev_filter *pfilter, *filter = NULL, *filter_components[2] = {0};

	if (!cmd->initialized.connections) {
		log_error(INTERNAL_ERROR "connections must be initialized before filters");
		return 0;
	}

	filter = _init_filter_chain(cmd);
	if (!filter)
		goto_bad;

	init_ignore_suspended_devices(find_config_tree_bool(cmd, devices_ignore_suspended_devices_CFG, NULL));
	init_ignore_lvm_mirrors(find_config_tree_bool(cmd, devices_ignore_lvm_mirrors_CFG, NULL));

	/*
	 * persistent filter is a cache of the previous result real filter result.
	 * If a dev is found in persistent filter, the pass/fail result saved by
	 * the pfilter is used.  If a dev does not existing in the persistent
	 * filter, the dev is passed on to the real filter, and when the result
	 * of the real filter is saved in the persistent filter.
	 *
	 * FIXME: we should apply the filter once at the start of the command,
	 * and not call the filters repeatedly.  In that case we would not need
	 * the persistent/caching filter layer.
	 */
	if (!(pfilter = persistent_filter_create(cmd->dev_types, filter))) {
		log_verbose("Failed to create persistent device filter.");
		goto bad;
	}

	cmd->filter = pfilter;

	cmd->initialized.filters = 1;
	return 1;
bad:
	if (!filter) {
		/*
		 * composite filter not created - destroy
		 * each component directly
		 */
		if (filter_components[0])
			filter_components[0]->destroy(filter_components[0]);
		if (filter_components[1])
			filter_components[1]->destroy(filter_components[1]);
	} else {
		/*
		 * composite filter created - destroy it - this
		 * will also destroy any of its components
		 */
		filter->destroy(filter);
	}

	cmd->initialized.filters = 0;
	return 0;
}

struct format_type *get_format_by_name(struct cmd_context *cmd, const char *format)
{
        struct format_type *fmt;

        dm_list_iterate_items(fmt, &cmd->formats)
                if (!strcasecmp(fmt->name, format) ||
                    !strcasecmp(fmt->name + 3, format) ||
                    (fmt->alias && !strcasecmp(fmt->alias, format)))
                        return fmt;

        return NULL;
}

/* FIXME: there's only one format, get rid of the list of formats */

static int _init_formats(struct cmd_context *cmd)
{
	struct format_type *fmt;

	if (!(fmt = create_text_format(cmd)))
		return 0;

	dm_list_add(&cmd->formats, &fmt->list);
	cmd->fmt_backup = fmt;
	cmd->default_settings.fmt_name = fmt->name;
	cmd->fmt = fmt;

	return 1;
}

int init_lvmcache_orphans(struct cmd_context *cmd)
{
	struct format_type *fmt;

	dm_list_iterate_items(fmt, &cmd->formats)
		if (!lvmcache_add_orphan_vginfo(cmd, fmt->orphan_vg_name, fmt))
			return_0;

	return 1;
}

struct segtype_library {
	struct cmd_context *cmd;
	void *lib;
	const char *libname;
};

int lvm_register_segtype(struct segtype_library *seglib,
			 struct segment_type *segtype)
{
	struct segment_type *segtype2;

	segtype->library = seglib->lib;

	dm_list_iterate_items(segtype2, &seglib->cmd->segtypes) {
		if (strcmp(segtype2->name, segtype->name))
			continue;
		log_error("Duplicate segment type %s: "
			  "unloading shared library %s",
			  segtype->name, seglib->libname);
		segtype->ops->destroy(segtype);
		return 0;
	}

	dm_list_add(&seglib->cmd->segtypes, &segtype->list);

	return 1;
}

static int _init_segtypes(struct cmd_context *cmd)
{
	int i;
	struct segment_type *segtype;
	struct segtype_library seglib = { .cmd = cmd, .lib = NULL };
	struct segment_type *(*init_segtype_array[])(struct cmd_context *cmd) = {
		init_striped_segtype,
		init_linear_segtype,
		init_zero_segtype,
		init_error_segtype,
		/* disabled until needed init_free_segtype, */
#ifdef SNAPSHOT_INTERNAL
		init_snapshot_segtype,
#endif
#ifdef MIRRORED_INTERNAL
		init_mirrored_segtype,
#endif
		NULL
	};

	for (i = 0; init_segtype_array[i]; i++) {
		if (!(segtype = init_segtype_array[i](cmd)))
			return 0;
		segtype->library = NULL;
		dm_list_add(&cmd->segtypes, &segtype->list);
	}

#ifdef RAID_INTERNAL
	if (!init_raid_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef THIN_INTERNAL
	if (!init_thin_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef CACHE_INTERNAL
	if (!init_cache_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef VDO_INTERNAL
	if (!init_vdo_segtypes(cmd, &seglib))
		return_0;
#endif

#ifdef WRITECACHE_INTERNAL
	if (!init_writecache_segtypes(cmd, &seglib))
		return 0;
#endif

#ifdef INTEGRITY_INTERNAL
	if (!init_integrity_segtypes(cmd, &seglib))
		return 0;
#endif

	return 1;
}

static int _init_hostname(struct cmd_context *cmd)
{
	struct utsname uts;

	if (uname(&uts)) {
		log_sys_error("uname", "_init_hostname");
		return 0;
	}

	if (!(cmd->hostname = dm_pool_strdup(cmd->libmem, uts.nodename))) {
		log_error("_init_hostname: dm_pool_strdup failed");
		return 0;
	}

	if (!(cmd->kernel_vsn = dm_pool_strdup(cmd->libmem, uts.release))) {
		log_error("_init_hostname: dm_pool_strdup kernel_vsn failed");
		return 0;
	}

	return 1;
}

static int _init_backup(struct cmd_context *cmd)
{
	uint32_t days, min;
	const char *dir;

	if (!cmd->system_dir[0]) {
		log_warn("WARNING: Metadata changes will NOT be backed up");
		backup_init(cmd, "", 0);
		archive_init(cmd, "", 0, 0, 0);
		return 1;
	}

	/* set up archiving */
	cmd->default_settings.archive =
	    find_config_tree_bool(cmd, backup_archive_CFG, NULL);

	days = (uint32_t) find_config_tree_int(cmd, backup_retain_days_CFG, NULL);

	min = (uint32_t) find_config_tree_int(cmd, backup_retain_min_CFG, NULL);

	if (!(dir = find_config_tree_str(cmd, backup_archive_dir_CFG, NULL)))
		return_0;

	if (!archive_init(cmd, dir, days, min,
			  cmd->default_settings.archive)) {
		log_debug("archive_init failed.");
		return 0;
	}

	/* set up the backup */
	cmd->default_settings.backup = find_config_tree_bool(cmd, backup_backup_CFG, NULL);

	if (!(dir = find_config_tree_str(cmd, backup_backup_dir_CFG, NULL)))
		return_0;

	if (!backup_init(cmd, dir, cmd->default_settings.backup)) {
		log_debug("backup_init failed.");
		return 0;
	}

	return 1;
}

static void _init_rand(struct cmd_context *cmd)
{
	if (read_urandom(&cmd->rand_seed, sizeof(cmd->rand_seed))) {
		reset_lvm_errno(1);
		return;
	}

	cmd->rand_seed = (unsigned) ((time(NULL) + getpid()) & 0xffffffff);
	reset_lvm_errno(1);
}

static void _init_globals(struct cmd_context *cmd)
{
	init_mirror_in_sync(0);
}

static int _init_lvmpolld(struct cmd_context *cmd)
{
	const char *lvmpolld_socket;

	lvmpolld_disconnect();

	lvmpolld_socket = getenv("LVM_LVMPOLLD_SOCKET");
	if (!lvmpolld_socket)
		lvmpolld_socket = DEFAULT_RUN_DIR "/lvmpolld.socket";
	lvmpolld_set_socket(lvmpolld_socket);

	lvmpolld_set_active(find_config_tree_bool(cmd, global_use_lvmpolld_CFG, NULL));
	return 1;
}

int init_connections(struct cmd_context *cmd)
{
	if (!_init_lvmpolld(cmd)) {
		log_error("Failed to initialize lvmpolld connection.");
		goto bad;
	}

	cmd->initialized.connections = 1;
	return 1;
bad:
	cmd->initialized.connections = 0;
	return 0;
}

int init_run_by_dmeventd(struct cmd_context *cmd)
{
	init_dmeventd_monitor(DMEVENTD_MONITOR_IGNORE);
	init_ignore_suspended_devices(1);
	init_disable_dmeventd_monitoring(1); /* Lock settings */
	cmd->run_by_dmeventd = 1;

	return 0;
}

void destroy_config_context(struct cmd_context *cmd)
{
	_destroy_config(cmd);

	if (cmd->mem)
		dm_pool_destroy(cmd->mem);
	if (cmd->libmem)
		dm_pool_destroy(cmd->libmem);
	if (cmd->pending_delete_mem)
		dm_pool_destroy(cmd->pending_delete_mem);

	free(cmd);
}

/* Entry point */
struct cmd_context *create_toolcontext(unsigned is_clvmd,
				       const char *system_dir,
				       unsigned set_buffering,
				       unsigned threaded,
				       unsigned set_connections,
				       unsigned set_filters)
{
	struct cmd_context *cmd;

#ifdef M_MMAP_MAX
	mallopt(M_MMAP_MAX, 0);
#endif

	if (!setlocale(LC_ALL, ""))
		log_very_verbose("setlocale failed");

#ifdef INTL_PACKAGE
	bindtextdomain(INTL_PACKAGE, LOCALEDIR);
#endif

	if (!(cmd = zalloc(sizeof(*cmd)))) {
		log_error("Failed to allocate command context");
		return NULL;
	}
	cmd->is_long_lived = is_clvmd;
	cmd->is_clvmd = is_clvmd;
	cmd->threaded = threaded ? 1 : 0;
	cmd->handles_missing_pvs = 0;
	cmd->handles_unknown_segments = 0;
	cmd->hosttags = 0;
	cmd->check_devs_used = 1;
	cmd->running_on_valgrind = RUNNING_ON_VALGRIND;

	dm_list_init(&cmd->arg_value_groups);
	dm_list_init(&cmd->formats);
	dm_list_init(&cmd->segtypes);
	dm_list_init(&cmd->tags);
	dm_list_init(&cmd->config_files);
	label_init();

	/* FIXME Make this configurable? */
	reset_lvm_errno(1);

	/* Set in/out stream buffering before glibc */
	if (set_buffering
	    && !cmd->running_on_valgrind /* Skipping within valgrind execution. */
#ifdef SYS_gettid
	    /* For threaded programs no changes of streams */
            /* On linux gettid() is implemented only via syscall */
	    && (syscall(SYS_gettid) == getpid())
#endif
	   ) {
		int flags;

		/* Allocate 2 buffers */
		if (!(cmd->linebuffer = malloc(2 * _linebuffer_size))) {
			log_error("Failed to allocate line buffer.");
			goto out;
		}

		/* nohup might set stdin O_WRONLY ! */
		if (is_valid_fd(STDIN_FILENO) &&
		    ((flags = fcntl(STDIN_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_WRONLY) {
			if (!reopen_standard_stream(&stdin, "r"))
				goto_out;
			if (setvbuf(stdin, cmd->linebuffer, _IOLBF, _linebuffer_size)) {
				log_sys_error("setvbuf", "");
				goto out;
			}
		}

		if (is_valid_fd(STDOUT_FILENO) &&
		    ((flags = fcntl(STDOUT_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_RDONLY) {
			if (!reopen_standard_stream(&stdout, "w"))
				goto_out;
			if (setvbuf(stdout, cmd->linebuffer + _linebuffer_size,
				     _IOLBF, _linebuffer_size)) {
				log_sys_error("setvbuf", "");
				goto out;
			}
		}
		/* Buffers are used for lines without '\n' */
	} else if (!set_buffering)
		/* Without buffering, must not use stdin/stdout */
		init_silent(1);

	/*
	 * Environment variable LVM_SYSTEM_DIR overrides this below.
	 */
	strncpy(cmd->system_dir, (system_dir) ? system_dir : DEFAULT_SYS_DIR,
		sizeof(cmd->system_dir) - 1);

	if (!_get_env_vars(cmd))
		goto_out;

	/* Create system directory if it doesn't already exist */
	if (*cmd->system_dir && !dm_create_dir(cmd->system_dir)) {
		log_error("Failed to create LVM2 system dir for metadata backups, config "
			  "files and internal cache.");
		log_error("Set environment variable LVM_SYSTEM_DIR to alternative location "
			  "or empty string.");
		goto out;
	}

	if (!(cmd->libmem = dm_pool_create("library", 4 * 1024))) {
		log_error("Library memory pool creation failed");
		goto out;
	}

	if (!(cmd->mem = dm_pool_create("command", 4 * 1024))) {
		log_error("Command memory pool creation failed");
		goto out;
	}

	if (!(cmd->pending_delete_mem = dm_pool_create("pending_delete", 1024)))
		goto_out;

	if (!_init_lvm_conf(cmd))
		goto_out;

	_init_logging(cmd);

	if (!_init_hostname(cmd))
		goto_out;

	if (!_init_tags(cmd, cmd->cft))
		goto_out;

	/* Load lvmlocal.conf */
	if (*cmd->system_dir && !_load_config_file(cmd, "", 1))
		goto_out;

	if (!_init_tag_configs(cmd))
		goto_out;

	if (!(cmd->cft = _merge_config_files(cmd, cmd->cft)))
		goto_out;

	if (!_process_config(cmd))
		goto_out;

	if (!_init_profiles(cmd))
		goto_out;

	if (!(cmd->dev_types = create_dev_types(cmd->proc_dir,
						find_config_tree_array(cmd, devices_types_CFG, NULL))))
		goto_out;

	init_use_aio(find_config_tree_bool(cmd, global_use_aio_CFG, NULL));

	if (!_init_dev_cache(cmd))
		goto_out;

	devices_file_init(cmd);

	memlock_init(cmd);

	if (!_init_formats(cmd))
		goto_out;

	if (!lvmcache_init(cmd))
		goto_out;

	/* FIXME: move into lvmcache_init */
	if (!init_lvmcache_orphans(cmd))
		goto_out;

	if (!_init_segtypes(cmd))
		goto_out;

	if (!_init_backup(cmd))
		goto_out;

	_init_rand(cmd);

	_init_globals(cmd);

	if (set_connections && !init_connections(cmd))
		goto_out;

	if (set_filters && !init_filters(cmd, 1))
		goto_out;

	cmd->current_settings = cmd->default_settings;

	cmd->initialized.config = 1;

	dm_list_init(&cmd->pending_delete);
out:
	if (!cmd->initialized.config) {
		destroy_toolcontext(cmd);
		cmd = NULL;
	}

	return cmd;
}

static void _destroy_formats(struct cmd_context *cmd, struct dm_list *formats)
{
	struct dm_list *fmtl, *tmp;
	struct format_type *fmt;

	dm_list_iterate_safe(fmtl, tmp, formats) {
		fmt = dm_list_item(fmtl, struct format_type);
		dm_list_del(&fmt->list);
		fmt->ops->destroy(fmt);
	}
}

static void _destroy_segtypes(struct dm_list *segtypes)
{
	struct dm_list *sgtl, *tmp;
	struct segment_type *segtype;

	dm_list_iterate_safe(sgtl, tmp, segtypes) {
		segtype = dm_list_item(sgtl, struct segment_type);
		dm_list_del(&segtype->list);
		segtype->ops->destroy(segtype);
	}
}

static void _destroy_dev_types(struct cmd_context *cmd)
{
	if (!cmd->dev_types)
		return;

	free(cmd->dev_types);
	cmd->dev_types = NULL;
}

static void _destroy_filters(struct cmd_context *cmd)
{
	if (cmd->filter) {
		cmd->filter->destroy(cmd->filter);
		cmd->filter = NULL;
	}
	cmd->initialized.filters = 0;
}

int refresh_filters(struct cmd_context *cmd)
{
	int r, saved_ignore_suspended_devices = ignore_suspended_devices();

	if (!cmd->initialized.filters)
		/* if filters not initialized, there's nothing to refresh */
		return 1;

	_destroy_filters(cmd);
	if (!(r = init_filters(cmd, 0)))
                stack;

	/*
	 * During repair code must not reset suspended flag.
	 */
	init_ignore_suspended_devices(saved_ignore_suspended_devices);

	return r;
}

int refresh_toolcontext(struct cmd_context *cmd)
{
	struct dm_config_tree *cft_cmdline, *cft_tmp;
	const char *profile_command_name, *profile_metadata_name;
	struct profile *profile;

	log_verbose("Reloading config files");

	/*
	 * Don't update the persistent filter cache as we will
	 * perform a full rescan.
	 */

	activation_release();
	hints_exit(cmd);
	lvmcache_destroy(cmd, 0, 0);
	label_scan_destroy(cmd);
	label_exit();
	_destroy_segtypes(&cmd->segtypes);
	_destroy_formats(cmd, &cmd->formats);

	if (!dev_cache_exit())
		stack;
	_destroy_dev_types(cmd);
	_destroy_tags(cmd);

	/* save config string passed on the command line */
	cft_cmdline = remove_config_tree_by_source(cmd, CONFIG_STRING);

	/* save the global profile name used */
	profile_command_name = cmd->profile_params->global_command_profile ?
				cmd->profile_params->global_command_profile->name : NULL;
	profile_metadata_name = cmd->profile_params->global_metadata_profile ?
				cmd->profile_params->global_metadata_profile->name : NULL;

	_destroy_config(cmd);

	cmd->initialized.config = 0;

	cmd->hosttags = 0;

	cmd->lib_dir = NULL;

	cmd->lvcreate_vcp = NULL;

	if (!_init_lvm_conf(cmd))
		return_0;

	/* Temporary duplicate cft pointer holding lvm.conf - replaced later */
	cft_tmp = cmd->cft;
	if (cft_cmdline)
		cmd->cft = dm_config_insert_cascaded_tree(cft_cmdline, cft_tmp);

	/* Reload the global profile. */
	if (profile_command_name) {
		if (!(profile = add_profile(cmd, profile_command_name, CONFIG_PROFILE_COMMAND)) ||
		    !override_config_tree_from_profile(cmd, profile))
			return_0;
	}
	if (profile_metadata_name) {
		if (!(profile = add_profile(cmd, profile_metadata_name, CONFIG_PROFILE_METADATA)) ||
		    !override_config_tree_from_profile(cmd, profile))
			return_0;
	}

	/* Uses cmd->cft i.e. cft_cmdline + lvm.conf */
	_init_logging(cmd);

	/* Init tags from lvm.conf. */
	if (!_init_tags(cmd, cft_tmp))
		return_0;

	/* Load lvmlocal.conf */
	if (*cmd->system_dir && !_load_config_file(cmd, "", 1))
		return_0;

	/* Doesn't change cmd->cft */
	if (!_init_tag_configs(cmd))
		return_0;

	/* Merge all the tag config files with lvm.conf, returning a
	 * fresh cft pointer in place of cft_tmp. */
	if (!(cmd->cft = _merge_config_files(cmd, cft_tmp)))
		return_0;

	/* Finally we can make the proper, fully-merged, cmd->cft */
	if (cft_cmdline)
		cmd->cft = dm_config_insert_cascaded_tree(cft_cmdline, cmd->cft);

	if (!_process_config(cmd))
		return_0;

	if (!_init_profiles(cmd))
		return_0;

	if (!(cmd->dev_types = create_dev_types(cmd->proc_dir,
						find_config_tree_array(cmd, devices_types_CFG, NULL))))
		return_0;

	if (!_init_dev_cache(cmd))
		return_0;

	devices_file_init(cmd);

	if (!_init_formats(cmd))
		return_0;

	if (!lvmcache_init(cmd))
		return_0;

	if (!init_lvmcache_orphans(cmd))
		return_0;

	if (!_init_segtypes(cmd))
		return_0;

	if (!_init_backup(cmd))
		return_0;

	cmd->initialized.config = 1;

	if (!dm_list_empty(&cmd->pending_delete)) {
		log_debug(INTERNAL_ERROR "Unprocessed pending delete for %d devices.",
			  dm_list_size(&cmd->pending_delete));
		dm_list_init(&cmd->pending_delete);
	}

	if (cmd->initialized.connections && !init_connections(cmd))
		return_0;

	if (!refresh_filters(cmd))
		return_0;

	reset_lvm_errno(1);
	return 1;
}

void destroy_toolcontext(struct cmd_context *cmd)
{
	struct dm_config_tree *cft_cmdline;

	archive_exit(cmd);
	backup_exit(cmd);
	hints_exit(cmd);
	lvmcache_destroy(cmd, 0, 0);
	label_scan_destroy(cmd);
	label_exit();
	_destroy_segtypes(&cmd->segtypes);
	_destroy_formats(cmd, &cmd->formats);
	_destroy_filters(cmd);
	dev_cache_exit();
	_destroy_dev_types(cmd);
	_destroy_tags(cmd);

	if ((cft_cmdline = remove_config_tree_by_source(cmd, CONFIG_STRING)))
		config_destroy(cft_cmdline);

	if (cmd->cft_def_hash)
		dm_hash_destroy(cmd->cft_def_hash);

	if (!cmd->running_on_valgrind && cmd->linebuffer) {
		int flags;
		/* Reset stream buffering to defaults */
		if (is_valid_fd(STDIN_FILENO) &&
		    ((flags = fcntl(STDIN_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_WRONLY) {
			if (reopen_standard_stream(&stdin, "r"))
				setlinebuf(stdin);
			else
				cmd->linebuffer = NULL;	/* Leave buffer in place (deliberate leak) */
		}

		if (is_valid_fd(STDOUT_FILENO) &&
		    ((flags = fcntl(STDOUT_FILENO, F_GETFL)) > 0) &&
		    (flags & O_ACCMODE) != O_RDONLY) {
			if (reopen_standard_stream(&stdout, "w"))
				setlinebuf(stdout);
			else
				cmd->linebuffer = NULL;	/* Leave buffer in place (deliberate leak) */
		}

		free(cmd->linebuffer);
	}

	destroy_config_context(cmd);

	lvmpolld_disconnect();

	activation_exit();
	reset_log_duplicated();
	fin_log();
	fin_syslog();
	reset_lvm_errno(0);
}
