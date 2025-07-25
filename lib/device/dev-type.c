/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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
#include "lib/device/dev-type.h"
#include "lib/device/device-types.h"
#include "lib/device/filesystem.h"
#include "lib/mm/xlate.h"
#include "lib/config/config.h"
#include "lib/metadata/metadata.h"
#include "lib/device/bcache.h"
#include "lib/label/label.h"
#include "lib/commands/toolcontext.h"
#include "lib/activate/activate.h"
#include "lib/display/display.h"
#include "device_mapper/misc/dm-ioctl.h"

#ifdef BLKID_WIPING_SUPPORT
#include <blkid/blkid.h>
#endif

#ifdef UDEV_SYNC_SUPPORT
#include <libudev.h>
#include "lib/device/dev-ext-udev-constants.h"
#endif

#include <libgen.h>
#include <ctype.h>
#include <dirent.h>

/*
 * An nvme device has major number 259 (BLKEXT), minor number <minor>,
 * and reading /sys/dev/block/259:<minor>/device/dev shows a character
 * device cmajor:cminor where cmajor matches the major number of the
 * nvme character device entry in /proc/devices.  Checking all of that
 * is excessive and unnecessary compared to just comparing /dev/name*.
 */

int dev_is_nvme(struct device *dev)
{
	return (dev->flags & DEV_IS_NVME) ? 1 : 0;
}

int dev_is_scsi(struct cmd_context *cmd, struct device *dev)
{
	return major_is_scsi_device(cmd->dev_types, MAJOR(dev->dev));
}

int dm_uuid_has_prefix(char *sysbuf, const char *prefix)
{
	if (!strncmp(sysbuf, prefix, strlen(prefix)))
		return 1;

	/*
	 * If it's a kpartx partitioned dm device the dm uuid will
	 * be part%d-<prefix>...  e.g. part1-mpath-abc...
	 * Check for the prefix after the part%-
	 */ 
	if (!strncmp(sysbuf, "part", 4)) {
		const char *dash = strchr(sysbuf, '-');

 		if (!dash)
			return 0;

		if (!strncmp(dash + 1, prefix, strlen(prefix)))
			return 1;
	}
	return 0;
}

int dev_is_mpath(struct cmd_context *cmd, struct device *dev)
{
	char buffer[128];

	if (dev_dm_uuid(cmd, dev, buffer, sizeof(buffer)) &&
	    dm_uuid_has_prefix(buffer, "mpath-"))
		return 1;

	return 0;
}

int dev_is_lv(struct cmd_context *cmd, struct device *dev)
{
	char buffer[128];

	if (dev_dm_uuid(cmd, dev, buffer, sizeof(buffer)) &&
	    !strncmp(buffer, UUID_PREFIX, sizeof(UUID_PREFIX) - 1))
		return 1;

	return 0;
}

int dev_is_used_by_active_lv(struct cmd_context *cmd, struct device *dev, int *used_by_lv_count,
			     char **used_by_dm_name, char **used_by_vg_uuid, char **used_by_lv_uuid)
{
	char holders_path[PATH_MAX];
	char dm_dev_path[PATH_MAX];
	char dm_uuid[DM_UUID_LEN];
	struct stat info;
	DIR *d;
	struct dirent *dirent;
	char *holder_name;
	unsigned dm_dev_major, dm_dev_minor;
	const size_t lvm_prefix_len = sizeof(UUID_PREFIX) - 1;
	const size_t lvm_uuid_len = lvm_prefix_len + 2 * ID_LEN;
	size_t uuid_len;
	int used_count = 0;
	char *used_name = NULL;
	char *used_vgid = NULL;
	char *used_lvid = NULL;

	/*
	 * An LV using this device will be listed as a "holder" in the device's
	 * sysfs "holders" dir.
	 */

	if (dm_snprintf(holders_path, sizeof(holders_path), "%sdev/block/%u:%u/holders/",
			dm_sysfs_dir(), MAJOR(dev->dev), MINOR(dev->dev)) < 0) {
		log_error("%s: dm_snprintf failed for path to holders directory.", dev_name(dev));
		return 0;
	}

	if (!(d = opendir(holders_path)))
		return 0;

	while ((dirent = readdir(d))) {
		if (!strcmp(".", dirent->d_name) || !strcmp("..", dirent->d_name))
			continue;

		holder_name = dirent->d_name;

		/*
		 * dirent->d_name is the dev name of the holder, e.g. "dm-1"
		 * from this name, create path "/dev/dm-1" to run stat on.
		 */
		
		if (dm_snprintf(dm_dev_path, sizeof(dm_dev_path), "%s%s", cmd->dev_dir, holder_name) < 0)
			continue;

		/*
		 * stat "/dev/dm-1" which is the holder of the dev we're checking
		 * dm_dev_major:dm_dev_minor come from stat("/dev/dm-1")
		 */
		if (stat(dm_dev_path, &info))
			continue;

		dm_dev_major = MAJOR(info.st_rdev);
		dm_dev_minor = MINOR(info.st_rdev);

		if (dm_dev_major != cmd->dev_types->device_mapper_major)
			continue;

		/*
		 * if "dm-1" is a dm device, then check if it's an LVM LV
		 * by reading DM status and seeing if the uuid begins
		 * with UUID_PREFIX  ("LVM-")
		 */
		if (!devno_dm_uuid(cmd, dm_dev_major, dm_dev_minor, dm_uuid, sizeof(dm_uuid)))
			continue;

		if (!strncmp(dm_uuid, UUID_PREFIX, lvm_prefix_len))
			used_count++;

		if (used_by_dm_name && !used_name)
			used_name = dm_pool_strdup(cmd->mem, holder_name);

		if (!used_by_vg_uuid && !used_by_lv_uuid)
			continue;

		/*
		 * UUID for LV is either "LVM-<vg_uuid><lv_uuid>" or
		 * "LVM-<vg_uuid><lv_uuid>-<suffix>", where vg_uuid and lv_uuid
		 * has length of ID_LEN and suffix len is not restricted (only
		 * restricted by whole DM UUID max len).
		 */

		uuid_len = strlen(dm_uuid);

		if (((uuid_len == lvm_uuid_len) ||
		    ((uuid_len > lvm_uuid_len) && (dm_uuid[lvm_uuid_len] == '-'))) &&
		    !strncmp(dm_uuid, UUID_PREFIX, lvm_prefix_len)) {

			if (used_by_vg_uuid && !used_vgid)
				used_vgid = dm_pool_strndup(cmd->mem, dm_uuid + lvm_prefix_len, ID_LEN);

			if (used_by_lv_uuid && !used_lvid)
				used_lvid = dm_pool_strndup(cmd->mem, dm_uuid + lvm_prefix_len + ID_LEN, ID_LEN);
		}
	}

	if (closedir(d))
		log_sys_debug("closedir", holders_path);

	if (used_by_lv_count)
		*used_by_lv_count = used_count;
	if (used_by_dm_name)
		*used_by_dm_name = used_name;
	if (used_by_vg_uuid)
		*used_by_vg_uuid = used_vgid;
	if (used_by_lv_uuid)
		*used_by_lv_uuid = used_lvid;

	if (used_count)
		return 1;
	return 0;
}

