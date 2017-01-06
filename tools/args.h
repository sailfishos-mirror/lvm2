/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2016 Red Hat, Inc. All rights reserved.
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

/*
 * Put all long args that don't have a corresponding short option first.
 */
/* *INDENT-OFF* */
arg(ARG_UNUSED, '-', "", 0, 0, 0, NULL)  /* place holder for unused 0 value */

arg(abort_ARG, '\0', "abort", 0, 0, 0,
    "#pvmove\n"
    "Abort any pvmove operations in progress. If a pvmove was started\n"
    "with the --atomic option, then all LVs will remain on the source PV.\n"
    "Otherwise, segments that have been moved will remain on the\n"
    "destination PV, while unmoved segments will remain on the source PV.\n"
    "#lvpoll\n"
    "Stop processing a poll operation in lvmpolld.\n")

arg(activationmode_ARG, '\0', "activationmode", activationmode_VAL, 0, 0,
    "Determines if LV activation is allowed when PVs are missing,\n"
    "e.g. because of a device failure.\n"
    "\"complete\" only allows LVs with no missing PVs to be activated,\n"
    "and is the most restrictive mode.\n"
    "\"degraded\" allows RAID LVs with missing PVs to be activated.\n"
    "(This does not include the \"mirror\" type, see \"raid1\" instead.).\n"
    "\"partial\" allows any LV with missing PVs to be activated, and\n"
    "should only be used for recovery or repair.\n"
    "For default, see lvm.conf/activation_mode.\n")

arg(addtag_ARG, '\0', "addtag", tag_VAL, ARG_GROUPABLE, 0,
    "Adds a tag to a PV, VG or LV. This option can be repeated to add\n"
    "multiple tags at once. See lvm(8) for information about tags.\n")

arg(aligned_ARG, '\0', "aligned", 0, 0, 0,
    "Use with --separator to align the output columns\n")

arg(alloc_ARG, '\0', "alloc", alloc_VAL, 0, 0,
    "Determines the allocation policy when a command needs to allocate\n"
    "Physical Extents (PEs) from the VG. Each VG and LV has an allocation policy\n"
    "which can be changed with vgchange/lvchange, or overriden on the\n"
    "command line.\n"
    "\"normal\" applies common sense rules such as not placing parallel stripes\n"
    "on the same PV.\n"
    "\"inherit\" applies the VG policy to an LV.\n"
    "\"contiguous\" requires new PEs be placed adjacent to existing PEs.\n"
    "\"cling\" places new PEs on the same PV as existing PEs in the same\n"
    "stripe of the LV.\n"
    "If there are sufficient PEs for an allocation, but normal does not\n"
    "use them, \"anywhere\" will use them even if it reduces performance,\n"
    "e.g. by placing two stripes on the same PV.\n"
    "Positional PV args on the command line can also be used to limit\n"
    "which PVs the command will use for allocation.\n")

arg(atomic_ARG, '\0', "atomic", 0, 0, 0,
    "Makes a pvmove operation atomic, ensuring that all affected LVs are\n"
    "moved to the destination PV, or none are if the operation is aborted.\n")

arg(atversion_ARG, '\0', "atversion", string_VAL, 0, 0,
    "Specify an LVM version in x.y.z format where x is the major version,\n"
    "the y is the minor version and z is the patchlevel (e.g. 2.2.106).\n"
    "When configuration is displayed, the configuration settings recognized\n"
    "at this LVM version will be considered only. This can be used\n"
    "to display a configuration that a certain LVM version understands and\n"
    "which does not contain any newer settings for which LVM would\n"
    "issue a warning message when checking the configuration.\n")

arg(binary_ARG, '\0', "binary", 0, 0, 0,
    "Use binary values \"0\" or \"1\" instead of descriptive literal values\n"
    "for columns that have exactly two valid values to report (not counting\n"
    "the \"unknown\" value which denotes that the value could not be determined).\n")

