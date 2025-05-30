/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_TARGETS_H
#define _LVM_TARGETS_H

#include <stddef.h>
#include <stdint.h>

struct dev_manager;
struct lv_segment;
struct dm_tree_node;

int compose_areas_line(struct dev_manager *dm, struct lv_segment *seg,
		       char *params, size_t paramsize, int *pos,
		       int start_area, int areas);

int add_areas_line(struct dev_manager *dm, struct lv_segment *seg,
                   struct dm_tree_node *node, uint32_t start_area, uint32_t areas);

int build_dev_string(struct dev_manager *dm, char *dlid, char *devbuf,
		     size_t bufsize, const char *desc);

#endif