struct dev_types *create_dev_types(const char *proc_dir,
				   const struct dm_config_node *cn)
{
	struct dev_types *dt;
	char line[80];
	char proc_devices[PATH_MAX];
	FILE *pd = NULL;
	int i, j = 0;
	int line_maj = 0;
	int blocksection = 0;
	size_t dev_len = 0;
	const struct dm_config_value *cv;
	const char *name;
	char *nl;

	if (!(dt = zalloc(sizeof(struct dev_types)))) {
		log_error("Failed to allocate device type register.");
		return NULL;
	}

	if (!*proc_dir) {
		log_verbose("No proc filesystem found: using all block device types");
		for (i = 0; i < NUMBER_OF_MAJORS; i++)
			dt->dev_type_array[i].max_partitions = 1;
		return dt;
	}

	if (dm_snprintf(proc_devices, sizeof(proc_devices),
			 "%s/devices", proc_dir) < 0) {
		log_error("Failed to create /proc/devices string");
		goto bad;
	}

	if (!(pd = fopen(proc_devices, "r"))) {
		log_sys_error("fopen", proc_devices);
		goto bad;
	}

	while (fgets(line, sizeof(line), pd) != NULL) {
		i = 0;
		while (line[i] == ' ')
			i++;

		/* If it's not a number it may be name of section */
		line_maj = atoi(line + i);

		if (line_maj < 0 || line_maj >= NUMBER_OF_MAJORS) {
			/*
			 * Device numbers shown in /proc/devices are actually direct
			 * numbers passed to registering function, however the kernel
			 * uses only 12 bits, so use just 12 bits for major.
			 */
			if ((nl = strchr(line, '\n'))) *nl = '\0';
			log_warn("WARNING: /proc/devices line: %s, replacing major with %d.",
				 line, line_maj & (NUMBER_OF_MAJORS - 1));
			line_maj &= (NUMBER_OF_MAJORS - 1);
		}

		if (!line_maj) {
			blocksection = (line[i] == 'B') ? 1 : 0;
			continue;
		}

		/* We only want block devices ... */
		if (!blocksection)
			continue;

		/* Find the start of the device major name */
		while (line[i] != ' ' && line[i] != '\0')
			i++;
		while (line[i] == ' ')
			i++;

		/* Major is SCSI device */
		if (!strncmp("sd", line + i, 2) && isspace(*(line + i + 2)))
			dt->dev_type_array[line_maj].flags |= PARTITION_SCSI_DEVICE;

		else if (!strncmp("loop", line + i, 4) && isspace(*(line + i + 4)))
			dt->loop_major = line_maj;

		/* Look for device-mapper device */
		/* FIXME Cope with multiple majors */
		else if (!strncmp("device-mapper", line + i, 13) && isspace(*(line + i + 13)))
			dt->device_mapper_major = line_maj;

		/* Look for md device */
		else if (!strncmp("md", line + i, 2) && isspace(*(line + i + 2)))
			dt->md_major = line_maj;

		/* Look for blkext device */
		else if (!strncmp("blkext", line + i, 6) && isspace(*(line + i + 6)))
			dt->blkext_major = line_maj;

		/* Look for drbd device */
		else if (!strncmp("drbd", line + i, 4) && isspace(*(line + i + 4)))
			dt->drbd_major = line_maj;

		/* Look for DASD */
		else if (!strncmp("dasd", line + i, 4) && isspace(*(line + i + 4)))
			dt->dasd_major = line_maj;

		/* Look for EMC powerpath */
		else if (!strncmp("emcpower", line + i, 8) && isspace(*(line + i + 8)))
			dt->emcpower_major = line_maj;

		/* Look for Veritas Dynamic Multipathing */
		else if (!strncmp("VxDMP", line + i, 5) && isspace(*(line + i + 5)))
			dt->vxdmp_major = line_maj;

		else if (!strncmp("power2", line + i, 6) && isspace(*(line + i + 6)))
			dt->power2_major = line_maj;


		/* Go through the valid device names and if there is a
		   match store max number of partitions */
		for (j = 0; _dev_known_types[j].name[0]; j++) {
			dev_len = strlen(_dev_known_types[j].name);
			if (dev_len <= strlen(line + i) &&
			    !strncmp(_dev_known_types[j].name, line + i, dev_len) &&
			    (line_maj < NUMBER_OF_MAJORS)) {
				dt->dev_type_array[line_maj].max_partitions =
					_dev_known_types[j].max_partitions;
				break;
			}
		}

		if (!cn)
			continue;

		/* Check devices/types for local variations */
		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING) {
				log_error("Expecting string in devices/types "
					  "in config file");
				if (fclose(pd))
					log_sys_debug("fclose", proc_devices);
				goto bad;
			}
			dev_len = strlen(cv->v.str);
			name = cv->v.str;
			cv = cv->next;
			if (!cv || cv->type != DM_CFG_INT) {
				log_error("Max partition count missing for %s "
					  "in devices/types in config file",
					  name);
				if (fclose(pd))
					log_sys_debug("fclose", proc_devices);
				goto bad;
			}
			if (!cv->v.i) {
				log_error("Zero partition count invalid for "
					  "%s in devices/types in config file",
					  name);
				if (fclose(pd))
					log_sys_debug("fclose", proc_devices);
				goto bad;
			}
			if (dev_len <= strlen(line + i) &&
			    !strncmp(name, line + i, dev_len) &&
			    (line_maj < NUMBER_OF_MAJORS)) {
				dt->dev_type_array[line_maj].max_partitions = cv->v.i;
				break;
			}
		}
	}

	if (fclose(pd))
		log_sys_debug("fclose", proc_devices);

	return dt;
