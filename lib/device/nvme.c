/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
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
#include "lib/device/device.h"
#include "lib/device/device_id.h"

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>

#ifdef LIBNVME_SUPPORT
#include <libnvme.h>

static void _add_uuid(struct device *dev, unsigned char *uuid)
{
	char idname[DEV_WWID_SIZE] = {0};
	int max, pos, num, i;

	max = sizeof(idname);
	pos = 0;

	num = snprintf(idname + pos, max - pos, "uuid.");
	if (num >= max - pos)
		goto bad;
	pos += num;

	for (i = 0; i < NVME_UUID_LEN; ++i) {
		num = snprintf(idname + pos, max - pos, "%02x", uuid[i]);
		if (num >= max - pos)
			goto bad;
		pos += num;

		if (i == 3 || i == 5 || i == 7 || i == 9) {
			num = snprintf(idname + pos, max - pos, "-");
			if (num >= max - pos)
				goto bad;
			pos += num;
		}
	}

	idname[DEV_WWID_SIZE-1] = '\0';

	dev_add_nvme_wwid(idname, 3, &dev->wwids);

	return;
bad:
	log_debug("dev_read_nvme_wwids ignore invalid uuid %s for %s", uuid, dev_name(dev));
}

static void _add_nguid(struct device *dev, unsigned char *nguid)
{
	char idname[DEV_WWID_SIZE] = {0};
	int max, pos, num, i;

	max = sizeof(idname);
	pos = 0;

	num = snprintf(idname + pos, max - pos, "eui.");
	if (num >= max - pos)
		goto bad;
	pos += num;

	for (i = 0; i < 16; ++i) {
		num = snprintf(idname + pos, max - pos, "%02x", nguid[i]);
		if (num >= max - pos)
			goto bad;
		pos += num;
	}

	idname[DEV_WWID_SIZE-1] = '\0';

	dev_add_nvme_wwid(idname, 2, &dev->wwids);

	return;
bad:
	log_debug("dev_read_nvme_wwids ignore invalid nguid %s for %s", nguid, dev_name(dev));
}

static void _add_eui64(struct device *dev, unsigned char *eui64)
{
	char idname[DEV_WWID_SIZE] = {0};
	int max, pos, num, i;

	max = sizeof(idname);
	pos = 0;

	num = snprintf(idname + pos, max - pos, "eui.");
	if (num >= max - pos)
		goto bad;
	pos += num;

	for (i = 0; i < 8; ++i) {
		num = snprintf(idname + pos, max - pos, "%02x", eui64[i]);
		if (num >= max - pos)
			goto bad;
		pos += num;
	}

	idname[DEV_WWID_SIZE-1] = '\0';

	dev_add_nvme_wwid(idname, 1, &dev->wwids);

	return;
bad:
	log_debug("dev_read_nvme_wwids ignore invalid eui64 %s for %s", eui64, dev_name(dev));
}

void dev_read_nvme_wwids(struct device *dev)
{
	const char *devpath;
	unsigned char *data = NULL;
	struct nvme_id_ns *ns = NULL;
	unsigned char nguid[16] = {0};
	unsigned char eui64[8] = {0};
	unsigned char uuid[NVME_UUID_LEN] = {0};
	unsigned int nsid;
	int fd, i, len;

	dev->flags |= DEV_ADDED_NVME_WWIDS;

	/* shouldn't happen */
	if (dm_list_empty(&dev->aliases))
		return;

	devpath = dev_name(dev);

	if ((fd = open(devpath, O_RDONLY) < 0)) {
		log_debug("dev_read_nvme_wwids cannot open %s", devpath);
		return;
	}

	if (nvme_get_nsid(fd, &nsid) < 0) {
		log_debug("dev_read_nvme_wwids nvme_get_nsid error %s", devpath);
		goto out;
	}

	if (!(ns = malloc(sizeof(*ns))))
		goto_out;

	if (nvme_identify_ns(fd, nsid, ns)) {
		log_debug("dev_read_nvme_wwids nvme_identify_ns error %s", devpath);
		goto out;
	}

	memcpy(nguid, ns->nguid, 16);
	memcpy(eui64, ns->eui64, 8);

	if (!(data = malloc(NVME_IDENTIFY_DATA_SIZE)))
		goto_out;

	if (nvme_identify_ns_descs(fd, nsid, (struct nvme_ns_id_desc *)data)) {
		log_debug("dev_read_nvme_wwids nvme_identify_ns_descs error %s", devpath);
		goto out;
	}

	for (i = 0; i < NVME_IDENTIFY_DATA_SIZE; i += len) {
		struct nvme_ns_id_desc *cur = (struct nvme_ns_id_desc *)(data + i);

		if (cur->nidl == 0)
			break;

		memset(eui64, 0, sizeof(eui64));
		memset(nguid, 0, sizeof(nguid));
		memset(uuid, 0, sizeof(uuid));

		switch (cur->nidt) {
		case NVME_NIDT_EUI64:
			memcpy(eui64, data + i + sizeof(*cur), sizeof(eui64));
			len = sizeof(eui64);
			break;
		case NVME_NIDT_NGUID:
			memcpy(nguid, data + i + sizeof(*cur), sizeof(nguid));
			len = sizeof(nguid);
			break;
		case NVME_NIDT_UUID:
			memcpy(uuid, data + i + sizeof(*cur), NVME_UUID_LEN);
			len = sizeof(uuid);
			break;
		case NVME_NIDT_CSI:
			len = 1;
			break;
		default:
			len = cur->nidl;
			break;
		}

		len += sizeof(*cur);

		if (uuid[0])
			_add_uuid(dev, uuid);
		else if (nguid[0])
			_add_nguid(dev, nguid);
		else if (eui64[0])
			_add_eui64(dev, eui64);
	}
out:
	free(ns);
	free(data);
	close(fd);
}
#else
void dev_read_nvme_wwids(struct device *dev)
{
}
#endif
