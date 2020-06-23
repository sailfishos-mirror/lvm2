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

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/sysmacros.h>

#define DEVICES_FILE_MAJOR 1
#define DEVICES_FILE_MINOR 1

static int _devices_fd = -1;
static int _no_devices_file;
static int _devices_file_locked;
static char _devices_lockfile[PATH_MAX];
static char _devices_file_version[256];

/*
 * How the devices file and device IDs are used by an ordinary command:
 *
 * 1. device_ids_read() reads the devices file, and adds a 'struct uid'
 *    to cmd->use_device_ids for each entry.
 * 2. dev_cache_scan() gets a list of all devices on the system, and
 *    adds a 'struct device' to dev-cache for each.
 * 3. device_ids_match() matches uid entries from the devices file
 *    with devices from dev-cache.
 * 4. label_scan() (or equivalent) iterates through all devices in
 *    dev-cache, check each one with filters, which excludes many,
 *    and reads lvm headers and metadata from the devs that pass the
 *    filters.  In most cases, lvmcache is populated with summary
 *    info about each PV during this phase.
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
static int _dev_has_mpath_uuid(struct cmd_context *cmd, struct device *dev, const char **idname)
{
	dev_t devt = dev->dev;
	dev_t prim;
	int ret;

	/* if it's a partitioned mpath device, use the primary */
	ret = dev_get_primary_dev(cmd->dev_types, dev, &prim);
	if (ret == 2)
		devt = prim;

	if (MAJOR(devt) != cmd->dev_types->device_mapper_major)
		return 0;

	_read_sys_block(cmd, dev, "dm/uuid", idname);

	if (*idname)
		return 1;
	return 0;
}

/*
 * Should there be a list like lvm.conf
 * device_id_types = [ "sys_wwid", "sys_serial" ]
 * that controls which idtype's will be used?
 *
 * If two partitions use the same device_id it doesn't really
 * matter since the device_id is primarily about selecting
 * an acceptable device to process for the PV.
 */
static const char *_device_id_system_read(struct cmd_context *cmd, struct device *dev, uint16_t idtype)
{
	const char *idname = NULL;

	if (idtype == DEV_ID_TYPE_SYS_WWID)
		_read_sys_block(cmd, dev, "device/wwid", &idname);

	else if (idtype == DEV_ID_TYPE_SYS_SERIAL)
		_read_sys_block(cmd, dev, "device/serial", &idname);

	else if (idtype == DEV_ID_TYPE_MPATH_UUID)
		_read_sys_block(cmd, dev, "dm/uuid", &idname);

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
	char *idtype, *idname, *devname, *pvid;
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

	free_uids(&cmd->use_device_ids);

	if (!(fp = fopen(cmd->devices_file_path, "r"))) {
		log_warn("Cannot open devices_file to read.");
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		if (!strncmp(line, "VERSION", 7)) {
			_copy_idline_str(line, _devices_file_version, sizeof(_devices_file_version));
			log_debug("read devices file version %s", _devices_file_version);
			continue;
		}

		idtype = strstr(line, "IDTYPE");
		idname = strstr(line, "IDNAME");
		devname = strstr(line, "DEVNAME");
		pvid = strstr(line, "PVID");

		/* These two are the minimum required. */
		if (!idtype || !idname)
			continue;

		if (!(uid = zalloc(sizeof(struct use_id))))
			return 0;

		_copy_idline_str(idtype, buf, PATH_MAX);
		if (buf[0])
			uid->idtype = idtype_from_str(buf);

		_copy_idline_str(idname, buf, PATH_MAX);
		if (buf[0])
			uid->idname = strdup(buf);

		if (!uid->idtype || !uid->idname) {
			log_print("Ignoring device: %s", line);
			free_uid(uid);
			continue;
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

		dm_list_add(&cmd->use_device_ids, &uid->list);
	}

	if (fclose(fp))
		stack;

	return ret;
}

int device_ids_write(struct cmd_context *cmd)
{
	char tmpfile[PATH_MAX];
	FILE *fp;
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

	t = time(NULL);

	fprintf(fp, "# LVM will use devices listed in this file.\n");
	fprintf(fp, "# IDTYPE and IDNAME fields are required, the DEVNAME path may change.\n");
	fprintf(fp, "# Created by LVM command %s pid %d at %s", cmd->name, getpid(), ctime(&t));
	fprintf(fp, "VERSION=%u.%u.%u\n", DEVICES_FILE_MAJOR, DEVICES_FILE_MINOR, df_counter+1);

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		devname = uid->dev ? dev_name(uid->dev) : uid->devname;
		if (!devname || devname[0] != '/')
			devname = ".";

		if (!uid->pvid || !uid->pvid[0] || (uid->pvid[0] == '.'))
			pvid = ".";
		else
			pvid = uid->pvid;

		fprintf(fp, "IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s\n",
			idtype_to_str(uid->idtype) ?: ".",
			uid->idname ?: ".", devname, pvid);
	}

	if (fflush(fp))
		stack;
	if (fclose(fp))
		stack;

	if (rename(tmpfile, cmd->devices_file_path)) {
		log_error("Failed to replace devices file errno %d", errno);
		ret = 0;
	}
out:
	return ret;
}