bad:
	free(dt);
	return NULL;
}

int dev_subsystem_part_major(struct dev_types *dt, struct device *dev)
{
	dev_t primary_dev;

	if (MAJOR(dev->dev) == dt->device_mapper_major)
		return 1;

	if (MAJOR(dev->dev) == dt->md_major)
		return 1;

	if (MAJOR(dev->dev) == dt->drbd_major)
		return 1;

	if (MAJOR(dev->dev) == dt->emcpower_major)
		return 1;

	if (MAJOR(dev->dev) == dt->power2_major)
		return 1;

	if (MAJOR(dev->dev) == dt->vxdmp_major)
		return 1;

	if ((MAJOR(dev->dev) == dt->blkext_major) &&
	    dev_get_primary_dev(dt, dev, &primary_dev) &&
	    (MAJOR(primary_dev) == dt->md_major))
		return 1;

	return 0;
}

const char *dev_subsystem_name(struct dev_types *dt, struct device *dev)
{
	if (dev->flags & DEV_IS_NVME)
		return "NVME";

	if (MAJOR(dev->dev) == dt->device_mapper_major)
		return "DM";

	if (MAJOR(dev->dev) == dt->md_major)
		return "MD";

	if (MAJOR(dev->dev) == dt->drbd_major)
		return "DRBD";

	if (MAJOR(dev->dev) == dt->dasd_major)
		return "DASD";

	if (MAJOR(dev->dev) == dt->emcpower_major)
		return "EMCPOWER";

	if (MAJOR(dev->dev) == dt->power2_major)
		return "POWER2";

	if (MAJOR(dev->dev) == dt->vxdmp_major)
		return "VXDMP";

	if (MAJOR(dev->dev) == dt->blkext_major)
		return "BLKEXT";

	if (MAJOR(dev->dev) == dt->loop_major)
		return "LOOP";

	return "";
}

int major_max_partitions(struct dev_types *dt, int major)
{
	if (major >= NUMBER_OF_MAJORS)
		return 0;

	return dt->dev_type_array[major].max_partitions;
}

int major_is_scsi_device(struct dev_types *dt, int major)
{
	if (major >= NUMBER_OF_MAJORS)
		return 0;

	return (dt->dev_type_array[major].flags & PARTITION_SCSI_DEVICE) ? 1 : 0;
}

static int _loop_is_with_partscan(struct device *dev)
{
	FILE *fp;
	int partscan = 0;
	char path[PATH_MAX];
	char buffer[64];

	if (dm_snprintf(path, sizeof(path), "%sdev/block/%u:%u/loop/partscan",
			dm_sysfs_dir(), MAJOR(dev->dev), MINOR(dev->dev)) < 0) {
		log_warn("Sysfs path for partscan is too long.");
		return 0;
	}

	if (!(fp = fopen(path, "r")))
		return 0; /* not there -> no partscan */

	if (!fgets(buffer, sizeof(buffer), fp)) {
		log_warn("Failed to read %s.", path);
	} else if (sscanf(buffer, "%d", &partscan) != 1) {
		log_warn("Failed to parse %s '%s'.", path, buffer);
		partscan = 0;
	}

	if (fclose(fp))
		log_sys_debug("fclose", path);

	return partscan;
}

int dev_get_partition_number(struct device *dev, int *num)
{
	char path[PATH_MAX];
	char buf[8] = { 0 };
	dev_t devt = dev->dev;
	struct stat sb;

	if (dev->part != -1) {
		*num = dev->part;
		return 1;
	}

	if (dm_snprintf(path, sizeof(path), "%sdev/block/%u:%u/partition",
			dm_sysfs_dir(), MAJOR(devt), MINOR(devt)) < 0) {
		log_error("Failed to create sysfs path for %s", dev_name(dev));
		return 0;
	}

	if (stat(path, &sb)) {
		dev->part = 0;
		*num = 0;
		return 1;
	}

	if (!get_sysfs_value(path, buf, sizeof(buf), 0)) {
		log_error("Failed to read sysfs path for %s", dev_name(dev));
		return 0;
	}

	if (!buf[0]) {
		log_error("Failed to read sysfs partition value for %s", dev_name(dev));
		return 0;
	}

	dev->part = atoi(buf);
	*num = dev->part;
	return 1;
}

/* See linux/genhd.h and fs/partitions/msdos */
#define PART_MSDOS_MAGIC 0xAA55
#define PART_MSDOS_MAGIC_OFFSET UINT64_C(0x1FE)
#define PART_MSDOS_OFFSET UINT64_C(0x1BE)
#define PART_MSDOS_TYPE_GPT_PMBR UINT8_C(0xEE)

#define PART_GPT_HEADER_OFFSET_LBA 0x01
#define PART_GPT_MAGIC 0x5452415020494645UL /* "EFI PART" string */
#define PART_GPT_ENTRIES_FIELDS_OFFSET UINT64_C(0x48)

struct partition {
	uint8_t boot_ind;
	uint8_t head;
	uint8_t sector;
	uint8_t cyl;
	uint8_t sys_ind;	/* partition type */
	uint8_t end_head;
	uint8_t end_sector;
	uint8_t end_cyl;
	uint32_t start_sect;
	uint32_t nr_sects;
} __attribute__((packed));

static int _has_sys_partition(struct device *dev)
{
	char path[PATH_MAX];
	struct stat info;
	unsigned major = MAJOR(dev->dev);
	unsigned minor = MINOR(dev->dev);

	/* check if dev is a partition */
	if (dm_snprintf(path, sizeof(path), "%sdev/block/%u:%u/partition",
			dm_sysfs_dir(), major, minor) < 0) {
		log_warn("WARNING: %s: partition path is too long.", dev_name(dev));
		return 0;
	}

	if (stat(path, &info) == -1) {
		if (errno != ENOENT)
			log_sys_debug("stat", path);
		return 0;
	}
	return 1;
}

