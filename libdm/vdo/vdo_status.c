/*
 * Copyright (C) 2018 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Note: this object is also used by VDO dmeventd plugin for parsing status */
/* File could be included by VDO plugin and can use original libdm library */
#ifndef LIB_DMEVENT_H
#include "libdm/libdevmapper.h"
#endif

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

//----------------------------------------------------------------

static int _tok_eq(const char *b, const char *e, const char *str)
{
	while (b != e) {
		if (!*str || *b != *str)
			return 0;

		b++;
		str++;
	}

	return !*str;
}

static int _parse_operating_mode(const char *b, const char *e, void *context)
{
	static const struct {
		const char str[12];
		enum dm_vdo_operating_mode mode;
	} _table[] = {
		{ "recovering", DM_VDO_MODE_RECOVERING },
		{ "read-only", DM_VDO_MODE_READ_ONLY },
		{ "normal", DM_VDO_MODE_NORMAL },
	};

	enum dm_vdo_operating_mode *r = context;
	unsigned i;
	for (i = 0; i < DM_ARRAY_SIZE(_table); i++) {
		if (_tok_eq(b, e, _table[i].str)) {
			*r = _table[i].mode;
			return 1;
		}
	}

	return 0;
}

static int _parse_compression_state(const char *b, const char *e, void *context)
{
	static const struct {
		const char str[8];
		enum dm_vdo_compression_state state;
	} _table[] = {
		{ "online", DM_VDO_COMPRESSION_ONLINE },
		{ "offline", DM_VDO_COMPRESSION_OFFLINE },
	};

	enum dm_vdo_compression_state *r = context;
	unsigned i;
	for (i = 0; i < DM_ARRAY_SIZE(_table); i++) {
		if (_tok_eq(b, e, _table[i].str)) {
			*r = _table[i].state;
			return 1;
		}
	}

	return 0;
}

static int _parse_recovering(const char *b, const char *e, void *context)
{
	int *r = context;

	if (_tok_eq(b, e, "recovering"))
		*r = 1;

	else if (_tok_eq(b, e, "-"))
		*r = 0;

	else
		return 0;

	return 1;
}

static int _parse_index_state(const char *b, const char *e, void *context)
{
	static const struct {
		const char str[8];
		enum dm_vdo_index_state state;
	} _table[] = {
		{ "error", DM_VDO_INDEX_ERROR },
		{ "closed", DM_VDO_INDEX_CLOSED },
		{ "opening", DM_VDO_INDEX_OPENING },
		{ "closing", DM_VDO_INDEX_CLOSING },
		{ "offline", DM_VDO_INDEX_OFFLINE },
		{ "online", DM_VDO_INDEX_ONLINE },
		{ "unknown", DM_VDO_INDEX_UNKNOWN },
	};

	enum dm_vdo_index_state *r = context;
	unsigned i;
	for (i = 0; i < DM_ARRAY_SIZE(_table); i++) {
		if (_tok_eq(b, e, _table[i].str)) {
			*r = _table[i].state;
			return 1;
		}
	}

	return 0;
}

static int _parse_uint64(const char *b, const char *e, void *context)
{
	uint64_t *r = context, n;

	n = 0;
	while (b != e) {
		if (!isdigit(*b))
			return 0;

		n = (n * 10) + (*b - '0');
		b++;
	}

	*r = n;
	return 1;
}

static const char *_eat_space(const char *b, const char *e)
{
	while (b != e && isspace(*b))
		b++;

	return b;
}

static const char *_next_tok(const char *b, const char *e)
{
	const char *te = b;
	while (te != e && !isspace(*te))
		te++;

	return te == b ? NULL : te;
}

static void _set_error(struct dm_vdo_status_parse_result *result, const char *fmt, ...)
__attribute__ ((format(printf, 2, 3)));

static void _set_error(struct dm_vdo_status_parse_result *result, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) vsnprintf(result->error, sizeof(result->error), fmt, ap);
	va_end(ap);
}

static int _parse_field(const char **b, const char *e,
			 int (*p_fn)(const char *, const char *, void *),
			 void *field, const char *field_name,
			 struct dm_vdo_status_parse_result *result)
{
	const char *te;

	te = _next_tok(*b, e);
	if (!te) {
		_set_error(result, "couldn't get token for '%s'", field_name);
		return 0;
	}

	if (!p_fn(*b, te, field)) {
		_set_error(result, "couldn't parse '%s'", field_name);
		return 0;
	}

	*b = _eat_space(te, e);
	return 1;

}

int dm_vdo_status_parse(struct dm_pool *mem, const char *input,
			 struct dm_vdo_status_parse_result *result)
{
	const char *b = input;
	const char *e = input + strlen(input);
	const char *te;
	struct dm_vdo_status *s;

	s = (!mem) ? dm_zalloc(sizeof(*s)) : dm_pool_zalloc(mem, sizeof(*s));

	if (!s) {
		_set_error(result, "out of memory");
		return 0;
	}

	b = _eat_space(b, e);
	te = _next_tok(b, e);
	if (!te) {
		_set_error(result, "couldn't get token for device");
		goto bad;
	}

	if (!(s->device = (!mem) ? strndup(b, (te - b)) : dm_pool_strndup(mem, b, (te - b)))) {
		_set_error(result, "out of memory");
		goto bad;
	}

	b = _eat_space(te, e);

#define XX(p, f, fn) if (!_parse_field(&b, e, p, f, fn, result)) goto bad;
	XX(_parse_operating_mode, &s->operating_mode, "operating mode");
	XX(_parse_recovering, &s->recovering, "recovering");
	XX(_parse_index_state, &s->index_state, "index state");
	XX(_parse_compression_state, &s->compression_state, "compression state");
	XX(_parse_uint64, &s->used_blocks, "used blocks");
	XX(_parse_uint64, &s->total_blocks, "total blocks");
#undef XX

	if (b != e) {
		_set_error(result, "too many tokens");
		goto bad;
	}

	result->status = s;
	return 1;

bad:
	if (s && !mem) {
		free(s->device);
		free(s);
	}
	return 0;
}

//----------------------------------------------------------------
