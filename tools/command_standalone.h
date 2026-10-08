/*
 * Copyright (C) 2024-2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * You may not use this file except in compliance with the GNU Lesser General Public License v.2.1.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License v.2.1 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Minimal environment for building command.c without tools.h, i.e. for
 * the stand-alone man page generator and for unit tests that textually
 * include command.c.  Everything below is what command.c needs that
 * tools.h would otherwise provide: a stub cmd_context, the log and
 * string shims, the ARG_* flags for args.h and one stub for every arg
 * parser named by vals.h.
 *
 * Include this before command.h and command.c, and define
 * MAN_PAGE_GENERATOR between them.
 */

#ifndef COMMAND_STANDALONE_H
#define COMMAND_STANDALONE_H

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>


#define stack

struct cmd_context {
	void *libmem;
};

#define log_error(fmt, args...) \
do { \
	fprintf(stderr, fmt "\n", ##args); \
} while (0)

#define dm_snprintf snprintf

static int dm_strncpy(char *dest, const char *src, size_t n)
{
	if (memccpy(dest, src, 0, n))
		return 1;

	if (n > 0)
		dest[n - 1] = '\0';

	return 0;
}

static inline int _dm_strncpy(char *dest, const char *src, size_t n) {
	return dm_strncpy(dest, src, n);
}

/*
 * Stand-in for the libdm pool: every allocation is recorded and released
 * together at exit, like dm_pool_destroy().  command.c never frees these
 * strings itself, so a duplicate that is later discarded is still reclaimed.
 */
static void **_standalone_pool_items;
static size_t _standalone_pool_count;
static size_t _standalone_pool_max;

static void _standalone_pool_free_all(void)
{
	size_t i;

	for (i = 0; i < _standalone_pool_count; i++)
		free(_standalone_pool_items[i]);

	free(_standalone_pool_items);
	_standalone_pool_items = NULL;
	_standalone_pool_count = 0;
	_standalone_pool_max = 0;
}

static void *_standalone_pool_track(void *mem)
{
	void **items;
	size_t max;

	if (!mem)
		return NULL;

	if (_standalone_pool_count == _standalone_pool_max) {
		if (!_standalone_pool_max)
			atexit(_standalone_pool_free_all);

		max = _standalone_pool_max ? 2 * _standalone_pool_max : 64;
		if (!(items = realloc(_standalone_pool_items, max * sizeof(*items)))) {
			free(mem);
			return NULL;
		}

		_standalone_pool_items = items;
		_standalone_pool_max = max;
	}

	_standalone_pool_items[_standalone_pool_count++] = mem;

	return mem;
}

static char *dm_pool_strdup(void *p, const char *str)
{
	return _standalone_pool_track(strdup(str));
}

static void *dm_pool_alloc(void *p, size_t size)
{
	return _standalone_pool_track(malloc(size));
}

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
#define ARG_NONINTERACTIVE 0x00000004
#define ARG_LONG_OPT  0x00000008
struct arg_values;

/* needed to include vals.h */
static inline int yes_no_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int activation_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int cachemetadataformat_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int cachemode_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int discards_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int mirrorlog_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int ssize_kb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int size_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int ssize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int psize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int nsize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int uint32_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_sign(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int int_arg_with_plus(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int extents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int sextents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int pextents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int nextents_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int string_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int tag_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int permission_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int metadatatype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int segtype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int alloc_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int locktype_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int readahead_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int regionsize_mb_arg(struct cmd_context *cmd, struct arg_values *av) { return 0; }
static inline int vgmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int pvmetadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int metadatacopies_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int polloperation_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int writemostly_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int syncaction_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int reportformat_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configreport_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int configtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int repairtype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int dumptype_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }
static inline int headings_arg(struct cmd_context *cmd __attribute__((unused)), struct arg_values *av) { return 0; }

#endif /* COMMAND_STANDALONE_H */
