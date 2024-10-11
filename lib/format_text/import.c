/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2008 Red Hat, Inc. All rights reserved.
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

#include "lib/misc/lib.h"
#include "lib/metadata/metadata.h"
#include "lib/commands/toolcontext.h"
#include "import-export.h"

/* FIXME Use tidier inclusion method */
static const struct text_vg_version_ops *(_text_vsn_list[2]);

static int _text_import_initialised = 0;

static void _init_text_import(void)
{
	if (_text_import_initialised)
		return;

	_text_vsn_list[0] = text_vg_vsn1_init();
	_text_vsn_list[1] = NULL;
	_text_import_initialised = 1;
}

/*
 * Find out vgname on a given device.
 */
int text_read_metadata_summary(const struct format_type *fmt,
		       struct device *dev,
		       off_t offset, uint32_t size,
		       off_t offset2, uint32_t size2,
		       checksum_fn_t checksum_fn,
		       int checksum_only,
		       struct lvmcache_vgsummary *vgsummary,
		       struct dm_config_tree **cft_out)
{
	struct dm_config_tree *cft;
	const struct text_vg_version_ops **vsn;
	int r = 0;

	_init_text_import();

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0)))
		return_0;

	if (dev) {
		log_debug_metadata("Reading metadata summary from %s at %llu size %d (+%d)",
				   dev_name(dev), (unsigned long long)offset,
				   size, size2);

		if (!config_file_read_fd(cft, dev, offset, size,
					 offset2, size2, checksum_fn,
					 vgsummary->mda_checksum, 0,
					 checksum_only, 0, 1, NULL)) {
			log_warn("WARNING: invalid metadata text from %s at %llu.",
				 dev_name(dev), (unsigned long long)offset);
			goto out;
		}
	} else {
		if (!config_file_read_from_file(cft)) {
			log_warn("WARNING: invalid metadata text from file.");
			goto out;
		}
	}

	if (checksum_only) {
		/* Checksum matches already-cached content - no need to reparse. */
		log_debug_metadata("Skipped parsing metadata on %s", dev_name(dev));
		r = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(*vsn)->read_vgsummary(fmt, cft, vgsummary))
			goto_out;

		r = 1;
		break;
	}

      out:
	if (cft_out)
		*cft_out = cft;
	else
		config_destroy(cft);
	return r;
}

struct cached_vg_fmtdata {
        uint32_t cached_mda_checksum;
        size_t cached_mda_size;
};

