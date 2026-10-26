/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
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

#ifndef BLKIO_THREADS_TEST_H
#define BLKIO_THREADS_TEST_H

#include "lib/blkio/internal.h"

/*
 * Test hooks for the threads io engine.  Production callers use
 * create_threads_io_engine(), which passes NULL for the hooks and so pays
 * only a NULL check per I/O and per worker; this header exists only so the
 * unit tests can inject faults the engine cannot reach from outside.
 */
struct threads_test_hooks {
	/* Called in the worker right before the I/O, so a test can park it. */
	void (*before_io)(void);

	/* Make worker creation fail from worker number fail_after on. */
	bool fail_create;
	unsigned fail_after;
};

/*
 * create_threads_io_engine() with test hooks.  The hooks are copied into
 * the engine, so the caller's struct may go away once this returns.
 */
struct io_engine *create_threads_io_engine_with_hooks(unsigned n_threads,
						      const struct threads_test_hooks *test);

#endif
