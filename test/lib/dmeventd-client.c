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

/*
 * Raw dmeventd protocol client for tests: talks to the daemon fifos
 * directly so short, oversized and malformed messages can be sent
 * without going through libdevmapper-event.
 */

#include "daemons/dmeventd/dmeventd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DM_EVENT_MAX_MSG_SIZE (16 * 1024 * 1024)
#define REPLY_TIMEOUT 5		/* seconds */

/* EAGAIN and EWOULDBLOCK may be the same value, avoid a logical-or warning */
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
#  define WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#else
#  define WOULD_BLOCK(e) ((e) == EAGAIN)
#endif

static void _usage(const char *name)
{
	fprintf(stderr,
		"Usage: %s <mode> [args]\n"
		"  hello                 send HELLO and expect a valid reply\n"
		"  status                send GET_STATUS and print the reply\n"
		"  badreg <mask> <time>  send REGISTER with given fields, expect -EINVAL\n"
		"  truncated             send an incomplete message, expect no reply\n"
		"  oversize              send an oversized message, expect no reply\n"
		"  request               send GET_STATUS without reading the reply\n"
		"  fill                  fill the daemon reply fifo\n"
		"  drain                 drain the daemon reply fifo\n",
		name);
}

static int _open_fifo(const char *path)
{
	int fd = open(path, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		fprintf(stderr, "%s: %s\n", path, strerror(errno));

	return fd;
}

static int _write_all(int fd, const void *buf, size_t size)
{
	const char *p = buf;
	size_t done = 0;
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	time_t deadline = time(NULL) + REPLY_TIMEOUT;

	while (done < size) {
		ssize_t n = write(fd, p + done, size - done);

		if (n > 0) {
			done += n;
			continue;
		}
		if ((n < 0) && (errno == EINTR))
			continue;
		if ((n < 0) && WOULD_BLOCK(errno)) {
			if ((poll(&pfd, 1, 1000) < 0) && (errno != EINTR))
				break;
			if (time(NULL) >= deadline)
				break;
			continue;
		}
		break;
	}

	if (done != size)
		fprintf(stderr, "write: wrote %zu of %zu bytes\n", done, size);

	return done == size;
}

static int _send(int fd, uint32_t cmd, const char *data, size_t size)
{
	uint32_t header[2];
	char *buf;
	int ret = 0;

	header[0] = htonl(cmd);
	header[1] = htonl((uint32_t) size);

	if (!(buf = malloc(2 * sizeof(uint32_t) + size)))
		return 0;

	memcpy(buf, header, sizeof(header));
	if (size)
		memcpy(buf + sizeof(header), data, size);

	if (_write_all(fd, buf, sizeof(header) + size))
		ret = 1;

	free(buf);

	return ret;
}

static int _read_reply(int fd, uint32_t *cmd, char **data)
{
	uint32_t header[2];
	char *buf = NULL;
	size_t size = 0, done = 0;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	time_t deadline = time(NULL) + REPLY_TIMEOUT;
	ssize_t n;

	*cmd = 0;
	*data = NULL;

	while (done < sizeof(header)) {
		if ((poll(&pfd, 1, 1000) < 0) && (errno != EINTR))
			goto bad;
		if (time(NULL) >= deadline)
			goto bad;
		if (!(pfd.revents & POLLIN))
			continue;

		n = read(fd, (char *) header + done, sizeof(header) - done);
		if (n < 0) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			goto bad;
		}
		if (!n)
			goto bad;
		done += n;
	}

	*cmd = ntohl(header[0]);
	size = ntohl(header[1]);
	if (size > DM_EVENT_MAX_MSG_SIZE)
		goto bad;

	if (!(buf = malloc(size + 1)))
		goto bad;

	for (done = 0; done < size; ) {
		if ((poll(&pfd, 1, 1000) < 0) && (errno != EINTR))
			goto bad;
		if (time(NULL) >= deadline)
			goto bad;
		if (!(pfd.revents & POLLIN))
			continue;

		n = read(fd, buf + done, size - done);
		if (n < 0) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			goto bad;
		}
		if (!n)
			goto bad;
		done += n;
	}

	buf[size] = 0;
	*data = buf;

	return 1;
bad:
	free(buf);

	return 0;
}

static int _do_hello(int client, int server)
{
	uint32_t cmd;
	char *data = NULL;
	int ret = 1;

	if (!_send(client, DM_EVENT_CMD_HELLO, "1:1 HELLO", strlen("1:1 HELLO")))
		return 0;

	if (!_read_reply(server, &cmd, &data)) {
		fprintf(stderr, "no reply to HELLO\n");
		ret = 0;
	} else if (cmd || !data || !strstr(data, "HELLO")) {
		fprintf(stderr, "unexpected HELLO reply: status %d data %s\n",
			(int32_t) cmd, data ? : "(none)");
		ret = 0;
	}

	free(data);

	return ret;
}

/* A malformed registration must be rejected with -EINVAL. */
static int _do_badreg(int client, int server, const char *mask, const char *timeout)
{
	char message[256];
	uint32_t cmd;
	char *data = NULL;
	int ret = 1;

	snprintf(message, sizeof(message), "1:1 nosuchplugin nosuchuuid %s %s",
		 mask, timeout);

	if (!_send(client, DM_EVENT_CMD_REGISTER_FOR_EVENT, message, strlen(message)))
		return 0;

	if (!_read_reply(server, &cmd, &data)) {
		fprintf(stderr, "no reply to malformed REGISTER\n");
		ret = 0;
	} else if ((int32_t) cmd != -EINVAL) {
		fprintf(stderr, "malformed REGISTER returned status %d, expected %d\n",
			(int32_t) cmd, -EINVAL);
		ret = 0;
	}

	free(data);

	return ret;
}

