/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * Shared dmeventd LVM2 lock/unlock stubs for unit tests.
 * Define DMEVENTD_UNIT_LVM2_PREFIX (e.g. mirror_test) before including.
 */

#ifndef DMEVENTD_UNIT_LVM2_LOCK_H
#define DMEVENTD_UNIT_LVM2_LOCK_H

#ifndef DMEVENTD_UNIT_LVM2_PREFIX
#error "Define DMEVENTD_UNIT_LVM2_PREFIX before including dmeventd_unit_lvm2_lock.h"
#endif

#define DMEVENTD_UNIT_LVM2_CAT(a, b) a##b
#define DMEVENTD_UNIT_LVM2_CAT2(a, b) DMEVENTD_UNIT_LVM2_CAT(a, b)
#define DMEVENTD_UNIT_LVM2_SYM(suffix) \
	DMEVENTD_UNIT_LVM2_CAT2(DMEVENTD_UNIT_LVM2_PREFIX, suffix)

int DMEVENTD_UNIT_LVM2_SYM(_lvm2_init)(void) { return 1; }
void DMEVENTD_UNIT_LVM2_SYM(_lvm2_exit)(void) {}
void DMEVENTD_UNIT_LVM2_SYM(_lvm2_lock)(void) { T_ASSERT_EQUAL(_locks++, 0); }
void DMEVENTD_UNIT_LVM2_SYM(_lvm2_unlock)(void) { T_ASSERT_EQUAL(_locks--, 1); }

#undef DMEVENTD_UNIT_LVM2_SYM
#undef DMEVENTD_UNIT_LVM2_CAT2
#undef DMEVENTD_UNIT_LVM2_CAT

#endif
