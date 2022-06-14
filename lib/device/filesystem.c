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

#include <dirent.h>
#include <mntent.h>
#include <sys/ioctl.h>

/* LUKS header is usually 2MB for LUKS1 and 16MB for LUKS2 */

static uint32_t _get_luks_header_bytes(struct logical_volume *lv, char *lv_path, char *crypt_path)
{
	uint64_t lv_size = 0;
	uint64_t crypt_size = 0;
	int fd;

	if ((fd = open(lv_path, O_RDONLY)) < 0)
		goto_out;

	if (ioctl(fd, BLKGETSIZE64, &lv_size) < 0) {
		close(fd);
		goto_out;
	}
	close(fd);

	if ((fd = open(crypt_path, O_RDONLY)) < 0)
		goto_out;

	if (ioctl(fd, BLKGETSIZE64, &crypt_size) < 0) {
		close(fd);
		goto_out;
	}
	close(fd);

	if (lv_size <= crypt_size)
		goto_out;

	return (uint32_t)(lv_size - crypt_size);
out:
	log_error("Failed to get LUKS header size for %s on %s.", crypt_path, lv_path);
	return 0;
}

/*
 * Set the path of the dm-crypt device, i.e. /dev/dm-N, that is using the LV.
 */
static int _get_crypt_path(dev_t lv_devt, char *lv_path, char *crypt_path)
{
	char holders_path[PATH_MAX];
	char *holder_name;
	DIR *dr;
	struct stat st;
	struct dirent *de;
	int ret = 0;

	if (dm_snprintf(holders_path, sizeof(holders_path), "%sdev/block/%d:%d/holders",
			dm_sysfs_dir(), (int)MAJOR(lv_devt), (int)MINOR(lv_devt)) < 0)
		return_0;

	/* If the crypt dev is not active, there will be no LV holder. */
	if (stat(holders_path, &st)) {
		log_error("Missing %s for %s", crypt_path, lv_path);
		return 0;
	}

	if (!(dr = opendir(holders_path))) {
		log_error("Cannot open %s", holders_path);
		return 0;
	}

	while ((de = readdir(dr))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		holder_name = de->d_name;

		if (strncmp(holder_name, "dm", 2)) {
			log_error("Unrecognized holder %s of %s", holder_name, lv_path);
			ret = 0;
			break;
		}

		/* We could read the holder's dm uuid to verify it's a crypt dev. */

		if (dm_snprintf(crypt_path, PATH_MAX, "/dev/%s", holder_name) < 0) {
			ret = 0;
			stack;
			break;
		}
		ret = 1;
		break;
	}
	closedir(dr);
	if (ret)
		log_debug("Found holder %s of %s.", crypt_path, lv_path);
	else
		log_debug("No holder in %s", holders_path);
	return ret;
}