arg(bootloaderareasize_ARG, '\0', "bootloaderareasize", sizemb_VAL, 0, 0,
    "Create a separate bootloader area of specified size besides PV's data\n"
    "area. The bootloader area is an area of reserved space on the PV from\n"
    "which LVM will not allocate any extents and it's kept untouched. This is\n"
    "primarily aimed for use with bootloaders to embed their own data or metadata.\n"
    "The start of the bootloader area is always aligned, see also --dataalignment\n"
    "and --dataalignmentoffset. The bootloader area size may eventually\n"
    "end up increased due to the alignment, but it's never less than the\n"
    "size that is requested. To see the bootloader area start and size of\n"
    "an existing PV use pvs -o +pv_ba_start,pv_ba_size.\n")

arg(cache_long_ARG, '\0', "cache", 0, 0, 0,
    "#pvscan\n"
    "Scan one or more devices and send the metadata to lvmetad.\n"
    "#vgscan\n"
    "Scan all devices and send the metadata to lvmetad.\n"
    "#lvscan\n"
    "Scan the devices used by an LV and send the metadata to lvmetad.\n")

arg(cachemode_ARG, '\0', "cachemode", cachemode_VAL, 0, 0,
    "Specifies when writes to a cache LV should be considered complete.\n"
    "\"writeback\": a write is considered complete as soon as it is\n"
    "stored in the cache pool.\n"
    "\"writethough\": a write is considered complete only when it has\n"
    "been stored in the cache pool and on the origin LV.\n"
    "While writethrough may be slower for writes, it is more\n"
    "resilient if something should happen to a device associated with the\n"
    "cache pool LV. With writethrough, all reads are served\n"
    "from the origin LV (all reads miss the cache) and all writes are\n"
    "forwarded to the origin LV; additionally, write hits cause cache\n"
    "block invalidates. See lvmcache(7) for more information.\n")

arg(cachepool_ARG, '\0', "cachepool", lv_VAL, 0, 0,
    "The name of a cache pool LV.\n")

arg(commandprofile_ARG, '\0', "commandprofile", string_VAL, 0, 0,
    "The command profile to use for command configuration.\n"
    "See lvm.conf(5) for more information about profiles.\n")

arg(config_ARG, '\0', "config", string_VAL, 0, 0,
    "Config settings for the command. These override lvm.conf settings.\n"
    "The String arg uses the same format as lvm.conf,\n"
    "or may use section/field syntax.\n"
    "See lvm.conf(5) for more information about config.\n")

arg(configreport_ARG, '\0', "configreport", configreport_VAL, ARG_GROUPABLE, 1,
    "See lvmreport(7).\n")

arg(configtype_ARG, '\0', "typeconfig", configtype_VAL, 0, 0,
    "See lvmreport(7).\n")

arg(corelog_ARG, '\0', "corelog", 0, 0, 0,
    "An alias for --mirrorlog core.\n")

arg(dataalignment_ARG, '\0', "dataalignment", sizekb_VAL, 0, 0,
    "Align the start of the data to a multiple of this number.\n"
    "Also specify an appropriate Physical Extent size when creating a VG.\n"
    "To see the location of the first Physical Extent of an existing PV,\n"
    "use pvs -o +pe_start. In addition, it may be shifted by an alignment offset.\n"
    "See lvm.conf/data_alignment_offset_detection and --dataalignmentoffset.\n")

arg(dataalignmentoffset_ARG, '\0', "dataalignmentoffset", sizekb_VAL, 0, 0,
    "Shift the start of the data area by this additional offset.\n")

arg(deltag_ARG, '\0', "deltag", tag_VAL, ARG_GROUPABLE, 0,
    "Deletes a tag from a PV, VG or LV. This option can be repeated to delete\n"
    "multiple tags at once. See lvm(8) for information about tags.\n")

arg(detachprofile_ARG, '\0', "detachprofile", 0, 0, 0,
    "Detaches a metadata profile from a VG or LV.\n"
    "See lvm.conf(5) for more information about profiles.\n")

