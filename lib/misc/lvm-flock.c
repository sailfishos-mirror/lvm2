/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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

#include "lib/misc/lib.h"
#include "lib/config/config.h"
#include "lib/misc/lvm-flock.h"
#include "lib/misc/lvm-signal.h"
#include "lib/locking/locking.h"

#include <sys/file.h>
#include <fcntl.h>

struct lock_list {
	struct dm_list list;
	int lf;
	unsigned ex:1;
	unsigned remove_on_unlock:1;
	char *res;
	struct timespec save_time;
};

static struct dm_list _lock_list;
static int _prioritise_write_locks;

#define AUX_LOCK_SUFFIX ":aux"

static struct lock_list *_get_lock_list_entry(const char *file)
{
	struct lock_list *ll;
	struct dm_list *llh;

	dm_list_iterate(llh, &_lock_list) {
		ll = dm_list_item(llh, struct lock_list);

		if (!strcmp(ll->res, file))
			return ll;
	}
	return NULL;
}

static void _unlink_aux(const char *file)
{
	char aux_path[PATH_MAX];

	if (dm_snprintf(aux_path, sizeof(aux_path), "%s%s", file, AUX_LOCK_SUFFIX) < 0) {
		stack;
		return;
	}

	if (unlink(aux_path))
		log_sys_debug("unlink", aux_path);
}

static int _release_lock(const char *file, int unlock)
{
	struct lock_list *ll;
	struct dm_list *llh, *llt;

	dm_list_iterate_safe(llh, llt, &_lock_list) {
		ll = dm_list_item(llh, struct lock_list);

		if (ll->lf < 0)
			continue;

		if (!file || !strcmp(ll->res, file)) {
			/*
			 * When a VG is being removed, and the flock is still
			 * held for the VG, it sets the remove_on_unlock flag,
			 * so that when the flock is unlocked, the lock file is
			 * then also removed.
			 */
			if (file && unlock && ll->remove_on_unlock) {
				log_debug("Unlocking %s and removing", ll->res);

				if (_prioritise_write_locks)
					_unlink_aux(ll->res);
				if (flock(ll->lf, LOCK_NB | LOCK_UN))
					log_sys_debug("flock", ll->res);
				if (unlink(ll->res))
					log_sys_debug("unlink", ll->res);
				if (close(ll->lf) < 0)
					log_sys_debug("close", ll->res);

				dm_list_del(&ll->list);
				free(ll->res);
				free(ll);
				return 1;
			}

			/*
			 * Update the lock file timestamp when unlocking an
			 * exclusive flock.  Other commands may use the
			 * timestamp change to detect that the VG was changed.
			 */
			if (file && unlock && ll->ex) {
				if (futimens(ll->lf, NULL) < 0)
					log_debug("lock file %s time update error %d", file, errno);
			}

			if (unlock) {
				log_very_verbose("Unlocking %s", ll->res);
				if (flock(ll->lf, LOCK_NB | LOCK_UN))
					log_sys_debug("flock", ll->res);
			}

			if (close(ll->lf) < 0)
				log_sys_debug("close", ll->res);

			ll->lf = -1;

			if (file)
				return 1;
		}
	}

	return 0;
}

void release_flocks(int unlock)
{
	_release_lock(NULL, unlock);
}

static int _do_flock(const char *file, int *fd, int operation, uint32_t nonblock)
{
	int r;
	int old_errno;
	struct stat buf1, buf2;

	log_debug_locking("_do_flock %s %c%c", file,
			  operation == LOCK_EX ? 'W' : 'R', nonblock ? ' ' : 'B');
	do {
		if ((*fd > -1) && close(*fd))
			log_sys_debug("close", file);

		if ((*fd = open(file, O_CREAT | O_APPEND | O_RDWR, 0777)) < 0) {
			log_sys_error("open", file);
			return 0;
		}

		if (nonblock)
			operation |= LOCK_NB;
		else
			sigint_allow();

		r = flock(*fd, operation);
		old_errno = errno;
		if (!nonblock) {
			sigint_restore();
			if (sigint_caught()) {
				log_error("Giving up waiting for lock.");
				break;
			}
		}

		if (r) {
			errno = old_errno;
			log_sys_error("flock", file);
			break;
		}

		if (!stat(file, &buf1) && !fstat(*fd, &buf2) &&
		    is_same_inode(buf1, buf2))
			return 1;
	} while (!nonblock);

	if (close(*fd))
		log_sys_debug("close", file);
	*fd = -1;

	return_0;
}

static int _do_write_priority_flock(const char *file, int *fd, int operation, uint32_t nonblock)
{
	int r, fd_aux = -1;
	char *file_aux = alloca(strlen(file) + sizeof(AUX_LOCK_SUFFIX));

	strcpy(file_aux, file);
	strcat(file_aux, AUX_LOCK_SUFFIX);

	if ((r = _do_flock(file_aux, &fd_aux, LOCK_EX, 0))) {
		if (operation == LOCK_EX) {
			r = _do_flock(file, fd, operation, nonblock);
			if (close(fd_aux) < 0)
				log_sys_debug("close", file_aux);
		} else {
			if (close(fd_aux) < 0)
				log_sys_debug("close", file_aux);
			r = _do_flock(file, fd, operation, nonblock);
		}
	}

	return r;
}

