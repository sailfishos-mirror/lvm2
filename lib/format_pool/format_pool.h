/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_FORMAT_POOL_H
#define _LVM_FORMAT_POOL_H

#include "metadata.h"

#define FMT_POOL_NAME "pool"
#define FMT_POOL_ORPHAN_VG_NAME ORPHAN_VG_NAME(FMT_POOL_NAME)

#ifdef POOL_INTERNAL
struct format_type *init_pool_format(struct cmd_context *cmd);
#endif

#endif
