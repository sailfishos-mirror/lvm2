/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2024 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_TEXT_IMPORT_H
#define _LVM_TEXT_IMPORT_H

#include <inttypes.h>

struct dm_hash_table;
struct lv_segment;
struct dm_config_node;

int text_import_areas(struct lv_segment *seg, const struct dm_config_node *sn,
		      const struct dm_config_value *cv, uint64_t status);

typedef enum {
	CONFIG_VALUE_STRING,    /* const char * */
	CONFIG_VALUE_UINT64,    /* uint64_t * */
	CONFIG_VALUE_UINT32,    /* uint32_t * */
	CONFIG_VALUE_LIST,      /* struct dm_config_value * */
} config_value_t;

struct config_value {
	const char *name;	/* config value name/path to look for */
	void *result;		/* where to store resulting value of expected type */
	config_value_t type;	/* expected value type */
	int mandatory;		/* fail import if this value is missing in config node */
};

/*
 * Parses config values out of config node out of sorted array like this
 *
 * struct config_value values[] = {
 *	{ "value1", &uint_value1, CONFIG_VALUE_UINT32, 1 },
 *	{ "value2", &list_value2, CONFIG_VALUE_LIST,     },
 * };
 */
int text_import_values(const struct dm_config_node *cn,
		       struct config_value *values, size_t values_count);

#endif