int lock_file(const char *file, uint32_t flags)
{
	int operation;
	uint32_t nonblock = flags & LCK_NONBLOCK;
	uint32_t convert = flags & LCK_CONVERT;
	int r;
	int ex = 0;
	struct lock_list *ll;
	char state;

	switch (flags & LCK_TYPE_MASK) {
	case LCK_READ:
		operation = LOCK_SH;
		state = 'R';
		break;
	case LCK_WRITE:
		operation = LOCK_EX;
		state = 'W';
		ex = 1;
		break;
	case LCK_UNLOCK:
		return _release_lock(file, 1);
	default:
		log_error("Unrecognised lock type: %d", flags & LCK_TYPE_MASK);
		return 0;
	}

	if (convert) {
		if (nonblock)
			operation |= LOCK_NB;
		if (!(ll = _get_lock_list_entry(file)))
			return 0;
		log_very_verbose("Locking %s %c%c convert", ll->res, state,
			 	 nonblock ? ' ' : 'B');
		r = flock(ll->lf, operation);
		if (!r) {
			ll->ex = ex;
			return 1;
		}
		log_error("Failed to convert flock on %s %d", file, errno);
		return 0;
	}

	if (!(ll = _get_lock_list_entry(file))) {
		if (!(ll = zalloc(sizeof(struct lock_list))))
			return_0;

		if (!(ll->res = strdup(file))) {
			free(ll);
			return_0;
		}

		ll->lf = -1;
		dm_list_add(&_lock_list, &ll->list);
	}

	log_very_verbose("Locking %s %c%c", ll->res, state,
			 nonblock ? ' ' : 'B');

	(void) dm_prepare_selinux_context(file, S_IFREG);
	if (_prioritise_write_locks)
		r = _do_write_priority_flock(file, &ll->lf, operation, nonblock);
	else 
		r = _do_flock(file, &ll->lf, operation, nonblock);
	(void) dm_prepare_selinux_context(NULL, 0);

	if (r)
		ll->ex = ex;
	else
		stack;

	return r;
}

void init_flock(struct cmd_context *cmd)
{
	dm_list_init(&_lock_list);

	_prioritise_write_locks =
	    find_config_tree_bool(cmd, global_prioritise_write_locks_CFG, NULL);
}

void free_flocks(void)
{
	struct lock_list *ll, *ll2;

	dm_list_iterate_items_safe(ll, ll2, &_lock_list) {
		dm_list_del(&ll->list);
		free(ll->res);
		free(ll);
	}
}

/*
 * Save the lock file timestamps prior to scanning, so that the timestamps can
 * be checked later (lock_file_time_unchanged) to see if the VG has been
 * changed.
 */

void lock_file_time_init(const char *file)
{
	struct lock_list *ll;
	struct stat buf;

	if (stat(file, &buf) < 0)
		return;

	if (!(ll = _get_lock_list_entry(file))) {
		if (!(ll = zalloc(sizeof(struct lock_list))))
			return;

		if (!(ll->res = strdup(file))) {
			free(ll);
			return;
		}

		ll->lf = -1;
		ll->save_time = buf.st_mtim;
		dm_list_add(&_lock_list, &ll->list);
	}
}

/*
 * Check if a lock file timestamp has been changed (by other command) since we
 * saved it (lock_file_time_init).  Another command may have updated the lock
 * file timestamp when releasing an ex flock (futimens above.)
 */

bool lock_file_time_unchanged(const char *file)
{
	struct lock_list *ll;
	struct stat buf;
	struct timespec *prev, *now;

	if (stat(file, &buf) < 0) {
		log_debug("lock_file_time_unchanged no file %s", file);
		return false;
	}

	if (!(ll = _get_lock_list_entry(file))) {
		log_debug("lock_file_time_unchanged no list item %s", file);
		return false;
	}

	prev = &ll->save_time;
	now = &buf.st_mtim;

	if ((now->tv_sec == prev->tv_sec) && (now->tv_nsec == prev->tv_nsec)) {
		log_debug("lock file %s unchanged from %llu.%llu", file,
			  (unsigned long long)prev->tv_sec,
			  (unsigned long long)prev->tv_nsec);
		return true;
	}

	log_debug("lock file %s changed from %llu.%llu to %llu.%llu", file,
		  (unsigned long long)prev->tv_sec,
		  (unsigned long long)prev->tv_nsec,
		  (unsigned long long)now->tv_sec,
		  (unsigned long long)now->tv_nsec);

	return false;
}

void lock_file_remove_on_unlock(const char *file)
{
	struct lock_list *ll;

	if ((ll = _get_lock_list_entry(file)))
		ll->remove_on_unlock = 1;
}

