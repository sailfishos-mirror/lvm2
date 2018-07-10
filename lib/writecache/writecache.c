/*
 * Copyright (C) 2013-2016 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/segtype.h"
#include "lib/display/display.h"
#include "lib/format_text/text_export.h"
#include "lib/config/config.h"
#include "lib/datastruct/str_list.h"
#include "lib/misc/lvm-string.h"
#include "lib/activate/activate.h"
#include "lib/metadata/metadata.h"
#include "lib/metadata/lv_alloc.h"
#include "lib/config/defaults.h"

static const char _writecache_module[] = "writecache";

#define SEG_LOG_ERROR(t, p...) \
        log_error(t " segment %s of logical volume %s.", ## p,	\
                  dm_config_parent_name(sn), seg->lv->name), 0;

static void _writecache_display(const struct lv_segment *seg)
{
	/* TODO: lvdisplay segments */
}

static int _writecache_text_import(struct lv_segment *seg,
				   const struct dm_config_node *sn,
				   struct dm_hash_table *pv_hash __attribute__((unused)))
{
	struct logical_volume *origin_lv = NULL;
	struct logical_volume *cachevol;
	const char *lv_name = NULL;
	const char *cv_name = NULL;

	if (!dm_config_has_node(sn, "origin"))
		return SEG_LOG_ERROR("origin not specified in");

	if (!dm_config_get_str(sn, "origin", &lv_name))
		return SEG_LOG_ERROR("origin must be a string in");

	if (!(origin_lv = find_lv(seg->lv->vg, lv_name)))
		return SEG_LOG_ERROR("Unknown LV specified for writecache origin %s in", lv_name);

	if (!set_lv_segment_area_lv(seg, 0, origin_lv, 0, 0))
		return_0;

	if (!dm_config_has_node(sn, "cachevol"))
		return SEG_LOG_ERROR("cachevol not specified in");

	if (!dm_config_get_str(sn, "cachevol", &cv_name))
		return SEG_LOG_ERROR("cachevol must be a string in");

	if (!(cachevol = find_lv(seg->lv->vg, cv_name)))
		return SEG_LOG_ERROR("Unknown logical volume %s specified for cachevol in",
				     cv_name);

	/* use attach_cachevol() ? */

	seg->origin = origin_lv;
	seg->cachevol = cachevol;
	seg->lv->status |= WRITECACHE;

	if (!add_seg_to_segs_using_this_lv(cachevol, seg))
		return_0;

	return 1;
}

static int _writecache_text_import_area_count(const struct dm_config_node *sn,
					      uint32_t *area_count)
{
	*area_count = 1;

	return 1;
}

static int _writecache_text_export(const struct lv_segment *seg,
				   struct formatter *f)
{
	outf(f, "cachevol = \"%s\"", seg->cachevol->name);
	outf(f, "origin = \"%s\"", seg_lv(seg, 0)->name);
	return 1;
}

static void _destroy(struct segment_type *segtype)
{
	free((void *) segtype);
}

#ifdef DEVMAPPER_SUPPORT

static int _target_present(struct cmd_context *cmd,
			   const struct lv_segment *seg __attribute__((unused)),
			   unsigned *attributes __attribute__((unused)))
{
	static int _writecache_checked = 0;
	static int _writecache_present = 0;

	if (!activation())
		return 0;

	if (!_writecache_checked) {
		_writecache_checked = 1;
		_writecache_present =  target_present(cmd, TARGET_NAME_WRITECACHE, 0);
	}

	return _writecache_present;
}

static int _modules_needed(struct dm_pool *mem,
			   const struct lv_segment *seg __attribute__((unused)),
			   struct dm_list *modules)
{
	if (!str_list_add(mem, modules, MODULE_NAME_WRITECACHE)) {
		log_error("String list allocation failed for writecache module.");
		return 0;
	}

	return 1;
}
#endif /* DEVMAPPER_SUPPORT */

#ifdef DEVMAPPER_SUPPORT
static int _writecache_add_target_line(struct dev_manager *dm,
				 struct dm_pool *mem,
				 struct cmd_context *cmd __attribute__((unused)),
				 void **target_state __attribute__((unused)),
				 struct lv_segment *seg,
				 const struct lv_activate_opts *laopts __attribute__((unused)),
				 struct dm_tree_node *node, uint64_t len,
				 uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	char *origin_uuid;
	char *cache_uuid;
	int pmem;

	if (!seg_is_writecache(seg)) {
		log_error(INTERNAL_ERROR "Passed segment is not writecache.");
		return 0;
	}

	if (!seg->cachevol) {
		log_error(INTERNAL_ERROR "Passed segment has no cachevol.");
		return 0;
	}

	pmem = seg->cachevol->cachevol_pmem;

	if (!(origin_uuid = build_dm_uuid(mem, seg_lv(seg, 0), NULL)))
		return_0;

	if (!(cache_uuid = build_dm_uuid(mem, seg->cachevol, NULL)))
		return_0;

	if (!dm_tree_node_add_writecache_target(node, len, origin_uuid, cache_uuid, pmem))
		return_0;

	return 1;
}
#endif /* DEVMAPPER_SUPPORT */

static struct segtype_handler _writecache_ops = {
	.display = _writecache_display,
	.text_import = _writecache_text_import,
	.text_import_area_count = _writecache_text_import_area_count,
	.text_export = _writecache_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _writecache_add_target_line,
	.target_present = _target_present,
	.modules_needed = _modules_needed,
#endif
	.destroy = _destroy,
};

int init_writecache_segtypes(struct cmd_context *cmd,
			struct segtype_library *seglib)
{
	struct segment_type *segtype = zalloc(sizeof(*segtype));

	if (!segtype) {
		log_error("Failed to allocate memory for cache_pool segtype");
		return 0;
	}

	segtype->name = SEG_TYPE_NAME_WRITECACHE;
	segtype->flags = SEG_WRITECACHE;
	segtype->ops = &_writecache_ops;

	if (!lvm_register_segtype(seglib, segtype))
		return_0;
	log_very_verbose("Initialised segtype: %s", segtype->name);

	return 1;
}
