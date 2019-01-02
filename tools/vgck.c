/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

static int vgck_single(struct cmd_context *cmd __attribute__((unused)),
		       const char *vg_name,
		       struct volume_group *vg,
		       struct processing_handle *handle __attribute__((unused)))
{
	if (!vg_check_status(vg, EXPORTED_VG))
		return_ECMD_FAILED;

	if (!vg_validate(vg))
		return_ECMD_FAILED;

	if (vg_missing_pv_count(vg)) {
		log_error("The volume group is missing %d physical volumes.",
			  vg_missing_pv_count(vg));
		return ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

/*
 * vgck --repairmetadata [--sourcedevice PV_src] VG [PV_dst ...]
 *
 * PV_src: if specified, the metadata from this PV is written to
 * the PVs needing repair.  If not specified, a copy of the metadata
 * with the largest seqno is used.
 *
 * PV_dst: if specified, new metadata is written to these PVs.
 */

static int _repair_metadata(struct cmd_context *cmd, int argc, char **argv)
{
	const char *dev_src_name = NULL;
	const char *file_src_name = NULL;
	const char *vgname;
	struct device_list *devl;
	struct dm_list dev_dst_list;
	int ret = 1;
	int i;

	dm_list_init(&dev_dst_list);

	vgname = cmd->position_argv[0];
	/* TODO: verify valid vgname */

	if (!lock_vol(cmd, vgname, LCK_VG_WRITE, NULL))
		return ECMD_FAILED;

	lvmcache_label_scan(cmd);

	/*
	 * specific PVs to repair
	 */
	for (i = 1; i < cmd->position_argc; i++) {
		if (!(devl = malloc(sizeof(*devl))))
			return ECMD_FAILED;
		if (!(devl->dev = dev_cache_get(cmd->position_argv[i], NULL))) {
			log_error("Device not found to repair: %s", cmd->position_argv[i]);
			return ECMD_FAILED;
		}
		dm_list_add(&dev_dst_list, &devl->list);
	}

	/*
	 * specific PV or file to use as source of metadata to use for repair
	 */
	if (arg_is_set(cmd, sourcedevice_ARG)) {
		if (!(dev_src_name = arg_str_value(cmd, sourcedevice_ARG, NULL)))
			return ECMD_FAILED;

	} else if (arg_is_set(cmd, file_ARG)) {
		if (!(file_src_name = arg_str_value(cmd, file_ARG, NULL)))
			return ECMD_FAILED;
	}

	ret = vg_repair_metadata(cmd, vgname, dev_src_name, file_src_name, &dev_dst_list);

	unlock_vg(cmd, NULL, vgname);

	if (!ret)
		return ECMD_FAILED;
	return ECMD_PROCESSED;
}

static int _dump_metadata(struct cmd_context *cmd, int argc, char **argv)
{
	const char *dev_src_name = NULL;
	const char *vgname;
	const char *tofile = NULL;
	int ret = 1;

	vgname = cmd->position_argv[0];
	/* TODO: verify valid vgname */

	if (!lock_vol(cmd, vgname, LCK_VG_READ, NULL))
		return ECMD_FAILED;

	lvmcache_label_scan(cmd);

	if (arg_is_set(cmd, sourcedevice_ARG)) {
		if (!(dev_src_name = arg_str_value(cmd, sourcedevice_ARG, NULL)))
			return ECMD_FAILED;
	}

	if (arg_is_set(cmd, file_ARG)) {
		if (!(tofile = arg_str_value(cmd, file_ARG, NULL)))
			return ECMD_FAILED;
	}

	ret = vg_dump_metadata(cmd, vgname, dev_src_name, arg_is_set(cmd, force_ARG), tofile);

	unlock_vg(cmd, NULL, vgname);

	if (!ret)
		return ECMD_FAILED;
	return ECMD_PROCESSED;
}

int vgck(struct cmd_context *cmd, int argc, char **argv)
{
	lvmetad_make_unused(cmd);

	if (arg_is_set(cmd, repairmetadata_ARG))
		return _repair_metadata(cmd, argc, argv);

	if (arg_is_set(cmd, dumpmetadata_ARG))
		return _dump_metadata(cmd, argc, argv);

	return process_each_vg(cmd, argc, argv, NULL, NULL, 0, 0, NULL,
			       &vgck_single);
}
