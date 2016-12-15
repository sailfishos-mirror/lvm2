/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * It includes tree drawing code based on pstree: http://psmisc.sourceforge.net/
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tool.h"

#include "dm-logging.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/fanotify.h>

#ifdef __linux__
#  include "kdev_t.h"
#else
#  define MAJOR(x) major((x))
#  define MINOR(x) minor((x))
#  define MKDEV(x,y) makedev((x),(y))
#endif

struct filemap_monitor {
	/* group_id to update */
	uint64_t group_id;
	char *path;
	int fanotify_fd;
	/* file to monitor */
	int fd;

	/* monitoring heuristics */
	int64_t blocks; /* allocated blocks, from stat.st_blocks */
	int64_t nr_regions;
};

static int _debug;
static int _verbose;

const char *const _usage = "dmfilemapd <fd> <group_id> <path> "
			   "[<debug>[<log_level>]]";

/*
 * Daemon logging. By default, all messages are thrown away: messages
 * are only written to the terminal if the daemon is run in the foreground.
 */
__attribute__((format(printf, 5, 0)))
static void _dmfilemapd_log_line(int level,
				 const char *file __attribute__((unused)),
				 int line __attribute__((unused)),
				 int dm_errno_or_class,
				 const char *f, va_list ap)
{
	static int _abort_on_internal_errors = -1;
	FILE *out = log_stderr(level) ? stderr : stdout;

	level = log_level(level);

	if (level <= _LOG_WARN || _verbose) {
		if (level < _LOG_WARN)
			out = stderr;
		vfprintf(out, f, ap);
		fputc('\n', out);
	}

	if (_abort_on_internal_errors < 0)
		/* Set when env DM_ABORT_ON_INTERNAL_ERRORS is not "0" */
		_abort_on_internal_errors =
			strcmp(getenv("DM_ABORT_ON_INTERNAL_ERRORS") ? : "0", "0");

	if (_abort_on_internal_errors &&
	    !strncmp(f, INTERNAL_ERROR, sizeof(INTERNAL_ERROR) - 1))
		abort();
}

__attribute__((format(printf, 5, 6)))
static void _dmfilemapd_log_with_errno(int level,
				       const char *file, int line,
				       int dm_errno_or_class,
				       const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	_dmfilemapd_log_line(level, file, line, dm_errno_or_class, f, ap);
	va_end(ap);
}

/*
 * Only used for reporting errors before daemonise().
 */
__attribute__((format(printf, 1, 2)))
static void _early_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void _setup_logging(void)
{
	dm_log_init_verbose(_verbose - 1);
	dm_log_with_errno_init(_dmfilemapd_log_with_errno);
}

static int _bind_stats_from_fd(struct dm_stats *dms, int fd)
{
        int major, minor;
        struct stat buf;

        if (fstat(fd, &buf)) {
                log_error("fstat failed for fd %d.", fd);
                return 0;
        }

        major = (int) MAJOR(buf.st_dev);
        minor = (int) MINOR(buf.st_dev);

        if (!dm_stats_bind_devno(dms, major, minor))
                return_0;
        return 1;
}

static int _parse_args(int argc, char **argv, struct filemap_monitor *fm)
{
	char *endptr;

	/* we don't care what is in argv[0]. */
	argc--;
	argv++;

	if (argc < 4) {
		_early_log("Wrong number of arguments.\n");
		_early_log("usage: %s\n", _usage);
		return 1;
	}

	memset(fm, 0, sizeof(*fm));

	fm->fd = strtol(argv[0], &endptr, 10);
	if (*endptr) {
		_early_log("Could not parse file descriptor: %s", argv[0]);
		return 0;
	}

	argc--;
	argv++;

	fm->group_id = strtoull(argv[0], &endptr, 10);
	if (*endptr) {
		_early_log("Could not parse group identifier: %s", argv[0]);
		return 0;
	}

	argc--;
	argv++;

	if (!argv[0] || !strlen(argv[0])) {
		_early_log("Path argument is required.");
		return 0;
	}
	fm->path = dm_strdup(argv[0]);
	if (!fm->path) {
		_early_log("Could not allocate memory for path argument.");
		return 0;
	}

	argc--;
	argv++;

	if (argc) {
		_debug = strtol(argv[0], &endptr, 10);
		if (*endptr) {
			_early_log("Could not parse debug argument: %s.",
				   argv[0]);
			return 0;
		}
		argc--;
		argv++;
		if (argc) {
			_verbose = strtol(argv[0], &endptr, 10);
			if (*endptr) {
				_early_log("Could not parse verbose "
					   "argument: %s", argv[0]);
				return 0;
			}
			if (_verbose < 0 || _verbose > 3) {
				_early_log("Verbose argument out of range: %d.",
					   _verbose);
				return 0;
			}
		}
	}
	if (_verbose)
		_early_log("dmfilemapd starting: fd=%d", fm->fd);
	return 1;
}