struct volume_group *text_read_metadata(struct format_instance *fid,
				       const char *file,
				       struct cached_vg_fmtdata **vg_fmtdata,
				       unsigned *use_previous_vg,
				       struct device *dev, int primary_mda,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
				       checksum_fn_t checksum_fn,
				       uint32_t mda_header_checksum,
				       time_t *when, char **desc)
{
	struct volume_group *vg = NULL;
	struct dm_config_tree *cft;
	struct dm_config_tree *cft_scanned = NULL;
	const struct text_vg_version_ops **vsn;
	uint32_t total_size = size + size2;
	uint32_t scan_meta_checksum;
	uint32_t scan_meta_size;
	int skip_parse = 0;
	int skip_cft_if_scan_matches = 0;
	int scan_matches = 0;

	/*
	 * This struct holds the checksum and size of the VG metadata
	 * that was read from a previous device.  When we read the VG
	 * metadata from this device, we can skip parsing it into a
	 * cft (saving time) if the checksum of the metadata buffer
	 * we read from this device matches the size/checksum saved in
	 * the mda_header/rlocn struct on this device, and matches the
	 * size/checksum from the previous device.
	 * This optimization addresses the case of reading the same
	 * metadata from multiple PVs in the same VG.
	 */
	if (vg_fmtdata && !*vg_fmtdata &&
	    !(*vg_fmtdata = dm_pool_zalloc(fid->mem, sizeof(**vg_fmtdata)))) {
		log_error("Failed to allocate VG fmtdata for text format.");
		return NULL;
	}

	_init_text_import();

	*desc = NULL;
	*when = 0;

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, file, 0)))
		return_NULL;

	/* try to reuse results from a prior call to this function, i.e.
	   from the metadata that was read from another PV in the VG.
	   the mda header checksum should always match the text checksum,
	   otherwise something is wrong, and we ignore the mda.
	   skip_parse=1: we're asking config_file_read_fd() to read
	   the metadata text, calculate the checksum of it, and
	   verify it matches the checksum from the mda_header, or
	   return an error. */

	if (vg_fmtdata) {
		skip_parse = vg_fmtdata &&
			     ((*vg_fmtdata)->cached_mda_checksum == mda_header_checksum) &&
			     ((*vg_fmtdata)->cached_mda_size == total_size);
	}

	/* try to reuse results from read_vgsummary in scan phase.
	   the scanned metadata checksum may not match the text checksum,
	   it's expected to not match sometimes, and not an error if it happens.
	   when it happens we just ignore the cft from the scan phase.
	   skip_cft_if_scan_matches=1: we're asking config_file_read_fd() to
	   read the metadata text, calculate the checksum of it, and if it
	   matches the checksum from the scan phase, then return success
	   with scan_matches=1.  If it doesn't match, then parse the newly
	   read text into a new cft. */

	if (dev && (cft_scanned = lvmcache_get_cft(dev, &scan_meta_checksum, &scan_meta_size))) {
		if ((mda_header_checksum == scan_meta_checksum) && (total_size == scan_meta_size))
			skip_cft_if_scan_matches = 1;
		else {
			cft_scanned = NULL;
			lvmcache_free_cft(dev);
		}
	}

	if (dev) {
		log_debug_metadata("Reading metadata from %s at %llu size %d (+%d)",
				   dev_name(dev), (unsigned long long)offset,
				   size, size2);

		if (!config_file_read_fd(cft, dev, offset, size, offset2, size2,
					 checksum_fn, mda_header_checksum, scan_meta_checksum,
					 skip_parse, skip_cft_if_scan_matches, 1, &scan_matches)) {
			log_warn("WARNING: couldn't read volume group metadata from %s.", dev_name(dev));
			goto out;
		}
	} else {
		if (!config_file_read_from_file(cft)) {
			log_warn("WARNING: couldn't read volume group metadata from file.");
			goto out;
		}
	}

	if (skip_cft_if_scan_matches) {
		if (scan_matches) {
			log_debug("Reuse vg cft from scan");
			dm_config_destroy(cft);
			cft = cft_scanned;
		} else {
			log_debug("Drop vg cft from scan");
			cft_scanned = NULL;
			lvmcache_free_cft(dev);
		}
	}

	if (skip_parse) {
		if (use_previous_vg)
			*use_previous_vg = 1;
		log_debug_metadata("Skipped parsing metadata on %s", dev_name(dev));
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(vg = (*vsn)->read_vg(fid->fmt->cmd, fid->fmt, fid, cft)))
			goto_out;

		(*vsn)->read_desc(vg->vgmem, cft, when, desc);
		vg->committed_cft = cft; /* Reuse CFT for recreation of committed VG */
		vg->buffer_size_hint = total_size;
		cft = NULL;
		break;
	}

	if (vg && vg_fmtdata && *vg_fmtdata) {
		(*vg_fmtdata)->cached_mda_size = total_size;
		(*vg_fmtdata)->cached_mda_checksum = mda_header_checksum;
	}

	if (use_previous_vg)
		*use_previous_vg = 0;

      out:
	if (cft && (cft != cft_scanned))
		config_destroy(cft);
	return vg;
}

struct volume_group *text_read_metadata_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc)
{
	return text_read_metadata(fid, file, NULL, NULL, NULL, 0,
				  (off_t)0, 0, (off_t)0, 0, NULL, 0,
				  when, desc);
}

static struct volume_group *_import_vg_from_config_tree(struct cmd_context *cmd,
							struct format_instance *fid,
							const struct dm_config_tree *cft)
{
	struct volume_group *vg = NULL;
	const struct text_vg_version_ops **vsn;
	int vg_missing;

	_init_text_import();

	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;
		/*
		 * The only path to this point uses cached vgmetadata,
		 * so it can use cached PV state too.
		 */
		if (!(vg = (*vsn)->read_vg(cmd, fid->fmt, fid, cft)))
			stack;
		else {
			set_pv_devices(fid, vg);

			if ((vg_missing = vg_missing_pv_count(vg)))
				log_verbose("There are %d physical volumes missing.", vg_missing);
			vg_mark_partial_lvs(vg, 1);
			/* FIXME: move this code inside read_vg() */
		}
		break;
	}

	return vg;
}

struct volume_group *import_vg_from_config_tree(struct cmd_context *cmd,
						struct format_instance *fid,
						const struct dm_config_tree *cft)
{
	return _import_vg_from_config_tree(cmd, fid, cft);
}

struct volume_group *vg_from_config_tree(struct cmd_context *cmd, const struct dm_config_tree *cft)
{
	const struct text_vg_version_ops *ops;

	_init_text_import();

	ops = _text_vsn_list[0];

	return ops->read_vg(cmd, cmd->fmt, NULL, cft);
}

