/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Test that dlclose(libdevmapper) + thread exit does not segfault.
 *
 * Reproduces: https://gitlab.com/lvmteam/lvm2/-/work_items/44
 *
 * pthread_key_create() registers a TSD destructor in glibc's global
 * __pthread_keys[].  After dlclose(), the destructor function is
 * unmapped but the pointer remains.  Thread exit calls the dangling
 * pointer and crashes.
 *
 * The fix: the private library destructor now calls
 * pthread_key_delete() to deregister the TSD key before the library
 * is unmapped.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void *lib_handle;
static int (*fn_set_uuid_prefix)(const char *);
static const char *(*fn_uuid_prefix)(void);
static void (*fn_lib_exit)(void);
static pthread_barrier_t barrier;
static int worker_failed;

static void *worker(void *arg)
{
	(void) arg;

	fn_set_uuid_prefix("TEST");

	/* Tell main thread TSD is allocated */
	pthread_barrier_wait(&barrier);

	/* Wait for main thread to call the legacy dm_lib_exit(). */
	pthread_barrier_wait(&barrier);

	/* Legacy dm_lib_exit() must not invalidate state in this thread. */
	if (!fn_set_uuid_prefix("TEST2") ||
	    !fn_uuid_prefix() || strcmp(fn_uuid_prefix(), "TEST2"))
		worker_failed = 1;

	/* Tell main thread libdevmapper can now be unloaded. */
	pthread_barrier_wait(&barrier);

	/* Wait for main thread to dlclose */
	pthread_barrier_wait(&barrier);

	/*
	 * Thread exit triggers glibc's __nptl_deallocate_tsd().
	 * Before fix: calls unmapped _destroy_thread_state -> SIGSEGV.
	 * After fix: key deleted by dm_lib_exit -> no destructor call.
	 */
	return NULL;
}

static int run_test(const char *soname)
{
	pthread_t tid;

	lib_handle = dlopen(soname, RTLD_NOW);
	if (!lib_handle) {
		fprintf(stderr, "dlopen(%s): %s\n", soname, dlerror());
		return 2;
	}

	fn_set_uuid_prefix = dlsym(lib_handle, "dm_set_uuid_prefix");
	fn_uuid_prefix = dlsym(lib_handle, "dm_uuid_prefix");
	fn_lib_exit = dlsym(lib_handle, "dm_lib_exit");
	if (!fn_set_uuid_prefix || !fn_uuid_prefix || !fn_lib_exit) {
		fprintf(stderr, "dlsym: %s\n", dlerror());
		dlclose(lib_handle);
		return 2;
	}

	pthread_barrier_init(&barrier, NULL, 2);

	if (pthread_create(&tid, NULL, worker, NULL)) {
		perror("pthread_create");
		dlclose(lib_handle);
		pthread_barrier_destroy(&barrier);
		return 2;
	}

	/* Wait for worker to have TSD allocated */
	pthread_barrier_wait(&barrier);

	/* This must not tear down state while the DSO remains loaded. */
	fn_lib_exit();

	/* Let worker verify it can still use its thread state. */
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);

	/* Unload library -- unmaps _destroy_thread_state code */
	dlclose(lib_handle);
	lib_handle = NULL;
	fn_set_uuid_prefix = NULL;

	/* Let worker thread exit */
	pthread_barrier_wait(&barrier);

	pthread_join(tid, NULL);

	pthread_barrier_destroy(&barrier);

	return worker_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *soname = "libdevmapper.so";
	pid_t pid;
	int status;

	if (argc > 1)
		soname = argv[1];

	/* Run in child process to catch SIGSEGV gracefully */
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}

	if (pid == 0)
		_exit(run_test(soname));

	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return 1;
	}

	if (WIFSIGNALED(status)) {
		fprintf(stderr, "FAIL: child killed by signal %d",
			WTERMSIG(status));
		if (WTERMSIG(status) == SIGSEGV)
			fprintf(stderr, " (SIGSEGV)");
		fprintf(stderr, "\n"
			"  TSD destructor called after dlclose"
			" -- pthread_key_delete missing\n");
		return 1;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("PASS: thread exited cleanly after"
		       " dlclose(libdevmapper)\n");
		return 0;
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
		fprintf(stderr, "SKIP: could not load %s\n", soname);
		return 2;
	}

	fprintf(stderr, "FAIL: child exited with status %d\n",
		WEXITSTATUS(status));
	return 1;
}
