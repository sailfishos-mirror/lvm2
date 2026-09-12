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

#ifdef DMEVENTD
/*
 * Whitebox unit test: the client message parser and the client FIFO
 * helpers are static in dmeventd.c.  Include the daemon source itself
 * (with main() renamed) so the tests run against the exact code that
 * ships, without exporting the internals just for testing.
 */
int dmeventd_main_unused(int argc, char *argv[]);
#define main dmeventd_main_unused
#include "../daemons/dmeventd/dmeventd.c"
#undef main

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

/* Fetch a string field ending at a delimiter. */
static void test_fetch_string_delimiter(void *fixture)
{
	char buf[] = "id rest";
	char *src = buf;
	char *ptr = NULL;

	T_ASSERT(_fetch_string(&ptr, &src, ' '));
	T_ASSERT(ptr && !strcmp(ptr, "id"));
	T_ASSERT_EQUAL(*src, 'r');	/* Cursor on the next field */
	free(ptr);
}

/*
 * The last field of a message is not followed by a delimiter: the
 * cursor has to stay on the terminating NUL so that reading further
 * (missing) fields cannot walk past the buffer.
 */
static void test_fetch_string_last_field(void *fixture)
{
	char buf[] = "last";
	char *src = buf;
	char *ptr = NULL;
	char *missing = (char *) 1;

	T_ASSERT(_fetch_string(&ptr, &src, ' '));
	T_ASSERT(ptr && !strcmp(ptr, "last"));
	T_ASSERT_EQUAL(*src, '\0');

	/* Missing fields parse as empty, not out of bounds */
	T_ASSERT(_fetch_string(&missing, &src, ' '));
	T_ASSERT(!missing);
	free(ptr);
}

/* An empty ('-') field yields no string and consumes the field. */
static void test_fetch_string_empty_field(void *fixture)
{
	char buf[] = "- x";
	char *src = buf;
	char *ptr = NULL;

	T_ASSERT(_fetch_string(&ptr, &src, ' '));
	T_ASSERT(!ptr);
	T_ASSERT_EQUAL(*src, 'x');
}

static void _check_unsigned(const char *str, int expect_ret, unsigned expect_value)
{
	unsigned value = 1234;
	int ret = _fetch_unsigned(str, &value);

	T_ASSERT_EQUAL(ret, expect_ret);
	if (ret > 0)
		T_ASSERT_EQUAL(value, expect_value);
	else
		T_ASSERT_EQUAL(value, 0);
}

static void test_fetch_unsigned_valid(void *fixture)
{
	_check_unsigned(NULL, 0, 0);		/* Missing field */
	_check_unsigned("0", 1, 0);
	_check_unsigned("10", 1, 10);
	_check_unsigned("4294967295", 1, UINT_MAX);
}

static void test_fetch_unsigned_invalid(void *fixture)
{
	_check_unsigned("", -1, 0);		/* Empty string */
	_check_unsigned("abc", -1, 0);
	_check_unsigned("12abc", -1, 0);
	_check_unsigned("4294967296", -1, 0);	/* > UINT_MAX */
	_check_unsigned("-1", -1, 0);
	_check_unsigned("-18446744073709551615", -1, 0);
	_check_unsigned("+1", -1, 0);
	_check_unsigned("\t1", -1, 0);
}

static int _parse(const char *message, struct message_data *md,
		  struct dm_event_daemon_message *msg)
{
	msg->cmd = DM_EVENT_CMD_REGISTER_FOR_EVENT;
	msg->size = strlen(message);
	if (!(msg->data = strdup(message)))
		return 0;

	memset(md, 0, sizeof(*md));
	md->msg = msg;

	return _parse_message(md);
}