int device_ids_version_unchanged(struct cmd_context *cmd)
{
	char line[PATH_MAX];
	char version_buf[256];
	FILE *fp;

	if (!(fp = fopen(cmd->devices_file_path, "r"))) {
		log_warn("Cannot open devices_file to read.");
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
		if ((uid->idtype == idtype) && !strcmp(uid->idname, idname))
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
int device_id_add(struct cmd_context *cmd, struct device *dev, const char *pvid,
		  const char *idtype_arg, const char *id_arg)
{
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

	uid_dev = get_uid_for_dev(cmd, dev);
	uid_pvid = get_uid_for_pvid(cmd, pvid);
	uid_devname = _get_uid_for_devname(cmd, dev_name(dev));

	/*
	 * Choose the device_id type for the device being added.
	 *
	 * 1. use an idtype dictated if this is a special kind
	 * of device, e.g. loop, mpath, md, nbd, etc
	 * (TODO: some special types not yet implemented)
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
		/* TODO */
		log_print("Missing support for MD idtype");
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
	if (!(idname = _device_id_system_read(cmd, dev, idtype))) {
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
	 * use_device_ids list will be used to updatie the devices_file.
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
			check_idname = _device_id_system_read(cmd, dev, uid_pvid->idtype);

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
			check_idname = _device_id_system_read(cmd, uid_devid->dev, did->idtype);

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
				}
			}

			log_print("Duplicate device_id %s %s for %s and %s",
				  idtype_to_str(did->idtype), check_idname,
				  dev_name(dev), dev_name(uid_devid->dev));
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

	if (!uid->idname || !uid->idname || !uid->pvid) {
		free_uid(uid);
		return 0;
	}

	dm_list_add(&cmd->use_device_ids, &uid->list);

	return 1;
}

/*
 * Add an entry when there is no current device for it.
 * The known info, e.g. from metadata, is used to create
 * the entry.
 * devname arg could be wrong since there's no dev
 */
int device_id_add_nodev(struct cmd_context *cmd,
		        const char *idtype_str,
			const char *idname,
			const char *devname,
			const char *pvid)
{
	struct use_id *uid;
	uint16_t idtype = 0;

	if (!cmd->enable_devices_file)
		 return 1;

	if (!pvid || (pvid[0] == '.'))
		return 0;

	if (!idtype_str || !idname)
		return 0;

	if (idtype_str)
		idtype = idtype_from_str(idtype_str);

	if (!(uid = get_uid_for_pvid(cmd, pvid))) {
		if (!(uid = zalloc(sizeof(struct use_id))))
			return_0;
	}

	if (uid->idtype && (uid->idtype != idtype)) {
		log_print("Changing device_id_type from %s to %s for %s",
			   idtype_to_str(uid->idtype), idtype_to_str(idtype), devname);
	}
	if (uid->idtype && (uid->idtype == idtype) && strcmp(uid->idname, idname)) {
		log_print("Changing device_id from %s to %s for %s",
			   uid->idname, idname, devname);
	}

	if (uid->idname)
		free(uid->idname);
	if (uid->devname)
		free(uid->devname);
	if (uid->pvid)
		free(uid->pvid);
	uid->idname = NULL;
	uid->devname = NULL;
	uid->pvid = NULL;
	uid->dev = NULL;

	uid->idtype = idtype;

	if (pvid)
		uid->pvid = strdup(pvid);
	if (idname)
		uid->idname = strdup(idname);
	if (devname)
		uid->devname = strdup(devname);

	log_print("Add %s %s %s", devname ?: ".", uid->idname ?: ".", uid->pvid);

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

static int _match_uid_to_dev(struct cmd_context *cmd, struct use_id *uid, struct device *dev)
{
	struct dev_id *did;
	const char *idname;

	dm_list_iterate_items(did, &dev->ids) {
		if (did->idtype == uid->idtype) {
			if (did->idname && !strcmp(did->idname, uid->idname)) {
				uid->dev = dev;
				dev->id = did;
				dev->flags |= DEV_MATCHED_USE_ID;
				return 1;
			} else {
				return_0;
			}
		}
	}

	if (!(did = zalloc(sizeof(struct dev_id))))
		return_0;

	if (!(idname = _device_id_system_read(cmd, dev, uid->idtype))) {
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
		return 1;
	}

	return 0;
}

int device_ids_match_dev(struct cmd_context *cmd, struct device *dev)
{
	struct use_id *uid;

	/* First check the uid entry with matching devname since it's likely correct. */
	if ((uid = _get_uid_for_devname(cmd, dev_name(dev)))) {
		if (_match_uid_to_dev(cmd, uid, dev))
			return 1;
	}

	/* Check all uid entries since the devname could have changed. */
	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!_match_uid_to_dev(cmd, uid, dev))
			continue;
		return 1;
	}

	return 0;
}

