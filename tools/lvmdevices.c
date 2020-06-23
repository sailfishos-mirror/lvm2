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

static void _search_devs_for_pvids(struct cmd_context *cmd, struct dm_list *search_pvids, struct dm_list *found_devs)
{
	struct dev_iter *iter;
	struct device *dev;
	struct device_list *devl, *devl2;
	struct device_id_list *dil, *dil2;
	struct dm_list devs;
	int found;

	dm_list_init(&devs);

	/*
	 * Create a list of all devices on the system, without applying
	 * any filters, since we do not want filters to read any of the
	 * devices yet.
	 */
	if (!(iter = dev_iter_create(NULL, 0)))
		return;
	while ((dev = dev_iter_get(cmd, iter))) {

		/* Skip devs with a valid match to a du. */
		if (get_du_for_dev(cmd, dev))
			continue;

		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&devs, &devl->list);
	}
	dev_iter_destroy(iter);

	/*
	 * Apply the filters that do not require reading the devices
	 * The regex filter will be used and filter-deviceid not used.
	 */
	log_debug("Filtering devices (no data) for pvid search");
	cmd->filter_nodata_only = 1;
	cmd->filter_deviceid_skip = 1;
	cmd->filter_regex_with_devices_file = 1;
	dm_list_iterate_items_safe(devl, devl2, &devs) {
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL))
			dm_list_del(&devl->list);
	}

	/*
	 * Read header from each dev to see if it has one of the pvids we're
	 * searching for.
	 */
	dm_list_iterate_items_safe(devl, devl2, &devs) {
		/* sets dev->pvid if an lvm label with pvid is found */
		if (!label_read_pvid(devl->dev))
			continue;

		found = 0;
		dm_list_iterate_items_safe(dil, dil2, search_pvids) {
			if (!strcmp(devl->dev->pvid, dil->pvid)) {
				dm_list_del(&devl->list);
				dm_list_del(&dil->list);
				dm_list_add(found_devs, &devl->list);
				log_print("Found PVID %s on %s.", dil->pvid, dev_name(devl->dev));
				found = 1;
				break;
			}
		}
		if (!found)
			label_scan_invalidate(devl->dev);

		/*
		 * FIXME: search all devs in case pvid is duplicated on multiple devs.
		 */
		if (dm_list_empty(search_pvids))
			break;
	}

	dm_list_iterate_items(dil, search_pvids)
		log_error("PVID %s not found on any devices.", dil->pvid);

	/*
	 * Now that the device has been read, apply the filters again
	 * which will now include filters that read data from the device.
	 * N.B. we've already skipped devs that were excluded by the
	 * no-data filters, so if the PVID exists on one of those devices
	 * no warning is printed.
	 */
	log_debug("Filtering devices (with data) for pvid search");
	cmd->filter_nodata_only = 0;
	cmd->filter_deviceid_skip = 1;
	cmd->filter_regex_with_devices_file = 1;
	dm_list_iterate_items_safe(devl, devl2, found_devs) {
		dev = devl->dev;
		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			log_warn("WARNING: PVID %s found on %s which is excluded by filter: %s",
			 	  dev->pvid, dev_name(dev), dev_filtered_reason(dev));
			dm_list_del(&devl->list);
		}
	}
}