static void test_parse_message_valid(void *fixture)
{
	struct dm_event_daemon_message msg = { 0 };
	struct message_data md;

	T_ASSERT(_parse("1:1 thin /dev/vg/lv 5 20", &md, &msg));
	T_ASSERT(md.id && !strcmp(md.id, "1:1"));
	T_ASSERT(md.dso_name && !strcmp(md.dso_name, "thin"));
	T_ASSERT(md.device_uuid && !strcmp(md.device_uuid, "/dev/vg/lv"));
	T_ASSERT_EQUAL(md.events_field, 5);
	T_ASSERT_EQUAL(md.timeout_secs, 20);
	T_ASSERT(!msg.data);

	_free_message(&md);
}

/* A missing (or zero) timeout gets the default; missing fields are empty. */
static void test_parse_message_defaults(void *fixture)
{
	struct dm_event_daemon_message msg = { 0 };
	struct message_data md;

	T_ASSERT(_parse("1:1 thin uuid 5 0", &md, &msg));
	T_ASSERT_EQUAL(md.events_field, 5);
	T_ASSERT_EQUAL(md.timeout_secs, DM_EVENT_DEFAULT_TIMEOUT);
	_free_message(&md);

	memset(&msg, 0, sizeof(msg));
	T_ASSERT(_parse("1:1 thin uuid", &md, &msg));
	T_ASSERT(!md.events_str);
	T_ASSERT(!md.timeout_str);
	T_ASSERT_EQUAL(md.events_field, 0);
	T_ASSERT_EQUAL(md.timeout_secs, DM_EVENT_DEFAULT_TIMEOUT);
	_free_message(&md);
}

static void test_parse_message_malformed(void *fixture)
{
	struct dm_event_daemon_message msg = { 0 };
	struct message_data md;

	T_ASSERT(!_parse("1:1 thin uuid abc 10", &md, &msg));
	T_ASSERT(!msg.data);
	_free_message(&md);

	memset(&msg, 0, sizeof(msg));
	T_ASSERT(!_parse("1:1 thin uuid 1 xyz", &md, &msg));
	T_ASSERT(!msg.data);
	_free_message(&md);

	memset(&msg, 0, sizeof(msg));
	T_ASSERT(!_parse("1:1 thin uuid 4294967296 10", &md, &msg));
	T_ASSERT(!msg.data);
	_free_message(&md);
}

static void _write_message(int fd, uint32_t cmd, const char *data)
{
	size_t size = strlen(data);
	size_t total = 2 * sizeof(uint32_t) + size;
	uint32_t *buf;
	ssize_t written;

	T_ASSERT((buf = malloc(total)));
	buf[0] = htonl(cmd);
	buf[1] = htonl(size);
	memcpy((char *) buf + 2 * sizeof(uint32_t), data, size);

	for (written = 0; written < (ssize_t) total; ) {
		ssize_t n = write(fd, (char *) buf + written, total - written);
		T_ASSERT(n > 0);
		written += n;
	}

	free(buf);
}

/* Complete message over a non-blocking fifo. */
static void test_client_read(void *fixture)
{
	char payload[] = "hello";
	struct dm_event_daemon_message msg = { 0 };
	struct dm_event_fifos fifos = { .client = -1, .client_path = "test" };
	int fds[2];

	T_ASSERT(!pipe(fds));
	T_ASSERT(!fcntl(fds[0], F_SETFL, O_NONBLOCK));
	fifos.client = fds[0];

	_write_message(fds[1], DM_EVENT_CMD_GET_STATUS, payload);

	T_ASSERT(_client_read(&fifos, &msg));
	T_ASSERT_EQUAL(msg.cmd, DM_EVENT_CMD_GET_STATUS);
	T_ASSERT_EQUAL(msg.size, strlen(payload));
	T_ASSERT(msg.data && !strcmp(msg.data, payload));

	free(msg.data);
	close(fds[0]);
	close(fds[1]);
}

/*
 * A size above the sanity limit is rejected and the fifo is drained,
 * so leftovers cannot be parsed as part of the next request.
 */
