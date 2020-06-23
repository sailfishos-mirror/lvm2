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

	if (!lock_global(cmd, "ex"))
		return ECMD_FAILED;

	clear_hint_file(cmd);

	/* FIXME: require a valid devices_file?
	   Does this command have any useful job without a devices_file? */

	cmd->enable_device_ids = 1;

	device_ids_read(cmd);
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
		device_ids_write(cmd);
		goto out;
	}

	if (arg_is_set(cmd, adddev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, adddev_ARG, NULL)))
			return ECMD_FAILED;

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
			return ECMD_FAILED;
		}

		/*
		 * reads pvid from dev header, sets dev->pvid.
		 * (the device may not be a PV and have no PVID)
		 */
		label_scan_setup_bcache();
		device_id_read_pvid(cmd, dev);

		/*
		 * Allow filtered devices to be added to devices_file, but
		 * check if it's excluded by filters to print a warning.
		 * Since device_id_read_pvid has read the first 4K of the device,
		 * the filters should not for the most part need to do any further
		 * reading of the device.
		 *
		 * (This is the first time filters are being run, so we do
		 * not need to wipe filters of any previous result that was
		 * based on skip_filter_deviceid=0.)
		 */
		cmd->skip_filter_deviceid = 1;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* FIXME: print which filters */
			log_warn("WARNING: %s is currently excluded by filters.", dev_name(dev));
		}

		device_id_add(cmd, dev, dev->pvid,
			      arg_str_value(cmd, deviceidtype_ARG, NULL),
			      arg_str_value(cmd, deviceid_ARG, NULL));
		device_ids_write(cmd);
		goto out;
	}

	if (arg_is_set(cmd, addpvid_ARG)) {
		struct dev_iter *iter;
		const char *pvid;
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

		pvid = arg_str_value(cmd, addpvid_ARG, NULL);

		if ((uid = get_uid_for_pvid(cmd, pvid))) {
			log_error("PVID already exists in devices_file for %s.", dev_name(uid->dev));
			return ECMD_FAILED;
		}

		/*
		 * Create a list of all devices on the system, without
		 * applying any filters, since we do not want filters
		 * to read any of the devices yet.
		 */
		if (!(iter = dev_iter_create(NULL, 0)))
			return ECMD_FAILED;
		while ((dev = dev_iter_get(cmd, iter))) {
			if (!(devl = zalloc(sizeof(*devl))))
				continue;
			devl->dev = dev;
			dm_list_add(&devs, &devl->list);
                }
		dev_iter_destroy(iter);

		/*
		 * Apply the filters that do not require reading the
		 * devices (bcache is not set up yet, so any filter
		 * that requires reading the device will return EAGAIN.)
		 * The regex filter will be used (and filter-deviceid not
		 * used) because we set enable_device_ids = 0.
		 */
		log_debug("Filtering devices without data");
		cmd->enable_device_ids = 0;
		dm_list_iterate_items_safe(devl, safe, &devs) {
			if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL))
				dm_list_del(&devl->list);
		}
		cmd->enable_device_ids = 1;

		label_scan_setup_bcache();
		dev = NULL;
		dm_list_iterate_items(devl, &devs) {
			device_id_read_pvid(cmd, devl->dev);
			if (!strcmp(devl->dev->pvid, pvid)) {
				dev = devl->dev;
				break;
			}
		}
		cmd->enable_device_ids = 1;

		if (!dev) {
			log_print("PVID %s not found on any devices.", pvid);
			goto out;
		}

		/*
		 * Now that the device has been read, apply the filters again
		 * which will now include filters that read data from the device.
		 * N.B. we've already skipped devs that were excluded by the
		 * no-data filters, so if the PVID exists on one of those devices
		 * no warning is printed.
		 */
		log_debug("Filtering device with data");
		cmd->enable_device_ids = 0;
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			/* FIXME: print which filters */
			log_error("PVID %s found on %s which is excluded by filters",
				 pvid, dev_name(dev));
			goto out;
		}
		cmd->enable_device_ids = 1;

		device_id_add(cmd, dev, dev->pvid, NULL, NULL);
		device_ids_write(cmd);
		goto out;
	}

	if (arg_is_set(cmd, deldev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, deldev_ARG, NULL)))
			return ECMD_FAILED;

		/* we don't need to skip_filter_deviceid since we're
		   removing a dev from devices_file, that dev should
		   be in the devices_file and pass the filter */
		if (!(dev = dev_cache_get(cmd, devname, cmd->filter))) {
			log_error("No device found for %s.", devname);
			return ECMD_FAILED;
		}

		if (!(uid = get_uid_for_dev(cmd, dev))) {
			log_error("Device not found in devices_file.");
			return ECMD_FAILED;
		}

		dm_list_del(&uid->list);
		free_uid(uid);
		device_ids_write(cmd);
		goto out;
	}

	if (arg_is_set(cmd, delpvid_ARG)) {
		const char *pvid;

		pvid = arg_str_value(cmd, delpvid_ARG, NULL);

		if (!(uid = get_uid_for_pvid(cmd, pvid))) {
			log_error("Device not found in devices_file.");
			return ECMD_FAILED;
		}

		dm_list_del(&uid->list);
		free_uid(uid);
		device_ids_write(cmd);
		goto out;
	}

	/* If no options, print use_device_ids list */

	dm_list_iterate_items(uid, &cmd->use_device_ids) {
		log_print("Device %s IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s",
			  uid->dev ? dev_name(uid->dev) : ".",
			  uid->idtype ? idtype_to_str(uid->idtype) : ".",
			  uid->idname ? uid->idname : ".",
			  uid->devname ? uid->devname : ".",
			  uid->pvid ? (char *)uid->pvid : ".");
	}

out:
	return ECMD_PROCESSED;
}

