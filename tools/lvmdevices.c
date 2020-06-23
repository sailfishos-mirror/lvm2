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

#include "tools.h"
#include "lib/cache/lvmcache.h"
#include "lib/filters/filter.h"
#include "lib/device/device_id.h"

int lvmdevices(struct cmd_context *cmd, int argc, char **argv)
{
	struct device *dev;
	struct use_id *uid;

	if (!setup_devices_file(cmd))
		return ECMD_FAILED;

	if (!cmd->enable_devices_file) {
		log_error("Devices file not enabled.");
		return ECMD_FAILED;
	}

	if (arg_is_set(cmd, update_ARG) ||
	    arg_is_set(cmd, adddev_ARG) || arg_is_set(cmd, deldev_ARG) ||
	    arg_is_set(cmd, addpvid_ARG) || arg_is_set(cmd, delpvid_ARG)) {
		if (!lock_devices_file(cmd, LOCK_EX)) {
			log_error("Failed to lock the devices file to create.");
			return ECMD_FAILED;
		}
		if (!devices_file_exists(cmd)) {
			if (!devices_file_touch(cmd)) {
				log_error("Failed to create the devices file.");
				return ECMD_FAILED;
			}
		}

		/*
		 * The hint file is associated with the default/system devices file,
		 * so don't clear hints when using a different --devicesfile.
		 */
		if (!cmd->devicesfile)
			clear_hint_file(cmd);
	} else {
		if (!lock_devices_file(cmd, LOCK_SH)) {
			log_error("Failed to lock the devices file.");
			return ECMD_FAILED;
		}
		if (!devices_file_exists(cmd)) {
			log_error("Devices file does not exist.");
			return ECMD_FAILED;
		}
	}

	if (!device_ids_read(cmd)) {
		log_error("Failed to read the devices file.");
		return ECMD_FAILED;
	}
	dev_cache_scan();
	device_ids_match(cmd);

	if (arg_is_set(cmd, check_ARG)) {
		/* For each cmd->use_device_ids:
		   reads pvid from header, sets dev->pvid and uid->pvid */
		label_scan_setup_bcache();
		device_ids_read_pvids(cmd);
		goto out;
	}

	if (arg_is_set(cmd, update_ARG)) {
		/* For each cmd->use_device_ids:
		   reads pvid from header, sets dev->pvid and uid->pvid */
		label_scan_setup_bcache();
		device_ids_read_pvids(cmd);

		/* Any uid fields found/set/fixed will be written. */
		if (!device_ids_write(cmd))
			goto_bad;
		goto out;
	}

	if (arg_is_set(cmd, adddev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, adddev_ARG, NULL)))
			goto_bad;

		/*
		 * addev will add a device to devices_file even if that device
		 * is excluded by filters.
		 */

		/*
		 * No filter applied here (only the non-data filters would
		 * be applied since we haven't read the device yet.
		 */
		if (!(dev = dev_cache_get(cmd, devname, NULL))) {
			log_error("No device found for %s.", devname);
			goto_bad;
		}

		/*
		 * reads pvid from dev header, sets dev->pvid.
		 * (it's ok if the device is not a PV and has no PVID)
		 */
		label_scan_setup_bcache();
		label_read_pvid(dev);

		/*
		 * Allow filtered devices to be added to devices_file, but
		 * check if it's excluded by filters to print a warning.
		 * Since label_read_pvid has read the first 4K of the device,
		 * the filters should not for the most part need to do any further
		 * reading of the device.
		 *
		 * (This is the first time filters are being run, so we do
		 * not need to wipe filters of any previous result that was
		 * based on filter_deviceid_skip=0.)
		 */
		cmd->filter_deviceid_skip = 1;
		cmd->filter_regex_with_devices_file = 1;

		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* FIXME: print which filters */
			log_warn("WARNING: %s is currently excluded by filters.", dev_name(dev));
		}

		if (!device_id_add(cmd, dev, dev->pvid,
			      arg_str_value(cmd, deviceidtype_ARG, NULL),
			      arg_str_value(cmd, deviceid_ARG, NULL)))
			goto_bad;
		if (!device_ids_write(cmd))
			goto_bad;
		goto out;
	}

	if (arg_is_set(cmd, addpvid_ARG)) {
		struct dev_iter *iter;
		struct id id;
		char pvid[ID_LEN+1] = { 0 };
		const char *pvid_arg;
		struct device_list *devl, *safe;
		struct dm_list devs;

		dm_list_init(&devs);

		/*
		 * Iterate through all devs on the system, reading the
		 * pvid of each to check if it has this pvid.
		 * Devices that are excluded by no-data filters will not
		 * be checked for the PVID.
		 * addpvid will not add a device to devices_file if it's
		 * excluded by filters.
		 */

		pvid_arg = arg_str_value(cmd, addpvid_ARG, NULL);
		if (!id_read_format_try(&id, pvid_arg)) {
			log_error("Invalid PVID.");
			goto bad;
		}
		memcpy(pvid, &id.uuid, ID_LEN);

		if ((uid = get_uid_for_pvid(cmd, pvid))) {
			log_error("PVID already exists in devices_file for %s.", dev_name(uid->dev));
			goto bad;
		}

		/*
		 * Create a list of all devices on the system, without applying
		 * any filters, since we do not want filters to read any of the
		 * devices yet.
		 */
		if (!(iter = dev_iter_create(NULL, 0)))
			goto_bad;
		while ((dev = dev_iter_get(cmd, iter))) {
			if (!(devl = zalloc(sizeof(*devl))))
				continue;
			devl->dev = dev;
			dm_list_add(&devs, &devl->list);
                }
		dev_iter_destroy(iter);

		/*
		 * Apply the filters that do not require reading the devices
		 * The regex filter will be used and filter-deviceid not used.
		 */
		log_debug("Filtering devices without data");
		cmd->filter_nodata_only = 1;
		cmd->filter_deviceid_skip = 1;
		cmd->filter_regex_with_devices_file = 1;
		dm_list_iterate_items_safe(devl, safe, &devs) {
			if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL))
				dm_list_del(&devl->list);
		}

		label_scan_setup_bcache();
		dev = NULL;
		dm_list_iterate_items(devl, &devs) {
			if (!label_read_pvid(devl->dev))
				continue;
			if (!strcmp(devl->dev->pvid, pvid)) {
				dev = devl->dev;
				break;
			}
		}

		if (!dev) {
			log_error("PVID %s not found on any devices.", pvid);
			goto bad;
		}

		/*
		 * Now that the device has been read, apply the filters again
		 * which will now include filters that read data from the device.
		 * N.B. we've already skipped devs that were excluded by the
		 * no-data filters, so if the PVID exists on one of those devices
		 * no warning is printed.
		 */
		log_debug("Filtering device with data");
		cmd->filter_nodata_only = 0;
		cmd->filter_deviceid_skip = 1;
		cmd->filter_regex_with_devices_file = 1;
		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* FIXME: print which filters */
			log_error("PVID %s found on %s which is excluded by filters",
				 pvid, dev_name(dev));
			goto_bad;
		}

		if (!device_id_add(cmd, dev, dev->pvid, NULL, NULL))
			goto_bad;
		if (!device_ids_write(cmd))
			goto_bad;
		goto out;
	}

	if (arg_is_set(cmd, deldev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, deldev_ARG, NULL)))
			goto_bad;

		/* we don't need to filter_deviceid_skip since we're
		   removing a dev from devices_file, that dev should
		   be in the devices_file and pass the filter */
		if (!(dev = dev_cache_get(cmd, devname, cmd->filter))) {
			log_error("No device found for %s.", devname);
			goto bad;
		}

		/* dev_cache_scan uses sysfs to check if an LV is using each dev
		   and sets this flag is so. */
		if (dev->flags & DEV_USED_FOR_LV) {
			if (!arg_count(cmd, yes_ARG) &&
			    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", devname) == 'n') {
				log_error("Device not removed.");
				goto bad;
			}
		}

		if (!(uid = get_uid_for_dev(cmd, dev))) {
			log_error("Device not found in devices_file.");
			goto bad;
		}

		dm_list_del(&uid->list);
		free_uid(uid);
		device_ids_write(cmd);
		goto out;
	}

	if (arg_is_set(cmd, delpvid_ARG)) {
		struct id id;
		char pvid[ID_LEN+1] = { 0 };
		const char *pvid_arg;

		pvid_arg = arg_str_value(cmd, delpvid_ARG, NULL);
		if (!id_read_format_try(&id, pvid_arg)) {
			log_error("Invalid PVID.");
			goto bad;
		}
		memcpy(pvid, &id.uuid, ID_LEN);

		if (!(uid = get_uid_for_pvid(cmd, pvid))) {
			log_error("PVID not found in devices_file.");
			goto_bad;
		}

		if (uid->devname && (uid->devname[0] != '.')) {
			if ((dev = dev_cache_get(cmd, uid->devname, NULL)) &&
			    (dev->flags & DEV_USED_FOR_LV)) {
				if (!arg_count(cmd, yes_ARG) &&
			    	    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", uid->devname) == 'n') {
					log_error("Device not removed.");
					goto bad;
				}
			}
		}

		dm_list_del(&uid->list);
		free_uid(uid);
		device_ids_write(cmd);
		goto out;
	}

	/* If no options, print use_device_ids list */

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		char part_buf[64] = { 0 };

		if (uid->part)
			snprintf(part_buf, 63, " PART=%d", uid->part);

		log_print("Device %s IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s",
			  uid->dev ? dev_name(uid->dev) : ".",
			  uid->idtype ? idtype_to_str(uid->idtype) : ".",
			  uid->idname ? uid->idname : ".",
			  uid->devname ? uid->devname : ".",
			  uid->pvid ? (char *)uid->pvid : ".",
			  part_buf);
	}

out:
	return ECMD_PROCESSED;

bad:
	return ECMD_FAILED;
}