arg(discards_ARG, '\0', "discards", discards_VAL, 0, 0,
    "Specifies how the device-mapper thin pool layer in the kernel should\n"
    "handle discards.\n"
    "\"ignore\": the thin pool will ignore discards.\n"
    "\"nopassdown\": the thin pool will process discards itself to\n"
    "allow reuse of unneeded extents in the thin pool.\n"
    "\"passdown\": the thin pool will process discards as with nopassdown\n"
    "and will also pass the discards to the underlying device.\n")

arg(driverloaded_ARG, '\0', "driverloaded", bool_VAL, 0, 0,
    "If set to no, the command will not attempt to use device-mapper.\n"
    "For testing and debugging.\n")

arg(errorwhenfull_ARG, '\0', "errorwhenfull", bool_VAL, 0, 0,
    "Specifies thin pool behavior when data space is exhausted.\n"
    "When yes, device-mapper will immediately return an error\n"
    "when a thin pool is full and an I/O request requires space.\n"
    "When no, device-mapper will queue these I/O requests for a\n"
    "period of time to allow the thin pool to be extended.\n"
    "Errors are returned if no space is available after the timeout.\n"
    "(Also see dm-thin-pool kernel module option no_space_timeout.)\n")

arg(force_long_ARG, '\0', "force", 0, ARG_COUNTABLE, 0, NULL)

arg(foreign_ARG, '\0', "foreign", 0, 0, 0,
    "Report foreign VGs that would otherwise be skipped.\n"
    "See lvmsystemid(7) for more information about foreign VGs.\n")

arg(handlemissingpvs_ARG, '\0', "handlemissingpvs", 0, 0, 0,
    "Allows a polling operation to continue when PVs are missing,\n"
    "e.g. for repairs due to faulty devices.\n")

arg(ignoreadvanced_ARG, '\0', "ignoreadvanced", 0, 0, 0,
    "Exclude advanced configuration settings from the output.\n")

arg(ignorelocal_ARG, '\0', "ignorelocal", 0, 0, 0,
    "Ignore local section.\n")

arg(ignorelockingfailure_ARG, '\0', "ignorelockingfailure", 0, 0, 0,
    "Allows a command to continue with read-only metadata\n"
    "operations after locking failures.\n")

arg(ignoremonitoring_ARG, '\0', "ignoremonitoring", 0, 0, 0,
    "Do not interact with dmeventd unless --monitor is specified.\n"
    "Do not use this if dmeventd is already monitoring a device.\n")

arg(ignoreskippedcluster_ARG, '\0', "ignoreskippedcluster", 0, 0, 0,
    "Use to avoid exiting with an non-zero status code if the command is run\n"
    "without clustered locking and clustered VGs are skipped.\n")

arg(ignoreunsupported_ARG, '\0', "ignoreunsupported", 0, 0, 0,
    "Exclude unsupported configuration settings from the output. These settings are\n"
    "either used for debugging and development purposes only or their support is not\n"
    "yet complete and they are not meant to be used in production. The \\fBcurrent\\fP\n"
    "and \\fBdiff\\fP types include unsupported settings in their output by default,\n"
    "all the other types ignore unsupported settings.\n")

arg(labelsector_ARG, '\0', "labelsector", number_VAL, 0, 0, NULL)
arg(lockopt_ARG, '\0', "lockopt", string_VAL, 0, 0, NULL)
arg(lockstart_ARG, '\0', "lockstart", 0, 0, 0, NULL)
arg(lockstop_ARG, '\0', "lockstop", 0, 0, 0, NULL)
arg(locktype_ARG, '\0', "locktype", locktype_VAL, 0, 0, NULL)
arg(logonly_ARG, '\0', "logonly", 0, 0, 0, NULL)
arg(maxrecoveryrate_ARG, '\0', "maxrecoveryrate", sizekb_VAL, 0, 0, NULL)
arg(merge_ARG, '\0', "merge", 0, 0, 0, NULL)
arg(mergemirrors_ARG, '\0', "mergemirrors", 0, 0, 0, NULL)
arg(mergesnapshot_ARG, '\0', "mergesnapshot", 0, 0, 0, NULL)
arg(mergethin_ARG, '\0', "mergethin", 0, 0, 0, NULL)
arg(mergedconfig_ARG, '\0', "mergedconfig", 0, 0, 0, NULL)
arg(metadatacopies_ARG, '\0', "metadatacopies", metadatacopies_VAL, 0, 0, NULL)
arg(metadataignore_ARG, '\0', "metadataignore", bool_VAL, 0, 0, NULL)

