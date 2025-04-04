/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#ifndef _DM_TTREE_H
#define _DM_TTREE_H

struct ttree;
struct dm_pool;

struct ttree *ttree_create(struct dm_pool *mem, unsigned int klen);

void *ttree_lookup(struct ttree *tt, unsigned *key);
int ttree_insert(struct ttree *tt, unsigned *key, void *data);

#endif