int fs_get_info(struct cmd_context *cmd, struct logical_volume *lv,
		struct fs_info *fsi, int include_mount)
{
	char lv_path[PATH_MAX];
	char crypt_path[PATH_MAX];
	struct stat st_lv;
	struct stat st_crypt;
	struct stat st_top;
	struct stat stme;
	struct fs_info info;
	FILE *fme = NULL;
	struct mntent *me;
	int ret;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", lv->vg->cmd->dev_dir,
			lv->vg->name, lv->name) < 0) {
		log_error("Couldn't create LV path for %s.", display_lvname(lv));
		return 0;
	}

	if (stat(lv_path, &st_lv) < 0) {
		log_error("Failed to get LV path %s", lv_path);
		return 0;
	}

	memset(&info, 0, sizeof(info));

	if (!fs_get_blkid(lv_path, &info)) {
		log_error("No file system info from blkid for %s", display_lvname(lv));
		return 0;
	}

	if (fsi->nofs)
		return 1;

	/*
	 * If there's a LUKS dm-crypt layer over the LV, then
	 * return fs info from that layer, setting needs_crypt
	 * to indicate a crypt layer between the fs and LV.
	 */
	if (!strcmp(info.fstype, "crypto_LUKS")) {
		if (!_get_crypt_path(st_lv.st_rdev, lv_path, crypt_path)) {
			log_error("Cannot find active LUKS dm-crypt device using %s.",
				  display_lvname(lv));
			return 0;
		}

		if (stat(crypt_path, &st_crypt) < 0) {
			log_error("Failed to get crypt path %s", crypt_path);
			return 0;
		}

		memset(&info, 0, sizeof(info));

		log_print("File system found on crypt device %s on LV %s.",
			  crypt_path, display_lvname(lv));

		if (!fs_get_blkid(crypt_path, &info)) {
			log_error("No file system info from blkid for dm-crypt device %s on LV %s.",
				  crypt_path, display_lvname(lv));
			return 0;
		}
		*fsi = info;
		fsi->needs_crypt = 1;
		fsi->crypt_devt = st_crypt.st_rdev;
		memcpy(fsi->fs_dev_path, crypt_path, PATH_MAX);
		st_top = st_crypt;

		if (!(fsi->luks_header_bytes = _get_luks_header_bytes(lv, lv_path, crypt_path)))
			return_0;
	} else {
		*fsi = info;
		memcpy(fsi->fs_dev_path, lv_path, PATH_MAX);
		st_top = st_lv;
	}

	if (!include_mount)
		return 1;

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
		if (stme.st_dev != st_top.st_rdev)
			continue;

		log_debug("fs_get_info %s is mounted \"%s\"", fsi->fs_dev_path, me->mnt_dir);
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
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (strncmp(fsi->fstype, "ext", 3)) {
		log_error("fsck not supported for %s.", fsi->fstype);
		return_0;
	}

	/*
	 * ext234: e2fsck -f -p path
	 * TODO: replace -p with -y based on yes_ARG, or other?
	 */

	argv[0] = E2FSCK_PATH; /* defined by configure */
	argv[++args] = "-f";
	argv[++args] = "-p";
	argv[++args] = fsi->fs_dev_path;
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
	char newsize_kb_str[16] = { 0 };
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (!strncmp(fsi->fstype, "ext", 3)) {
		if (dm_snprintf(newsize_kb_str, 16, "%lluk", (unsigned long long)(newsize_bytes/1024)) < 0)
			return_0;

		/*
		 * ext234 shrink: resize2fs path newsize
		 */
		argv[0] = RESIZE2FS_PATH; /* defined by configure */
		argv[++args] = fsi->fs_dev_path;
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
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	if (!strncmp(fsi->fstype, "ext", 3)) {
		/* TODO: include -f if lvm command inclues -f ? */
		argv[0] = RESIZE2FS_PATH; /* defined by configure */
		argv[++args] = fsi->fs_dev_path;
		argv[++args] = NULL;

	} else if (!strcmp(fsi->fstype, "xfs")) {
		argv[0] = XFS_GROWFS_PATH; /* defined by configure */
		argv[++args] = fsi->fs_dev_path;
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
	char mountdir[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

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
	argv[++args] = fsi->fs_dev_path;
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
	const char *argv[FS_CMD_MAX_ARGS + 4];
	int args = 0;
	int status;

	argv[0] = UMOUNT_PATH; /* defined by configure */
	argv[++args] = fsi->fs_dev_path;
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

int crypt_resize_command(struct cmd_context *cmd, dev_t crypt_devt, uint64_t newsize_bytes)
{
	char crypt_path[PATH_MAX];
	const char *argv[FS_CMD_MAX_ARGS + 4];
	char size_str[16] = { 0 };
	int args = 0;
	int status;

	if (dm_snprintf(crypt_path, sizeof(crypt_path), "/dev/dm-%d", (int)MINOR(crypt_devt)) < 0)
		return_0;

	if (dm_snprintf(size_str, sizeof(size_str), "%llu", (unsigned long long)newsize_bytes/512) < 0)
		return_0;

	argv[0] = CRYPTSETUP_PATH; /* defined by configure */
	argv[++args] = "resize";
	if (newsize_bytes) {
		argv[++args] = "--size";
		argv[++args] = size_str;
	}
	argv[++args] = crypt_path;
	argv[++args] = NULL;

	log_print("Resizing crypt device %s...", crypt_path);

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("Failed to cryptsetup resize %s to %s (%llu sectors)",
			  crypt_path, display_size(cmd, newsize_bytes/512),
			  (unsigned long long)newsize_bytes/512);
		return 0;
	}

	if (newsize_bytes)
		log_print("Resized crypt device %s to %s (%llu sectors)",
			  crypt_path, display_size(cmd, newsize_bytes/512),
			  (unsigned long long)newsize_bytes/512);
	else 
		log_print("Resized crypt device %s.", crypt_path);

	return 1;
}

