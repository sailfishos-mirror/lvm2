/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * Shared dm_task/fork/waitpid transport mocks for thin and VDO dmeventd unit
 * tests.  Define DMEVENTD_UNIT_POOL_SETENV to a static setenv() replacement
 * before including this header.
 */

#ifndef DMEVENTD_UNIT_POOL_TRANSPORT_H
#define DMEVENTD_UNIT_POOL_TRANSPORT_H

#ifndef DMEVENTD_UNIT_POOL_SETENV
#error "Define DMEVENTD_UNIT_POOL_SETENV before including dmeventd_unit_pool_transport.h"
#endif

#include <sys/wait.h>

struct dmeventd_unit_pool_task {
	const char *type;
	const char *status;
};

static struct dmeventd_unit_pool_task _dmeventd_unit_pool_current, _dmeventd_unit_pool_fresh;
static unsigned _policies, _reads, _creates, _destroys, _runs, _no_flush;
static unsigned _forks, _waits, _locks;
static int _policy_result, _create_result, _uuid_result, _run_result;
static pid_t _fork_result, _wait_result;
static int _wait_status;

static const char *_dmeventd_unit_pool_task_name(const struct dm_task *dmt)
{
	return "test-pool";
}

static const char *_dmeventd_unit_pool_task_uuid(const struct dm_task *dmt)
{
	return "test-uuid";
}

static struct dm_task *_dmeventd_unit_pool_task_create(int type)
{
	T_ASSERT_EQUAL(type, DM_DEVICE_STATUS);
	_creates++;
	return _create_result ? (struct dm_task *) &_dmeventd_unit_pool_fresh : NULL;
}

static void _dmeventd_unit_pool_task_destroy(struct dm_task *dmt)
{
	T_ASSERT(dmt == (struct dm_task *) &_dmeventd_unit_pool_fresh);
	_destroys++;
}

static int _dmeventd_unit_pool_task_set_uuid(struct dm_task *dmt, const char *uuid)
{
	T_ASSERT(dmt == (struct dm_task *) &_dmeventd_unit_pool_fresh);
	T_ASSERT(!strcmp(uuid, "test-uuid"));
	return _uuid_result;
}

static int _dmeventd_unit_pool_task_no_flush(struct dm_task *dmt)
{
	T_ASSERT(dmt == (struct dm_task *) &_dmeventd_unit_pool_fresh);
	_no_flush++;
	return 1;
}

static int _dmeventd_unit_pool_task_run(struct dm_task *dmt)
{
	T_ASSERT(dmt == (struct dm_task *) &_dmeventd_unit_pool_fresh);
	T_ASSERT_EQUAL(_no_flush, _runs + 1);
	_runs++;
	return _run_result;
}

static void *_dmeventd_unit_pool_next_target(struct dm_task *dmt, void *next,
					     uint64_t *start, uint64_t *length,
					     char **type, char **params)
{
	struct dmeventd_unit_pool_task *task = (struct dmeventd_unit_pool_task *) dmt;

	T_ASSERT(!next);
	_reads++;
	*start = 0;
	*length = 1024;
	*type = (char *) task->type;
	*params = (char *) task->status;
	return NULL;
}

static pid_t _dmeventd_unit_pool_fork(void)
{
	_forks++;
	T_ASSERT(_fork_result != 0);
	return _fork_result;
}

static pid_t _dmeventd_unit_pool_waitpid(pid_t pid, int *status, int options)
{
	T_ASSERT_EQUAL(pid, 123);
	T_ASSERT_EQUAL(options, WNOHANG);
	_waits++;
	*status = _wait_status;
	return _wait_result;
}

static int _dmeventd_unit_pool_setenv(const char *name, const char *value, int overwrite)
{
	return DMEVENTD_UNIT_POOL_SETENV(name, value, overwrite);
}

#define dm_task_get_name _dmeventd_unit_pool_task_name
#define dm_task_get_uuid _dmeventd_unit_pool_task_uuid
#define dm_task_create _dmeventd_unit_pool_task_create
#define dm_task_destroy _dmeventd_unit_pool_task_destroy
#define dm_task_set_uuid _dmeventd_unit_pool_task_set_uuid
#define dm_task_no_flush _dmeventd_unit_pool_task_no_flush
#define dm_task_run _dmeventd_unit_pool_task_run
#define dm_get_next_target _dmeventd_unit_pool_next_target
#define fork _dmeventd_unit_pool_fork
#define waitpid _dmeventd_unit_pool_waitpid
#define setenv _dmeventd_unit_pool_setenv

#endif