arg(metadataprofile_ARG, '\0', "metadataprofile", string_VAL, 0, 0,
    "The metadata profile to use for command configuration.\n"
    "See lvm.conf(5) for more information about profiles.\n")

arg(metadatasize_ARG, '\0', "metadatasize", sizemb_VAL, 0, 0, NULL)
arg(minor_ARG, '\0', "minor", number_VAL, ARG_GROUPABLE, 0, NULL)
arg(minrecoveryrate_ARG, '\0', "minrecoveryrate", sizekb_VAL, 0, 0, NULL)
arg(mirrorlog_ARG, '\0', "mirrorlog", mirrorlog_VAL, 0, 0, NULL)
arg(mirrorsonly_ARG, '\0', "mirrorsonly", 0, 0, 0, NULL)
arg(mknodes_ARG, '\0', "mknodes", 0, 0, 0, NULL)
arg(monitor_ARG, '\0', "monitor", bool_VAL, 0, 0, NULL)
arg(nameprefixes_ARG, '\0', "nameprefixes", 0, 0, 0, NULL)
arg(noheadings_ARG, '\0', "noheadings", 0, 0, 0, NULL)
arg(nohistory_ARG, '\0', "nohistory", 0, 0, 0, NULL)
arg(nolocking_ARG, '\0', "nolocking", 0, 0, 0, NULL)
arg(norestorefile_ARG, '\0', "norestorefile", 0, 0, 0, NULL)
arg(nosuffix_ARG, '\0', "nosuffix", 0, 0, 0, NULL)
arg(nosync_ARG, '\0', "nosync", 0, 0, 0, NULL)
arg(notifydbus_ARG, '\0', "notifydbus", 0, 0, 0, NULL)
arg(noudevsync_ARG, '\0', "noudevsync", 0, 0, 0, NULL)
arg(originname_ARG, '\0', "originname", lv_VAL, 0, 0, NULL)
arg(physicalvolumesize_ARG, '\0', "setphysicalvolumesize", sizemb_VAL, 0, 0, NULL)
arg(poll_ARG, '\0', "poll", bool_VAL, 0, 0, NULL)
arg(polloperation_ARG, '\0', "polloperation", polloperation_VAL, 0, 0, NULL)
arg(pooldatasize_ARG, '\0', "pooldatasize", sizemb_VAL, 0, 0, NULL)
arg(poolmetadata_ARG, '\0', "poolmetadata", lv_VAL, 0, 0, NULL)
arg(poolmetadatasize_ARG, '\0', "poolmetadatasize", sizemb_VAL, 0, 0, NULL)
arg(poolmetadataspare_ARG, '\0', "poolmetadataspare", bool_VAL, 0, 0, NULL)

arg(profile_ARG, '\0', "profile", string_VAL, 0, 0,
    "An alias for --commandprofile or --metadataprofile, depending\n"
    "on the command.\n")