static int _is_partitionable(struct dev_types *dt, struct device *dev)
{
	int parts = major_max_partitions(dt, MAJOR(dev->dev));

	if (MAJOR(dev->dev) == dt->device_mapper_major)
		return 1;

	/* All MD devices are partitionable via blkext (as of 2.6.28) */
	if (MAJOR(dev->dev) == dt->md_major)
		return 1;

	/* All loop devices are partitionable via blkext (as of 3.2) */
	if ((MAJOR(dev->dev) == dt->loop_major) &&
	    _loop_is_with_partscan(dev))
		return 1;

	if (dev_is_nvme(dev)) {
		/* If this dev is already a partition then it's not partitionable. */
		if (_has_sys_partition(dev))
			return 0;
		return 1;
	}

	if ((parts <= 1) || (MINOR(dev->dev) % parts))
		return 0;

	return 1;
}

static int _has_gpt_partition_table(struct device *dev)
{
	unsigned int pbs, lbs;
	uint64_t entries_start;
	uint32_t nr_entries, sz_entry, i;

	struct {
		uint64_t magic;
		/* skip fields we're not interested in */
		uint8_t skip[PART_GPT_ENTRIES_FIELDS_OFFSET - sizeof(uint64_t)];
		uint64_t part_entries_lba;
		uint32_t nr_part_entries;
		uint32_t sz_part_entry;
	} __attribute__((packed)) gpt_header;

	struct {
		uint64_t part_type_guid;
		/* not interested in any other fields */
	} __attribute__((packed)) gpt_part_entry;

	if (!dev_get_direct_block_sizes(dev, &pbs, &lbs))
		return_0;

	if (!dev_read_bytes(dev, PART_GPT_HEADER_OFFSET_LBA * lbs, sizeof(gpt_header), &gpt_header))
		return_0;

	/* the gpt table is always written using LE on disk */

	if (le64toh(gpt_header.magic) != PART_GPT_MAGIC)
		return 0;

	entries_start = le64toh(gpt_header.part_entries_lba) * lbs;
	nr_entries = le32toh(gpt_header.nr_part_entries);
	sz_entry = le32toh(gpt_header.sz_part_entry);

	for (i = 0; i < nr_entries; i++) {
		if (!dev_read_bytes(dev, entries_start + (uint64_t)i * sz_entry,
				    sizeof(gpt_part_entry), &gpt_part_entry))
			return_0;

		/* just check if the guid is nonzero, no need to call le64toh here */
		if (gpt_part_entry.part_type_guid)
			return 1;
	}

	return 0;
}

/*
 * Check if there's a partition table present on the device dev, either msdos or gpt.
 * Returns:
 *
 *   1 - if it has a partition table with at least one real partition defined
 *       (note: the gpt's PMBR partition alone does not count as a real partition)
 *
 *   0 - if it has no partition table,
 *     - or if it does have a partition table, but without any partition defined,
 *     - or on error
 */
static int _has_partition_table(struct device *dev)
{
	int ret = 0;
	unsigned p;
	struct {
		uint8_t skip[PART_MSDOS_OFFSET];
		struct partition part[4];
		uint16_t magic;
	} __attribute__((packed)) buf; /* sizeof() == SECTOR_SIZE */

	if (!dev_read_bytes(dev, UINT64_C(0), sizeof(buf), &buf))
		return_0;

	/* FIXME Check for other types of partition table too */

	/* Check for msdos partition table */
	if (buf.magic == htole16(PART_MSDOS_MAGIC)) {
		for (p = 0; p < 4; ++p) {
			/* Table is invalid if boot indicator not 0 or 0x80 */
			if (buf.part[p].boot_ind & 0x7f) {
				ret = 0;
				break;
			}
			/* Must have at least one non-empty partition */
			if (buf.part[p].nr_sects) {
				/*
				 * If this is GPT's PMBR, then also
				 * check for gpt partition table.
				 */
				if (buf.part[p].sys_ind == PART_MSDOS_TYPE_GPT_PMBR && !ret)
					ret = _has_gpt_partition_table(dev);
				else
					ret = 1;
			}
		}
	} else
		/* Check for gpt partition table. */
		ret = _has_gpt_partition_table(dev);

	return ret;
}

#ifdef UDEV_SYNC_SUPPORT
static int _dev_is_partitioned_udev(struct dev_types *dt, struct device *dev)
{
	struct dev_ext *ext;
	struct udev_device *device;
	const char *value;

	/*
	 * external_device_info_source="udev" enables these udev checks.
	 * external_device_info_source="none" disables them.
	 */
	if (!(ext = dev_ext_get(dev)))
		return_0;

	device = (struct udev_device *) ext->handle;
	if (!(value = udev_device_get_property_value(device, DEV_EXT_UDEV_BLKID_PART_TABLE_TYPE)))
		return 0;

	/*
	 * Device-mapper devices have DEV_EXT_UDEV_BLKID_PART_TABLE_TYPE
	 * variable set if there's partition table found on whole device.
	 * Partitions do not have this variable set - it's enough to use
	 * only this variable to decide whether this device has partition
	 * table on it.
	 */
	if (MAJOR(dev->dev) == dt->device_mapper_major)
		return 1;

	/*
	 * Other devices have DEV_EXT_UDEV_BLKID_PART_TABLE_TYPE set for
	 * *both* whole device and partitions. We need to look at the
	 * DEV_EXT_UDEV_DEVTYPE in addition to decide - whole device
	 * with partition table on it has this variable set to
	 * DEV_EXT_UDEV_DEVTYPE_DISK.
	 */
	if (!(value = udev_device_get_property_value(device, DEV_EXT_UDEV_DEVTYPE)))
		return_0;

	return !strcmp(value, DEV_EXT_UDEV_DEVTYPE_DISK);
}
#else
static int _dev_is_partitioned_udev(struct dev_types *dt, struct device *dev)
{
	return 0;
}
#endif

static int _dev_is_partitioned_native(struct dev_types *dt, struct device *dev)
{
	int r;

	/* Unpartitioned DASD devices are not supported. */
	if ((MAJOR(dev->dev) == dt->dasd_major) && dasd_is_cdl_formatted(dev))
		return 1;

	r = _has_partition_table(dev);

	return r;
}

int dev_is_partitioned(struct cmd_context *cmd, struct device *dev)
{
	struct dev_types *dt = cmd->dev_types;

	if (!_is_partitionable(dt, dev))
		return 0;

	if (_dev_is_partitioned_native(dt, dev) == 1)
		return 1;

	if (external_device_info_source() == DEV_EXT_UDEV) {
		if (_dev_is_partitioned_udev(dt, dev) == 1)
			return 1;
	}

	return 0;
}

