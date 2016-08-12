/* Do not edit. This file is generated by scripts/create-commands */
/* using command definitions from scripts/command-lines.in */
#define COMMAND_COUNT 153
enum {
	no_CMD,
	lvchange_properties_CMD,
	lvchange_resync_CMD,
	lvchange_syncaction_CMD,
	lvchange_rebuild_CMD,
	lvchange_activate_CMD,
	lvchange_refresh_CMD,
	lvchange_monitor_CMD,
	lvchange_poll_CMD,
	lvchange_persistent_CMD,
	lvconvert_merge_CMD,
	lvconvert_combine_split_snapshot_CMD,
	lvconvert_to_thin_with_external_CMD,
	lvconvert_to_cache_vol_CMD,
	lvconvert_to_thinpool_CMD,
	lvconvert_to_cachepool_CMD,
	lvconvert_to_mirror_CMD,
	lvconvert_raid1_to_mirror_CMD,
	lvconvert_general_to_raid_CMD,
	lvconvert_to_mirrored_or_change_image_count_CMD,
	lvconvert_raid_to_striped_CMD,
	lvconvert_raid_or_mirror_to_linear_CMD,
	lvconvert_split_mirror_images_to_new_CMD,
	lvconvert_split_mirror_images_and_track_CMD,
	lvconvert_repair_pvs_or_thinpool_CMD,
	lvconvert_replace_pv_CMD,
	lvconvert_change_mirrorlog_CMD,
	lvconvert_split_and_keep_cachepool_CMD,
	lvconvert_split_and_delete_cachepool_CMD,
	lvconvert_split_cow_snapshot_CMD,
	lvconvert_poll_mirror_CMD,
	lvconvert_swap_pool_metadata_CMD,
	lvcreate_error_vol_CMD,
	lvcreate_zero_vol_CMD,
	lvcreate_linear_CMD,
	lvcreate_striped_CMD,
	lvcreate_mirror_CMD,
	lvcreate_raid_any_CMD,
	lvcreate_cow_snapshot_CMD,
	lvcreate_striped_cow_snapshot_CMD,
	lvcreate_cow_snapshot_with_virtual_origin_CMD,
	lvcreate_thinpool_CMD,
	lvcreate_cachepool_CMD,
	lvcreate_thin_vol_CMD,
	lvcreate_thin_snapshot_CMD,
	lvcreate_thin_snapshot_of_external_CMD,
	lvcreate_thin_vol_with_thinpool_CMD,
	lvcreate_thin_vol_with_thinpool_or_sparse_snapshot_CMD,
	lvcreate_cache_vol_with_new_origin_CMD,
	lvcreate_cache_vol_with_new_origin_or_convert_to_cache_vol_with_cachepool_CMD,
	lvdisplay_general_CMD,
	lvextend_by_size_CMD,
	lvextend_by_pv_CMD,
	lvextend_pool_metadata_by_size_CMD,
	lvextend_by_policy_CMD,
	lvmconfig_general_CMD,
	lvreduce_general_CMD,
	lvremove_general_CMD,
	lvrename_vg_lv_lv_CMD,
	lvrename_lv_lv_CMD,
	lvresize_by_size_CMD,
	lvresize_by_pv_CMD,
	lvresize_pool_metadata_by_size_CMD,
	lvs_general_CMD,
	lvscan_general_CMD,
	pvchange_properties_all_CMD,
	pvchange_properties_some_CMD,
	pvresize_general_CMD,
	pvck_general_CMD,
	pvcreate_general_CMD,
	pvdisplay_general_CMD,
	pvmove_one_CMD,
	pvmove_any_CMD,
	pvremove_general_CMD,
	pvs_general_CMD,
	pvscan_show_CMD,
	pvscan_cache_CMD,
	vgcfgbackup_general_CMD,
	vgcfgrestore_by_vg_CMD,
	vgcfgrestore_by_file_CMD,
	vgchange_properties_CMD,
	vgchange_monitor_CMD,
	vgchange_poll_CMD,
	vgchange_activate_CMD,
	vgchange_refresh_CMD,
	vgchange_lockstart_CMD,
	vgchange_lockstop_CMD,
	vgck_general_CMD,
	vgconvert_general_CMD,
	vgcreate_general_CMD,
	vgdisplay_general_CMD,
	vgexport_some_CMD,
	vgexport_all_CMD,
	vgextend_general_CMD,
	vgimport_some_CMD,
	vgimport_all_CMD,
	vgimportclone_general_CMD,
	vgmerge_general_CMD,
	vgmknodes_general_CMD,
	vgreduce_by_pv_CMD,
	vgreduce_all_CMD,
	vgreduce_missing_CMD,
	vgremove_general_CMD,
	vgrename_by_name_CMD,
	vgrename_by_uuid_CMD,
	vgs_general_CMD,
	vgscan_general_CMD,
	vgsplit_by_pv_CMD,
	vgsplit_by_lv_CMD,
	devtypes_general_CMD,
	fullreport_general_CMD,
	lastlog_general_CMD,
	lvpoll_general_CMD,
	formats_general_CMD,
	help_general_CMD,
	version_general_CMD,
	pvdata_general_CMD,
	segtypes_general_CMD,
	systemid_general_CMD,
	tags_general_CMD,
	lvmchange_general_CMD,
	lvmdiskscan_general_CMD,
	lvmsadc_general_CMD,
	lvmsar_general_CMD,
	COMMAND_ID_COUNT,
};
