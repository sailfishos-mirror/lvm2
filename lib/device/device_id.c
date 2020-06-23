/*
 * Copyright (C) 2020 Red Hat, Inc. All rights reserved.
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
#include "lib/device/device_id.h"
#include "lib/device/dev-type.h"
#include "lib/device/device-types.h"
#include "lib/label/label.h"
#include "lib/metadata/metadata.h"
#include "lib/format_text/layout.h"
#include "lib/cache/lvmcache.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/sysmacros.h>

#define DEVICES_FILE_MAJOR 1
#define DEVICES_FILE_MINOR 1
#define VERSION_LINE_MAX 256

static int _devices_fd = -1;
static int _using_devices_file;
static int _devices_file_locked;
static char _devices_lockfile[PATH_MAX];
static char _devices_file_systemid[PATH_MAX];
static char _devices_file_version[VERSION_LINE_MAX];

char *devices_file_version(void)
{
	return _devices_file_version;
}

/*
 * How the devices file and device IDs are used by an ordinary command:
 *
 * 1. device_ids_read() reads the devices file, and adds a 'struct uid'
 *    to cmd->use_device_ids for each entry.  These are the devices lvm
 *    can use, but we do not yet know which devnames they correspond to.
 * 2. dev_cache_scan() gets a list of all devices (devnames) on the system,
 *    and adds a 'struct device' to dev-cache for each.
 * 3. device_ids_match() matches uid entries from the devices file
 *    with devices from dev-cache.  With this complete, we know the
 *    devnames to use for each of the entries in the devices file.
 * 4. label_scan (or equivalent) iterates through all devices in
 *    dev-cache, checks each one with filters, which excludes many,
 *    and reads lvm headers and metadata from the devs that pass the
 *    filters.  lvmcache is populated with summary info about each PV
 *    during this phase.
 * 5. device_ids_validate() checks if the PVIDs saved in the devices
 *    file are correct based on the PVIDs read from disk in the 
 *    previous step.  If not it updates the devices file.
 *
 * cmd->use_device_ids reflect the entries in the devices file.
 * When reading the devices file, a 'uid' struct is added to use_device_ids
 * for each entry.
 * When adding devices to the devices file, a new uid struct is added
 * to use_device_ids, and then a new file entry is written for each uid.
 *
 * After reading the devices file, we want to "match" each uid from
 * the file to an actual device on the system.  We look at struct device's
 * in dev-cache to find one that matches each uid, based on the device_id.
 * When a match is made, uid->dev is set, and DEV_MATCHED_USE_ID is set
 * in the dev.
 *
 * After the use_device_ids entries are matched to system devices,
 * label_scan can be called to filter and scan devices.  After
 * label_scan, device_ids_validate() is called to check if the
 * PVID read from each device matches the PVID recorded in the
 * devices file for the device.
 *
 * A device can have multiple device IDs, e.g. a dev could have
 * both a wwid and a serial number, but only one of these IDs is
 * used as the device ID in the devices file, e.g. the wwid is
 * preferred so that would be used in the devices file.
 * Each of the different types of device IDs can be saved in
 * dev->ids list (struct dev_id).  So, one dev may have two
 * entries in dev->ids, one for wwid and one for serial.
 * The dev_id struct that is actually being used for the device
 * is set in dev->id.
 * The reason for saving multiple IDs in dev->ids is because
 * the process of matching devs to devices file entries can
 * involve repeatedly checking other dev_id types for a given
 * device, so we save each type as it is read to avoid rereading
 * the same id type many times.
 */

void free_uid(struct use_id *uid)
{
	if (uid->idname)
		free(uid->idname);
	if (uid->devname)
		free(uid->devname);
	if (uid->pvid)
		free(uid->pvid);
	free(uid);
}

void free_uids(struct dm_list *uids)
{
	struct use_id *uid, *safe;

	dm_list_iterate_items_safe(uid, safe, uids) {
		dm_list_del(&uid->list);
		free_uid(uid);
	}
}

void free_did(struct dev_id *did)
{
	if (did->idname)
		free(did->idname);
	free(did);
}

void free_dids(struct dm_list *dids)
{
	struct dev_id *did, *safe;

	dm_list_iterate_items_safe(did, safe, dids) {
		dm_list_del(&did->list);
		free_did(did);
	}
}

static int _read_sys_block(struct cmd_context *cmd, struct device *dev, const char *suffix, const char **idname)
{
	char path[PATH_MAX];
	char buf[PATH_MAX] = { 0 };
	dev_t devt = dev->dev;
	dev_t prim = 0;
	int ret;

 retry:
	if (dm_snprintf(path, sizeof(path), "%sdev/block/%d:%d/%s",
			dm_sysfs_dir(), (int)MAJOR(devt), (int)MINOR(devt), suffix) < 0) {
		log_error("Failed to create sysfs path for %s", dev_name(dev));
		return 0;
	}

	get_sysfs_value(path, buf, sizeof(buf), 0);

	if (buf[0]) {
		if (prim)
			log_debug("Using primary device_id for partition %s.", dev_name(dev));
		if (!(*idname = strdup(buf)))
			return 0;
		return 1;
	}

	if (prim)
		goto fail;

	/* in case it failed because dev is a partition... */

	ret = dev_get_primary_dev(cmd->dev_types, dev, &prim);
	if (ret == 2) {
		devt = prim;
		goto retry;
	}

 fail:
	*idname = NULL;
	return 1;
}

/* the dm uuid uses the wwid of the underlying dev */
static int _dev_has_mpath_uuid(struct cmd_context *cmd, struct device *dev, const char **idname_out)
{
	dev_t devt = dev->dev;
	dev_t prim;
	const char *idname = NULL;
	int ret;

	/* if it's a partitioned mpath device, use the primary */
	ret = dev_get_primary_dev(cmd->dev_types, dev, &prim);
	if (ret == 2)
		devt = prim;

	if (MAJOR(devt) != cmd->dev_types->device_mapper_major)
		return 0;

	_read_sys_block(cmd, dev, "dm/uuid", &idname);

	if (idname) {
		if (!strncmp(idname, "mpath-", 6)) {
			*idname_out = idname;
			return 1;
		}
		free((void *)idname);
	}
	return 0;
}

const char *device_id_system_read(struct cmd_context *cmd, struct device *dev, uint16_t idtype)
{
	const char *idname = NULL;

	if (idtype == DEV_ID_TYPE_SYS_WWID) {
		_read_sys_block(cmd, dev, "device/wwid", &idname);

		if (!idname)
			_read_sys_block(cmd, dev, "wwid", &idname);
	}

	else if (idtype == DEV_ID_TYPE_SYS_SERIAL)
		_read_sys_block(cmd, dev, "device/serial", &idname);

	else if (idtype == DEV_ID_TYPE_MPATH_UUID)
		_read_sys_block(cmd, dev, "dm/uuid", &idname);

	else if (idtype == DEV_ID_TYPE_MD_UUID)
		_read_sys_block(cmd, dev, "md/uuid", &idname);

	else if (idtype == DEV_ID_TYPE_LOOP_FILE)
		_read_sys_block(cmd, dev, "loop/backing_file", &idname);

	else if (idtype == DEV_ID_TYPE_DEVNAME)
		idname = strdup(dev_name(dev));

	return idname;
}

const char *idtype_to_str(uint16_t idtype)
{
	if (idtype == DEV_ID_TYPE_SYS_WWID)
		return "sys_wwid";
	if (idtype == DEV_ID_TYPE_SYS_SERIAL)
		return "sys_serial";
	if (idtype == DEV_ID_TYPE_DEVNAME)
		return "devname";
	if (idtype == DEV_ID_TYPE_MPATH_UUID)
		return "mpath_uuid";
	if (idtype == DEV_ID_TYPE_MD_UUID)
		return "md_uuid";
	if (idtype == DEV_ID_TYPE_LOOP_FILE)
		return "loop_file";
	return "unknown";
}

