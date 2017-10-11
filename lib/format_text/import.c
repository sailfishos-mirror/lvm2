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

#include "lib.h"
#include "metadata.h"
#include "import-export.h"

/* FIXME Use tidier inclusion method */
static struct text_vg_version_ops *(_text_vsn_list[2]);

static int _text_import_initialised = 0;

static void _init_text_import(void)
{
	if (_text_import_initialised)
		return;

	_text_vsn_list[0] = text_vg_vsn1_init();
	_text_vsn_list[1] = NULL;
	_text_import_initialised = 1;
}

int text_read_metadata_summary(const struct format_type *fmt,
		       struct device *dev,
		       struct label_read_data *ld,
		       off_t offset, uint32_t size,
		       off_t offset2, uint32_t size2,
		       checksum_fn_t checksum_fn,
		       int checksum_only,
		       struct lvmcache_vgsummary *vgsummary,
		       uint64_t *failed_flags)
{
	struct dm_config_tree *cft;
	struct text_vg_version_ops **vsn;
	char *buf = NULL;
	int r = 0;

	if (ld) {
		if (ld->buf_len >= (offset + size))
			buf = ld->buf;
		else {
			/*
			 * Needs data beyond the end of the ld buffer.
			 * Will do a new synchronous read to get the data.
			 * (scan_size could also be made larger.)
			 */
			log_debug_metadata("label scan buffer for %s too small %u for metadata offset %llu size %u",
					   dev_name(dev), ld->buf_len, (unsigned long long)offset, size);
			buf = NULL;
		}
	}

	_init_text_import();

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0))) {
		*failed_flags |= FAILED_INTERNAL;
		return_0;
	}

	if (dev) {
		if (buf)
			log_debug_metadata("Copying metadata summary for %s at %llu size %d (+%d)",
					   dev_name(dev), (unsigned long long)offset,
					   size, size2);
		else
			log_debug_metadata("Reading metadata summary from %s at %llu size %d (+%d)",
					    dev_name(dev), (unsigned long long)offset,
					    size, size2);

		if (!config_file_read_fd(cft, dev, buf, offset, size,
					 offset2, size2, checksum_fn,
					 vgsummary->mda_checksum,
					 checksum_only, 1, failed_flags)) {
			log_error("Couldn't read volume group metadata from %s at %llu.", dev_name(dev), (unsigned long long)offset);
			goto out;
		}
	} else {
		if (!config_file_read(cft)) {
			log_error("Couldn't read volume group metadata from file.");
			goto out;
		}
	}

	if (checksum_only) {
		/* Checksum matches already-cached content - no need to reparse. */
		log_debug_metadata("Metadata summary checksum matches previous for %s.", dev ? dev_name(dev) : "file");
		r = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(*vsn)->read_vgsummary(fmt, cft, vgsummary, failed_flags)) {
			log_debug_metadata("Metadata summary is invalid for %s.", dev ? dev_name(dev) : "file");
			goto_out;
		}

		r = 1;
		break;
	}

      out:
	config_destroy(cft);
	return r;
}