static void test_client_read_oversized(void *fixture)
{
	struct dm_event_daemon_message msg = { 0 };
	struct dm_event_fifos fifos = { .client = -1, .client_path = "test" };
	uint32_t header[2];
	char buf[8];
	ssize_t n;
	int fds[2];

	T_ASSERT(!pipe(fds));
	T_ASSERT(!fcntl(fds[0], F_SETFL, O_NONBLOCK));
	fifos.client = fds[0];

	header[0] = htonl(DM_EVENT_CMD_GET_STATUS);
	header[1] = htonl(DM_EVENT_MAX_MSG_SIZE + 1);
	T_ASSERT_EQUAL(write(fds[1], header, sizeof(header)), (ssize_t) sizeof(header));
	T_ASSERT_EQUAL(write(fds[1], "junk", 4), (ssize_t) 4);

	T_ASSERT(!_client_read(&fifos, &msg));
	T_ASSERT(!msg.data);

	/* The payload was drained */
	n = read(fds[0], buf, sizeof(buf));
	T_ASSERT((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)));

	close(fds[0]);
	close(fds[1]);
}

static void *_write_delayed_body(void *arg)
{
	int fd = *(int *) arg;

	usleep(1500000);
	(void) write(fd, "hello", 5);
	return NULL;
}

/* Receiving a header starts a message, even before any body bytes arrive. */
static void test_client_read_delayed_body(void *fixture)
{
	struct dm_event_daemon_message msg = { 0 };
	struct dm_event_fifos fifos = { .client_path = "test" };
	uint32_t header[] = { htonl(DM_EVENT_CMD_HELLO), htonl(5) };
	pthread_t writer;
	int fds[2], ret;

	T_ASSERT(!pipe(fds));
	T_ASSERT(!fcntl(fds[0], F_SETFL, O_NONBLOCK));
	fifos.client = fds[0];
	T_ASSERT_EQUAL(write(fds[1], header, sizeof(header)), (ssize_t) sizeof(header));
	T_ASSERT(!pthread_create(&writer, NULL, _write_delayed_body, &fds[1]));
	ret = _client_read(&fifos, &msg);
	T_ASSERT(!pthread_join(writer, NULL));
	close(fds[0]);
	close(fds[1]);
	T_ASSERT(ret);
	T_ASSERT(msg.data && !strcmp(msg.data, "hello"));
	free(msg.data);
}

static void test_client_read_header_only(void *fixture)
{
	struct dm_event_daemon_message msg = { 0 };
	struct dm_event_fifos fifos = { .client_path = "test" };
	uint32_t header[] = { htonl(DM_EVENT_CMD_HELLO), htonl(5) };
	time_t start;
	int fds[2];

	T_ASSERT(!pipe(fds));
	T_ASSERT(!fcntl(fds[0], F_SETFL, O_NONBLOCK));
	fifos.client = fds[0];
	T_ASSERT_EQUAL(write(fds[1], header, sizeof(header)), (ssize_t) sizeof(header));
	start = _get_curr_time();
	T_ASSERT(!_client_read(&fifos, &msg));
	close(fds[0]);
	close(fds[1]);
	T_ASSERT(!msg.data);
	T_ASSERT(_get_curr_time() - start >= DM_EVENT_MSG_READ_TIMEOUT);
}

static void test_client_write(void *fixture)
{
	struct dm_event_daemon_message msg = {
		.cmd = DM_EVENT_CMD_GET_STATUS,
		.data = (char *) "ok",
		.size = 2
	};
	struct dm_event_fifos fifos = { .server = -1, .server_path = "test" };
	uint32_t header[2];
	char payload[3] = { 0 };
	int fds[2];

	T_ASSERT(!pipe(fds));
	T_ASSERT(!fcntl(fds[1], F_SETFL, O_NONBLOCK));
	fifos.server = fds[1];

	T_ASSERT(_client_write(&fifos, &msg));
	T_ASSERT_EQUAL(read(fds[0], header, sizeof(header)), (ssize_t) sizeof(header));
	T_ASSERT_EQUAL(ntohl(header[0]), (uint32_t) DM_EVENT_CMD_GET_STATUS);
	T_ASSERT_EQUAL(ntohl(header[1]), (uint32_t) 2);
	T_ASSERT_EQUAL(read(fds[0], payload, 2), (ssize_t) 2);
	T_ASSERT(!strcmp(payload, "ok"));

	close(fds[0]);
	close(fds[1]);
}