uint16_t idtype_from_str(const char *str)
{
	if (!strcmp(str, "sys_wwid"))
		return DEV_ID_TYPE_SYS_WWID;
	if (!strcmp(str, "sys_serial"))
		return DEV_ID_TYPE_SYS_SERIAL;
	if (!strcmp(str, "devname"))
		return DEV_ID_TYPE_DEVNAME;
	if (!strcmp(str, "mpath_uuid"))
		return DEV_ID_TYPE_MPATH_UUID;
	if (!strcmp(str, "md_uuid"))
		return DEV_ID_TYPE_MD_UUID;
	if (!strcmp(str, "loop_file"))
		return DEV_ID_TYPE_LOOP_FILE;
	return 0;
}

const char *dev_idtype(struct device *dev)
{
	if (!dev || !dev->id)
		return NULL;

	return idtype_to_str(dev->id->idtype);
}

const char *dev_id(struct device *dev)
{
	if (dev && dev->id)
		return dev->id->idname;
	return NULL;
}

static void _copy_idline_str(char *src, char *dst, int len)
{
	char *s, *d = dst;

	memset(dst, 0, len);

	if (!(s = strchr(src, '=')))
		return;
	s++;
	while ((*s == ' ') && (s < src + len))
		s++;
	while ((*s != ' ') && (*s != '\0') && (*s != '\n') && (s < src + len)) {
		*d = *s;
		s++;
		d++;
	}

	dst[len-1] = '\0';
}

int device_ids_read(struct cmd_context *cmd)
{
	char line[PATH_MAX];
	char buf[PATH_MAX];
	char *idtype, *idname, *devname, *pvid, *part;
	struct use_id *uid;
	FILE *fp;
	int ret = 1;

	/*
	 * Allow the use_device_ids list to come from a command line option
	 * instead of devices_file?  If so, add use_id structs to
	 * use_device_ids based on the reading the command line args here.
	 */
	 
	if (!cmd->enable_devices_file)
		return 1;

	/*
	 * use_device_ids should rarely if ever be
	 * non-empty, it means device_ids_read has
	 * been called twice.
	 *
	 * If we wanted to redo reading the file, we'd
	 * need to free_uids(&cmd->use_device_ids) and
	 * clear the MATCHED_USE_ID flag in all dev->flags.
	 */
	if (!dm_list_empty(&cmd->use_device_ids)) {
		log_debug("device_ids_read already done");
		return 1;
	}

	log_debug("device_ids_read %s", cmd->devices_file_path);

	if (!(fp = fopen(cmd->devices_file_path, "r"))) {
		log_warn("Cannot open devices file to read.");
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		if (!strncmp(line, "SYSTEMID", 8)) {
			_copy_idline_str(line, _devices_file_systemid, sizeof(_devices_file_systemid));
			log_debug("read devices file systemid %s", _devices_file_systemid);
			if ((!cmd->system_id && _devices_file_systemid[0]) ||
			    strcmp(cmd->system_id, _devices_file_systemid)) {
				log_print("Ignoring devices file with wrong system id %s vs local %s.",
					  _devices_file_systemid[0] ? _devices_file_systemid : ".", cmd->system_id ?: ".");
				free_uids(&cmd->use_device_ids);
				ret = 0;
				goto out;
			}
			continue;
		}
		if (!strncmp(line, "VERSION", 7)) {
			_copy_idline_str(line, _devices_file_version, sizeof(_devices_file_version));
			log_debug("read devices file version %s", _devices_file_version);
			continue;
		}

		idtype = strstr(line, "IDTYPE");
		idname = strstr(line, "IDNAME");
		devname = strstr(line, "DEVNAME");
		pvid = strstr(line, "PVID");
		part = strstr(line, "PART");

		/* These two are the minimum required. */
		if (!idtype || !idname)
			continue;

		if (!(uid = zalloc(sizeof(struct use_id))))
			return 0;

		_copy_idline_str(idtype, buf, PATH_MAX);
		if (buf[0])
			uid->idtype = idtype_from_str(buf);

		_copy_idline_str(idname, buf, PATH_MAX);
		if (buf[0]) {
			if (buf[0] && (buf[0] != '.'))
				uid->idname = strdup(buf);
		}

		if (devname) {
			_copy_idline_str(devname, buf, PATH_MAX);
			if (buf[0] && (buf[0] != '.'))
				uid->devname = strdup(buf);
		}

		if (pvid) {
			_copy_idline_str(pvid, buf, PATH_MAX);
			if (buf[0] && (buf[0] != '.'))
				uid->pvid = strdup(buf);
		}

		if (part) {
			_copy_idline_str(part, buf, PATH_MAX);
			if (buf[0] && (buf[0] != '.'))
				uid->part = atoi(buf);
		}

		dm_list_add(&cmd->use_device_ids, &uid->list);
	}
out:
	if (fclose(fp))
		stack;

	return ret;
}

int device_ids_write(struct cmd_context *cmd)
{
	char dirpath[PATH_MAX];
	char tmpfile[PATH_MAX];
	char version_buf[VERSION_LINE_MAX];
	FILE *fp;
	int dir_fd;
	time_t t;
	struct use_id *uid;
	const char *devname;
	const char *pvid;
	uint32_t df_major = 0, df_minor = 0, df_counter = 0;
	int ret = 1;

	if (!cmd->enable_devices_file)
		return 1;

	if (_devices_file_version[0]) {
		if (sscanf(_devices_file_version, "%u.%u.%u", &df_major, &df_minor, &df_counter) != 3) {
			/* don't update a file we can't parse */
			log_print("Not updating devices file with unparsed version.");
			return 0;
		}
		if (df_major > DEVICES_FILE_MAJOR) {
			/* don't update a file with a newer major version */
			log_print("Not updating devices file with larger major version.");
			return 0;
		}
	}

	if (dm_snprintf(dirpath, sizeof(dirpath), "%s/devices", cmd->system_dir) < 0) {
		ret = 0;
		goto out;
	}

	if (dm_snprintf(tmpfile, sizeof(tmpfile), "%s_new", cmd->devices_file_path) < 0) {
		ret = 0;
		goto out;
	}

	unlink(tmpfile); /* in case a previous file was left */

	if (!(fp = fopen(tmpfile, "w+"))) {
		log_warn("Cannot open tmp devices_file to write.");
		ret = 0;
		goto out;
	}

	if ((dir_fd = open(dirpath, O_RDONLY)) < 0) {
		fclose(fp);
		ret = 0;
		goto out;
	}

	t = time(NULL);

	fprintf(fp, "# LVM will use devices listed in this file.\n");
	fprintf(fp, "# IDTYPE and IDNAME fields are required, the DEVNAME path may change.\n");
	fprintf(fp, "# Created by LVM command %s pid %d at %s", cmd->name, getpid(), ctime(&t));

	/*
	 * It's useful to ensure that this devices file is associated to a
	 * single system because this file can be used to control access to
	 * shared devices.  If this file is copied/cloned to another system,
	 * that new system should not automatically gain access to the devices
	 * that the original system is using.
	 */
	if (cmd->system_id)
		fprintf(fp, "SYSTEMID=%s\n", cmd->system_id);

	if (dm_snprintf(version_buf, VERSION_LINE_MAX, "VERSION=%u.%u.%u", DEVICES_FILE_MAJOR, DEVICES_FILE_MINOR, df_counter+1) < 0)
		stack;
	else
		fprintf(fp, "%s\n", version_buf);

	/* as if we had read this version in case we want to write again */
	_copy_idline_str(version_buf, _devices_file_version, sizeof(_devices_file_version));

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		devname = uid->dev ? dev_name(uid->dev) : uid->devname;
		if (!devname || devname[0] != '/')
			devname = ".";

		if (!uid->pvid || !uid->pvid[0] || (uid->pvid[0] == '.'))
			pvid = ".";
		else
			pvid = uid->pvid;

		if (uid->part) {
			fprintf(fp, "IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s PART=%d\n",
				idtype_to_str(uid->idtype) ?: ".",
				uid->idname ?: ".", devname, pvid, uid->part);
		} else {
			fprintf(fp, "IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s\n",
				idtype_to_str(uid->idtype) ?: ".",
				uid->idname ?: ".", devname, pvid);
		}
	}

	if (fflush(fp))
		stack;
	if (fclose(fp))
		stack;

	if (rename(tmpfile, cmd->devices_file_path) < 0) {
		log_error("Failed to replace devices file errno %d", errno);
		ret = 0;
	}

	if (fsync(dir_fd) < 0)
		stack;
	if (close(dir_fd) < 0)
		stack;

	log_debug("Wrote devices file %s", version_buf);