/*
 * Get primary dev for the dev supplied.
 *
 * We can get a primary device for a partition either by:
 *   A: knowing the number of partitions allowed for the dev and also
 *      which major:minor number represents the primary and partition device
 *      (by using the dev_types->dev_type_array)
 *   B: by the existence of the 'partition' sysfs attribute
 *      (/dev/block/<major>:<minor>/partition)
 *
 * Method A is tried first, then method B as a fallback if A fails.
 *
 * N.B. Method B can only do the decision based on the pure existence of
 *      the 'partition' sysfs item. There's no direct scan for partition
 *      tables whatsoever!
 *
 * Returns:
 *   0 on error
 *   1 if the dev is already a primary dev, primary dev in 'result'
 *   2 if the dev is a partition, primary dev in 'result'
 */
int dev_get_primary_dev(struct dev_types *dt, struct device *dev, dev_t *result)
{
	unsigned major = MAJOR(dev->dev);
	unsigned minor = MINOR(dev->dev);
	char path[PATH_MAX];
	char temp_path[PATH_MAX];
	char buffer[64];
	FILE *fp = NULL;
	int parts, residue, size, ret = 0;

	/*
	 * /dev/nvme devs don't use the major:minor numbering like
	 * block dev types that have their own major number, so
	 * the calculation based on minor number doesn't work.
	 */
	if (dev_is_nvme(dev))
		goto sys_partition;

	/*
	 * Try to get the primary dev out of the
	 * list of known device types first.
	 */
	if ((parts = dt->dev_type_array[major].max_partitions) > 1) {
		if ((residue = minor % parts)) {
			*result = MKDEV(major, (minor - residue));
			ret = 2;
		} else {
			*result = dev->dev;
			ret = 1; /* dev is not a partition! */
		}
		goto out;
	}

 sys_partition:
	/*
	 * If we can't get the primary dev out of the list of known device
	 * types, try to look at sysfs directly then. This is more complex
	 * way and it also requires certain sysfs layout to be present
	 * which might not be there in old kernels!
	 */
	if (!_has_sys_partition(dev)) {
		*result = dev->dev;
		ret = 1;
		goto out; /* dev is not a partition! */
	}

	/*
	 * extract parent's path from the partition's symlink, e.g.:
	 * - readlink /sys/dev/block/259:0 = ../../block/md0/md0p1
	 * - dirname ../../block/md0/md0p1 = ../../block/md0
	 * - basename ../../block/md0/md0  = md0
	 * Parent's 'dev' sysfs attribute  = /sys/block/md0/dev
	 */
	if (dm_snprintf(path, sizeof(path), "%sdev/block/%u:%u",
			dm_sysfs_dir(), major, minor) < 0) {
		log_warn("WARNING: %s: major:minor sysfs path is too long.", dev_name(dev));
		return 0;
	}
	if ((size = readlink(path, temp_path, sizeof(temp_path) - 1)) < 0) {
		log_warn("WARNING: Readlink of %s failed.", path);
		goto out;
	}

	temp_path[size] = '\0';

	if (dm_snprintf(path, sizeof(path), "%sblock/%s/dev",
			dm_sysfs_dir(), basename(dirname(temp_path))) < 0) {
		log_warn("WARNING: sysfs path for %s is too long.",
			 basename(dirname(temp_path)));
		goto out;
	}

	/* finally, parse 'dev' attribute and create corresponding dev_t */
	if (!(fp = fopen(path, "r"))) {
		if (errno == ENOENT)
			log_debug("sysfs file %s does not exist.", path);
		else
			log_sys_debug("fopen", path);
		goto out;
	}

	if (!fgets(buffer, sizeof(buffer), fp)) {
		log_sys_error("fgets", path);
		goto out;
	}

	if (sscanf(buffer, "%d:%d", &major, &minor) != 2) {
		log_warn("WARNING: sysfs file %s not in expected MAJ:MIN format: %s",
			  path, buffer);
		goto out;
	}
	*result = MKDEV(major, minor);
	ret = 2;
out:
	if (fp && fclose(fp))
		log_sys_debug("fclose", path);

	return ret;
}

#ifdef BLKID_WIPING_SUPPORT
int fs_block_size_and_type(const char *pathname, uint32_t *fs_block_size_bytes, char *fstype, int *nofs)
{
	blkid_probe probe = NULL;
	const char *type_str = NULL, *size_str = NULL;
	size_t len = 0;
	int ret = 1;
	int rc;

	if (!(probe = blkid_new_probe_from_filename(pathname))) {
		log_error("Failed libblkid probe setup for %s", pathname);
		return 0;
	}

	blkid_probe_enable_superblocks(probe, 1);
	blkid_probe_set_superblocks_flags(probe,
			BLKID_SUBLKS_LABEL | BLKID_SUBLKS_LABELRAW |
			BLKID_SUBLKS_UUID | BLKID_SUBLKS_UUIDRAW |
			BLKID_SUBLKS_TYPE | BLKID_SUBLKS_SECTYPE |
			BLKID_SUBLKS_USAGE | BLKID_SUBLKS_VERSION |
#ifdef BLKID_SUBLKS_FSINFO
			BLKID_SUBLKS_FSINFO |
#endif
			BLKID_SUBLKS_MAGIC);
	rc = blkid_do_safeprobe(probe);
	if (rc < 0) {
		log_debug("Failed libblkid probe for %s", pathname);
		ret = 0;
		goto out;
	} else if (rc == 1) {
		/* no file system on the device */
		log_debug("No file system found on %s.", pathname);
		if (nofs)
			*nofs = 1;
		goto out;
	}

	if (!blkid_probe_lookup_value(probe, "TYPE", &type_str, &len) && len && type_str) {
		if (fstype)
			strncpy(fstype, type_str, FSTYPE_MAX);
	} else {
		/* any difference from blkid_do_safeprobe rc=1? */
		log_debug("No file system type on %s.", pathname);
		if (nofs)
			*nofs = 1;
		goto out;
	}

	if (fs_block_size_bytes) {
		if (!blkid_probe_lookup_value(probe, "BLOCK_SIZE", &size_str, &len) && len && size_str)
			*fs_block_size_bytes = atoi(size_str);
		else
			*fs_block_size_bytes = 0;
	}

	log_debug("Found blkid fstype %s fsblocksize %s on %s",
		  type_str ?: "none", size_str ?: "unused", pathname);
out:
	blkid_free_probe(probe);
	return ret;
}