/*

pvid is needed in the devices_file, and wwid (device_id more generally)
is needed in metadata in order to handle cases where a device has no wwid
or the wwid changes.  In these cases the correct set of devices can be
found and the devices_file can be corrected.  (A wwid in the metadata will
also eliminate the problem of duplicate pvs for those devices.)

Three identifiers: wwid, devname, pvid
- devname can change, cannot be duplicated, cannot be unknown
- wwid can change (rare), can be duplicated (rare), can be unknown
- pvid cannot change, can be duplicated, cannot be unknown

(wwid is more generally the device_id, and would only change or
be duplicated when the device_id is not a wwid but some other
identifier used when wwid is not available.)


if devname changes
------------------
. if wwid exists, lvm corrects devname (by reading wwid of all devices)
. if no wwid exists for the entry in devices_file, and new devname is
  out of the devices_file, then PV appears missing.  lvm would need
  to read headers from all devices on the system to find the PVID, or
  the user could run "lvmdevices --addpvid <pvid>" to read device headers
  to find PVID.  When found, devices_file is updated.
. if no wwid, and the new devname is in devices_file, then lvm will
  find the PV on the new devname during normal scan, and update the
  devices_file.


if wwid changes
---------------
. same underlying storage, different wwid reported for it

. if the new wwid is not used in another devices_file entry
  devices_file: WWID=XXX DEVNAME=/dev/foo PVID=AAA
  after reboot the wwid for the device changes to YYY
  YYY does not appear in devices_file or lvm metadata, so lvm doesn't know to scan
  the dev with that wwid
  device_ids_match() will find no dev with XXX, so uid->dev will be null
  the PV appears to be missing
  user runs a cmd to scan all devnames on the system (outside devices_file)
  to find a device with pvid AAA. when it's found, the cmd updates devices_file
  entry to have WWID=YYY PVID=AAA.  "lvmdevices --addpvid AAA"
  (if the devname for this entry remained unchanged, then lvm could likely
   avoid scanning all devs, by just scanning /dev/foo and finding AAA,
   this is basically an optimization that could be applied automatically
   and might avoid requiring the user to run a cmd to find the pv)

. new wwid value is included in devices_file for a different PV,
  causing duplicate wwids
  devices_file: WWID=XXX DEVNAME=/dev/foo PVID=AAA
                WWID=YYY DEVNAME=/dev/bar PVID=BBB
  after reboot the wwid for the first device changes to YYY
  device_ids_match() will see two devs with wwid YYY
  lvm will scan both devices and find one with AAA and the other BBB
  lvm will update devices_file to have WWID=YYY for both devs
  lvm may suggest using a different idtype if that would help

. two wwids in devices_file are swapped
  devices_file: WWID=XXX DEVNAME=/dev/foo PVID=AAA
                WWID=YYY DEVNAME=/dev/bar PVID=BBB
  the wwid for AAA changes to YYY
  the wwid for BBB changes to XXX
  device_ids_match() will seem to be ok (possibly complain about devnames)
  both devs will be scanned by label_scan
  device_ids_validate() will see different pvids for each entry
  and will update devices_file


if pvid and wwid are both duplicated
------------------------------------
. devices_file: WWID=XXX DEVNAME=/dev/foo PVID=AAA
  new state:
  WWID=XXX DEVNAME=/dev/foo PVID=AAA
  WWID=XXX DEVNAME=/dev/bar PVID=AAA
  This would fall back to the old duplicate device handling.
  We would need to keep the old duplicate pv handling to handle
  this case.
  If the wwid originates from data blocks on the storage,
  then this can easily happen by cloning the disks.


if wwids begin as duplicates
----------------------------
. lvm can still use the wwid for filtering,
  but it won't help if pvid is also duplicated


if pvid is duplicated but wwid is not
-------------------------------------
. lvm will use the wwid to choose the right one


if wwid is unknown
------------------
. and no other unique device_id is available
. this would work similarly to the old filter accepting only a list of devnames
. if devname changes and new devname in list, lvm fixes devname
. if devname changes and new devname out of filter, pv missing, then
  either automatically read all devs to find pvid, or have user run cmd to do this
  (lvmdevices --addpvid <pvid> would read every device on the system for header with pvid,
   and if found update devices_file with the new devname/pvid)
*/