/*
 * Preloaded (systemd) fifos must be fifos with the attributes
 * _open_fifo() requires and get O_NONBLOCK set.
 */
static void test_preloaded_fifo(void *fixture)
{
	char dir[] = "dmeventd-fifo-XXXXXX";
	char path[PATH_MAX];
	int fd;

	T_ASSERT(mkdtemp(dir));
	T_ASSERT(dm_snprintf(path, sizeof(path), "%s/fifo", dir) > 0);

	/* A regular file is not accepted */
	fd = open("/dev/null", O_RDONLY);
	T_ASSERT(fd >= 0);
	T_ASSERT(!fcntl(fd, F_SETFD, 0));
	T_ASSERT(!_handle_preloaded_fifo(fd, "/dev/null"));
	close(fd);

	T_ASSERT(!mkfifo(path, 0600));
	T_ASSERT(!chmod(path, 0600));
	fd = open(path, O_RDWR | O_NONBLOCK);
	T_ASSERT(fd >= 0);
	T_ASSERT(!fcntl(fd, F_SETFD, 0));	/* Pretend it came from systemd */

	if (geteuid() == 0) {
		T_ASSERT(_handle_preloaded_fifo(fd, path));
		T_ASSERT(fcntl(fd, F_GETFD) & FD_CLOEXEC);
		T_ASSERT(fcntl(fd, F_GETFL) & O_NONBLOCK);
	} else {
		/* Fifo not owned by root is rejected */
		T_ASSERT(!_handle_preloaded_fifo(fd, path));
	}

	close(fd);
	T_ASSERT(!unlink(path));
	T_ASSERT(!rmdir(dir));
}

#define T(path, desc, fn) register_test(ts, "/dmeventd/" path, desc, fn)
#endif

void dmeventd_tests(struct dm_list *all_tests)
{
#ifdef DMEVENTD
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}

	T("parse/fetch-string-delimiter", "fetch string before delimiter",
	  test_fetch_string_delimiter);
	T("parse/fetch-string-last-field", "last field leaves cursor on NUL",
	  test_fetch_string_last_field);
	T("parse/fetch-string-empty-field", "empty field yields no string",
	  test_fetch_string_empty_field);
	T("parse/fetch-unsigned-valid", "valid unsigned fields",
	  test_fetch_unsigned_valid);
	T("parse/fetch-unsigned-invalid", "invalid unsigned fields rejected",
	  test_fetch_unsigned_invalid);
	T("parse/message-valid", "full registration message",
	  test_parse_message_valid);
	T("parse/message-defaults", "missing fields and default timeout",
	  test_parse_message_defaults);
	T("parse/message-malformed", "malformed numeric fields rejected",
	  test_parse_message_malformed);
	T("fifo/client-read", "read a complete message",
	  test_client_read);
	T("fifo/client-read-oversized", "oversized message drained",
	  test_client_read_oversized);
	if (extended_tests) {
		T("fifo/client-read-delayed-body", "wait for body after complete header",
		  test_client_read_delayed_body);
		T("fifo/client-read-header-only", "header-only message times out",
		  test_client_read_header_only);
	}
	T("fifo/client-write", "write a reply",
	  test_client_write);
	T("fifo/preloaded-fifo", "preloaded fifo validated",
	  test_preloaded_fifo);

	dm_list_add(all_tests, &ts->list);
#endif
}
