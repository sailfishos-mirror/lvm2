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

struct vgimportdevices_params {
	uint32_t added_devices;
};

static int _vgimportdevices_single(struct cmd_context *cmd,
				   const char *vg_name,
				   struct volume_group *vg,
				   struct processing_handle *handle)
{
	struct vgimportdevices_params *vp = (struct vgimportdevices_params *) handle->custom_handle;
	struct pv_list *pvl;
	struct physical_volume *pv;
	int update_vg = 1;
	int updated_pvs = 0;
	const char *idtypestr;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (is_missing_pv(pvl->pv) || !pvl->pv->dev) {
			log_error("Not importing devices for VG %s with missing PV %32s.",
				 vg->name, (const char *)&pvl->pv->id.uuid);
			goto bad;
		}
	}

	/*
	 * We want to allow importing devices of foreign and shared 
	 * VGs, but we do not want to update device_ids in those VGs.
	 *
	 * If --foreign is set, then foreign VGs will be passed
	 * to this function; add devices but don't update vg.
	 * shared VGs are passed to this function; add devices
	 * and do not update.
	 */
	if (vg_is_foreign(vg) || vg_is_shared(vg))
		update_vg = 0;

	/*
	 * TODO: let users import devices without updating VG device_ids.
	 * if --nodeviceidupdate; update_vg = 0;
	 */

	/*
	 * User can select the idtype to use when importing.
	 */
	idtypestr = arg_str_value(cmd, deviceidtype_ARG, NULL);

	dm_list_iterate_items(pvl, &vg->pvs) {
		pv = pvl->pv;

		if (!idtypestr && pv->device_id_type)
			idtypestr = pv->device_id_type;

		device_id_add(cmd, pv->dev, (const char *)&pvl->pv->id.uuid, idtypestr, NULL);
		vp->added_devices++;

		/* We could skip update if the device_id has not changed. */

		if (!update_vg)
			continue;

		updated_pvs++;
	}

	if (updated_pvs) {
		if (!vg_write(vg) || !vg_commit(vg))
			goto_bad;
		backup(vg);
	}

	return ECMD_PROCESSED;
bad:
	return ECMD_FAILED;
}

/*
 * This command always scans all devices on the system,
 * any pre-existing devices_file does not limit the scope.
 *
 * This command adds the VG's devices to whichever
 * devices_file is set in config or command line.
 * If devices_file doesn't exist, it's created.
 *
 * If devices_file is "" then this file will scan all devices
 * and show the devices that it would otherwise have added to
 * the devices_file.  The VG is not updated with device_ids.
 *
 * This command updates the VG metadata to add device_ids
 * (if the metadata is missing them), unless an option is
 * set to skip that, e.g. --nodeviceidupdate?
 *
 * If the VG found has a foreign system ID then an error
 * will be printed.  To import devices from a foreign VG:
 * vgimportdevices --foreign -a
 * vgimportdevices --foreign VG
 *
 * If there are duplicate VG names it will do nothing.
 *
 * If there are duplicate PVIDs related to VG it will do nothing,
 * the user would need to add the PVs they want with lvmdevices --add.
 *
 * vgimportdevices -a (no vg arg) will import all accesible VGs.
 */

int vgimportdevices(struct cmd_context *cmd, int argc, char **argv)
{
	struct vgimportdevices_params vp = { 0 };
	struct processing_handle *handle;
	int ret = ECMD_PROCESSED;

	if (arg_is_set(cmd, foreign_ARG))
		cmd->include_foreign_vgs = 1;

	cmd->include_shared_vgs = 1;

	/* So that we can warn about this. */
	cmd->handles_missing_pvs = 1;

	/* Print a notice if a regex filter is being applied?
	   Possibly offer an option to ignore a regex filter? */

	if (!lock_global(cmd, "ex"))
		return ECMD_FAILED;

	/*
	 * Prepare devices file preemptively because the error path for this
	 * case from process_each is not as clean.
	 */
	if (!setup_devices_file(cmd)) {
		log_error("Failed to set up devices file.");
		return ECMD_FAILED;
	}
	if (!cmd->enable_devices_file) {
		log_error("Devices file not enabled.");
		return ECMD_FAILED;
	}
	if (!devices_file_exists(cmd) && !devices_file_touch(cmd)) {
		log_error("Failed to create devices file.");
		return ECMD_FAILED;
	}

	/*
	 * The hint file is associated with the default/system devices file,
	 * so don't clear hints when using a different --devicesfile.
	 */
	if (!cmd->devicesfile)
		clear_hint_file(cmd);

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}
	handle->custom_handle = &vp;

	/*
	 * import is a case where we do not want to be limited by an existing
	 * devices file because we want to search outside the devices file for
	 * new devs to add to it, but we do want devices file entries on
	 * use_device_ids so we can update and write out that list.
	 *
	 * Ususally when devices file is enabled, we use filter-deviceid and
	 * skip filter-regex.  In this import case it's reversed, and we skip
	 * filter-deviceid and use filter-regex.
	 */
	cmd->filter_deviceid_skip = 1;
	cmd->filter_regex_with_devices_file = 1;
	cmd->create_edit_devices_file = 1;

	/*
	 * For each VG:
	 * device_id_add() each PV in the VG
	 * update device_ids in the VG (potentially)
	 */
	ret = process_each_vg(cmd, argc, argv, NULL, NULL, READ_FOR_UPDATE,
			      0, handle, _vgimportdevices_single);
	if (ret == ECMD_FAILED)
		goto out;

	if (!vp.added_devices) {
		log_print("No devices to add.");
		goto out;
	}

	if (!device_ids_write(cmd)) {
		log_print("Failed to update devices file.");
		ret = ECMD_FAILED;
		goto out;
	}

	log_print("Added %u devices to devices file.", vp.added_devices);
out:
	destroy_processing_handle(cmd, handle);
	return ret;
}