out:
	return ret;
}

static void _device_ids_update_try(struct cmd_context *cmd)
{
	int held;

	/* Defer updates to non-pvscan-cache commands. */
	if (cmd->pvscan_cache_single) {
		log_print("pvscan[%d] skip updating devices file.", getpid());
		return;
	}

	/*
	 * Use a non-blocking lock since it's not essential to
	 * make this update, the next cmd will make these changes
	 * if we skip it this update.
	 * If this command already holds an ex lock on the
	 * devices file, lock_devices_file ex succeeds and
	 * held is set.
	 * If we get the lock, only update the devices file if
	 * it's not been changed since we read it.
	 */
	if (!lock_devices_file_try(cmd, LOCK_EX, &held)) {
		log_debug("Skip devices file update (busy).");
	} else {
		if (device_ids_version_unchanged(cmd))
			device_ids_write(cmd);
		else
			log_debug("Skip devices file update (changed).");
	}
	if (!held)
		unlock_devices_file(cmd);
}

int device_ids_version_unchanged(struct cmd_context *cmd)
{
	char line[PATH_MAX];
	char version_buf[VERSION_LINE_MAX];
	FILE *fp;

	if (!(fp = fopen(cmd->devices_file_path, "r"))) {
		log_warn("Cannot open devices file to read.");
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		if (!strncmp(line, "VERSION", 7)) {
			if (fclose(fp))
				stack;

			_copy_idline_str(line, version_buf, sizeof(version_buf));

			log_debug("check devices file version %s prev %s", version_buf, _devices_file_version);

			if (!strcmp(version_buf, _devices_file_version))
				return 1;
			return 0;
		}
	}

	return 0;
}

int device_ids_use_devname(struct cmd_context *cmd)
{
	struct use_id *uid;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (uid->idtype == DEV_ID_TYPE_DEVNAME)
			return 1;
	}
	return 0;
}

struct use_id *get_uid_for_dev(struct cmd_context *cmd, struct device *dev)
{
	struct use_id *uid;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (uid->dev == dev)
			return uid;
	}
	return NULL;
}

struct use_id *get_uid_for_pvid(struct cmd_context *cmd, const char *pvid)
{
	struct use_id *uid;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!uid->pvid)
			continue;
		if (!strcmp(uid->pvid, pvid))
			return uid;
	}
	return NULL;
}

static struct use_id *_get_uid_for_devname(struct cmd_context *cmd, const char *devname)
{
	struct use_id *uid;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!uid->devname)
			continue;
		if (!strcmp(uid->devname, devname))
			return uid;
	}
	return NULL;
}

static struct use_id *_get_uid_for_device_id(struct cmd_context *cmd, uint16_t idtype, const char *idname)
{
	struct use_id *uid;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (uid->idname && (uid->idtype == idtype) && !strcmp(uid->idname, idname))
			return uid;
	}
	return NULL;
}

/*
 * Add or update entry for this dev.
 * IDTYPE=sys_wwid IDNAME=01234566 DEVNAME=/dev/sdb PVID=99393939 [OPTS=xx,yy,zz]
 *
 * add an entry to dev->ids and point dev->id to it.
 * add or update entry in cmd->use_device_ids
 */
