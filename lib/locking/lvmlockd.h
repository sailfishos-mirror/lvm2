/*
 * Copyright (C) 2014-2015 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#ifndef _LVMLOCKD_H
#define _LVMLOCKD_H

#include "libdaemon/client/config-util.h"
#include "libdaemon/client/daemon-client.h"
#include "lib/metadata/metadata-exported.h" /* is_lockd_type() */
#include "daemons/lvmlockd/lvmlockd-client.h"

#define LOCKD_SANLOCK_LV_NAME "lvmlock"

/* lockd_lv flags */
#define LDLV_MODE_NO_SH           0x00000001
#define LDLV_PERSISTENT           0x00000002
#define LDLV_SH_EXISTS_OK         0x00000004
#define LDLV_CREATING_THIN_VOLUME 0x00000008
#define LDLV_CREATING_THIN_POOL   0x00000010
#define LDLV_CREATING_COW_SNAP_ON_THIN 0x00000020

/* lvmlockd result flags */
#define LD_RF_NO_LOCKSPACES     0x00000001
#define LD_RF_NO_GL_LS          0x00000002
#define LD_RF_WARN_GL_REMOVED   0x00000004
#define LD_RF_DUP_GL_LS         0x00000008
#define LD_RF_NO_LM		0x00000010
#define LD_RF_SH_EXISTS		0x00000020

/* lockd_state flags */
#define LDST_EX			0x00000001
#define LDST_SH			0x00000002
#define LDST_FAIL_REQUEST	0x00000004
#define LDST_FAIL_NOLS		0x00000008
#define LDST_FAIL_STARTING	0x00000010
#define LDST_FAIL_OTHER		0x00000020
#define LDST_FAIL		(LDST_FAIL_REQUEST | LDST_FAIL_NOLS | LDST_FAIL_STARTING | LDST_FAIL_OTHER)

/* --lockopt flags */
#define LOCKOPT_FORCE		0x00000001
#define LOCKOPT_SHUPDATE	0x00000002
#define LOCKOPT_NOREFRESH	0x00000004
#define LOCKOPT_SKIPGL		0x00000008
#define LOCKOPT_SKIPVG		0x00000010
#define LOCKOPT_SKIPLV		0x00000020
#define LOCKOPT_AUTO		0x00000040
#define LOCKOPT_NOWAIT		0x00000080
#define LOCKOPT_AUTONOWAIT	0x00000100
#define LOCKOPT_ADOPTLS		0x00000200
#define LOCKOPT_ADOPTGL		0x00000400
#define LOCKOPT_ADOPTVG		0x00000800
#define LOCKOPT_ADOPTLV		0x00001000
#define LOCKOPT_ADOPT		0x00002000
#define LOCKOPT_NODELAY		0x00004000
#define LOCKOPT_REPAIR		0x00008000
#define LOCKOPT_REPAIRGL	0x00010000
#define LOCKOPT_REPAIRVG	0x00020000
#define LOCKOPT_REPAIRLV	0x00040000

#ifdef LVMLOCKD_SUPPORT

void lockd_lockopt_get_flags(const char *str, uint32_t *flags);
int lockd_lockargs_get_user_flags(const char *str, uint32_t *flags);
int lockd_lockargs_get_meta_flags(const char *str, uint32_t *flags);

struct lvresize_params;
struct lvcreate_params;

/* lvmlockd connection and communication */

void lvmlockd_set_socket(const char *sock);
void lvmlockd_set_use(int use);
int lvmlockd_use(void);
void lvmlockd_init(struct cmd_context *cmd);
void lvmlockd_connect(void);
void lvmlockd_disconnect(void);


/* vgcreate/vgremove use init/free */

int lockd_init_vg(struct cmd_context *cmd, struct volume_group *vg,
                  const char *lock_type, int lv_lock_count, const char *set_args);
int lockd_free_vg_before(struct cmd_context *cmd, struct volume_group *vg, int changing, int yes);
void lockd_free_vg_final(struct cmd_context *cmd, struct volume_group *vg);

/* vgrename */

int lockd_rename_vg_before(struct cmd_context *cmd, struct volume_group *vg);
int lockd_rename_vg_final(struct cmd_context *cmd, struct volume_group *vg, int success);