int fs_get_blkid(const char *pathname, struct fs_info *fsi)
{
	blkid_probe probe = NULL;
	const char *str = "";
	size_t len = 0;
	uint64_t fslastblock = 0;
	uint64_t fssize = 0;
	unsigned int fsblocksize = 0;
	int rc;

	if (!(probe = blkid_new_probe_from_filename(pathname))) {
		log_error("Failed libblkid probe setup for %s", pathname);
		return 0;
	}

	blkid_probe_enable_superblocks(probe, 1);
	blkid_probe_set_superblocks_flags(probe,
			BLKID_SUBLKS_LABEL | BLKID_SUBLKS_LABELRAW |
			BLKID_SUBLKS_UUID | BLKID_SUBLKS_UUIDRAW |
			BLKID_SUBLKS_TYPE | BLKID_SUBLKS_SECTYPE |
			BLKID_SUBLKS_USAGE | BLKID_SUBLKS_VERSION |
#ifdef BLKID_SUBLKS_FSINFO
			BLKID_SUBLKS_FSINFO |
#endif
			BLKID_SUBLKS_MAGIC);
	rc = blkid_do_safeprobe(probe);
	if (rc < 0) {
		log_error("Failed libblkid probe for %s", pathname);
		blkid_free_probe(probe);
		return 0;
	} else if (rc == 1) {
		/* no file system on the device */
		log_print_unless_silent("No file system found on %s.", pathname);
		fsi->nofs = 1;
		blkid_free_probe(probe);
		return 1;
	}

	if (!blkid_probe_lookup_value(probe, "TYPE", &str, &len) && len)
		strncpy(fsi->fstype, str, sizeof(fsi->fstype)-1);
	else {
		/* any difference from blkid_do_safeprobe rc=1? */
		log_print_unless_silent("No file system type on %s.", pathname);
		fsi->nofs = 1;
		blkid_free_probe(probe);
		return 1;
	}

	if (!blkid_probe_lookup_value(probe, "BLOCK_SIZE", &str, &len) && len)
		fsi->fs_block_size_bytes = atoi(str);

	if (!blkid_probe_lookup_value(probe, "FSLASTBLOCK", &str, &len) && len)
		fslastblock = strtoull(str, NULL, 0);

	if (!blkid_probe_lookup_value(probe, "FSBLOCKSIZE", &str, &len) && len)
		fsblocksize = (unsigned int)atoi(str);

	if (!blkid_probe_lookup_value(probe, "FSSIZE", &str, &len) && len)
		fssize = strtoull(str, NULL, 0);

	if (!blkid_probe_lookup_value(probe, "UUID", &str, &len) && len)
		memcpy(fsi->uuid, str, UUID_LEN);

	blkid_free_probe(probe);

	if (fslastblock && fsblocksize)
		fsi->fs_last_byte = fslastblock * fsblocksize;
	else if (fssize) {
		fsi->fs_last_byte = fssize;

		/*
		 * For swap, FSLASTBLOCK is reported by blkid since v2.41 so use that directly.
		 * Otherwise, we do have FSSIZE reported since v2.39. Then. then last block is calculated as:
		 *    FSSIZE (== size of the usable swap area) + FSBLOCKSIZE (== size of the swap header)
		 */
		if (!strcmp(fsi->fstype, "swap"))
			fsi->fs_last_byte += fsblocksize;

	}
	/*
	 * For a multi-devices btrfs, fslastblock * fsblocksize means the whole fs size.
	 * Thus here fs_last_byte can't be used as a device size boundary.
	 * Let btrfs handle it.
	 */
	if (!strcmp(fsi->fstype, "btrfs"))
		fsi->fs_last_byte = 0;

	log_debug("libblkid TYPE %s BLOCK_SIZE %d FSLASTBLOCK %llu FSBLOCKSIZE %u fs_last_byte %llu",
		  fsi->fstype, fsi->fs_block_size_bytes, (unsigned long long)fslastblock, fsblocksize,
		  (unsigned long long)fsi->fs_last_byte);
	return 1;
}

#else
int fs_block_size_and_type(const char *pathname, uint32_t *fs_block_size_bytes, char *fstype, int *nofs)
{
	log_debug("Disabled blkid BLOCK_SIZE for fs.");
	return 0;
}
int fs_get_blkid(const char *pathname, struct fs_info *fsi)
{
	log_debug("Disabled blkid for fs info.");
	return 0;
}
#endif

#ifdef BLKID_WIPING_SUPPORT

static inline int _type_in_flag_list(const char *type, uint32_t flag_list)
{
	return (((flag_list & TYPE_LVM2_MEMBER) && !strcmp(type, "LVM2_member")) ||
		((flag_list & TYPE_LVM1_MEMBER) && !strcmp(type, "LVM1_member")) ||
		((flag_list & TYPE_DM_SNAPSHOT_COW) && !strcmp(type, "DM_snapshot_cow")));
}

#define MSG_FAILED_SIG_OFFSET "Failed to get offset of the %s signature on %s."
#define MSG_FAILED_SIG_LENGTH "Failed to get length of the %s signature on %s."
#define MSG_WIPING_SKIPPED " Wiping skipped."