int device_id_add(struct cmd_context *cmd, struct device *dev, const char *pvid_arg,
		  const char *idtype_arg, const char *id_arg)
{
	char pvid[ID_LEN+1] = { 0 };
	uint16_t idtype = 0;
	const char *idname = NULL;
	const char *check_idname = NULL;
	const char *update_matching_kind = NULL;
	const char *update_matching_name = NULL;
	struct use_id *uid, *update_uid = NULL, *uid_dev, *uid_pvid, *uid_devname, *uid_devid;
	struct dev_id *did;
	int found_did = 0;

	if (!cmd->enable_devices_file)
		 return 1;

	/*
	 * The pvid_arg may be passed from a 'struct id' (pv->id) which
	 * may not have a terminating \0.
	 * Make a terminated copy to use as a string.
	 */
	memcpy(&pvid, pvid_arg, ID_LEN);

	uid_dev = get_uid_for_dev(cmd, dev);
	uid_pvid = get_uid_for_pvid(cmd, pvid);
	uid_devname = _get_uid_for_devname(cmd, dev_name(dev));

	/*
	 * Choose the device_id type for the device being added.
	 *
	 * 1. use an idtype dictated if this is a special kind
	 * of device, e.g. loop, mpath, md, nbd, etc
	 *
	 * 2. use an idtype specified by user option.
	 *
	 * 3. use an idtype from an existing matching devices_file entry.
	 *
	 * 4. use sys_wwid, if it exists.
	 *
	 * 5. use sys_serial, if it exists.
	 *
	 * 6. use devname as the last resort.
	 *
	 * TODO: allow lvm.conf device_id_types to control the
	 * idtypes that can be used above?
	 *
	 * If this device is part of a VG, and the VG metadata already
	 * includes a device_id for this device, then it would be nice
	 * to use that device_id.  But, lvmdevices is in principle not
	 * reading/writing VG metadata.  Adding with vgimportdevices
	 * would have access to the VG metadata and use a device_id
	 * from the metadata if it's set.
	 */

	if (_dev_has_mpath_uuid(cmd, dev, &idname)) {
		idtype = DEV_ID_TYPE_MPATH_UUID;
		goto id_done;
	}

	if (MAJOR(dev->dev) == cmd->dev_types->loop_major) {
		idtype = DEV_ID_TYPE_LOOP_FILE;
		goto id_name;
	}

	if (MAJOR(dev->dev) == cmd->dev_types->md_major) {
		idtype = DEV_ID_TYPE_MD_UUID;
		goto id_name;
	}

	if (MAJOR(dev->dev) == cmd->dev_types->drbd_major) {
		/* TODO */
		log_print("Missing support for DRBD idtype");
	}

	if (idtype_arg) {
		if (!(idtype = idtype_from_str(idtype_arg)))
			log_warn("WARNING: ignoring unknown device_id type %s.", idtype_arg);
		else {
			if (id_arg) {
				idname = id_arg;
				goto id_done;
			}
			goto id_name;
		}
	}

	/*
	 * If there's an existing entry for this pvid, use that idtype.
	 */
	if (!idtype && uid_pvid) {
		idtype = uid_pvid->idtype;
		goto id_name;
	}

	/*
	 * No device-specific, existing, or user-specified idtypes,
	 * so use first available of sys_wwid / sys_serial / devname.
	 */
	idtype = DEV_ID_TYPE_SYS_WWID;

id_name:
	if (!(idname = device_id_system_read(cmd, dev, idtype))) {
		if (idtype == DEV_ID_TYPE_SYS_WWID) {
			idtype = DEV_ID_TYPE_SYS_SERIAL;
			goto id_name;
		}
		idtype = DEV_ID_TYPE_DEVNAME;
		goto id_name;
	}

id_done:

	/*
	 * Create a dev_id struct for the new idtype on dev->ids.
	 */
	dm_list_iterate_items(did, &dev->ids) {
		if (did->idtype == idtype) {
			found_did = 1;
			break;
		}
	}
	if (found_did && !strcmp(did->idname, idname))
		free((char *)idname);
	else if (found_did && strcmp(did->idname, idname)) {
		dm_list_del(&did->list);
		free_did(did);
		found_did = 0;
	}
	if (!found_did) {
		if (!(did = zalloc(sizeof(struct dev_id))))
			return_0;
		did->idtype = idtype;
		did->idname = (char *)idname;
		did->dev = dev;
		dm_list_add(&dev->ids, &did->list);
	}
	dev->id = did;
	dev->flags |= DEV_MATCHED_USE_ID;

	/*
	 * Update the cmd->use_device_ids list for the new device.  The
	 * use_device_ids list will be used to update the devices file.
	 *
	 * The dev being added can potentially overlap existing entries
	 * in various ways.  If one of the existing entries is truely for
	 * this device being added, then we want to update that entry.
	 * If some other existing entries are not for the same device, but
	 * have some overlapping values, then we want to try to update
	 * those other entries to fix any incorrect info.
	 */

	uid_devid = _get_uid_for_device_id(cmd, did->idtype, did->idname);

	if (uid_dev)
		log_debug("device_id_add %s pvid %s matches uid_dev %p dev %s",
			  dev_name(dev), pvid, uid_dev, dev_name(uid_dev->dev));
	if (uid_pvid)
		log_debug("device_id_add %s pvid %s matches uid_pvid %p dev %s pvid %s",
			  dev_name(dev), pvid, uid_pvid, uid_pvid->dev ? dev_name(uid_pvid->dev) : ".",
			  uid_pvid->pvid);
	if (uid_devid)
		log_debug("device_id_add %s pvid %s matches uid_devid %p dev %s pvid %s",
			  dev_name(dev), pvid, uid_devid, uid_devid->dev ? dev_name(uid_devid->dev) : ".",
			  uid_devid->pvid);
	if (uid_devname)
		log_debug("device_id_add %s pvid %s matches uid_devname %p dev %s pvid %s",
			  dev_name(dev), pvid, uid_devname, uid_devname->dev ? dev_name(uid_devname->dev) : ".",
			  uid_devname->pvid);

	/*
	 * If one of the existing entries (uid_dev, uid_pvid, uid_devid, uid_devname)
	 * is truely for the same device that is being added, then set update_uid to
	 * that existing entry to be updated.
	 */

	if (uid_dev) {
		update_uid = uid_dev;
		dm_list_del(&update_uid->list);
		update_matching_kind = "device";
		update_matching_name = dev_name(dev);

		if (uid_devid && (uid_devid != uid_dev)) {
			log_warn("WARNING: device %s (%s) and %s (%s) have duplicate device ID.",
				 dev_name(dev), idname,
				 uid_pvid->dev ? dev_name(uid_pvid->dev) : ".", uid_pvid->idname);
		}

		if (uid_pvid && (uid_pvid != uid_dev)) {
			log_warn("WARNING: device %s (%s) and %s (%s) have duplicate PVID %s",
				 dev_name(dev), idname,
				 uid_pvid->dev ? dev_name(uid_pvid->dev) : ".", uid_pvid->idname,
				 pvid);
		}

		if (uid_devname && (uid_devname != uid_dev)) {
			/* clear devname in another entry with our devname */
			log_print("Clearing stale devname %s for PVID %s",
				  uid_devname->devname, uid_devname->pvid);
			free(uid_devname->devname);
			uid_devname->devname = NULL;
		}

	} else if (uid_pvid) {
		/*
		 * If the device_id of the existing entry for PVID is the same
		 * as the device_id of the device being added, then update the
		 * existing entry.  If the device_ids differ, then the devices
		 * have duplicate PVIDs, and the new device gets a new entry
		 * (if we allow it to be added.)
		 */
		if (uid_pvid->idtype == idtype)
			check_idname = idname;
		else
			check_idname = device_id_system_read(cmd, dev, uid_pvid->idtype);

		if (check_idname && !strcmp(check_idname, uid_pvid->idname)) {
			update_uid = uid_pvid;
			dm_list_del(&update_uid->list);
			update_matching_kind = "PVID";
			update_matching_name = pvid;
		} else {
			log_warn("WARNING: device %s (%s) and %s (%s) have duplicate PVID %s",
				 dev_name(dev), idname,
				 uid_pvid->dev ? dev_name(uid_pvid->dev) : ".", uid_pvid->idname,
				 pvid);

			/* require a force or similar option to allow adding duplicate? */
		}

		if (uid_devid && (uid_devid != uid_pvid)) {
			/* warn about another entry using the same device_id */
			log_warn("WARNING: duplicate device_id %s for PVIDs %s %s",
				 uid_devid->idname, uid_devid->pvid, uid_pvid->pvid);
		}

		if (uid_devname && (uid_devname != uid_pvid)) {
			/* clear devname in another entry with our devname */
			log_print("Clearing stale devname %s for PVID %s",
				  uid_devname->devname, uid_devname->pvid);
			free(uid_devname->devname);
			uid_devname->devname = NULL;
		}

	} else if (uid_devid) {
		/*
		 * Do we create a new uid or update the existing uid?
		 * If it's the same device, update the existing uid,
		 * but if it's two devices with the same device_id, then
		 * create a new uid.
		 *
		 * We know that 'dev' has device_id 'did'.
		 * Check if uid_devid->dev is different from 'dev'
		 * and that uid_devid->idname matches did.
		 * If so, then there are two different devices with
		 * the same device_id (create a new uid for dev.)
		 * If not, then update the existing uid_devid.
		 */
		
		if (uid_devid->dev != dev)
			check_idname = device_id_system_read(cmd, uid_devid->dev, did->idtype);

		if (check_idname && !strcmp(check_idname, did->idname)) {
			int ret1, ret2;
			dev_t devt1, devt2;

			/* two different devices have the same device_id,
			   create a new uid for the device being added */

			/* dev_is_partitioned() the dev open to read it. */
			if (!label_scan_open(uid_devid->dev))
				log_print("Cannot open %s", dev_name(uid_devid->dev));

			if (dev_is_partitioned(cmd->dev_types, uid_devid->dev)) {
				/* Check if existing entry is whole device and new entry is a partition of it. */
				ret1 = dev_get_primary_dev(cmd->dev_types, dev, &devt1);
				if ((ret1 == 2) && (devt1 == uid_devid->dev->dev))
					log_print("WARNING: remove partitioned device %s from devices file.", dev_name(uid_devid->dev));
			} else {
				/* Check if both entries are partitions of the same device. */
				ret1 = dev_get_primary_dev(cmd->dev_types, dev, &devt1);
				ret2 = dev_get_primary_dev(cmd->dev_types, uid_devid->dev, &devt2);

				if ((ret1 == 2) && (ret2 == 2) && (devt1 == devt2)) {
					log_print("Partitions %s %s have same device_id %s",
						  dev_name(dev), dev_name(uid_devid->dev), idname);
				} else {
					log_print("Duplicate device_id %s %s for %s and %s",
						  idtype_to_str(did->idtype), check_idname,
						  dev_name(dev), dev_name(uid_devid->dev));
				}
			}
		} else {
			/* update the existing entry with matching devid */
			update_uid = uid_devid;
			dm_list_del(&update_uid->list);
			update_matching_kind = "device_id";
			update_matching_name = did->idname;
		}

		if (uid_devname && (uid_devname != uid_devid)) {
			/* clear devname in another entry with our devname */
			log_print("Clearing stale devname %s for PVID %s",
				  uid_devname->devname, uid_devname->pvid);
			free(uid_devname->devname);
			uid_devname->devname = NULL;
		}

	} else if (uid_devname) {
		/* clear devname in another entry with our devname */
		log_print("Clearing stale devname %s for PVID %s",
			   uid_devname->devname, uid_devname->pvid);
		free(uid_devname->devname);
		uid_devname->devname = NULL;
	}

	if (!update_uid) {
		log_print("Adding new entry to devices file for %s PVID %s %s %s.",
			  dev_name(dev), pvid, idtype_to_str(did->idtype), did->idname);
		if (!(uid = zalloc(sizeof(struct use_id))))
			return_0;
	} else {
		uid = update_uid;
		log_print("Updating existing entry in devices file for %s that matches %s %s.",
			  dev_name(dev), update_matching_kind, update_matching_name);
	}

	if (uid->idname)
		free(uid->idname);
	if (uid->devname)
		free(uid->devname);
	if (uid->pvid)
		free(uid->pvid);

	uid->idtype = did->idtype;
	uid->idname = strdup(did->idname);
	uid->devname = strdup(dev_name(dev));
	uid->dev = dev;
	uid->pvid = strdup(pvid);

	dev_get_partition_number(dev, &uid->part);

	if (!uid->idname || !uid->devname || !uid->pvid) {
		free_uid(uid);
		return 0;
	}

	dm_list_add(&cmd->use_device_ids, &uid->list);

	return 1;
}

