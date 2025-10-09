/*
 * Copyright (C) 2014-2015 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#ifndef _LVM_LVMLOCKD_INTERNAL_H
#define _LVM_LVMLOCKD_INTERNAL_H

#include "base/memory/container_of.h"

#include <stdint.h>
#include <pthread.h>

#define MAX_NAME 64
#define MAX_ARGS 64

#define R_NAME_GL_DISABLED "_GLLK_disabled"
#define R_NAME_GL          "GLLK"
#define R_NAME_VG          "VGLK"
#define S_NAME_GL_DLM      "lvm_global"
#define S_NAME_GL_IDM      "lvm_global"
#define LVM_LS_PREFIX      "lvm_"           /* ls name is prefix + vg_name */
/* global lockspace name for sanlock is a vg name */

/* lock manager types */
enum {
	LD_LM_NONE = 0,
	LD_LM_UNUSED = 1, /* place holder so values match lib/locking/lvmlockd.h */
	LD_LM_DLM = 2,
	LD_LM_SANLOCK = 3,
	LD_LM_IDM = 4,
};

/* operation types */
enum {
	LD_OP_HELLO = 1,
	LD_OP_QUIT,
	LD_OP_INIT,
	LD_OP_FREE,
	LD_OP_START,
	LD_OP_STOP,
	LD_OP_LOCK,
	LD_OP_UPDATE,
	LD_OP_CLOSE,
	LD_OP_ENABLE,
	LD_OP_DISABLE,
	LD_OP_START_WAIT,
	LD_OP_STOP_ALL,
	LD_OP_DUMP_INFO,
	LD_OP_DUMP_LOG,
	LD_OP_RENAME_BEFORE,
	LD_OP_RENAME_FINAL,
	LD_OP_RUNNING_LM,
	LD_OP_FIND_FREE_LOCK,
	LD_OP_KILL_VG,
	LD_OP_DROP_VG,
	LD_OP_BUSY,
	LD_OP_QUERY_LOCK,
	LD_OP_REFRESH_LV,
	LD_OP_VG_STATUS,
	LD_OP_FENCE,
	LD_OP_FENCE_RESULT,
	LD_OP_SETLOCKARGS_BEFORE,
	LD_OP_SETLOCKARGS_FINAL,
};

/* resource types */
enum {
	LD_RT_GL = 1,
	LD_RT_VG,
	LD_RT_LV,
};

/* lock modes, more restrictive must be larger value */
enum {
	LD_LK_IV = -1,
	LD_LK_UN = 0,
	LD_LK_NL = 1,
	LD_LK_SH = 2,
	LD_LK_EX = 3,
};

struct list_head {
	struct list_head *next, *prev;
};

struct client {
	struct list_head list;
	pthread_mutex_t mutex;
	int pid;
	int fd;
	int pi;
	uint32_t id;
	unsigned int recv : 1;
	unsigned int dead : 1;
	unsigned int poll_ignore : 1;
	unsigned int lock_ops : 1;
	char name[MAX_NAME+1];
};

#define LD_AF_PERSISTENT           0x00000001
#define LD_AF_NO_CLIENT            0x00000002
#define LD_AF_UNLOCK_CANCEL        0x00000004
#define LD_AF_NEXT_VERSION         0x00000008
#define LD_AF_WAIT                 0x00000010
#define LD_AF_FORCE                0x00000020
#define LD_AF_EX_DISABLE           0x00000040
#define LD_AF_ENABLE               0x00000080
#define LD_AF_DISABLE              0x00000100
#define LD_AF_SEARCH_LS            0x00000200
#define LD_AF_WAIT_STARTING        0x00001000
#define LD_AF_DUP_GL_LS            0x00002000
#define LD_AF_ADOPT                0x00010000 /* adopt ok but not required */
#define LD_AF_WARN_GL_REMOVED	   0x00020000
#define LD_AF_LV_LOCK              0x00040000
#define LD_AF_LV_UNLOCK            0x00080000
#define LD_AF_SH_EXISTS            0x00100000
#define LD_AF_ADOPT_ONLY           0x00200000 /* adopt orphan or fail */
#define LD_AF_NODELAY              0x00400000
#define LD_AF_REPAIR		   0x00800000
#define LD_AF_NO_TIMEOUT	   0x01000000

