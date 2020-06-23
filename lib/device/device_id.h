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
int device_id_pvcreate(struct cmd_context *cmd, struct device *dev, const char *pvid,
                       const char *idtype_arg, const char *id_arg);
void device_id_pvremove(struct cmd_context *cmd, struct device *dev);
int device_id_match(struct cmd_context *cmd, struct use_id *uid, struct device *dev);
void device_ids_match(struct cmd_context *cmd);
void device_ids_validate(struct cmd_context *cmd);

int devices_file_valid(struct cmd_context *cmd);

#endif