/* Send an incomplete message: the daemon has to discard it. */
static int _do_truncated(int client)
{
	uint32_t header[2];

	header[0] = htonl(DM_EVENT_CMD_HELLO);
	header[1] = htonl(64);

	if (!_write_all(client, header, sizeof(header)))
		return 0;

	return _write_all(client, "1:1 HEL", 7);
}

/* A message above the sanity limit has to be discarded. */
static int _do_oversize(int client)
{
	const char leftover[] = "leftover-data";
	uint32_t header[2];
	char message[sizeof(header) + sizeof(leftover) - 1];

	header[0] = htonl(DM_EVENT_CMD_GET_STATUS);
	header[1] = htonl(DM_EVENT_MAX_MSG_SIZE + 1);

	/* Queue header and leftovers atomically, before the daemon drains them. */
	memcpy(message, header, sizeof(header));
	memcpy(message + sizeof(header), leftover, sizeof(leftover) - 1);
	return _write_all(client, message, sizeof(message));
}

static int _do_status(int client, int server)
{
	const char *message = "1:1 GET_STATUS";
	uint32_t cmd;
	char *data = NULL;
	int ret = 1;

	if (!_send(client, DM_EVENT_CMD_GET_STATUS, message, strlen(message)))
		return 0;

	if (!_read_reply(server, &cmd, &data)) {
		fprintf(stderr, "no reply to GET_STATUS\n");
		ret = 0;
	} else
		printf("%d %s\n", (int32_t) cmd, data ? : "");

	free(data);

	return ret;
}

/* Fill the daemon reply fifo so the next write cannot make progress. */
static int _do_fill(int server)
{
	char buf[4096];

	memset(buf, 'x', sizeof(buf));

	for (;;) {
		ssize_t n = write(server, buf, sizeof(buf));

		if (n > 0)
			continue;
		if ((n < 0) && (errno == EINTR))
			continue;
		if ((n < 0) && WOULD_BLOCK(errno))
			return 1;
		if (n < 0)
			fprintf(stderr, "fill: %s\n", strerror(errno));
		return 0;
	}
}

/* Discard everything buffered in the daemon reply fifo. */
static int _do_drain(int server)
{
	char buf[4096];

	for (;;) {
		ssize_t n = read(server, buf, sizeof(buf));

		if (n > 0)
			continue;
		if ((n < 0) && (errno == EINTR))
			continue;
		if ((n < 0) && WOULD_BLOCK(errno))
			return 1;
		if (!n)
			return 1;
		fprintf(stderr, "drain: %s\n", strerror(errno));
		return 0;
	}
}

int main(int argc, char *argv[])
{
	const char *mode;
	int client = -1, server = -1, ret = 0;
	int need_client, need_server;

	if (argc < 2) {
		_usage(argv[0]);
		return 1;
	}
	mode = argv[1];

	if (strcmp(mode, "hello") && strcmp(mode, "truncated") &&
	    strcmp(mode, "oversize") && strcmp(mode, "fill") &&
	    strcmp(mode, "drain") && strcmp(mode, "badreg") &&
	    strcmp(mode, "status") && strcmp(mode, "request")) {
		_usage(argv[0]);
		return 1;
	}

	need_client = !strcmp(mode, "hello") || !strcmp(mode, "status") ||
		      !strcmp(mode, "badreg") || !strcmp(mode, "truncated") ||
		      !strcmp(mode, "oversize") || !strcmp(mode, "request");
	need_server = !strcmp(mode, "hello") || !strcmp(mode, "status") ||
		      !strcmp(mode, "badreg") || !strcmp(mode, "fill") ||
		      !strcmp(mode, "drain");

	if (need_client && ((client = _open_fifo(DM_EVENT_FIFO_CLIENT)) < 0))
		goto out;
	if (need_server && ((server = _open_fifo(DM_EVENT_FIFO_SERVER)) < 0))
		goto out;

	if (!strcmp(mode, "hello"))
		ret = _do_hello(client, server);
	else if (!strcmp(mode, "status"))
		ret = _do_status(client, server);
	else if (!strcmp(mode, "badreg")) {
		if (argc != 4) {
			_usage(argv[0]);
			goto out;
		}
		ret = _do_badreg(client, server, argv[2], argv[3]);
	} else if (!strcmp(mode, "truncated"))
		ret = _do_truncated(client);
	else if (!strcmp(mode, "oversize"))
		ret = _do_oversize(client);
	else if (!strcmp(mode, "request"))
		ret = _send(client, DM_EVENT_CMD_GET_STATUS, "1:1 GET_STATUS",
			    strlen("1:1 GET_STATUS"));
	else if (!strcmp(mode, "fill"))
		ret = _do_fill(server);
	else if (!strcmp(mode, "drain"))
		ret = _do_drain(server);

	if (!ret)
		fprintf(stderr, "%s failed\n", mode);
out:
	if (client >= 0)
		close(client);
	if (server >= 0)
		close(server);

	return ret ? 0 : 1;
}