/*
 * Number of times to repeat a lock request after
 * a lock conflict (-EAGAIN) if unspecified in the
 * request.
 */
#define DEFAULT_MAX_RETRIES 4

struct pvs {
	char **path;
	int num;
};

#define RUN_COMMAND_LEN 1024
#define MAX_AV_COUNT 32
#define ONE_ARG_LEN 256

/* helper_msg types */
#define HELPER_COMMAND 0x1
#define HELPER_COMMAND_RESULT 0x2

struct helper_msg {
	uint8_t  type;
	uint8_t  act;
	uint16_t unused1;
	uint32_t msg_id;
	int pid;
	int result;
	char ls_name[MAX_NAME+1];
	uint8_t unused2;
	uint16_t unused3;
	char command[RUN_COMMAND_LEN];
};

struct helper_msg_list {
	struct helper_msg msg;
	struct list_head list;
};

#define OWNER_NAME_SIZE 64
#define OWNER_STATE_SIZE 32

struct owner {
	uint32_t host_id;
	uint32_t generation;
	uint32_t timestamp;
	char state[OWNER_STATE_SIZE];
	char name[OWNER_NAME_SIZE];
};

struct action {
	struct list_head list;
	uint32_t client_id;
	uint32_t flags;			/* LD_AF_ */
	uint32_t msg_id;
	uint32_t version;
	uint32_t host_id;
	uint64_t ourkey;
	uint64_t remkey;
	uint64_t lv_size_bytes;
	uint64_t ls_generation;
	int8_t op;			/* operation type LD_OP_ */
	int8_t rt;			/* resource type LD_RT_ */
	int8_t mode;			/* lock mode LD_LK_ */
	int8_t lm_type;			/* lock manager: LM_DLM, LM_SANLOCK */
	int retries;
	int max_retries;
	int result;
	int lm_rv;			/* return value from lm_ function */
	int align_mb;
	char *path;
	char vg_uuid[64];
	char vg_name[MAX_NAME+1];
	char lv_name[MAX_NAME+1];
	char lv_uuid[MAX_NAME+1];
	char vg_args[MAX_ARGS+1];
	char lv_args[MAX_ARGS+1];
	char other_args[MAX_ARGS+1];
	struct owner owner;
	struct pvs pvs;			/* PV list for idm */
};

struct resource {
	struct list_head list;		/* lockspace.resources */
	char name[MAX_NAME+1];		/* vg name or lv name */
	int8_t type;			/* resource type LD_RT_ */
	int8_t mode;
	int8_t adopt_mode;
	unsigned int sh_count;		/* number of sh locks on locks list */
	uint32_t version;
	uint32_t last_client_id;	/* last client_id to lock or unlock resource */
	uint32_t dispose_client_id;	/* client_id disposing of resource struct */
	unsigned int lm_init : 1;	/* lm_data is initialized */
	unsigned int adopt : 1;		/* temp flag in remove_inactive_lvs */
	unsigned int version_zero_valid : 1;
	unsigned int use_vb : 1;
	struct list_head locks;
	struct list_head actions;
	struct list_head fence_wait_actions;
	char lv_args[MAX_ARGS+1];
	char lm_data[];			/* lock manager specific data */
};

#define LD_LF_PERSISTENT 0x00000001

struct lock {
	struct list_head list;		/* resource.locks */
	int8_t mode;			/* lock mode LD_LK_ */
	uint32_t version;
	uint32_t flags;			/* LD_LF_ */
	uint32_t client_id; /* may be 0 for persistent or internal locks */
};

struct lockspace {
	struct list_head list;		/* lockspaces */
	char name[MAX_NAME+1];
	char vg_name[MAX_NAME+1];
	char vg_uuid[64];
	char vg_args[MAX_ARGS+1];	/* lock manager specific args */
	int8_t lm_type;			/* lock manager: LM_DLM, LM_SANLOCK */
	void *lm_data;
	uint32_t lock_args_flags;
	uint32_t host_id;
	uint64_t generation;
	uint64_t free_lock_offset;	/* for sanlock, start search for free lock here */
	struct pvs pvs;			/* for idm: PV list */