static int _blkid_wipe(blkid_probe probe, struct device *dev, const char *name,
		       uint32_t types_to_exclude, uint32_t types_no_prompt,
		       int yes, force_t force)
{
	static const char _msg_wiping[] = "Wiping %s signature on %s.";
	const char *offset = NULL, *type = NULL, *magic = NULL,
		   *usage = NULL, *label = NULL, *uuid = NULL;
	loff_t offset_value;
	size_t len = 0;

	if (!blkid_probe_lookup_value(probe, "TYPE", &type, NULL)) {
		if (_type_in_flag_list(type, types_to_exclude))
			return 2;
		if (blkid_probe_lookup_value(probe, "SBMAGIC_OFFSET", &offset, NULL)) {
			if (force < DONT_PROMPT) {
				log_error(MSG_FAILED_SIG_OFFSET, type, name);
				return 0;
			}

			log_warn("WARNING: " MSG_FAILED_SIG_OFFSET MSG_WIPING_SKIPPED, type, name);
			return 2;
		}
		if (blkid_probe_lookup_value(probe, "SBMAGIC", &magic, &len)) {
			if (force < DONT_PROMPT) {
				log_error(MSG_FAILED_SIG_LENGTH, type, name);
				return 0;
			}

			log_warn("WARNING: " MSG_FAILED_SIG_LENGTH MSG_WIPING_SKIPPED, type, name);
			return 2;
		}
	} else if (!blkid_probe_lookup_value(probe, "PTTYPE", &type, NULL)) {
		if (blkid_probe_lookup_value(probe, "PTMAGIC_OFFSET", &offset, NULL)) {
			if (force < DONT_PROMPT) {
				log_error(MSG_FAILED_SIG_OFFSET, type, name);
				return 0;
			}

			log_warn("WARNING: " MSG_FAILED_SIG_OFFSET MSG_WIPING_SKIPPED, type, name);
			return 2;
		}
		if (blkid_probe_lookup_value(probe, "PTMAGIC", &magic, &len)) {
			if (force < DONT_PROMPT) {
				log_error(MSG_FAILED_SIG_LENGTH, type, name);
				return 0;
			}

			log_warn("WARNING: " MSG_FAILED_SIG_LENGTH MSG_WIPING_SKIPPED, type, name);
			return 2;
		}
		usage = "partition table";
	} else
		return_0;

	offset_value = strtoll(offset, NULL, 10);

	if (!usage)
		(void) blkid_probe_lookup_value(probe, "USAGE", &usage, NULL);
	(void) blkid_probe_lookup_value(probe, "LABEL", &label, NULL);
	(void) blkid_probe_lookup_value(probe, "UUID", &uuid, NULL);
	/* Return values ignored here, in the worst case we print NULL */

	log_verbose("Found existing signature on %s at offset %s: LABEL=\"%s\" "
		    "UUID=\"%s\" TYPE=\"%s\" USAGE=\"%s\"",
		     name, offset, label, uuid, type, usage);

	if (!_type_in_flag_list(type, types_no_prompt)) {
		if (!yes && (force == PROMPT) &&
		    yes_no_prompt("WARNING: %s signature detected on %s at offset %s. "
				  "Wipe it? [y/n]: ", type, name, offset) == 'n') {
			log_error("Aborted wiping of %s.", type);
			return 0;
		}
		log_print_unless_silent(_msg_wiping, type, name);
	} else
		log_verbose(_msg_wiping, type, name);

	if (!dev_write_zeros(dev, offset_value, len)) {
		log_error("Failed to wipe %s signature on %s.", type, name);
		return 0;
	}

	return 1;
}

static int _wipe_known_signatures_with_blkid(struct device *dev, const char *name,
					     uint32_t types_to_exclude,
					     uint32_t types_no_prompt,
					     int yes, force_t force, int *wiped)
{
	blkid_probe probe = NULL;
	int found = 0, left = 0, wiped_tmp;
	int r_wipe;
	int r = 0;

	if (!wiped)
		wiped = &wiped_tmp;
	*wiped = 0;

	/* TODO: Should we check for valid dev - _dev_is_valid(dev)? */

	if (dm_list_empty(&dev->aliases))
		goto_out;

	if (!(probe = blkid_new_probe_from_filename(dev_name(dev)))) {
		log_error("Failed to create a new blkid probe for device %s.", dev_name(dev));
		goto out;
	}

	blkid_probe_enable_partitions(probe, 1);
	blkid_probe_set_partitions_flags(probe, BLKID_PARTS_MAGIC);

	blkid_probe_enable_superblocks(probe, 1);
	blkid_probe_set_superblocks_flags(probe, BLKID_SUBLKS_LABEL |
						 BLKID_SUBLKS_UUID |
						 BLKID_SUBLKS_TYPE |
						 BLKID_SUBLKS_USAGE |
						 BLKID_SUBLKS_VERSION |
						 BLKID_SUBLKS_MAGIC |
						 BLKID_SUBLKS_BADCSUM);

	while (!blkid_do_probe(probe)) {
		if ((r_wipe = _blkid_wipe(probe, dev, name, types_to_exclude, types_no_prompt, yes, force)) == 1) {
			(*wiped)++;
			if (blkid_probe_step_back(probe)) {
				log_error("Failed to step back blkid probe to check just wiped signature.");
				goto out;
			}
		}
		/* do not count excluded types */
		if (r_wipe != 2)
			found++;
	}

	if (!found)
		r = 1;

	left = found - *wiped;
	if (!left)
		r = 1;
	else
		log_warn("%d existing signature%s left on the device.",
			  left, left > 1 ? "s" : "");
out:
	if (probe)
		blkid_free_probe(probe);
	return r;
}

#endif /* BLKID_WIPING_SUPPORT */

static int _wipe_signature(struct cmd_context *cmd, struct device *dev, const char *type, const char *name,
			   int wipe_len, int yes, force_t force, int *wiped,
			   int (*signature_detection_fn)(struct cmd_context *cmd, struct device *dev, uint64_t *offset_found, int full))
{
	int wipe;
	uint64_t offset_found = 0;

	wipe = signature_detection_fn(cmd, dev, &offset_found, 1);
	if (wipe == -1) {
		log_error("Fatal error while trying to detect %s on %s.",
			  type, name);
		return 0;
	}

	if (wipe == 0)
		return 1;

	/* Specifying --yes => do not ask. */
	if (!yes && (force == PROMPT) &&
	    yes_no_prompt("WARNING: %s detected on %s. Wipe it? [y/n]: ",
			  type, name) == 'n') {
		log_error("Aborted wiping of %s.", type);
		return 0;
	}

	log_print_unless_silent("Wiping %s on %s.", type, name);
	if (!dev_write_zeros(dev, offset_found, wipe_len)) {
		log_error("Failed to wipe %s on %s.", type, name);
		return 0;
	}

	(*wiped)++;
	return 1;
}

static int _wipe_known_signatures_with_lvm(struct cmd_context *cmd, struct device *dev, const char *name,
					   uint32_t types_to_exclude __attribute__((unused)),
					   uint32_t types_no_prompt __attribute__((unused)),
					   int yes, force_t force, int *wiped)
{
	int wiped_tmp;

	if (!wiped)
		wiped = &wiped_tmp;
	*wiped = 0;

	if (!_wipe_signature(cmd, dev, "software RAID md superblock", name, 4, yes, force, wiped, dev_is_md_component) ||
	    !_wipe_signature(cmd, dev, "swap signature", name, 10, yes, force, wiped, dev_is_swap) ||
	    !_wipe_signature(cmd, dev, "LUKS signature", name, 8, yes, force, wiped, dev_is_luks))
		return 0;

	return 1;
}

