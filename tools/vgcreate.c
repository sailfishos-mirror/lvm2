/*
 * Copyright (C) 2001 Sistina Software
 *
 * vgcreate is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * vgcreate is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

/* FIXME From config file? */
#define DEFAULT_PV 255
#define DEFAULT_LV 255

#define DEFAULT_EXTENT 4096	/* In KB */

int vgcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int max_lv, max_pv;
	uint32_t extent_size;
	char *vg_name;
	char vg_path[PATH_MAX];
	struct volume_group *vg;

	if (!argc) {
		log_error("Please provide volume group name and "
			  "physical volumes");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume name(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = argv[0];
	max_lv = arg_int_value(cmd, maxlogicalvolumes_ARG, DEFAULT_LV);
	max_pv = arg_int_value(cmd, maxphysicalvolumes_ARG, DEFAULT_PV);

	/* Units of 512-byte sectors */
	extent_size =
	    arg_int_value(cmd, physicalextentsize_ARG, DEFAULT_EXTENT) * 2;

	if (max_lv < 1) {
		log_error("maxlogicalvolumes too low");
		return EINVALID_CMD_LINE;
	}

	if (max_pv < 1) {
		log_error("maxphysicalvolumes too low");
		return EINVALID_CMD_LINE;
	}

	/* Strip dev_dir if present */
	if (!strncmp(vg_name, cmd->dev_dir, strlen(cmd->dev_dir)))
		vg_name += strlen(cmd->dev_dir);

	snprintf(vg_path, PATH_MAX, "%s%s", cmd->dev_dir, vg_name);
	if (path_exists(vg_path)) {
		log_error("%s: already exists in filesystem", vg_path);
		return ECMD_FAILED;
	}

	if (!is_valid_chars(vg_name)) {
		log_error("New volume group name \"%s\" has invalid characters",
			  vg_name);
		return ECMD_FAILED;
	}

	/* Create the new VG */
	if (!(vg = vg_create(cmd->fid, vg_name, extent_size, max_pv, max_lv,
			     argc - 1, argv + 1)))
		return ECMD_FAILED;

	if (max_lv != vg->max_lv)
		log_error("Warning: Setting maxlogicalvolumes to %d",
			  vg->max_lv);

	if (max_pv != vg->max_pv)
		log_error("Warning: Setting maxphysicalvolumes to %d",
			  vg->max_pv);

	if (!lock_vol(cmd, "", LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return ECMD_FAILED;
	}

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE | LCK_NONBLOCK)) {
		log_error("Can't get lock for %s", vg_name);
		lock_vol(cmd, "", LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
		lock_vol(cmd, "", LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	/* Store VG on disk(s) */
	if (!cmd->fid->ops->vg_write(cmd->fid, vg)) {
		lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
		lock_vol(cmd, "", LCK_VG_UNLOCK);
		return ECMD_FAILED;
	}

	lock_vol(cmd, vg_name, LCK_VG_UNLOCK);
	lock_vol(cmd, "", LCK_VG_UNLOCK);

	backup(vg);

	log_print("Volume group \"%s\" successfully created", vg->name);

	return 0;
}
