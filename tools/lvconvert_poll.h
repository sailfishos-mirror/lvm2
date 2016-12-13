/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
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

#ifndef _LVM_LVCONVERT_H
#define _LVM_LVCONVERT_H

#include "polldaemon.h"

struct cmd_context;
struct logical_volume;
struct volume_group;

 /*
  * Data/results accumulated during processing.
  */
struct lvconvert_result {
	int need_polling;
	struct dm_list poll_idls;
};

struct convert_poll_id_list {
	struct dm_list list;
	struct poll_operation_id *id;
	unsigned is_merging_origin:1;
	unsigned is_merging_origin_thin:1;
};

int lvconvert_poll_by_id(struct cmd_context *cmd, struct poll_operation_id *id,
		unsigned background, int is_merging_origin, int is_merging_origin_thin);
struct convert_poll_id_list *convert_poll_id_list_create(struct cmd_context *cmd,
		const struct logical_volume *lv);

int lvconvert_mirror_finish(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct logical_volume *lv,
			    struct dm_list *lvs_changed __attribute__((unused)));

int swap_lv_identifiers(struct cmd_context *cmd,
			struct logical_volume *a, struct logical_volume *b);

int thin_merge_finish(struct cmd_context *cmd,
		      struct logical_volume *merge_lv,
		      struct logical_volume *lv);

int lvconvert_merge_finish(struct cmd_context *cmd,
			   struct volume_group *vg,
			   struct logical_volume *lv,
			   struct dm_list *lvs_changed __attribute__((unused)));

progress_t poll_merge_progress(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       const char *name __attribute__((unused)),
			       struct daemon_parms *parms);

progress_t poll_thin_merge_progress(struct cmd_context *cmd,
				    struct logical_volume *lv,
				    const char *name __attribute__((unused)),
				    struct daemon_parms *parms);

#endif  /* _LVM_LVCONVERT_H */
