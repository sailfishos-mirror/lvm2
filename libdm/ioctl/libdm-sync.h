/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
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

#ifndef LIBDM_SYNC_H
#define LIBDM_SYNC_H

#include "libdm-async.h"

/* Synchronous single-slot backend (always compiled). */
struct dm_async_ctx *dm_async_ctx_alloc_sync(int fd);

#endif /* LIBDM_SYNC_H */
