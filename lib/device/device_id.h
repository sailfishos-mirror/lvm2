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

#ifndef _LVM_DEVICE_ID_H
#define _LVM_DEVICE_ID_H

void free_uid(struct use_id *uid);
void free_uids(struct dm_list *list);
void free_did(struct dev_id *did);
void free_dids(struct dm_list *list);
const char *idtype_to_str(uint16_t idtype);
uint16_t idtype_from_str(const char *str);
const char *dev_idtype(struct device *dev);
const char *dev_id(struct device *dev);
int device_ids_read(struct cmd_context *cmd);
int device_ids_write(struct cmd_context *cmd);
int device_id_add(struct cmd_context *cmd, struct device *dev, const char *pvid,
                  const char *idtype_arg, const char *id_arg);
void device_id_pvremove(struct cmd_context *cmd, struct device *dev);
void device_ids_match(struct cmd_context *cmd);
int device_ids_match_dev(struct cmd_context *cmd, struct device *dev);
void device_ids_validate(struct cmd_context *cmd, int force_update);
int device_ids_version_unchanged(struct cmd_context *cmd);
void device_ids_read_pvids(struct cmd_context *cmd);
void device_ids_find_renamed_devs(struct cmd_context *cmd, struct dm_list *dev_list);
const char *device_id_system_read(struct cmd_context *cmd, struct device *dev, uint16_t idtype);

struct use_id *get_uid_for_dev(struct cmd_context *cmd, struct device *dev);
struct use_id *get_uid_for_pvid(struct cmd_context *cmd, const char *pvid);

int devices_file_exists(struct cmd_context *cmd);
int devices_file_touch(struct cmd_context *cmd);
int lock_devices_file(struct cmd_context *cmd, int mode);
int lock_devices_file_try(struct cmd_context *cmd, int mode, int *held);
void unlock_devices_file(struct cmd_context *cmd);

void device_ids_init(struct cmd_context *cmd);
void device_ids_exit(struct cmd_context *cmd);

#endif