/*
 * Update entry for this dev.
 * Set PVID=.
 * update entry in cmd->use_device_ids
 */
void device_id_pvremove(struct cmd_context *cmd, struct device *dev)
{
	struct use_id *uid;

	if (!cmd->enable_devices_file)
		return;

	if (!(uid = get_uid_for_dev(cmd, dev))) {
		log_warn("WARNING: use_device_ids does not include %s", dev_name(dev));
		return;
	}

	if (uid->pvid) {
		free(uid->pvid);
		uid->pvid = NULL;
	}
}

/*
 * check for dev->ids entry with uid->idtype, if found compare it,
 * if not, system_read of this type and add entry to dev->ids, compare it.
 * When a match is found, set up links among uid/did/dev.
 */

static int _match_uid_deviceid_to_dev(struct cmd_context *cmd, struct use_id *uid, struct device *dev)
{
	struct dev_id *did;
	const char *idname;
	int part;

	if (!uid->idname || !uid->idtype)
		return_0;

	if (!dev_get_partition_number(dev, &part))
		return_0;
	if (part != uid->part)
		return_0;

	dm_list_iterate_items(did, &dev->ids) {
		if (did->idtype == uid->idtype) {
			if (did->idname && !strcmp(did->idname, uid->idname)) {
				uid->dev = dev;
				dev->id = did;
				dev->flags |= DEV_MATCHED_USE_ID;
				log_debug("devices idname %s devname %s matched %s", uid->idname, uid->devname, dev_name(dev));
				return 1;
			} else {
				return_0;
			}
		}
	}

	if (!(did = zalloc(sizeof(struct dev_id))))
		return_0;

	if (!(idname = device_id_system_read(cmd, dev, uid->idtype))) {
		/* Save a new did in dev->ids for this type to indicate no match
		   to avoid repeated system_read, since this called many times.
		   Setting idtype and NULL idname means no id of this type. */
		did->idtype = uid->idtype;
		did->dev = dev;
		dm_list_add(&dev->ids, &did->list);
		return 0;
	}

	/* Save this id for the device (so it can be quickly checked again), even
	   if it's not the idtype used to identify the dev in device_id_file. */
	did->idtype = uid->idtype;
	did->idname = (char *)idname;
	did->dev = dev;
	dm_list_add(&dev->ids, &did->list);

	if (!strcmp(idname, uid->idname)) {
		uid->dev = dev;
		dev->id = did;
		dev->flags |= DEV_MATCHED_USE_ID;
		log_debug("devices idname %s devname %s matched %s", uid->idname, uid->devname, dev_name(dev));
		return 1;
	}

	return 0;
}

int device_ids_match_dev(struct cmd_context *cmd, struct device *dev)
{
	struct use_id *uid;

	/* First check the uid entry with matching devname since it's likely correct. */
	if ((uid = _get_uid_for_devname(cmd, dev_name(dev)))) {
		if (_match_uid_deviceid_to_dev(cmd, uid, dev))
			return 1;
	}

	/* Check all uid entries since the devname could have changed. */
	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!_match_uid_deviceid_to_dev(cmd, uid, dev))
			continue;
		return 1;
	}

	return 0;
}

/*
 * For each entry on cmd->use_device_ids (entries in the devices file),
 * find a struct device from dev-cache.  They are paired based strictly
 * on the device id.
 *
 * This must not open or read devices.  This function cannot use filters.
 * filters are applied after this, and the filters may open devs in the first
 * nodata filtering.  The second filtering, done after label_scan has read
 * a device, is allowed to read a device to evaluate filters that need to see
 * data from the dev.
 *
 * When a device id of a particular type is obtained for a dev, a did for that
 * type is saved in dev->ids in case it needs to be checked again.
 *
 * When a device in dev-cache is matched to an entry in the devices file
 * (a struct use_id), then:
 * . uid->dev = dev;
 * . dev->id = did;
 * . dev->flags |= DEV_MATCHED_USE_ID;
 *
 * Later when filter-deviceid is run to exclude devices that are not
 * included in the devices file, the filter checks if DEV_MATCHED_USE_ID
 * is set which means that the dev matches a devices file entry and
 * passes the filter.
 */

void device_ids_match(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct use_id *uid;
	struct device *dev;

	if (cmd->enable_devices_list) {
		dm_list_iterate_items(uid, &cmd->use_device_ids) {
			if (uid->dev)
				continue;
			if (!(uid->dev = dev_cache_get(cmd, uid->devname, NULL))) {
				log_print("Device not found for %s.", uid->devname);
			} else {
				/* Should we set dev->id?  Which idtype?  Use --deviceidtype? */
				uid->dev->flags |= DEV_MATCHED_USE_ID;
			}
		}
		return;
	}

	if (!cmd->enable_devices_file)
		return;

	log_debug("matching devices file entries to devices");

	/*
	 * We would set cmd->filter_deviceid_skip but we are disabling
	 * all filters (dev_cache_get NULL arg) so it's not necessary.
	 */

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		/* already matched */
		if (uid->dev) {
			log_debug("devices idname %s previously matched %s",
				  uid->idname, dev_name(uid->dev));
			continue;
		}

		/*
		 * uid->devname from the devices file is the last known
		 * device name.  It may be incorrect, but it's usually
		 * correct, so it's an efficient place to check for a
		 * match first.
		 *
		 * NULL filter is used because we are just setting up the
		 * the uid/dev pairs in preparation for using the filters.
		 */
		if (uid->devname &&
		    (dev = dev_cache_get(cmd, uid->devname, NULL))) {
			/* On successful match, uid, dev, and did are linked. */
			if (_match_uid_deviceid_to_dev(cmd, uid, dev))
				continue;
			else {
				/* The device node may exist but the device is disconnected / zero size,
				   and likely has no sysfs entry to check for wwid.  Continue to look
				   for the device id on other devs. */
				log_debug("devices entry %s %s devname found but not matched", uid->devname, uid->pvid ?: ".");
			}
		}

		/*
		 * Iterate through all devs and try to match uid.
		 *
		 * If a match is made here it means the uid->devname is wrong,
		 * so the device_id file should be updated with a new devname.
		 *
		 * NULL filter is used because we are just setting up the
		 * the uid/dev pairs in preparation for using the filters.
		 */
		if (!(iter = dev_iter_create(NULL, 0)))
			continue;
		while ((dev = dev_iter_get(cmd, iter))) {
			if (dev->flags & DEV_MATCHED_USE_ID)
				continue;
			if (_match_uid_deviceid_to_dev(cmd, uid, dev))
				break;
		}
		dev_iter_destroy(iter);
	}

	/*
	 * Look for entries in devices file for which we found no device.
	 */
	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		/* Found a device for this entry. */
		if (uid->dev && (uid->dev->flags & DEV_MATCHED_USE_ID))
			continue;

		/* This shouldn't be possible. */
		if (uid->dev && !(uid->dev->flags & DEV_MATCHED_USE_ID)) {
			log_error("Device %s not matched to device_id", dev_name(uid->dev));
			continue;
		}

		/* The device is detached, this is not uncommon. */
		log_print("No device matches devices file IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s",
			  idtype_to_str(uid->idtype),
			  uid->idname ?: ".",
			  uid->devname ?: ".",
			  uid->pvid ?: ".");
	}
}