/* start and stop the lockspace for a vg */

int lockd_start_vg(struct cmd_context *cmd, struct volume_group *vg, int *exists);
int lockd_stop_vg(struct cmd_context *cmd, struct volume_group *vg);
int lockd_start_wait(struct cmd_context *cmd);
int lockd_vg_is_started(struct cmd_context *cmd, struct volume_group *vg, uint32_t *cur_gen);
int lockd_vg_is_busy(struct cmd_context *cmd, struct volume_group *vg);

/* locking */

int lockd_global_create(struct cmd_context *cmd, const char *def_mode, const char *vg_lock_type);
int lockd_global(struct cmd_context *cmd, const char *def_mode);
int lockd_vg(struct cmd_context *cmd, const char *vg_name, const char *def_mode,
	     uint32_t flags, uint32_t *lockd_state);
int lockd_vg_update(struct volume_group *vg);

int lockd_lv_name(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id,
		  const char *lock_args, const char *def_mode, uint32_t flags);
int lockd_lv(struct cmd_context *cmd, struct logical_volume *lv,
	     const char *def_mode, uint32_t flags);
int lockd_lv_resize(struct cmd_context *cmd, struct logical_volume *lv,
	     const char *def_mode, uint32_t flags, struct lvresize_params *lp);

/* lvcreate/lvremove use init/free */

int lockd_init_lv(struct cmd_context *cmd, struct volume_group *vg, struct logical_volume *lv, struct lvcreate_params *lp);
int lockd_init_lv_args(struct cmd_context *cmd, struct volume_group *vg,
		       struct logical_volume *lv, const char *lock_type, const char *last_args, const char **lock_args);
int lockd_free_lv(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id, const char *lock_args);
void lockd_free_lv_queue(struct cmd_context *cmd, struct volume_group *vg,
		const char *lv_name, struct id *lv_id, const char *lock_args);
void lockd_free_removed_lvs(struct cmd_context *cmd, struct volume_group *vg, int remove_success);

const char *lockd_running_lock_type(struct cmd_context *cmd, int *found_multiple);

int lockd_lv_uses_lock(struct logical_volume *lv);

int lockd_lv_refresh(struct cmd_context *cmd, struct lvresize_params *lp);

int lockd_query_lv(struct cmd_context *cmd, struct logical_volume *lv, int *ex, int *sh);

int lockd_lvcreate_prepare(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp);
int lockd_lvcreate_lock(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp,
			int creating_thin_pool, int creating_thin_volume, int creating_cow_snapshot, int creating_vdo_volume);
void lockd_lvcreate_done(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp);

int lockd_lvremove_lock(struct cmd_context *cmd, struct logical_volume *lv, struct logical_volume **lv_other, int *other_unlock);
void lockd_lvremove_done(struct cmd_context *cmd, struct logical_volume *lv, struct logical_volume *lv_other, int other_unlock);

int lockd_setlockargs(struct cmd_context *cmd, struct volume_group *vg, const char *set_args, uint64_t *our_key_held);

#else /* LVMLOCKD_SUPPORT */

static inline void lockd_lockopt_get_flags(const char *str, uint32_t *flags)
{
}

static inline int lockd_lockargs_get_user_flags(const char *str, uint32_t *flags)
{
	return 0;
}

static inline void lvmlockd_set_socket(const char *sock)
{
}

static inline void lvmlockd_set_use(int use)
{
}

static inline void lvmlockd_init(struct cmd_context *cmd)
{
}

static inline void lvmlockd_disconnect(void)
{
}

static inline void lvmlockd_connect(void)
{
}

static inline int lvmlockd_use(void)
{
	return 0;
}

static inline int lockd_init_vg(struct cmd_context *cmd, struct volume_group *vg,
                  const char *lock_type, int lv_lock_count, const char *set_args)
{
	return 1;
}

static inline int lockd_free_vg_before(struct cmd_context *cmd, struct volume_group *vg, int changing, int yes)
{
	return 1;
}

static inline void lockd_free_vg_final(struct cmd_context *cmd, struct volume_group *vg)
{
	return;
}

static inline int lockd_rename_vg_before(struct cmd_context *cmd, struct volume_group *vg)
{
	return 1;
}

