/*
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/device/device.h"
#include "lib/device/dev-type.h"
#include "lib/misc/lvm-exec.h"

#include <mntent.h>

static int _get_lv_path(struct logical_volume *lv, char *lv_path)
{
	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir,
			lv->vg->name, lv->name) < 0) {
		log_error("Couldn't create LV path for %s.", display_lvname(lv));
		return 0;
	}
	return 1;
}

int fs_get_info(struct cmd_context *cmd, struct logical_volume *lv,
		struct fs_info *fsi, int include_mount)
{
	char lv_path[PATH_MAX];
	FILE *fme = NULL;
	struct stat stlv;
	struct stat stme;
	struct mntent *me;
	int ret;

	if (!_get_lv_path(lv, lv_path))
		return_0;

	if (!fs_get_blkid(lv_path, fsi)) {
		log_error("Missing libblkid file system info for %s", display_lvname(lv));
		return 0;
	}

	if (fsi->nofs)
		return 1;

	if (!include_mount)
		return 1;

	if (stat(lv_path, &stlv) < 0)
		return_0;

	if (!(fme = setmntent("/etc/mtab", "r")))
		return_0;

	ret = 1;

	while ((me = getmntent(fme))) {
		if (strcmp(me->mnt_type, fsi->fstype))
			continue;
		if (me->mnt_dir[0] != '/')
			continue;
		if (me->mnt_fsname[0] != '/')
			continue;
		if (stat(me->mnt_dir, &stme) < 0)
			continue;
		if (stme.st_dev != stlv.st_rdev)
			continue;

		log_debug("fs_get_info %s is mounted \"%s\"", lv_path, me->mnt_dir);
		fsi->mounted = 1;
		strncpy(fsi->mount_dir, me->mnt_dir, PATH_MAX-1);
	}
	endmntent(fme);

	fsi->unmounted = !fsi->mounted;
	return ret;
}

#define FS_CMD_MAX_ARGS 6

int fs_fsck_command(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi)
{
	char lv_path[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (strncmp(fsi->fstype, "ext", 3)) {
		log_error("fsck not supported for %s.", fsi->fstype);
		return_0;
	}

	if (!_get_lv_path(lv, lv_path))
		return_0;

	/*
	 * ext234: e2fsck -f -p path
	 * TODO: replace -p with -y based on yes_ARG, or other?
	 */

	argv[0] = E2FSCK_PATH; /* defined by configure */
	argv[++args] = "-f";
	argv[++args] = "-p";
	argv[++args] = lv_path;
	argv[++args] = NULL;

	log_print("Checking file system %s with %s on %s...",
		  fsi->fstype, E2FSCK_PATH, display_lvname(lv));

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("e2fsck failed on %s.", display_lvname(lv));
		return 0;
	}

	log_print("Checked file system %s on %s.", fsi->fstype, display_lvname(lv));
	return 1;
}

int fs_reduce_command(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		      uint64_t newsize_bytes)
{
	char lv_path[PATH_MAX];
	char newsize_kb_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (!_get_lv_path(lv, lv_path))
		return_0;

	if (!strncmp(fsi->fstype, "ext", 3)) {
		if (dm_snprintf(newsize_kb_str, 16, "%lluk", (unsigned long long)(newsize_bytes/1024)) < 0)
			return_0;

		/*
		 * ext234 shrink: resize2fs path newsize
		 */
		argv[0] = RESIZE2FS_PATH; /* defined by configure */
		argv[++args] = lv_path;
		argv[++args] = newsize_kb_str;
		argv[++args] = NULL;

	} else {
		log_error("fs reduce not supported for %s.", fsi->fstype);
		return_0;
	}

	log_print("Reducing file system %s on %s...", fsi->fstype, display_lvname(lv));

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to reduce %s file system on %s.", fsi->fstype, display_lvname(lv));
		return 0;
	}

	log_print("Reduced file system %s to %s (%llu bytes) on %s.",
		  fsi->fstype, display_size(cmd, newsize_bytes/512),
		  (unsigned long long)newsize_bytes, display_lvname(lv));
	return 1;
}

int fs_extend_command(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi)
{
	char lv_path[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (!_get_lv_path(lv, lv_path))
		return_0;

	if (!strncmp(fsi->fstype, "ext", 3)) {
		/* TODO: include -f if lvm command inclues -f ? */
		argv[0] = RESIZE2FS_PATH; /* defined by configure */
		argv[++args] = lv_path;
		argv[++args] = NULL;

	} else if (!strcmp(fsi->fstype, "xfs")) {
		argv[0] = XFS_GROWFS_PATH; /* defined by configure */
		argv[++args] = lv_path;
		argv[++args] = NULL;

	} else {
		log_error("Extend not supported for %s file system.", fsi->fstype);
		return_0;
	}

	log_print("Extending file system %s on %s...", fsi->fstype, display_lvname(lv));

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to extend %s file system on %s.", fsi->fstype, display_lvname(lv));
		return 0;
	}

	log_print("Extended file system %s on %s.", fsi->fstype, display_lvname(lv));
	return 1;
}

int fs_mount_command(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		     int reuse_mount_dir)
{
	char lv_path[PATH_MAX];
	char mountdir[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (!_get_lv_path(lv, lv_path))
		return_0;

	if (reuse_mount_dir) {
		if (!fsi->mount_dir[0]) {
			log_error("Cannot remount fs without previous mount dir.");
			return 0;
		}
		memcpy(mountdir, fsi->mount_dir, PATH_MAX);
	} else {
		if (dm_snprintf(mountdir, sizeof(mountdir), "/tmp/%s_XXXXXX", cmd->name) < 0)
			return_0;
		if (!mkdtemp(mountdir)) {
			log_error("Failed to create temp dir for mount: %s", strerror(errno));
			return 0;
		}
		memcpy(fsi->mount_dir, mountdir, PATH_MAX);
		fsi->temp_mount_dir = 1;
	}
	
	argv[0] = MOUNT_PATH; /* defined by configure */
	argv[++args] = lv_path;
	argv[++args] = mountdir;
	argv[++args] = NULL;

	log_print("Mounting %s.", display_lvname(lv));

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to mount file system on %s at %s.", display_lvname(lv), mountdir);
		return 0;
	}

	log_print("Mounted %s at %s dir %s.", display_lvname(lv),
		  reuse_mount_dir ? "original" : "temporary", mountdir);
	return 1;
}

int fs_unmount_command(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi)
{
	char lv_path[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (!_get_lv_path(lv, lv_path))
		return_0;

	argv[0] = UMOUNT_PATH; /* defined by configure */
	argv[++args] = lv_path;
	argv[++args] = NULL;

	log_print("Unmounting %s.", display_lvname(lv));

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to unmount file system on %s.", display_lvname(lv));
		return 0;
	}

	log_print("Unmounted %s.", display_lvname(lv));

	if (fsi->temp_mount_dir) {
		if (rmdir(fsi->mount_dir) < 0)
			log_print("Error removing temp dir %s", fsi->mount_dir);
		fsi->mount_dir[0] = '\0';
		fsi->temp_mount_dir = 0;
	}
	return 1;
}