/*
 * This is called after devices are scanned to compare what was found on disks
 * vs what's in the devices file.  The devices file could be outdated and need
 * correcting; the authoritative data is what's on disk.  Now that we have read
 * the device labels and know the PVID's from disk we can check the PVID's in
 * use_device_ids entries from the devices file.
 */

void device_ids_validate(struct cmd_context *cmd, int *device_ids_invalid, int noupdate)
{
	struct dm_list wrong_devs;
	struct device *dev;
	struct device_list *devl;
	struct use_id *uid;
	int update_file = 0;

	dm_list_init(&wrong_devs);

	if (!cmd->enable_devices_file)
		return;

	log_debug("validating devices file entries");

	/*
	 * Validate entries with proper device id types.
	 * idname is the authority for pairing uid and dev.
	 */
	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!uid->dev)
			continue;

		/* For this idtype the idname match is unreliable. */
		if (uid->idtype == DEV_ID_TYPE_DEVNAME)
			continue;

		dev = uid->dev;

		/*
		 * uid and dev may have been matched, but the dev could still
		 * have been excluded by other filters during label scan.
		 * This shouldn't generally happen, but if it does the user
		 * probably wants to do something about it.
		 */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "persistent")) {
			log_print("Devices file %s excluded by filter: %s.",
				  dev_name(dev), dev_filtered_reason(dev));
			continue;
		}

		/*
		 * If this device hasn't been scanned, or is not a PV, then a
		 * pvid has not been read and there's nothing to validate.
		 */
		if (!(dev->flags & DEV_SCAN_FOUND_LABEL))
			continue;

		/*
		 * If the uid pvid from the devices file does not match the
		 * pvid read from disk, replace the uid pvid with the pvid from
		 * disk and update the pvid in the devices file entry.
		 */
		if (dev->pvid[0]) {
			if (!uid->pvid || memcmp(dev->pvid, uid->pvid, ID_LEN)) {
				log_print("Device %s has PVID %s (devices file %s)",
					  dev_name(dev), dev->pvid, uid->pvid ?: ".");
				if (uid->pvid)
					free(uid->pvid);
				if (!(uid->pvid = strdup(dev->pvid)))
					stack;
				update_file = 1;
				*device_ids_invalid = 1;
			}
		} else {
			if (uid->pvid && (uid->pvid[0] != '.')) {
				log_print("Device %s has no PVID (devices file %s)",
					  dev_name(dev), uid->pvid);
				if (uid->pvid)
					free(uid->pvid);
				uid->pvid = NULL;
				update_file = 1;
				*device_ids_invalid = 1;
			}
		}

		if (!uid->devname || strcmp(dev_name(uid->dev), uid->devname)) {
			log_print("Device %s has updated name (devices file %s)",
				   dev_name(uid->dev), uid->devname ?: ".");
			if (uid->devname)
				free(uid->devname);
			if (!(uid->devname = strdup(dev_name(uid->dev))))
				stack;
			update_file = 1;
			*device_ids_invalid = 1;
		}
	}

	/*
	 * Validate entries with unreliable devname id type.
	 * pvid match overrides devname id match.
	 */
	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!uid->dev)
			continue;

		if (uid->idtype != DEV_ID_TYPE_DEVNAME)
			continue;

		dev = uid->dev;

		if (!uid->pvid || uid->pvid[0] == '.')
			continue;

		/*
		 * A good match based on pvid.
		 */
		if (dev->pvid[0] && !strcmp(dev->pvid, uid->pvid)) {
			const char *devname = dev_name(dev);

			if (strcmp(devname, uid->idname)) {
				/* shouldn't happen since this was basis for match */
				log_error("uid for pvid %s unexpected idname %s mismatch dev %s",
					  uid->pvid, uid->idname, devname);
				*device_ids_invalid = 1;
				continue;
			}

			if (!uid->devname || strcmp(devname, uid->devname)) {
				log_print("Device %s has updated name (devices file %s)",
					  devname, uid->devname ?: ".");
				if (uid->devname)
					free(uid->devname);
				if (!(uid->devname = strdup(devname)))
					stack;
				update_file = 1;
				*device_ids_invalid = 1;
			}
			continue;
		}

		/*
		 * An incorrect match, the pvid read from dev does not match
		 * uid->pvid for the uid dev was matched to.
		 * uid->idname is wrong, uid->devname is probably wrong.
		 * undo the incorrect match between uid and dev
		 */

		log_print("Devices file PVID %s matched to wrong device %s with PVID %s",
			  uid->pvid, dev_name(dev), dev->pvid[0] ? dev->pvid : ".");

		if ((devl = dm_pool_zalloc(cmd->mem, sizeof(*devl)))) {
			/* If this dev matches no uid, drop it at the end. */
			devl->dev = dev;
			dm_list_add(&wrong_devs, &devl->list);
		}

		if (uid->idname) {
			free(uid->idname);
			uid->idname = NULL;
		}
		if (uid->devname) {
			free(uid->devname);
			uid->devname = NULL;
		}
		dev->flags &= ~DEV_MATCHED_USE_ID;
		dev->id = NULL;
		uid->dev = NULL;
		update_file = 1;
		*device_ids_invalid = 1;
	}

	/*
	 * devs that were wrongly matched to a uid and are not being
	 * used in another correct uid should be dropped.
	 */
	dm_list_iterate_items(devl, &wrong_devs) {
		if (!get_uid_for_dev(cmd, devl->dev)) {
			log_debug("Drop incorrectly matched %s", dev_name(devl->dev));
			cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);
			lvmcache_del_dev(devl->dev);
		}
	}

	/*
	 * Check for other problems for which we want to set *device_ids_invalid,
	 * even if we don't have a way to fix them right here.  In particular,
	 * issues that may be fixed shortly by device_ids_find_renamed_devs.
	 * Setting device_ids_invalid tells the caller to not use hints.
	 *
	 * (Check for other issues here? e.g. duplicate idname or duplicate pvid?)
	 */
	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (*device_ids_invalid)
			break;

		if (!uid->idname || (uid->idname[0] == '.'))
			*device_ids_invalid = 1;

		if ((uid->idtype == DEV_ID_TYPE_DEVNAME) && !uid->dev && uid->pvid)
			*device_ids_invalid = 1;
	}

	/* FIXME: for wrong devname cases, wait to write new until device_ids_find_renamed_devs? */

	/*
	 * try lock and device_ids_write(), the update is not required and will
	 * be done by a subsequent command if it's not done here.
	 */
	if (update_file && noupdate) {
		log_debug("device ids validate update disabled.");
	} else if (update_file) {
		log_debug("device ids validate trying to update devices file.");
		_device_ids_update_try(cmd);
	} else {
		log_debug("device ids validate found no update is needed.");
	}
}