int lvmdevices(struct cmd_context *cmd, int argc, char **argv)
{
	struct dm_list search_pvids;
	struct dm_list found_devs;
	struct device_id_list *dil;
	struct device_list *devl;
	struct device *dev;
	struct dev_use *du, *du2;
	int changes = 0;

	dm_list_init(&search_pvids);
	dm_list_init(&found_devs);

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

	if (arg_is_set(cmd, check_ARG) || arg_is_set(cmd, update_ARG)) {
		int search_count = 0;
		int invalid = 0;

		label_scan_setup_bcache();

		dm_list_iterate_items(du, &cmd->use_devices) {
			if (!du->dev)
				continue;
			dev = du->dev;

			if (!label_read_pvid(dev))
				continue;

			/*
			 * label_read_pvid has read the first 4K of the device
			 * so these filters should not for the most part need
			 * to do any further reading of the device.
			 */
			log_debug("Checking filters with data for %s", dev_name(dev));
			if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
				log_warn("WARNING: %s in devices file is excluded by filter: %s.",
					 dev_name(dev), dev_filtered_reason(dev));
			}
			label_scan_invalidate(dev);
		}

		device_ids_validate(cmd, &invalid, 1);

		device_ids_find_renamed_devs(cmd, &found_devs, &search_count, 1);

		if (search_count && !strcmp(cmd->search_for_devnames, "none"))
			log_print("Not searching for missing devnames, search_for_devnames=\"none\".");

		/*
		 * check du->part
		 * FIXME: shouldn't device_ids_validate() check this?
		 */
		dm_list_iterate_items(du, &cmd->use_devices) {
			int part = 0;
			if (!du->dev)
				continue;
			dev = du->dev;

			dev_get_partition_number(dev, &part);

			if (part != du->part) {
				log_warn("WARNING: device %s partition %u has incorrect PART in devices file (%u)",
					 dev_name(dev), part, du->part);
				du->part = part;
				changes++;
			}
		}

		if (arg_is_set(cmd, update_ARG)) {
			if (invalid || !dm_list_empty(&found_devs)) {
				if (!device_ids_write(cmd))
					goto_bad;
				log_print("Updated devices file to version %s", devices_file_version());
			} else {
				log_print("No update for devices file is needed.");
			}
		}
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
			log_warn("WARNING: %s is currently excluded by filter: %s.",
				 dev_name(dev), dev_filtered_reason(dev));
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
		struct id id;
		char pvid[ID_LEN+1] = { 0 };
		const char *pvid_arg;

		label_scan_setup_bcache();

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

		if ((du = get_du_for_pvid(cmd, pvid))) {
			log_error("PVID already exists in devices_file for %s.", dev_name(du->dev));
			goto bad;
		}

		if (!(dil = dm_pool_zalloc(cmd->mem, sizeof(*dil))))
			goto_bad;
		memcpy(dil->pvid, &pvid, ID_LEN);
		dm_list_add(&search_pvids, &dil->list);

		_search_devs_for_pvids(cmd, &search_pvids, &found_devs);

		if (dm_list_empty(&found_devs)) {
			log_error("PVID %s not found on any devices.", pvid);
			goto bad;
		}
		dm_list_iterate_items(devl, &found_devs) {
			if (!device_id_add(cmd, devl->dev, devl->dev->pvid, NULL, NULL))
				goto_bad;
		}
		if (!device_ids_write(cmd))
			goto_bad;
		goto out;
	}

	if (arg_is_set(cmd, deldev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, deldev_ARG, NULL)))
			goto_bad;

		/*
		 * No filter because we always want to allow removing a device
		 * by name from the devices file.
		 */
		if (!(dev = dev_cache_get(cmd, devname, NULL))) {
			log_error("No device found for %s.", devname);
			goto bad;
		}

		/*
		 * dev_cache_scan uses sysfs to check if an LV is using each dev
		 * and sets this flag is so.
		 */
		if (dev->flags & DEV_USED_FOR_LV) {
			if (!arg_count(cmd, yes_ARG) &&
			    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", devname) == 'n') {
				log_error("Device not removed.");
				goto bad;
			}
		}

		if (!(du = get_du_for_dev(cmd, dev))) {
			log_error("Device not found in devices_file.");
			goto bad;
		}

		dm_list_del(&du->list);
		free_du(du);
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

		if (!(du = get_du_for_pvid(cmd, pvid))) {
			log_error("PVID not found in devices_file.");
			goto_bad;
		}

		dm_list_del(&du->list);

		if ((du2 = get_du_for_pvid(cmd, pvid))) {
			log_error("Multiple devices file entries for PVID %s (%s %s), remove by device name.",
				  pvid, du->devname, du2->devname);
			goto_bad;
		}

		if (du->devname && (du->devname[0] != '.')) {
			if ((dev = dev_cache_get(cmd, du->devname, NULL)) &&
			    (dev->flags & DEV_USED_FOR_LV)) {
				if (!arg_count(cmd, yes_ARG) &&
			    	    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", du->devname) == 'n') {
					log_error("Device not removed.");
					goto bad;
				}
			}
		}

		free_du(du);
		device_ids_write(cmd);
		goto out;
	}

	/* If no options, print use_devices list */

	dm_list_iterate_items(du, &cmd->use_devices) {
		char part_buf[64] = { 0 };

		if (du->part)
			snprintf(part_buf, 63, " PART=%d", du->part);

		log_print("Device %s IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s",
			  du->dev ? dev_name(du->dev) : ".",
			  du->idtype ? idtype_to_str(du->idtype) : ".",
			  du->idname ? du->idname : ".",
			  du->devname ? du->devname : ".",
			  du->pvid ? (char *)du->pvid : ".",
			  part_buf);
	}

out:
	return ECMD_PROCESSED;

bad:
	return ECMD_FAILED;
}