/*
 * For each entry on cmd->use_device_ids, find a struct device from dev-cache.
 * This must not open or read devices.  filters are applied after this,
 * and the filters may open devs in the first filter stage.  The second
 * filtering stage, done as a part of label_scan, is allowed to read devices
 * to evaluate filters that need to see data from the dev.
 *
 * When a device id of a particular type is read for a dev, a did for that
 * type is saved in dev->ids in case it needs to be checked again.
 *
 * When a particular dev_id for a dev (in dev-cache) is matched to a use_dev
 * (from use_device_ids), then:
 * . uid->dev = dev;
 * . dev->id = did;
 * . dev->flags |= DEV_MATCHED_USE_ID;
 */

void device_ids_match(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct use_id *uid;
	struct device *dev;

	if (!cmd->enable_devices_file)
		return;

	/*
	 * We would set cmd->skip_filter_deviceid but we are disabling
	 * all filters (dev_cache_get NULL arg) so it's not necessary.
	 */

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		/* already matched */
		if (uid->dev && (uid->dev->flags & DEV_MATCHED_USE_ID))
			continue;

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
			/* On success, device_id_match_uid_to_dev() links the uid, dev, and did. */
			if (_match_uid_to_dev(cmd, uid, dev))
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
		 * so the device_id file should be udpated with a new devname.
		 *
		 * NULL filter is used because we are just setting up the
		 * the uid/dev pairs in preparation for using the filters.
		 */
		if (!(iter = dev_iter_create(NULL, 0)))
			continue;
		while ((dev = dev_iter_get(cmd, iter))) {
			if (dev->flags & DEV_MATCHED_USE_ID)
				continue;
			if (_match_uid_to_dev(cmd, uid, dev))
				break;
		}
		dev_iter_destroy(iter);
	}

	/*
	 * Look for entries in devices_file for which we found no device.
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
		log_debug("devices entry not found %s %s %s %s.",
			  uid->devname, idtype_to_str(uid->idtype), uid->idname, uid->pvid);
	}
}

/*
 * This is called after label_scan() to compare what was found on disks
 * vs what's in the devices_file.  The devices_file could be outdated
 * and need correcting; the authoritative data is what's on disk.
 * Now that we have read the device labels in label_scan and have the PVID's
 * we can check the pvid's of use_device_ids entries from the device_id_file.
 */