	uint32_t start_client_id;	/* client_id that started the lockspace */
	pthread_t thread;		/* makes synchronous lock requests */
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	unsigned int create_fail : 1;
	unsigned int create_done : 1;
	unsigned int thread_work : 1;
	unsigned int thread_stop : 1;
	unsigned int thread_done : 1;
	unsigned int sanlock_gl_enabled: 1;
	unsigned int sanlock_gl_dup: 1;
	unsigned int kill_vg: 1;
	unsigned int fence_pr: 1;
	unsigned int no_timeout: 1;

	struct list_head actions;	/* new client actions */
	struct list_head resources;	/* resource/lock state for gl/vg/lv */
	struct list_head dispose;	/* resources to free */
	struct list_head fence_history;	/* internally created actions for fencing */
};

/* val_blk version */
#define VAL_BLK_VERSION 0x0101

/* val_blk flags */
#define VBF_REMOVED 0x0001

struct val_blk {
	uint16_t version;
	uint16_t flags;
	uint32_t r_version;
};

/* lm_unlock flags */
#define LMUF_FREE_VG 0x00000001

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *new,
                              struct list_head *prev,
                              struct list_head *next)
{
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}

static inline int list_empty(const struct list_head *head)
{
	return head->next == head;
}

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

#define list_for_each_entry(pos, head, member)                          \
	for (pos = list_entry((head)->next, __typeof__(*pos), member);      \
	     &pos->member != (head);    \
	     pos = list_entry(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member)                  \
	for (pos = list_entry((head)->next, __typeof__(*pos), member),      \
	     n = list_entry(pos->member.next, __typeof__(*pos), member); \
	     &pos->member != (head);                                    \
	     pos = n, n = list_entry(n->member.next, __typeof__(*n), member))


/* to improve readability */
#define WAIT     1
#define NO_WAIT  0
#define FORCE    1
#define NO_FORCE 0

/*
 * global variables
 */

#ifndef EXTERN
#define EXTERN extern
#define INIT(X)
#else
#undef EXTERN
#define EXTERN
#define INIT(X) =X
#endif

/*
 * gl_type_static and gl_use_ are set by command line or config file
 * to specify whether the global lock comes from dlm or sanlock.
 * Without a static setting, lvmlockd will figure out where the
 * global lock should be (but it could get mixed up in cases where
 * both sanlock and dlm vgs exist.)
 *
 * gl_use_dlm means that the gl should come from lockspace gl_lsname_dlm
 * gl_use_sanlock means that the gl should come from lockspace gl_lsname_sanlock
 *
 * gl_use_dlm has precedence over gl_use_sanlock, so if a node sees both
 * dlm and sanlock vgs, it will use the dlm gl.
 *
 * gl_use_ is set when the first evidence of that lm_type is seen
 * in any command.
 *
 * gl_lsname_sanlock is set when the first vg is seen in which an
 * enabled gl is exists, or when init_vg creates a vg with gl enabled,
 * or when enable_gl is used.
 *
 * gl_lsname_sanlock is cleared when free_vg deletes a vg with gl enabled
 * or when disable_gl matches.
 */

EXTERN int gl_type_static;
EXTERN int gl_use_dlm;
EXTERN int gl_use_sanlock;
EXTERN int gl_use_idm;
EXTERN int gl_vg_removed;
EXTERN char gl_lsname_dlm[MAX_NAME+1];
EXTERN char gl_lsname_sanlock[MAX_NAME+1];
EXTERN char gl_lsname_idm[MAX_NAME+1];
EXTERN int global_dlm_lockspace_exists;
EXTERN int global_idm_lockspace_exists;

EXTERN int daemon_test; /* run as much as possible without a live lock manager */
EXTERN int daemon_debug;
EXTERN int daemon_host_id;
EXTERN const char *daemon_host_id_file;
EXTERN int sanlock_io_timeout;

/*
 * This flag is set to 1 if we see multiple vgs with the global
 * lock enabled.  While this is set, we return a special flag
 * with the vg lock result indicating to the lvm command that
 * there is a duplicate gl in the vg which should be resolved.
 * While this is set, find_lockspace_name has the side job of
 * counting the number of lockspaces with enabled gl's so that
 * this can be set back to zero when the duplicates are disabled.
 */
EXTERN int sanlock_gl_dup;

void log_level(int level, const char *fmt, ...)  __attribute__((format(printf, 2, 3)));
#define log_debug(fmt, args...) log_level(LOG_DEBUG, fmt, ##args)
#define log_error(fmt, args...) log_level(LOG_ERR, fmt, ##args)
#define log_warn(fmt, args...) log_level(LOG_WARNING, fmt, ##args)
#define log_sys_debug(x, y) \
		log_debug("%s: %s failed: %s", y, x, strerror(errno))

struct lockspace *alloc_lockspace(void);
int lockspaces_empty(void);
int last_string_from_args(char *args_in, char *last);
void helper_main(int in_fd, int out_fd, int log_stderr);
int lockd_lockargs_get_user_flags(const char *str, uint32_t *flags);
int lockd_lockargs_get_version(char *str, unsigned int *major, unsigned int *minor, unsigned int *patch);

static inline const char *mode_str(int x)
{
	switch (x) {
	case LD_LK_IV:
		return "iv";
	case LD_LK_UN:
		return "un";
	case LD_LK_NL:
		return "nl";
	case LD_LK_SH:
		return "sh";
	case LD_LK_EX:
		return "ex";
	default:
		return ".";
	};
}

#ifdef LOCKDDLM_SUPPORT

int lm_init_vg_dlm(char *ls_name, char *vg_name, uint32_t flags, char *vg_args);
int lm_prepare_lockspace_dlm(struct lockspace *ls);
int lm_add_lockspace_dlm(struct lockspace *ls, int adopt_only, int adopt_ok);
int lm_purge_locks_dlm(struct lockspace *ls);
int lm_rem_lockspace_dlm(struct lockspace *ls, int free_vg);
int lm_add_resource_dlm(struct lockspace *ls, struct resource *r, int with_lock_nl);
int lm_lock_dlm(struct lockspace *ls, struct resource *r, int ld_mode,
		struct val_blk *vb_out, int adopt_only, int adopt_ok);
int lm_convert_dlm(struct lockspace *ls, struct resource *r,
		   int ld_mode, uint32_t r_version);
int lm_unlock_dlm(struct lockspace *ls, struct resource *r,
		  uint32_t r_version, uint32_t lmu_flags);
int lm_rem_resource_dlm(struct lockspace *ls, struct resource *r);
int lm_get_lockspaces_dlm(struct list_head *ls_rejoin);
int lm_data_size_dlm(void);
int lm_is_running_dlm(void);
int lm_hosts_dlm(struct lockspace *ls, int notify);
int lm_refresh_lv_start_dlm(struct action *act);
int lm_refresh_lv_check_dlm(struct action *act);

static inline int lm_support_dlm(void)
{
	return 1;
}

#else

static inline int lm_init_vg_dlm(char *ls_name, char *vg_name, uint32_t flags, char *vg_args)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_prepare_lockspace_dlm(struct lockspace *ls)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_add_lockspace_dlm(struct lockspace *ls, int adopt_only, int adopt_ok)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_purge_locks_dlm(struct lockspace *ls)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_rem_lockspace_dlm(struct lockspace *ls, int free_vg)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_add_resource_dlm(struct lockspace *ls, struct resource *r, int with_lock_nl)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_lock_dlm(struct lockspace *ls, struct resource *r, int ld_mode,
		struct val_blk *vb_out, int adopt_only, int adopt_ok)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_convert_dlm(struct lockspace *ls, struct resource *r,
		   int ld_mode, uint32_t r_version)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_unlock_dlm(struct lockspace *ls, struct resource *r,
		  uint32_t r_version, uint32_t lmu_flags)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_rem_resource_dlm(struct lockspace *ls, struct resource *r)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_get_lockspaces_dlm(struct list_head *ls_rejoin)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_data_size_dlm(void)
{
	if (daemon_test)
		return 0;
	return -1;
}

static inline int lm_is_running_dlm(void)
{
	if (daemon_test)
		return 1;
	return 0;
}

static inline int lm_support_dlm(void)
{
	if (daemon_test)
		return 1;
	return 0;
}

static inline int lm_hosts_dlm(struct lockspace *ls, int notify)
{
	return 0;
}

static inline int lm_refresh_lv_start_dlm(struct action *act)
{
	return 0;
}

static inline int lm_refresh_lv_check_dlm(struct action *act)
{
	return 0;
}

#endif /* dlm support */

#ifdef LOCKDSANLOCK_SUPPORT

int lm_init_vg_sanlock(char *ls_name, char *vg_name, uint32_t flags, char *vg_args, int opt_align_mb, char *other_args);
int lm_init_lv_sanlock(struct lockspace *ls, char *ls_name, char *vg_name, char *lv_name, char *vg_args, char *lv_args, char *prev_args);
int lm_free_lv_sanlock(struct lockspace *ls, struct resource *r);
int lm_rename_vg_sanlock(char *ls_name, char *vg_name, uint32_t flags, char *vg_args);
int lm_prepare_lockspace_sanlock(struct lockspace *ls, uint64_t *prev_generation, int repair);
int lm_add_lockspace_sanlock(struct lockspace *ls, int adopt_only, int adopt_ok, int nodelay);
int lm_rem_lockspace_sanlock(struct lockspace *ls, int free_vg);
int lm_add_resource_sanlock(struct lockspace *ls, struct resource *r);
int lm_lock_sanlock(struct lockspace *ls, struct resource *r, int ld_mode,
		    struct val_blk *vb_out, int *retry, struct owner *owner,
		    int adopt_only, int adopt_ok, int repair);
int lm_convert_sanlock(struct lockspace *ls, struct resource *r,
		       int ld_mode, uint32_t r_version);
int lm_unlock_sanlock(struct lockspace *ls, struct resource *r,
		      uint32_t r_version, uint32_t lmu_flags);
int lm_able_gl_sanlock(struct lockspace *ls, int enable);
int lm_ex_disable_gl_sanlock(struct lockspace *ls);
int lm_hosts_sanlock(struct lockspace *ls, int notify);
int lm_rem_resource_sanlock(struct lockspace *ls, struct resource *r);
int lm_gl_is_enabled(struct lockspace *ls);
int lm_get_lockspaces_sanlock(struct list_head *ls_rejoin);
int lm_data_size_sanlock(void);
int lm_is_running_sanlock(void);
int lm_find_free_lock_sanlock(struct lockspace *ls, uint64_t lv_size_bytes);
int lm_vg_status_sanlock(struct lockspace *ls, struct action *act);
void lm_set_host_dead_sanlock(struct lockspace *ls, struct owner *owner);
int lm_setlockargs_supported_sanlock(struct lockspace *ls, struct action *act);
int lm_setlockargs_vg_sanlock(char *ls_name, char *vg_name, struct action *act);

static inline int lm_support_sanlock(void)
{
	return 1;
}

#else

static inline int lm_init_vg_sanlock(char *ls_name, char *vg_name, uint32_t flags, char *vg_args, int opt_align_mb, char *other_args)
{
	return -1;
}

static inline int lm_init_lv_sanlock(struct lockspace *ls, char *ls_name, char *vg_name, char *lv_name, char *vg_args, char *lv_args, char *prev_args)
{
	return -1;
}

static inline int lm_free_lv_sanlock(struct lockspace *ls, struct resource *r)
{
	return -1;
}

static inline int lm_rename_vg_sanlock(char *ls_name, char *vg_name, uint32_t flags, char *vg_args)
{
	return -1;
}

static inline int lm_prepare_lockspace_sanlock(struct lockspace *ls, uint64_t *prev_generation, int repair)
{
	return -1;
}

static inline int lm_add_lockspace_sanlock(struct lockspace *ls, int adopt_only, int adopt_ok, int nodelay)
{
	return -1;
}

static inline int lm_rem_lockspace_sanlock(struct lockspace *ls, int free_vg)
{
	return -1;
}

static inline int lm_add_resource_sanlock(struct lockspace *ls, struct resource *r)
{
	return -1;
}

static inline int lm_lock_sanlock(struct lockspace *ls, struct resource *r, int ld_mode,
		    struct val_blk *vb_out, int *retry, struct owner *owner,
		    int adopt_only, int adopt_ok, int repair)
{
	return -1;
}

static inline int lm_convert_sanlock(struct lockspace *ls, struct resource *r,
		       int ld_mode, uint32_t r_version)
{
	return -1;
}

static inline int lm_unlock_sanlock(struct lockspace *ls, struct resource *r,
		      uint32_t r_version, uint32_t lmu_flags)
{
	return -1;
}

static inline int lm_able_gl_sanlock(struct lockspace *ls, int enable)
{
	return -1;
}

static inline int lm_ex_disable_gl_sanlock(struct lockspace *ls)
{
	return -1;
}

static inline int lm_hosts_sanlock(struct lockspace *ls, int notify)
{
	return -1;
}

static inline int lm_rem_resource_sanlock(struct lockspace *ls, struct resource *r)
{
	return -1;
}

static inline int lm_gl_is_enabled(struct lockspace *ls)
{
	return -1;
}

static inline int lm_get_lockspaces_sanlock(struct list_head *ls_rejoin)
{
	return -1;
}

static inline int lm_data_size_sanlock(void)
{
	return -1;
}

static inline int lm_is_running_sanlock(void)
{
	return 0;
}

static inline int lm_find_free_lock_sanlock(struct lockspace *ls, uint64_t lv_size_bytes)
{
	return -1;
}

static inline int lm_vg_status_sanlock(struct lockspace *ls, struct action *act)
{
	return -1;
}

static inline int lm_support_sanlock(void)
{
	return 0;
}

static inline void lm_set_host_dead_sanlock(struct lockspace *ls, struct owner *owner)
{
}

static inline int lm_setlockargs_supported_sanlock(struct lockspace *ls, struct action *act)
{
	return 0;
}

static inline int lm_setlockargs_vg_sanlock(char *ls_name, char *vg_name, struct action *act)
{
	return -1;
}

#endif /* sanlock support */

#ifdef LOCKDIDM_SUPPORT

int lm_data_size_idm(void);
int lm_init_vg_idm(char *ls_name, char *vg_name, uint32_t flags, char *vg_args);
int lm_prepare_lockspace_idm(struct lockspace *ls);
int lm_add_lockspace_idm(struct lockspace *ls, int adopt_only, int adopt_ok);
int lm_rem_lockspace_idm(struct lockspace *ls, int free_vg);
int lm_add_resource_idm(struct lockspace *ls, struct resource *r);
int lm_lock_idm(struct lockspace *ls, struct resource *r, int ld_mode,
		struct val_blk *vb_out, char *lv_uuid, struct pvs *pvs,
		int adopt_only, int adopt_ok);
int lm_convert_idm(struct lockspace *ls, struct resource *r,
		   int ld_mode, uint32_t r_version);
int lm_unlock_idm(struct lockspace *ls, struct resource *r,
		  uint32_t r_version, uint32_t lmu_flags);
int lm_hosts_idm(struct lockspace *ls, int notify);
int lm_get_lockspaces_idm(struct list_head *ls_rejoin);
int lm_is_running_idm(void);
int lm_rem_resource_idm(struct lockspace *ls, struct resource *r);

static inline int lm_support_idm(void)
{
	return 1;
}

#else

static inline int lm_data_size_idm(void)
{
	return -1;
}

static inline int lm_init_vg_idm(char *ls_name, char *vg_name, uint32_t flags,
			  char *vg_args)
{
	return -1;
}

static inline int lm_prepare_lockspace_idm(struct lockspace *ls)
{
	return -1;
}

static inline int lm_add_lockspace_idm(struct lockspace *ls, int adopt_only, int adopt_ok)
{
	return -1;
}

static inline int lm_rem_lockspace_idm(struct lockspace *ls, int free_vg)
{
	return -1;
}

static inline int lm_add_resource_idm(struct lockspace *ls, struct resource *r)
{
	return -1;
}

static inline int lm_lock_idm(struct lockspace *ls, struct resource *r, int ld_mode,
		       struct val_blk *vb_out, char *lv_uuid, struct pvs *pvs,
		       int adopt_only, int adopt_ok)
{
	return -1;
}

static inline int lm_convert_idm(struct lockspace *ls, struct resource *r,
			  int ld_mode, uint32_t r_version)
{
	return -1;
}

static inline int lm_unlock_idm(struct lockspace *ls, struct resource *r,
			 uint32_t r_version, uint32_t lmu_flags)
{
	return -1;
}

static inline int lm_hosts_idm(struct lockspace *ls, int notify)
{
	return -1;
}

static inline int lm_get_lockspaces_idm(struct list_head *ls_rejoin)
{
	return -1;
}

static inline int lm_is_running_idm(void)
{
	return 0;
}

static inline int lm_rem_resource_idm(struct lockspace *ls, struct resource *r)
{
	return -1;
}

static inline int lm_support_idm(void)
{
	return 0;
}

#endif /* Seagate IDM support */

#endif	/* _LVM_LVMLOCKD_INTERNAL_H */