/*
 * Read pv_header for each uid to get pvid.
 * Compare with uid->pvid, and fix uid->pvid if different.
 */
void device_ids_read_pvids(struct cmd_context *cmd)
{
	char buf[4096] __attribute__((aligned(8)));
	struct device *dev;
	struct pv_header *pvh;
	struct use_id *uid;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!uid->dev)
			continue;

		dev = uid->dev;

		if (!label_scan_open(dev))
			continue;

		memset(buf, 0, sizeof(buf));

		/*
		 * To read the label we could read 512 bytes at offset 512,
		 * but we read 4096 because some of the filters that are
		 * tested will want to look beyond the label sector.
		 */

		if (!dev_read_bytes(dev, 0, 4096, buf)) {
			label_scan_invalidate(dev);
			continue;
		}

		/*
		 * This device is already in the devices file, and this
		 * function is used to check/fix the devices file entries, so
		 * we don't want to exclude the device by applying filters.
		 * What may be useful is to call passes_filter on this device
		 * so that we can print a warning if a devices_file entry would
		 * be excluded by filters.
		 */

		pvh = (struct pv_header *)(buf + 512 + 32);

		memcpy(dev->pvid, pvh->pv_uuid, ID_LEN);

		/* Since we've read the first 4K of the device, the
		   filters should not for the most part need to do
		   any further reading of the device. */

		log_debug("Checking filters with data for %s", dev_name(dev));
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			log_warn("WARNING: %s in devices file is excluded by filter: %s.",
				 dev_name(dev), dev_filtered_reason(dev));
		}

		label_scan_invalidate(dev);
	}
}

/*
 * Devices with IDNAME=devname that are mistakenly included by filter-deviceid
 * due to a devname change are fully scanned and added to lvmcache.
 * device_ids_validate() catches this by seeing that the pvid on the device
 * doesn't match what's in the devices file, and then excludes the dev, and
 * drops the lvmcache info for the dev.  It would be nicer to catch the issue
 * earlier, before the dev is fully scanned (and populated in lvmcache).  This
 * could be done by checking the devices file for the pvid right after the dev
 * header is read and before scanning more metadata.  label_scan could read the
 * pvid from the pv_header and check it prior to calling _text_read().
 * Currently it's _text_read() that first gets the pvid from the dev, and
 * passes it to lvmcache_add() which sets it in dev->pvid.
 *
 * This function searches devs for missing PVIDs, and for those found
 * updates the uid structs (devices file entries) and writes an updated
 * devices file.
 */

void device_ids_find_renamed_devs(struct cmd_context *cmd, struct dm_list *dev_list, int noupdate)
{
	struct device *dev;
	struct use_id *uid;
	struct dev_id *did;
	struct dev_iter *iter;
	struct device_list *devl;           /* holds struct device */
	struct device_id_list *dil, *dil2;  /* holds struct device + pvid */
	struct dm_list search_pvids;        /* list of device_id_list */
	struct dm_list search_devs ;        /* list of device_list */
	const char *devname;
	int update_file = 0;

	dm_list_init(&search_pvids);
	dm_list_init(&search_devs);

	if (!cmd->enable_devices_file)
		return;

	if (!cmd->search_for_devnames)
		return;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (uid->dev)
			continue;
		if (!uid->pvid)
			continue;
		if (uid->idtype != DEV_ID_TYPE_DEVNAME)
			continue;
		if (!(dil = dm_pool_zalloc(cmd->mem, sizeof(*dil))))
			continue;

		memcpy(dil->pvid, uid->pvid, ID_LEN);
		dm_list_add(&search_pvids, &dil->list);

		log_print("No device found for devices file PVID %s.", uid->pvid);
	}

	if (dm_list_empty(&search_pvids))
		return;

	/*
	 * Now we want to look at devs on the system that were previously
	 * rejected by filter-deviceid (based on a devname device id) to check
	 * if the missing PVID is on a device with a new name.
	 */
	log_debug("Filtering for renamed devs search.");

	/*
	 * Initial list of devs to search, eliminating any that have already
	 * been matched, or don't pass filters that do not read dev.  We do not
	 * want to modify the command's existing filter chain (the persistent
	 * filter), in the process of doing this search outside the deviceid
	 * filter.
	 */
	cmd->filter_regex_with_devices_file = 0;
	if (!(iter = dev_iter_create(NULL, 0)))
		return;
	while ((dev = dev_iter_get(cmd, iter))) {
		if (dev->flags & DEV_MATCHED_USE_ID)
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "sysfs"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "regex"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "type"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "usable"))
			continue;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "mpath"))
			continue;
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&search_devs, &devl->list);
	}
	dev_iter_destroy(iter);
	cmd->filter_regex_with_devices_file = 1;

	log_debug("Reading labels for renamed devs search.");

	/*
	 * Read the dev to get the pvid, and run the filters that will use the
	 * data that has been read to get the pvid.  Like above, we do not want
	 * to modify the command's existing filter chain or the persistent
	 * filter values.
	 */
	dm_list_iterate_items(devl, &search_devs) {
		dev = devl->dev;

		/*
		 * Reads 4K from the start of the disk.
		 * Looks for LVM header, and sets dev->pvid if the device is a PV.
		 * Returns 0 if the dev has no lvm label or no PVID.
		 * This loop may look at and skip many non-LVM devices.
		 */
		if (!label_read_pvid(dev))
			continue;

		/*
		 * These filters will use the block of data from bcache that
		 * was read label_read_pvid(), and may read other
		 * data blocks beyond that.
		 */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "partitioned"))
			goto next;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "signature"))
			goto next;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "md"))
			goto next;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, "fwraid"))
			goto next;

		/*
		 * Check if the the PVID is one we are searching for.
		 * Loop below looks at search_pvid entries that have dil->dev set.
		 * This continues checking after all search_pvids entries have been
		 * matched in order to check if the PVID is on duplicate devs.
		 */
		dm_list_iterate_items_safe(dil, dil2, &search_pvids) {
			if (!memcmp(dil->pvid, dev->pvid, ID_LEN)) {
				if (dil->dev) {
					log_warn("WARNING: found PVID %s on multiple devices %s %s.",
						 dil->pvid, dev_name(dil->dev), dev_name(dev));
					log_warn("WARNING: duplicate PVIDs should be changed to be unique.");
					log_warn("WARNING: use lvmdevices to select a device for PVID %s.", dil->pvid);
					dm_list_del(&dil->list);
				} else {
					log_print("Found devices file PVID %s on %s.", dil->pvid, dev_name(dev));
					dil->dev = dev;
				}
			}
		}
         next:
		label_scan_invalidate(dev);
	}

	/*
	 * The use_device_ids entries (repesenting the devices file) are
	 * updated for the new devices on which the PVs reside.  The new
	 * correct devs are set as dil->dev on search_pvids entries.
	 *
	 * The uid/dev/did are set up and linked for the new devs.
	 *
	 * The command's full filter chain is updated for the new devs now that
	 * filter-deviceid will pass.
	 */
	dm_list_iterate_items(dil, &search_pvids) {
		if (!dil->dev)
			continue;
		dev = dil->dev;
		devname = dev_name(dev);

		if (!(uid = get_uid_for_pvid(cmd, dil->pvid))) {
			/* shouldn't happen */
			continue;
		}
		if (uid->idtype != DEV_ID_TYPE_DEVNAME) {
			/* shouldn't happen */
			continue;
		}

		log_print("Updating devices file PVID %s with IDNAME=%s.", dev->pvid, devname);

		if (uid->idname)
			free(uid->idname);
		if (uid->devname)
			free(uid->devname);
		if (!(uid->idname = strdup(devname)))
			stack;
		if (!(uid->devname = strdup(devname)))
			stack;

		free_dids(&dev->ids);
		
		if (!(did = zalloc(sizeof(struct dev_id)))) {
			stack;
			continue;
		}

		if (!(did->idname = strdup(devname))) {
			stack;
			continue;
		}
		did->idtype = DEV_ID_TYPE_DEVNAME;
		did->dev = dev;
		uid->dev = dev;
		dev->id = did;
		dev->flags |= DEV_MATCHED_USE_ID;
		dm_list_add(&dev->ids, &did->list);
		dev_get_partition_number(dev, &uid->part);
		update_file = 1;
	}

	dm_list_iterate_items(dil, &search_pvids) {
		if (!dil->dev)
			continue;
		dev = dil->dev;

		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);

		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* I don't think this would happen */
			log_warn("WARNING: new device %s for PVID %s does not pass filter %s.",
				 dev_name(dev), dil->pvid, dev_filtered_reason(dev));
			uid->dev = NULL;
			dev->flags &= ~DEV_MATCHED_USE_ID;
		}
	}

	/* FIXME: devices with idtype devname that are simply detached from
	   the system (and not renamed to a new devname) will not be found but
	   will cause every command to search for it.  We do not want a device
	   that's permanently removed/detached from the system to cause all
	   future commands to search for it.  The user can use lvmdevices to
	   remove the entry for the removed device, but we should also have an
	   automatic way to avoid the constant searching.  Perhaps add a
	   SEARCHED=<count> flag to the entry after searching for it, and then
	   avoid repeated searches, or search just once a minute for up to N
	   times, and then quit automatic searches. */

	/*
	 * try lock and device_ids_write(), the update is not required and will
	 * be done by a subsequent command if it's not done here.
	 *
	 * This command could have already done an earlier device_ids_update_try
	 * (successfully or not) in device_ids_validate().
	 */
	if (update_file && noupdate) {
		log_debug("find missing pvids update disabled");
	} else if (update_file) {
		log_debug("find missing pvids trying to update devices file");
		_device_ids_update_try(cmd);
	} else {
		log_debug("find missing pvids needs no update to devices file");
	}

	/*
	 * The entries in search_pvids with a dev set are the new devs found
	 * for the PVIDs that we want to return to the caller in a device_list
	 * format.
	 */
	dm_list_iterate_items(dil, &search_pvids) {
		if (!dil->dev)
			continue;
		dev = dil->dev;

		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(dev_list, &devl->list);
	}
}