static inline int lockd_rename_vg_final(struct cmd_context *cmd, struct volume_group *vg, int success)
{
	return 1;
}

static inline int lockd_start_vg(struct cmd_context *cmd, struct volume_group *vg, int *exists)
{
	return 0;
}

static inline int lockd_stop_vg(struct cmd_context *cmd, struct volume_group *vg)
{
	return 0;
}

static inline int lockd_start_wait(struct cmd_context *cmd)
{
	return 0;
}

static inline int lockd_vg_is_started(struct cmd_context *cmd, struct volume_group *vg, uint32_t *cur_gen)
{
	return 0;
}

static inline int lockd_global_create(struct cmd_context *cmd, const char *def_mode, const char *vg_lock_type)
{
	/*
	 * When lvm is built without lvmlockd support, creating a VG with
	 * a shared lock type should fail.
	 */
	if (is_lockd_type(vg_lock_type)) {
		log_error("Using a shared lock type requires lvmlockd.");
		return 0;
	}
	return 1;
}

static inline int lockd_global(struct cmd_context *cmd, const char *def_mode)
{
	return 1;
}

static inline int lockd_vg(struct cmd_context *cmd, const char *vg_name, const char *def_mode,
	     uint32_t flags, uint32_t *lockd_state)
{
	*lockd_state = 0;
	return 1;
}

static inline int lockd_vg_update(struct volume_group *vg)
{
	return 1;
}

static inline int lockd_lv_name(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id,
		  const char *lock_args, const char *def_mode, uint32_t flags)
{
	return 1;
}

static inline int lockd_lv(struct cmd_context *cmd, struct logical_volume *lv,
	     const char *def_mode, uint32_t flags)
{
	return 1;
}

static inline int lockd_lv_resize(struct cmd_context *cmd, struct logical_volume *lv,
	     const char *def_mode, uint32_t flags, struct lvresize_params *lp)
{
	return 1;
}

static inline int lockd_init_lv(struct cmd_context *cmd, struct volume_group *vg, struct logical_volume *lv, struct lvcreate_params *lp)
{
	return 1;
}

static inline int lockd_init_lv_args(struct cmd_context *cmd, struct volume_group *vg,
		       struct logical_volume *lv, const char *lock_type, const char *last_args, const char **lock_args)
{
	return 1;
}

static inline int lockd_free_lv(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id, const char *lock_args)
{
	return 1;
}

static inline void lockd_free_lv_queue(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id, const char *lock_args)
{
}

static inline void lockd_free_removed_lvs(struct cmd_context *cmd, struct volume_group *vg, int remove_success)
{
}

static inline const char *lockd_running_lock_type(struct cmd_context *cmd, int *found_multiple)
{
	log_error("Using a shared lock type requires lvmlockd.");
	return NULL;
}

static inline int lockd_lv_uses_lock(struct logical_volume *lv)
{
	return 0;
}

static inline int lockd_lv_refresh(struct cmd_context *cmd, struct lvresize_params *lp)
{
	return 0;
}

static inline int lockd_query_lv(struct cmd_context *cmd, struct logical_volume *lv, int *ex, int *sh)
{
	return 0;
}

static inline int lockd_lvcreate_prepare(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp)
{
	return 1;
}

static inline int lockd_lvcreate_lock(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp,
		int creating_thin_pool, int creating_thin_volume, int creating_cow_snapshot, int creating_vdo_volume)
{
	return 1;
}

static inline void lockd_lvcreate_done(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp)
{
}

static inline int lockd_lvremove_lock(struct cmd_context *cmd, struct logical_volume *lv, struct logical_volume **lv_other,
		int *other_unlock)
{
	return 1;
}

static inline void lockd_lvremove_done(struct cmd_context *cmd, struct logical_volume *lv, struct logical_volume *lv_other,
		int other_unlock)
{
}

static inline int lockd_vg_is_busy(struct cmd_context *cmd, struct volume_group *vg)
{
	return 0;
}

static inline int lockd_setlockargs(struct cmd_context *cmd, struct volume_group *vg, const char *set_args, uint64_t *our_key_held)
{
	return 0;
}

#endif	/* LVMLOCKD_SUPPORT */

#endif	/* _LVMLOCKD_H */