static int _filemap_fd_check_changed(struct filemap_monitor *fm)
{
	int64_t blocks, old_blocks;
	struct stat buf;

	if (fstat(fm->fd, &buf)) {
		log_error("Failed to fstat filemap file descriptor.");
		return -1;
	}

	blocks = buf.st_blocks;

	/* first check? */
	if (fm->blocks < 0)
		old_blocks = buf.st_blocks;
	else
		old_blocks = fm->blocks;

	fm->blocks = blocks;

	return (fm->blocks != old_blocks);
}

static int _filemap_monitor_get_events(struct filemap_monitor *fm)
{
	struct fanotify_event_metadata buf[64];
	ssize_t len;

	len = read(fm->fanotify_fd, (void *) &buf, sizeof(buf));

	if (!len)
		return 0;

	return 1;
}

static int _filemap_monitor_set_notify(struct filemap_monitor *fm)
{
	int fan_fd, fan_flags;
	fan_flags = FAN_CLOEXEC | FAN_CLASS_CONTENT;

	if ((fan_fd = fanotify_init(fan_flags, O_RDONLY | O_LARGEFILE)) < 0) {
		_early_log("Failed to initialise fanotify.");
		return 0;
	}

	if (fanotify_mark(fan_fd, FAN_MARK_ADD, FAN_MODIFY, 0, fm->path)) {
		_early_log("Failed to add fanotify mark.");
		perror("fanotify_mark");
		return 0;
	}
	fm->fanotify_fd = fan_fd;
	return 1;
}

static void _filemap_monitor_end_notify(struct filemap_monitor *fm)
{
	if (close(fm->fanotify_fd))
		log_error("Error closing fanotify fd.");
}

static void _filemap_monitor_destroy(struct filemap_monitor *fm)
{
	_filemap_monitor_end_notify(fm);
	if (close(fm->fd))
		log_error("Error closing fd %d.", fm->fd);
}

static int daemonise(struct filemap_monitor *fm)
{
	pid_t pid = 0, sid;
	int fd;

	if (!(sid = setsid())) {
		_early_log("setsid failed.");
		return 0;
	}

	if ((pid = fork()) < 0) {
		_early_log("Failed to fork daemon process.");
		return 0;
	}

	if (pid > 0) {
		if (_verbose)
			_early_log("Started dmfilemapd with pid=%d", pid);
		exit(0);
	}

	if (chdir("/")) {
		_early_log("Failed to change directory.");
		return 0;
	}

	if (!_verbose) {
		if (close(STDIN_FILENO))
			_early_log("Error closing stdin");
		if (close(STDOUT_FILENO))
			_early_log("Error closing stdout");
		if (close(STDERR_FILENO))
			_early_log("Error closing stderr");
		if ((open("/dev/null", O_RDONLY) < 0) ||
	            (open("/dev/null", O_WRONLY) < 0) ||
		    (open("/dev/null", O_WRONLY) < 0)) {
			_early_log("Error opening stdio streams.");
			return 0;
		}
	}

	for (fd = sysconf(_SC_OPEN_MAX) - 1; fd > STDERR_FILENO; fd--) {
		if (fd == fm->fd)
			continue;
		close(fd);
	}

	return 1;
}

static int _update_regions(struct dm_stats *dms, struct filemap_monitor *fm)
{
	uint64_t *regions = NULL, *region, nr_regions = 0;

	regions = dm_stats_update_regions_from_fd(dms, fm->fd, fm->group_id);
	if (!regions) {
		log_error("Failed to update filemap regions for group_id="
			  FMTu64 ".", fm->group_id);
		return 0;
	}

	for (region = regions; *region != DM_STATS_REGIONS_ALL; region++)
		nr_regions++;

	fm->nr_regions = nr_regions;
	return 1;
}

static void _filemap_monitor_wait(void)
{
	/* limit to two updates/second. */
	usleep(500000);
}

static int _dmfilemapd(struct filemap_monitor *fm)
{
	int running = 1, check = 0;
	struct dm_stats *dms;

	dms = dm_stats_create("dmstats"); /* FIXME */
	if (!_bind_stats_from_fd(dms, fm->fd)) {
		log_error("Could not bind dm_stats handle to file descriptor "
			  "%d", fm->fd);
		goto bad;
	}

	if (!_filemap_monitor_set_notify(fm))
		goto bad;

	do {
		_filemap_monitor_wait();
		if (!_filemap_monitor_get_events(fm))
			continue;

		if ((check = _filemap_fd_check_changed(fm)) < 0)
			goto bad;

		if (check)
			if (!_update_regions(dms, fm))
				goto bad;

		running = !!fm->nr_regions;
	} while (running);

	_filemap_monitor_destroy(fm);
	dm_stats_destroy(dms);
	return 0;

bad:
	_filemap_monitor_destroy(fm);
	dm_stats_destroy(dms);
	log_error("Exiting");
	return 1;
}

/*
 * dmfilemapd <fd> <group_id> <path> [<debug>[<log_level>]]
 */
int main(int argc, char **argv)
{
	struct filemap_monitor fm;

	if (!_parse_args(argc, argv, &fm))
		return 1;

	_setup_logging();

	log_info("Starting dmfilemapd with fd=%d, group_id=" FMTu64 " "
		 "path=%s", fm.fd, fm.group_id, fm.path);

	if (!_debug && !daemonise(&fm))
		return 1;

	return _dmfilemapd(&fm);
}