arg(pvmetadatacopies_ARG, '\0', "pvmetadatacopies", pvmetadatacopies_VAL, 0, 0, NULL)
arg(raidrebuild_ARG, '\0', "raidrebuild", pv_VAL, ARG_GROUPABLE, 0, NULL)
arg(raidmaxrecoveryrate_ARG, '\0', "raidmaxrecoveryrate", sizekb_VAL, 0, 0, NULL)
arg(raidminrecoveryrate_ARG, '\0', "raidminrecoveryrate", sizekb_VAL, 0, 0, NULL)
arg(raidsyncaction_ARG, '\0', "raidsyncaction", syncaction_VAL, 0, 0, NULL)
arg(raidwritebehind_ARG, '\0', "raidwritebehind", number_VAL, 0, 0, NULL)
arg(raidwritemostly_ARG, '\0', "raidwritemostly", writemostly_VAL, ARG_GROUPABLE, 0, NULL)
arg(readonly_ARG, '\0', "readonly", 0, 0, 0, NULL)
arg(refresh_ARG, '\0', "refresh", 0, 0, 0, NULL)
arg(removemissing_ARG, '\0', "removemissing", 0, 0, 0, NULL)
arg(rebuild_ARG, '\0', "rebuild", pv_VAL, ARG_GROUPABLE, 0, NULL)
arg(repair_ARG, '\0', "repair", 0, 0, 0, NULL)
arg(replace_ARG, '\0', "replace", pv_VAL, ARG_GROUPABLE, 0, NULL)
arg(reportformat_ARG, '\0', "reportformat", reportformat_VAL, 0, 0, NULL)
arg(restorefile_ARG, '\0', "restorefile", string_VAL, 0, 0, NULL)
arg(restoremissing_ARG, '\0', "restoremissing", 0, 0, 0, NULL)
arg(resync_ARG, '\0', "resync", 0, 0, 0, NULL)
arg(rows_ARG, '\0', "rows", 0, 0, 0, NULL)
arg(segments_ARG, '\0', "segments", 0, 0, 0, NULL)
arg(separator_ARG, '\0', "separator", string_VAL, 0, 0, NULL)
arg(shared_ARG, '\0', "shared", 0, 0, 0, NULL)
arg(sinceversion_ARG, '\0', "sinceversion", string_VAL, 0, 0, NULL)
arg(split_ARG, '\0', "split", 0, 0, 0, NULL)
arg(splitcache_ARG, '\0', "splitcache", 0, 0, 0, NULL)
arg(splitmirrors_ARG, '\0', "splitmirrors", number_VAL, 0, 0, NULL)
arg(splitsnapshot_ARG, '\0', "splitsnapshot", 0, 0, 0, NULL)
arg(showdeprecated_ARG, '\0', "showdeprecated", 0, 0, 0, NULL)
arg(showunsupported_ARG, '\0', "showunsupported", 0, 0, 0, NULL)
arg(startpoll_ARG, '\0', "startpoll", 0, 0, 0, NULL)
arg(stripes_long_ARG, '\0', "stripes", number_VAL, 0, 0, NULL)
arg(swapmetadata_ARG, '\0', "swapmetadata", 0, 0, 0, NULL)
arg(syncaction_ARG, '\0', "syncaction", syncaction_VAL, 0, 0, NULL)
arg(sysinit_ARG, '\0', "sysinit", 0, 0, 0, NULL)
arg(systemid_ARG, '\0', "systemid", string_VAL, 0, 0, NULL)
arg(thinpool_ARG, '\0', "thinpool", lv_VAL, 0, 0, NULL)
arg(trackchanges_ARG, '\0', "trackchanges", 0, 0, 0, NULL)
arg(trustcache_ARG, '\0', "trustcache", 0, 0, 0, NULL)
arg(type_ARG, '\0', "type", segtype_VAL, 0, 0, NULL)
arg(unbuffered_ARG, '\0', "unbuffered", 0, 0, 0, NULL)
arg(uncache_ARG, '\0', "uncache", 0, 0, 0, NULL)
arg(cachepolicy_ARG, '\0', "cachepolicy", string_VAL, 0, 0, NULL)
arg(cachesettings_ARG, '\0', "cachesettings", string_VAL, ARG_GROUPABLE, 0, NULL)
arg(unconfigured_ARG, '\0', "unconfigured", 0, 0, 0, NULL)
arg(units_ARG, '\0', "units", units_VAL, 0, 0, NULL)
arg(unquoted_ARG, '\0', "unquoted", 0, 0, 0, NULL)
arg(usepolicies_ARG, '\0', "usepolicies", 0, 0, 0, NULL)
arg(validate_ARG, '\0', "validate", 0, 0, 0, NULL)
arg(version_ARG, '\0', "version", 0, 0, 0, NULL)
arg(vgmetadatacopies_ARG, '\0', "vgmetadatacopies", vgmetadatacopies_VAL, 0, 0, NULL)
arg(virtualoriginsize_ARG, '\0', "virtualoriginsize", sizemb_VAL, 0, 0, NULL)
arg(withsummary_ARG, '\0', "withsummary", 0, 0, 0, NULL)
arg(withcomments_ARG, '\0', "withcomments", 0, 0, 0, NULL)
arg(withspaces_ARG, '\0', "withspaces", 0, 0, 0, NULL)
arg(withversions_ARG, '\0', "withversions", 0, 0, 0, NULL)
arg(writebehind_ARG, '\0', "writebehind", number_VAL, 0, 0, NULL)
arg(writemostly_ARG, '\0', "writemostly", writemostly_VAL, ARG_GROUPABLE, 0, NULL)