struct volume_group *text_read_metadata_vg(struct format_instance *fid,
				       struct device *dev,
				       const char *file,
				       struct label_read_data *ld,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
                                       uint32_t last_meta_checksum,
                                       size_t last_meta_size,
                                       unsigned *last_meta_matches,
				       checksum_fn_t checksum_fn,
				       uint32_t checksum,
				       time_t *when, char **desc,
				       uint64_t *failed_flags)
{
	struct volume_group *vg = NULL;
	struct dm_config_tree *cft;
	struct text_vg_version_ops **vsn;
	char *buf = NULL;
	unsigned last_matches;

	_init_text_import();

	*desc = NULL;
	*when = 0;

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, file, 0))) {
		*failed_flags |= FAILED_INTERNAL;
		return_NULL;
	}

	if (last_meta_checksum && last_meta_size &&
	    (checksum == last_meta_checksum) && ((size + size2) == last_meta_size))
		last_matches = 1;
	else
		last_matches = 0;

	if (last_meta_matches)
		*last_meta_matches = last_matches;

	if (ld) {
		if (ld->buf_len >= (offset + size))
			buf = ld->buf;
		else {
			/*
			 * Needs data beyond the end of the ld buffer.
			 * Will do a new synchronous read to get the data.
			 * (scan_size could also be made larger.)
			 */
			log_debug_metadata("scan buffer for %s too small %u for metadata offset %llu size %u",
					   dev_name(dev), ld->buf_len, (unsigned long long)offset, size);
			buf = NULL;
		}
	}

	if (dev) {
		if (buf)
			log_debug_metadata("Copying metadata for %s at %llu size %d (+%d)",
					   dev_name(dev), (unsigned long long)offset,
					   size, size2);
		else
			log_debug_metadata("Reading metadata from %s at %llu size %d (+%d)",
				   	   dev_name(dev), (unsigned long long)offset,
				           size, size2);

		if (!config_file_read_fd(cft, dev, buf, offset, size,
					 offset2, size2, checksum_fn, checksum,
					 last_matches, 1, failed_flags)) {
			log_error("Couldn't read volume group metadata from %s.", dev_name(dev));

			/* We have to be certain this has been set since it's the
			 * only way the caller knows if the function failed or not. */
			if (!*failed_flags)
				*failed_flags |= FAILED_VG_METADATA;
			goto out;
		}
	} else {
		if (!config_file_read(cft)) {
			log_error("Couldn't read volume group metadata from file.");

			if (!*failed_flags)
				*failed_flags |= FAILED_VG_METADATA;
			goto out;
		}
	}

	if (last_matches) {
		log_debug_metadata("Skipped parsing metadata on %s with matching checksum 0x%x size %zu.",
				   dev_name(dev), last_meta_checksum, last_meta_size);
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(vg = (*vsn)->read_vg(fid, cft, 0, failed_flags))) {
			if (!*failed_flags)
				*failed_flags |= FAILED_VG_METADATA;
			goto_out;
		}

		(*vsn)->read_desc(vg->vgmem, cft, when, desc);
		break;
	}

      out:
	config_destroy(cft);
	return vg;
}

struct volume_group *text_read_metadata_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc)
{
	uint64_t failed_flags = 0;

	return text_read_metadata_vg(fid, NULL, file, NULL,
				     (off_t)0, 0, (off_t)0, 0,
				     0, 0, NULL, NULL, 0,
				     when, desc, &failed_flags);
}

static struct volume_group *_import_vg_from_config_tree(const struct dm_config_tree *cft,
							struct format_instance *fid,
							unsigned for_lvmetad)
{
	struct volume_group *vg = NULL;
	struct text_vg_version_ops **vsn;
	uint64_t failed_flags = 0;
	int vg_missing;

	_init_text_import();

	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;
		/*
		 * The only path to this point uses cached vgmetadata,
		 * so it can use cached PV state too.
		 */
		if (!(vg = (*vsn)->read_vg(fid, cft, for_lvmetad, &failed_flags)))
			stack;
		else {
			/* FIXME: move this into vg_read() */
			set_pv_devices(fid, vg);
			
			if ((vg_missing = vg_missing_pv_count(vg)))
				log_verbose("There are %d physical volumes missing.", vg_missing);

			vg_mark_partial_lvs(vg, 1);
		}
		break;
	}

	return vg;
}

struct volume_group *import_vg_from_config_tree(const struct dm_config_tree *cft,
						struct format_instance *fid)
{
	return _import_vg_from_config_tree(cft, fid, 0);
}

struct volume_group *import_vg_from_lvmetad_config_tree(const struct dm_config_tree *cft,
							struct format_instance *fid)
{
	return _import_vg_from_config_tree(cft, fid, 1);
}
