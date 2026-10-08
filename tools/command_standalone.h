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

static int standalone_dm_strncpy(char *dest, const char *src, size_t n)
{
	if (memccpy(dest, src, 0, n))
		return 1;

	if (n > 0)
		dest[n - 1] = '\0';

	return 0;
}

static inline int _dm_strncpy(char *dest, const char *src, size_t n) {
	return standalone_dm_strncpy(dest, src, n);
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

static char *standalone_dm_pool_strdup(void *p __attribute__((unused)), const char *str)
{
	return _standalone_pool_track(strdup(str));
}

static void *standalone_dm_pool_alloc(void *p __attribute__((unused)), size_t size)
{
	return _standalone_pool_track(malloc(size));
}

#define dm_strncpy standalone_dm_strncpy
#define dm_pool_strdup standalone_dm_pool_strdup
#define dm_pool_alloc standalone_dm_pool_alloc

/* needed to include args.h */
#define ARG_COUNTABLE 0x00000001
#define ARG_GROUPABLE 0x00000002
#define ARG_NONINTERACTIVE 0x00000004
#define ARG_LONG_OPT  0x00000008
struct arg_values;

/*
 * vals.h names a parser function for every value type.  A stand-alone
 * build never parses a value, so every parser is the same stub.
 */
#define STANDALONE_ARG_STUB(name) \
static inline int name(struct cmd_context *cmd __attribute__((unused)), \
		       struct arg_values *av __attribute__((unused))) { return 0; }

/* needed to include vals.h */
STANDALONE_ARG_STUB(activation_arg)
STANDALONE_ARG_STUB(alloc_arg)
STANDALONE_ARG_STUB(cachemetadataformat_arg)
STANDALONE_ARG_STUB(cachemode_arg)
STANDALONE_ARG_STUB(configreport_arg)
STANDALONE_ARG_STUB(configtype_arg)
STANDALONE_ARG_STUB(discards_arg)
STANDALONE_ARG_STUB(dumptype_arg)
STANDALONE_ARG_STUB(extents_arg)
STANDALONE_ARG_STUB(headings_arg)
STANDALONE_ARG_STUB(int_arg)
STANDALONE_ARG_STUB(int_arg_with_plus)
STANDALONE_ARG_STUB(int_arg_with_sign)
STANDALONE_ARG_STUB(locktype_arg)
STANDALONE_ARG_STUB(metadatacopies_arg)
STANDALONE_ARG_STUB(metadatatype_arg)
STANDALONE_ARG_STUB(mirrorlog_arg)
STANDALONE_ARG_STUB(nextents_arg)
STANDALONE_ARG_STUB(nsize_mb_arg)
STANDALONE_ARG_STUB(permission_arg)
STANDALONE_ARG_STUB(pextents_arg)
STANDALONE_ARG_STUB(polloperation_arg)
STANDALONE_ARG_STUB(psize_mb_arg)
STANDALONE_ARG_STUB(pvmetadatacopies_arg)
STANDALONE_ARG_STUB(readahead_arg)
STANDALONE_ARG_STUB(regionsize_mb_arg)
STANDALONE_ARG_STUB(repairtype_arg)
STANDALONE_ARG_STUB(reportformat_arg)
STANDALONE_ARG_STUB(segtype_arg)
STANDALONE_ARG_STUB(sextents_arg)
STANDALONE_ARG_STUB(size_kb_arg)
STANDALONE_ARG_STUB(size_mb_arg)
STANDALONE_ARG_STUB(ssize_kb_arg)
STANDALONE_ARG_STUB(ssize_mb_arg)
STANDALONE_ARG_STUB(string_arg)
STANDALONE_ARG_STUB(syncaction_arg)
STANDALONE_ARG_STUB(tag_arg)
STANDALONE_ARG_STUB(uint32_arg)
STANDALONE_ARG_STUB(vgmetadatacopies_arg)
STANDALONE_ARG_STUB(writemostly_arg)
STANDALONE_ARG_STUB(yes_no_arg)

#endif /* COMMAND_STANDALONE_H */