/* Allow some variations */
arg(allocation_ARG, '\0', "allocation", bool_VAL, 0, 0, NULL)
arg(available_ARG, '\0', "available", activation_VAL, 0, 0, NULL)
arg(resizable_ARG, '\0', "resizable", bool_VAL, 0, 0, NULL)

/*
 * ... and now the short args.
 */
arg(activate_ARG, 'a', "activate", activation_VAL, 0, 0, NULL)
arg(all_ARG, 'a', "all", 0, 0, 0, NULL)
arg(autobackup_ARG, 'A', "autobackup", bool_VAL, 0, 0, NULL)
arg(activevolumegroups_ARG, 'A', "activevolumegroups", 0, 0, 0, NULL)
arg(background_ARG, 'b', "background", 0, 0, 0, NULL)
arg(backgroundfork_ARG, 'b', "background", 0, 0, 0, NULL)
arg(basevgname_ARG, 'n', "basevgname", string_VAL, 0, 0, NULL)
arg(blockdevice_ARG, 'b', "blockdevice", 0, 0, 0, NULL)
arg(chunksize_ARG, 'c', "chunksize", sizekb_VAL, 0, 0, NULL)
arg(clustered_ARG, 'c', "clustered", bool_VAL, 0, 0, NULL)
arg(colon_ARG, 'c', "colon", 0, 0, 0, NULL)
arg(columns_ARG, 'C', "columns", 0, 0, 0, NULL)
arg(contiguous_ARG, 'C', "contiguous", bool_VAL, 0, 0, NULL)
arg(debug_ARG, 'd', "debug", 0, ARG_COUNTABLE, 0, NULL)
arg(exported_ARG, 'e', "exported", 0, 0, 0, NULL)
arg(physicalextent_ARG, 'E', "physicalextent", 0, 0, 0, NULL)
arg(file_ARG, 'f', "file", string_VAL, 0, 0, NULL)
arg(force_ARG, 'f', "force", 0, ARG_COUNTABLE, 0, NULL)
arg(full_ARG, 'f', "full", 0, 0, 0, NULL)
arg(help_ARG, 'h', "help", 0, ARG_COUNTABLE, 0, NULL)
arg(cache_ARG, 'H', "cache", 0, 0, 0, NULL)
arg(history_ARG, 'H', "history", 0, 0, 0, NULL)
arg(help2_ARG, '?', "", 0, 0, 0, NULL)
arg(import_ARG, 'i', "import", 0, 0, 0, NULL)
arg(interval_ARG, 'i', "interval", number_VAL, 0, 0, NULL)
arg(iop_version_ARG, 'i', "iop_version", 0, 0, 0, NULL)
arg(stripes_ARG, 'i', "stripes", number_VAL, 0, 0, NULL)
arg(stripesize_ARG, 'I', "stripesize", sizekb_VAL, 0, 0, NULL)
arg(logicalvolume_ARG, 'l', "logicalvolume", number_VAL, 0, 0, NULL)
arg(maxlogicalvolumes_ARG, 'l', "maxlogicalvolumes", number_VAL, 0, 0, NULL)
arg(extents_ARG, 'l', "extents", numsignedper_VAL, 0, 0, NULL)
arg(list_ARG, 'l', "list", 0, 0, 0, NULL)
arg(lvmpartition_ARG, 'l', "lvmpartition", 0, 0, 0, NULL)
arg(size_ARG, 'L', "size", sizemb_VAL, 0, 0, NULL)
arg(persistent_ARG, 'M', "persistent", bool_VAL, 0, 0, NULL)
arg(major_ARG, 'j', "major", number_VAL, ARG_GROUPABLE, 0, NULL)
arg(setactivationskip_ARG, 'k', "setactivationskip", bool_VAL, 0, 0, NULL)
arg(ignoreactivationskip_ARG, 'K', "ignoreactivationskip", 0, 0, 0, NULL)
arg(maps_ARG, 'm', "maps", 0, 0, 0, NULL)
arg(mirrors_ARG, 'm', "mirrors", numsigned_VAL, 0, 0, NULL)
arg(metadatatype_ARG, 'M', "metadatatype", metadatatype_VAL, 0, 0, NULL)
arg(name_ARG, 'n', "name", string_VAL, 0, 0, NULL)
arg(nofsck_ARG, 'n', "nofsck", 0, 0, 0, NULL)
arg(novolumegroup_ARG, 'n', "novolumegroup", 0, 0, 0, NULL)
arg(oldpath_ARG, 'n', "oldpath", 0, 0, 0, NULL)
arg(options_ARG, 'o', "options", string_VAL, ARG_GROUPABLE, 0, NULL)
arg(sort_ARG, 'O', "sort", string_VAL, ARG_GROUPABLE, 0, NULL)
arg(maxphysicalvolumes_ARG, 'p', "maxphysicalvolumes", uint32_VAL, 0, 0, NULL)
arg(permission_ARG, 'p', "permission", permission_VAL, 0, 0, NULL)
arg(partial_ARG, 'P', "partial", 0, 0, 0, NULL)
arg(physicalvolume_ARG, 'P', "physicalvolume", 0, 0, 0, NULL)
arg(quiet_ARG, 'q', "quiet", 0, ARG_COUNTABLE, 0, NULL)
arg(readahead_ARG, 'r', "readahead", readahead_VAL, 0, 0, NULL)
arg(resizefs_ARG, 'r', "resizefs", 0, 0, 0, NULL)
arg(reset_ARG, 'R', "reset", 0, 0, 0, NULL)
arg(regionsize_ARG, 'R', "regionsize", sizemb_VAL, 0, 0, NULL)
arg(physicalextentsize_ARG, 's', "physicalextentsize", sizemb_VAL, 0, 0, NULL)
arg(snapshot_ARG, 's', "snapshot", 0, 0, 0, NULL)
arg(short_ARG, 's', "short", 0, 0, 0, NULL)
arg(stdin_ARG, 's', "stdin", 0, 0, 0, NULL)
arg(select_ARG, 'S', "select", string_VAL, ARG_GROUPABLE, 0, NULL)
arg(test_ARG, 't', "test", 0, 0, 0, NULL)
arg(thin_ARG, 'T', "thin", 0, 0, 0, NULL)
arg(uuid_ARG, 'u', "uuid", 0, 0, 0, NULL)
arg(uuidstr_ARG, 'u', "uuid", string_VAL, 0, 0, NULL)
arg(uuidlist_ARG, 'U', "uuidlist", 0, 0, 0, NULL)
arg(verbose_ARG, 'v', "verbose", 0, ARG_COUNTABLE, 0, NULL)
arg(volumegroup_ARG, 'V', "volumegroup", 0, 0, 0, NULL)
arg(virtualsize_ARG, 'V', "virtualsize", sizemb_VAL, 0, 0, NULL)
arg(wipesignatures_ARG, 'W', "wipesignatures", bool_VAL, 0, 0, NULL)
arg(allocatable_ARG, 'x', "allocatable", bool_VAL, 0, 0, NULL)
arg(resizeable_ARG, 'x', "resizeable", bool_VAL, 0, 0, NULL)
arg(yes_ARG, 'y', "yes", 0, 0, 0, NULL)
arg(zero_ARG, 'Z', "zero", bool_VAL, 0, 0, NULL)

/* this should always be last */
arg(ARG_COUNT, '-', "", 0, 0, 0, NULL)
/* *INDENT-ON* */