int wipe_known_signatures(struct cmd_context *cmd, struct device *dev,
			  const char *name, uint32_t types_to_exclude,
			  uint32_t types_no_prompt, int yes, force_t force,
			  int *wiped)
{
	int blkid_wiping_enabled = find_config_tree_bool(cmd, allocation_use_blkid_wiping_CFG, NULL);

#ifdef BLKID_WIPING_SUPPORT
	if (blkid_wiping_enabled)
		return _wipe_known_signatures_with_blkid(dev, name,
							 types_to_exclude,
							 types_no_prompt,
							 yes, force, wiped);
#endif
	if (blkid_wiping_enabled) {
		log_warn("WARNING: allocation/use_blkid_wiping=1 configuration setting is set "
			 "while LVM is not compiled with blkid wiping support.");
		log_warn("WARNING: Falling back to native LVM signature detection.");
	}
	return _wipe_known_signatures_with_lvm(cmd, dev, name,
					       types_to_exclude,
					       types_no_prompt,
					       yes, force, wiped);
}

#ifdef __linux__

static int _snprintf_attr(char *buf, size_t buf_size, const char *sysfs_dir,
			 const char *attribute, dev_t dev)
{
	if (dm_snprintf(buf, buf_size, "%sdev/block/%u:%u/%s", sysfs_dir,
			MAJOR(dev), MINOR(dev),	attribute) < 0) {
		log_warn("WARNING: sysfs path for %s attribute is too long.", attribute);
		return 0;
	}

	return 1;
}

static int _dev_sysfs_block_attribute(struct dev_types *dt,
				      const char *attribute,
				      struct device *dev,
				      unsigned long *value)
{
	const char *sysfs_dir = dm_sysfs_dir();
	char path[PATH_MAX], buffer[64];
	FILE *fp;
	dev_t primary = 0;
	int ret = 0;

	if (!attribute || !*attribute)
		goto_out;

	if (!sysfs_dir || !*sysfs_dir)
		goto_out;

	if (!_snprintf_attr(path, sizeof(path), sysfs_dir, attribute, dev->dev))
		goto_out;

	/*
	 * check if the desired sysfs attribute exists
	 * - if not: either the kernel doesn't have topology support
	 *   or the device could be a partition
	 */
	if (!(fp = fopen(path, "r"))) {
		if (errno != ENOENT) {
			log_sys_debug("fopen", path);
			goto out;
		}
		if (!dev_get_primary_dev(dt, dev, &primary))
			goto out;

		/* get attribute from partition's primary device */
		if (!_snprintf_attr(path, sizeof(path), sysfs_dir, attribute, primary))
			goto_out;

		if (!(fp = fopen(path, "r"))) {
			if (errno != ENOENT)
				log_sys_debug("fopen", path);
			goto out;
		}
	}

	if (!fgets(buffer, sizeof(buffer), fp)) {
		log_sys_debug("fgets", path);
		goto out_close;
	}

	if (sscanf(buffer, "%lu", value) != 1) {
		log_warn("WARNING: sysfs file %s not in expected format: %s", path, buffer);
		goto out_close;
	}

	ret = 1;

out_close:
	if (fclose(fp))
		log_sys_debug("fclose", path);

out:
	return ret;
}

static unsigned long _dev_topology_attribute(struct dev_types *dt,
					     const char *attribute,
					     struct device *dev,
					     unsigned long default_value)
{
	unsigned long result = default_value;
	unsigned long value = 0UL;

	if (_dev_sysfs_block_attribute(dt, attribute, dev, &value)) {
		log_very_verbose("Device %s: %s is %lu%s.",
				 dev_name(dev), attribute, value, default_value ? "" : " bytes");

		result = value >> SECTOR_SHIFT;

		if (!result && value) {
			log_warn("WARNING: Device %s: %s is %lu and is unexpectedly less than sector.",
				 dev_name(dev), attribute, value);
			result = 1;
		}
	}

	return result;
}

static unsigned long _dev_topology_attribute_4k(struct dev_types *dt,
						const char *attribute,
						struct device *dev,
						unsigned long default_value)
{
	unsigned long result = _dev_topology_attribute(dt, attribute, dev, default_value);

	if ((result > 1) && (result & 0x3)) {
		log_warn("WARNING: Ignoring %s = %lu for device %s (not divisible by 4KiB).",
			 attribute, result << SECTOR_SHIFT, dev_name(dev));
		result = 8;
	}

	return result;
}

unsigned long dev_alignment_offset(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "alignment_offset", dev, 0UL);
}

unsigned long dev_minimum_io_size(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute_4k(dt, "queue/minimum_io_size", dev, 0UL);
}

unsigned long dev_optimal_io_size(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute_4k(dt, "queue/optimal_io_size", dev, 0UL);
}

unsigned long dev_discard_max_bytes(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "queue/discard_max_bytes", dev, 0UL);
}

unsigned long dev_discard_granularity(struct dev_types *dt, struct device *dev)
{
	return _dev_topology_attribute(dt, "queue/discard_granularity", dev, 0UL);
}

int dev_is_rotational(struct dev_types *dt, struct device *dev)
{
	unsigned long value;
	return _dev_sysfs_block_attribute(dt, "queue/rotational", dev, &value) ? (int) value : 1;
}

/* dev is pmem if /sys/dev/block/<major>:<minor>/queue/dax is 1 */
int dev_is_pmem(struct dev_types *dt, struct device *dev)
{
	unsigned long value;
	return _dev_sysfs_block_attribute(dt, "queue/dax", dev, &value) ? (int) value : 0;
}

#else

int dev_get_primary_dev(struct dev_types *dt, struct device *dev, dev_t *result)
{
	return 0;
}

unsigned long dev_alignment_offset(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_minimum_io_size(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_optimal_io_size(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_discard_max_bytes(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

unsigned long dev_discard_granularity(struct dev_types *dt, struct device *dev)
{
	return 0UL;
}

int dev_is_rotational(struct dev_types *dt, struct device *dev)
{
	return 1;
}

int dev_is_pmem(struct dev_types *dt, struct device *dev)
{
	return 0;
}
#endif

