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
	CONFIG_VALUE_STRING,
	CONFIG_VALUE_UINT64,
	CONFIG_VALUE_UINT32,
	CONFIG_VALUE_LIST,
} config_value_t;

struct config_values {
	const char *path;       /* option name/path to look for */
	config_value_t type;	/* type of resulting value */
	void *result;		/* where to place resulting value of given type */
	int mandatory;		/* If this path is missing in config node, import fails */
};

int text_import_values(const struct dm_config_node *cn,
		       size_t values_count, struct config_values *values);

#endif