void device_ids_validate(struct cmd_context *cmd)
{
	struct use_id *uid;
	int update_file = 0;

	if (!cmd->enable_devices_file)
		return;

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		if (!uid->dev)
			continue;

		if (uid->dev->pvid[0] && (!uid->pvid || strcmp(uid->dev->pvid, uid->pvid))) {
			log_print("Device %s has updated PVID %s from devices_file (was %s)",
				   dev_name(uid->dev), uid->dev->pvid, uid->pvid);
			if (uid->pvid)
				free(uid->pvid);
			uid->pvid = strdup(uid->dev->pvid);
			update_file = 1;
		}

		if (!uid->devname || strcmp(dev_name(uid->dev), uid->devname)) {
			log_print("Device %s has updated devname from devices_file (was %s).",
				   dev_name(uid->dev), uid->devname ?: ".");
			if (uid->devname)
				free(uid->devname);
			uid->devname = strdup(dev_name(uid->dev));
			update_file = 1;
		}
	}

	if (update_file) {
		int held;

		/*
		 * Use a non-blocking lock since it's not essential to
		 * make this update, the next cmd will if we skip it.
		 * If the command already holds an ex lock on the
		 * devices file, lock_devices_file ex succeeds and
		 * held is set.
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

	/*
	 * Issue: if devices have no wwid or serial numbers, entries in
	 * devices_file are identified only by their unstable devnames.
	 * It the devnames then change, all devices on the system need to
	 * read to find the PVs.  Reading all devices on the system is
	 * one thing that the devices_file is meant to avoid.  A new
	 * config setting could be used to enable/disable this behavior.
	 *
	 * TODO: if there are entries on use_device_ids that have no dev
	 * and are using DEV_ID_TYPE_DEVNAME, then it's possible that the
	 * unstable devname simply changed and the new devname was not
	 * included in devices_file.  We need to read all devices on the
	 * system to find one with the missing PVID, label_scan it, and
	 * update devices_file with the new devname for the PVID.
	 *
	 * This function should tell setup_devices() that it should do a
	 * label_scan_for_pvid() (which only reads the headers for a PVID)
	 * on system devices that it did not already cover in the completed
	 * label_scan().  label_scan_for_pvid() would get a list of pvids
	 * to look for and return a list of devs on which they are found.
	 * That list of devs would then be passed to label_scan_devs()
	 * to do the full label_scan on them.
	 *
	 * A config setting would be able to disable this automatic scan
	 * of all devs for missing pvids, and the usage of the new devnames
	 * where those PVs are found.  Without this scan, PVs on unstable
	 * devnames would be missing until a user manually runs a command
	 * to search devices for the missing PVs. The user could selectively
	 * scan certain devs and avoid devs that should not be touched.
	 */
}

void device_id_read_pvid(struct cmd_context *cmd, struct device *dev)
{
	char buf[4096] __attribute__((aligned(8)));
	struct pv_header *pvh;

	memset(buf, 0, sizeof(buf));

	if (!label_scan_open(dev))
		return;

	/*
	 * We could do:
	 * dev_read_bytes(dev, 512, LABEL_SIZE, buf);
	 * which works, but there's a bcache issue that
	 * prevents proper invalidation after that.
	 */
	if (!dev_read_bytes(dev, 0, 4096, buf)) {
		label_scan_invalidate(dev);
		return;
	}

	pvh = (struct pv_header *)(buf + 512 + 32);

	memcpy(dev->pvid, pvh->pv_uuid, ID_LEN);

	label_scan_invalidate(dev);
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
		dev = uid->dev;

		if (!label_scan_open(dev))
			continue;

		memset(buf, 0, sizeof(buf));

		/* To read the label we could read 512 bytes at offset 512,
		   but we read 4096 because some of the filters that are
		   tested will want to look beyond the label sector. */

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

		if ((!uid->pvid && pvh->pv_uuid[0]) ||
		    (uid->pvid && memcmp(pvh->pv_uuid, uid->pvid, ID_LEN))) {
			memcpy(dev->pvid, pvh->pv_uuid, ID_LEN);

			log_print("Device %s has PVID %s devices_file has PVID %s",
				  dev_name(dev),
				  dev->pvid[0] ? (char *)dev->pvid : ".",
				  uid->pvid ?: ".");

			if (uid->pvid)
				free(uid->pvid);
			uid->pvid = strdup(dev->pvid);
		}

		/* Since we've read the first 4K of the device, the
		   filters should not for the most part need to do
		   any further reading of the device. */

		log_debug("Checking filters with data for %s", dev_name(dev));
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* FIXME: print which filters it doesn't pass */
			log_warn("WARNING: %s in devices file is excluded by filters.",
				 dev_name(dev));
		}

		label_scan_invalidate(dev);
	}
}

int devices_file_touch(struct cmd_context *cmd)
{
	int fd;
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

	if (!cmd->enable_devices_file) {
		_no_devices_file = 1;
		return 1;
	}

	_no_devices_file = 0;

	if (cmd->nolocking)
		return 1;

	if (_devices_file_locked == mode) {
		/* can happen when a command holds an ex lock and does an update in device_ids_validate */
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

	if (cmd->nolocking)
		return;

	if (_no_devices_file)
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