int devices_file_touch(struct cmd_context *cmd)
{
	struct stat buf;
	char dirpath[PATH_MAX];
	int fd;

	if (dm_snprintf(dirpath, sizeof(dirpath), "%s/devices", cmd->system_dir) < 0) {
		log_error("Failed to copy devices dir path");
		return 0;
	}

	if (stat(dirpath, &buf)) {
		log_error("Cannot create devices file, missing devices directory %s.", dirpath);
		return 0;
	}

	fd = open(cmd->devices_file_path, O_CREAT);
	if (fd < 0)
		return 0;
	if (close(fd))
		stack;
	return 1;
}

int devices_file_exists(struct cmd_context *cmd)
{
	struct stat buf;

	if (!cmd->devices_file_path[0])
		return 0;

	if (stat(cmd->devices_file_path, &buf))
		return 0;

	return 1;
}

/*
 * If a command also uses the global lock, the global lock
 * is acquired first, then the devices file is locked.
 *
 * There are three categories of commands in terms of
 * reading/writing the devices file:
 *
 * 1. Commands that we know intend to modify the file,
 *    lvmdevices --add|--del, vgimportdevices,
 *    pvcreate/vgcreate/vgextend, pvchange --uuid,
 *    vgimportclone.
 *
 * 2. Most other commands that do not normally modify the file.
 *
 * 3. Commands from 2 that find something to correct in
 *    the devices file during device_ids_validate().
 *    These corrections are not essential and can be
 *    skipped, they will just be done by a subsequent
 *    command.
 *
 * Locking for each case:
 *
 * 1. lock ex, read file, write file, unlock
 *
 * 2. lock sh, read file, unlock, (validate ok)
 *
 * 3. lock sh, read file, unlock, validate wants update,
 *    lock ex (nonblocking - skip update if fails),
 *    read file, check file is unchanged from prior read,
 *    write file, unlock
 */

static int _lock_devices_file(struct cmd_context *cmd, int mode, int nonblock, int *held)
{
	const char *lock_dir;
	const char *filename;
	int fd;
	int op = mode;
	int ret;

	if (!cmd->enable_devices_file || cmd->nolocking)
		return 1;

	_using_devices_file = 1;

	if (_devices_file_locked == mode) {
		/* can happen when a command holds an ex lock and does an update in device_ids_validate */
		if (held)
			*held = 1;
		return 1;
	}

	if (_devices_file_locked) {
		/* shouldn't happen */
		log_print("lock_devices_file %d already locked %d", mode, _devices_file_locked);
		return 0;
	}

	if (!(lock_dir = find_config_tree_str(cmd, global_locking_dir_CFG, NULL)))
		return_0;
	if (!(filename = cmd->devicesfile ?: find_config_tree_str(cmd, devices_devicesfile_CFG, NULL)))
		return_0;
	if (dm_snprintf(_devices_lockfile, sizeof(_devices_lockfile), "%s/D_%s", lock_dir, filename) < 0)
		return_0;

	if (nonblock)
		op |= LOCK_NB;

	if (_devices_fd != -1) {
		log_warn("lock_devices_file existing fd %d", _devices_fd);
		return 0;
	}

	fd = open(_devices_lockfile, O_CREAT|O_RDWR);
	if (fd < 0) {
		log_debug("lock_devices_file open errno %d", errno);
		return 0;
	}


	ret = flock(fd, op);
	if (!ret) {
		_devices_fd = fd;
		_devices_file_locked = mode;
		return 1;
	}

	if (close(fd))
		stack;
	return 0;
}

int lock_devices_file(struct cmd_context *cmd, int mode)
{
	return _lock_devices_file(cmd, mode, 0, NULL);
}

int lock_devices_file_try(struct cmd_context *cmd, int mode, int *held)
{
	return _lock_devices_file(cmd, mode, 1, held);
}

void unlock_devices_file(struct cmd_context *cmd)
{
	int ret;

	if (!cmd->enable_devices_file || cmd->nolocking || !_using_devices_file)
		return;

	if (_devices_fd == -1) {
		log_warn("unlock_devices_file no existing fd");
		return;
	}

	if (!_devices_file_locked)
		log_warn("unlock_devices_file not locked");

	ret = flock(_devices_fd, LOCK_UN);
	if (ret)
		log_warn("unlock_devices_file flock errno %d", errno);

	_devices_file_locked = 0;

	if (close(_devices_fd))
		stack;
	_devices_fd = -1;
}

void device_ids_init(struct cmd_context *cmd)
{
	dm_list_init(&cmd->use_device_ids);
}

void device_ids_exit(struct cmd_context *cmd)
{
	free_uids(&cmd->use_device_ids);
	if (_devices_fd == -1)
		return;
	unlock_devices_file(cmd);
}

