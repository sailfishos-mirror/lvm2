/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
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
#include "lib/device/dev-type.h"
#include "lib/device/persist.h"
#include "lib/config/config.h"
#include "lib/locking/lvmlockd.h"
#include "lib/misc/lvm-exec.h"
#include "lib/mm/xlate.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>

#define SCSI_PR_BUF_SIZE 8192 /* space for 1024 keys */

#define PR_KEY_BUF_SIZE 20 /* hex string. key is 8 bytes (16 hex chars) */

#define PRIN_CMD 0x5e
#define PRIN_CMDLEN 10
#define PRIN_RKEY 0x00 /* READ KEYS */
#define PRIN_RRES 0x01 /* READ RESERVATION */
#define READKEYS_RESPONSE_SIZE 8 + SCSI_PR_BUF_SIZE /* 4 byte pr_gen + 4 byte add_len + 1024 * 8 byte keys */
#define READRES_RESPONSE_SIZE 24

static int get_our_key(struct cmd_context *cmd, struct volume_group *vg,
		       char *local_key, int local_host_id,
		       char *ret_key_buf, uint64_t *ret_key_val);

static int dev_allow_pr(struct cmd_context *cmd, struct device *dev)
{
	if (dm_list_empty(&dev->aliases))
		return 0;

	if (dev_is_scsi(cmd, dev))
		return 1;

	if (dev_is_mpath(cmd, dev))
		return 1;

#ifdef NVME_SUPPORT
	if (dev_is_nvme(dev))
		return 1;
#endif
	return 0;
}

static int prtype_from_scsi(uint8_t scsi_type)
{
	switch (scsi_type) {
	case 1:
		return PR_TYPE_WE;
	case 3:
		return PR_TYPE_EA;
	case 5:
		return PR_TYPE_WERO;
	case 6:
		return PR_TYPE_EARO;
	case 7:
		return PR_TYPE_WEAR;
	case 8:
		return PR_TYPE_EAAR;
	default:
		return -1;
	};
}

static const char *prtype_to_str(int prtype)
{
	switch (prtype) {
	case PR_TYPE_WE:
		return PR_STR_WE;
	case PR_TYPE_EA:
		return PR_STR_EA;
	case PR_TYPE_WERO:
		return PR_STR_WERO;
	case PR_TYPE_EARO:
		return PR_STR_EARO;
	case PR_TYPE_WEAR:
		return PR_STR_WEAR;
	case PR_TYPE_EAAR:
		return PR_STR_EAAR;
	default:
		return "unknown";
	};
}

/* copied from multipath */
static int parse_prkey(const char *ptr, uint64_t *prkey)
{
	if (!ptr)
		return_0;
	if (*ptr == '0')
		ptr++;
	if (*ptr == 'x' || *ptr == 'X')
		ptr++;
	if (*ptr == '\0' || strlen(ptr) > 16)
		return_0;
	if (strlen(ptr) != strspn(ptr, "0123456789aAbBcCdDeEfF"))
		return_0;
	if (sscanf(ptr, "%" SCNx64 "", prkey) != 1)
		return_0;
	return 1;
}

void persist_key_file_remove(struct cmd_context *cmd, struct volume_group *vg)
{
	char path[PATH_MAX] = { 0 };

	if (dm_snprintf(path, PATH_MAX-1, "/var/lib/lvm/persist_key_%s", vg->name) < 0)
		return;

	if (unlink(path))
		stack;
}

static int key_file_exists(struct cmd_context *cmd, struct volume_group *vg)
{
	char path[PATH_MAX] = { 0 };
	struct stat info;

	if (dm_snprintf(path, PATH_MAX-1, "/var/lib/lvm/persist_key_%s", vg->name) < 0)
		return 0;

	if (!stat(path, &info))
		return 1;
	if (errno != ENOENT)
		log_debug("key_file_exists errno %d %s", errno, path);
	return 0;
}

static int read_key_file(struct cmd_context *cmd, struct volume_group *vg,
			  char *key_str, uint64_t *key_val, int *host_id, uint32_t *gen)
{
	char path[PATH_MAX] = { 0 };
	char line[128] = { 0 };
	char buf_key[128] = { 0 };
	char *p;
	uint64_t val = 0;
	uint32_t found_host_id = 0;
	uint32_t found_gen = 0;
	FILE *fp;

	if (dm_snprintf(path, PATH_MAX-1, "/var/lib/lvm/persist_key_%s", vg->name) < 0)
		return 0;

	if (!(fp = fopen(path, "r"))) {
		log_debug("key_file: cannot open %s", path);
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '#')
			continue;

		strncpy(buf_key, line, sizeof(buf_key) - 1);
		break;
	}

	if (fclose(fp))
		stack;

	if (!buf_key[0]) {
		log_debug("key_file: empty");
		unlink(path);
		return 0;
	}

	if ((p = strchr(buf_key, '\n')))
		*p = '\0';

	if (strlen(buf_key) >= PR_KEY_BUF_SIZE) {
		log_debug("key_file: too long");
		unlink(path);
		return 0;
	}

	if (!parse_prkey(buf_key, &val)) {
		log_debug("key_file: parse error %s", buf_key);
		unlink(path);
		return 0;
	}

	found_host_id = (val & 0xFFFF);
	found_gen = (val & 0xFFFFFF0000) >> 16;

	if (key_str)
		strncpy(key_str, buf_key, PR_KEY_BUF_SIZE-1);

	if (key_val)
		*key_val = val;

	if (host_id)
		*host_id = (int)found_host_id;

	if (gen)
		*gen = found_gen;

	log_debug("key_file: read 0x%llx host_id %u gen %u", (unsigned long long)val, found_host_id, found_gen);
	return 1;
}

static int write_key_file(struct cmd_context *cmd, struct volume_group *vg, uint64_t key)
{
	char path[PATH_MAX] = { 0 };
	FILE *fp;

	if (dm_snprintf(path, PATH_MAX-1, "/var/lib/lvm/persist_key_%s", vg->name) < 0)
		return 0;

	if (!(fp = fopen(path, "w"))) {
		log_debug("Failed to create key file");
		return 0;
	}

	fprintf(fp, "0x%llx\n", (unsigned long long)key);

	if (fflush(fp))
		log_debug("Failed to write/flush key file");
	if (fclose(fp))
		log_debug("Failed to write/close key file");

	log_debug("key_file: wrote 0x%llx", (unsigned long long)key);
	return 1;
}

static int dev_read_reservation_scsi(struct cmd_context *cmd, struct device *dev, uint64_t *holder_ret, int *prtype_ret)
{
	const char *devname;
	sg_io_hdr_t io_hdr;
	unsigned char sense_buf[32];
	unsigned char response_buf[READRES_RESPONSE_SIZE];
	unsigned char cdb[PRIN_CMDLEN];
	uint32_t add_len_be, add_len;
	uint32_t pr_gen_be, pr_gen;
	uint64_t key_be, key;
	int response_len;
	int ret_bytes;
	int num;
	int fd;
	int ret = 0;
	unsigned char pr_type_byte;
	unsigned char pr_type_scsi;

	devname = dev_name(dev);

	if ((fd = open(devname, O_RDONLY)) < 0) {
		log_error("dev_read_reservation %s open error %d", devname, errno);
		return 0;
	}

	response_len = READRES_RESPONSE_SIZE;

	memset(cdb, 0, sizeof(cdb));

	cdb[0] = PRIN_CMD;
	cdb[1] = (unsigned char)(PRIN_RRES & 0x1f);
	cdb[7] = (unsigned char)((response_len >> 8) & 0xff);
	cdb[8] = (unsigned char)(response_len & 0xff);

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));

	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = PRIN_CMDLEN;
	io_hdr.mx_sb_len = sizeof(sense_buf);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = response_len;
	io_hdr.dxferp = response_buf;
	io_hdr.cmdp = cdb;
	io_hdr.sbp = sense_buf;
	io_hdr.timeout = 2000;     /* millisecs */

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		log_error("dev_read_reservation %s sg_io ioctl error %d", devname, errno);
		ret = 0;
		goto out;
	}

	ret_bytes = response_len - io_hdr.resid;

	log_debug("dev_read_reservation %s sg_io bytes %u of %u status driver:%02x host:%02x scsi:%02x",
		  devname, ret_bytes, response_len, io_hdr.driver_status, io_hdr.host_status, io_hdr.status);

	io_hdr.status &= 0x7e;
	if (io_hdr.status) {
		log_error("dev_read_reservation %s error 0x%x", devname, io_hdr.status);
		ret = 0;
		goto out;
	}

	memcpy(&pr_gen_be, response_buf + 0, 4);
	memcpy(&add_len_be, response_buf + 4, 4);
	pr_gen = be32_to_cpu(pr_gen_be);
	add_len = be32_to_cpu(add_len_be);
	num = add_len / 16;

	log_debug("dev_read_reservation %s pr_gen %u add_len %u num %d", devname, pr_gen, add_len, num);

	if (num > 0) {
		memcpy(&key_be, response_buf + 8, 8);
		key = be64_to_cpu(key_be);

		pr_type_byte = response_buf[21];
		pr_type_scsi = pr_type_byte & 0xf; /* top half of byte is scope */

		if (holder_ret)
			*holder_ret = key;
		if (prtype_ret)
			*prtype_ret = prtype_from_scsi((uint8_t)pr_type_scsi);

		log_debug("dev_read_reservation %s holder key %llx type 0x%x",
			  devname, (unsigned long long)key, pr_type_scsi);
	} else {
		if (holder_ret)
			*holder_ret = 0;
		if (prtype_ret)
			*prtype_ret = 0;
	}
	ret = 1;
out:
	close(fd);
	return ret;
}

static int dev_read_reservation(struct cmd_context *cmd, struct device *dev, uint64_t *holder_ret, int *prtype_ret)
{
	if (!dev_allow_pr(cmd, dev)) {
		log_error("persistent reservation not supported for device type %s", dev_name(dev));
		return 0;
	}

	if (dev_is_nvme(dev))
		return dev_read_reservation_nvme(cmd, dev, holder_ret, prtype_ret);
	else
		return dev_read_reservation_scsi(cmd, dev, holder_ret, prtype_ret);
}

/*
 * If find_key is set, look for a matching key
 *   (set found_key to 1).
 *
 * If find_host_id is set, look for a key which
 * contains that host_id in the lower 2 bytes
 *   (set found_host_id_key to full key value).
 *
 * If find_all is set, get number of keys
 *   (set found_count to number).
 */

static int dev_find_key_scsi(struct cmd_context *cmd, struct device *dev, int may_fail,
		        uint64_t find_key, int *found_key,
		        int find_host_id, uint64_t *found_host_id_key,
			int find_all, int *found_count, uint64_t **found_all)
{
	const char *devname;
	sg_io_hdr_t io_hdr;
	unsigned char sense_buf[32];
	unsigned char cdb[PRIN_CMDLEN];
	unsigned char *response_buf;
	uint32_t add_len_be, add_len;
	uint32_t pr_gen_be, pr_gen;
	uint64_t key_be, key;
	uint64_t *all_keys;
	int response_len;
	int ret_bytes;
	int num_keys;
	int fd, i;
	int ret = 0;

	devname = dev_name(dev);

	if ((fd = open(devname, O_RDONLY)) < 0) {
		log_error("dev_find_key %s open error %d", devname, errno);
		return 0;
	}

	if (!(response_buf = malloc(READKEYS_RESPONSE_SIZE)))
		goto out;

	response_len = READKEYS_RESPONSE_SIZE;

	memset(cdb, 0, sizeof(cdb));

	cdb[0] = PRIN_CMD;
	cdb[1] = (unsigned char)(PRIN_RKEY & 0x1f);
	cdb[7] = (unsigned char)((response_len >> 8) & 0xff);
	cdb[8] = (unsigned char)(response_len & 0xff);

	memset(&io_hdr, 0, sizeof(sg_io_hdr_t));

	io_hdr.interface_id = 'S';
	io_hdr.cmd_len = PRIN_CMDLEN;
	io_hdr.mx_sb_len = sizeof(sense_buf);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = response_len;
	io_hdr.dxferp = response_buf;
	io_hdr.cmdp = cdb;
	io_hdr.sbp = sense_buf;
	io_hdr.timeout = 2000;     /* millisecs */

	if (ioctl(fd, SG_IO, &io_hdr) < 0) {
		if (may_fail)
			log_debug("dev_find_key %s sg_io ioctl error %d", devname, errno);
		else
			log_error("dev_find_key %s sg_io ioctl error %d", devname, errno);
		ret = 0;
		goto out;
	}

	ret_bytes = response_len - io_hdr.resid;

	log_debug("dev_find_key %s sg_io ret_bytes %u of %u status driver:%02x host:%02x scsi:%02x",
		   devname, ret_bytes, response_len, io_hdr.driver_status, io_hdr.host_status, io_hdr.status);

	io_hdr.status &= 0x7e;
	if (io_hdr.status) {
		if (may_fail)
			log_debug("dev_find_key %s error scsi:0x%02x driver:%02x host:%02x",
				  devname, io_hdr.status, io_hdr.driver_status, io_hdr.host_status);
		else
			log_error("dev_find_key %s error scsi:0x%02x driver:%02x host:%02x",
				  devname, io_hdr.status, io_hdr.driver_status, io_hdr.host_status);
		ret = 0;
		goto out;
	}

	/* response_buf: 4 byte pr_gen, 4 byte add_len, N * 8 byte keys */

	memcpy(&pr_gen_be, response_buf + 0, 4);
	memcpy(&add_len_be, response_buf + 4, 4);
	pr_gen = be32_to_cpu(pr_gen_be);
	add_len = be32_to_cpu(add_len_be);
	num_keys = add_len / 8;

	log_debug("dev_find_key %s pr_gen %u num %d", devname, pr_gen, num_keys);

	/* caller wants just a count of all keys */
	if (find_all && found_count && !found_all) {
		*found_count = num_keys;
		ret = 1;
		goto out;
	}

	/* caller wants a count and array of all keys */
	if (find_all && found_count && found_all) {
		*found_count = num_keys;
		*found_all = NULL;

		if (!num_keys) {
			ret = 1;
			goto out;
		}
		if (!(all_keys = dm_pool_zalloc(cmd->mem, num_keys * sizeof(uint64_t)))) {
			ret = 0;
			goto out;
		}
		*found_all = all_keys;
	}

	if (!num_keys) {
		ret = 1;
		goto out;
	}

	for (i = 0; i < num_keys; i++) {
		unsigned char *p = response_buf + 8 + (i * 8);

		memcpy(&key_be, p, 8);
		key = be64_to_cpu(key_be);

		log_debug("dev_find_key %s 0x%llx", devname, (unsigned long long)key);

		if (find_all && found_count && found_all)
			all_keys[i] = key;

		if (find_key && (find_key == key)) {
			if (found_key)
				*found_key = 1;
			break;
		}

		if (find_host_id && (find_host_id == (key & 0xFFFF))) {
			if (found_host_id_key)
				*found_host_id_key = key;
			break;
		}
	}
	ret = 1;
out:
	free(response_buf);
	close(fd);
	return ret;
}

static int dev_find_key(struct cmd_context *cmd, struct device *dev, int may_fail,
		        uint64_t find_key, int *found_key,
		        int find_host_id, uint64_t *found_host_id_key,
			int find_all, int *found_count, uint64_t **found_all)
{
	if (!dev_allow_pr(cmd, dev)) {
		log_error("persistent reservation not supported for device type %s", dev_name(dev));
		return 0;
	}

	if (dev_is_nvme(dev))
		return dev_find_key_nvme(cmd, dev, may_fail, find_key, found_key,
					 find_host_id, found_host_id_key,
					 find_all, found_count, found_all);
	else
		return dev_find_key_scsi(cmd, dev, may_fail, find_key, found_key,
					 find_host_id, found_host_id_key,
					 find_all, found_count, found_all);
}

static int vg_is_registered_by_key(struct cmd_context *cmd, struct volume_group *vg, uint64_t key, int *partial)
{
	struct pv_list *pvl;
	struct device *dev;
	int y = 0, n = 0, errors = 0, found;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		found = 0;

		if (!dev_find_key(cmd, dev, 0, key, &found, 0, NULL, 0, NULL, NULL)) {
			log_error("Failed to read persistent reservation key on %s", dev_name(dev));
			errors++;
			continue;
		}

		if (found)
			y++;
		else
			n++;
	}

	if (y && n)
		*partial = 1;
	if (errors)
		*partial = 1;
	return y;
}

static int vg_is_registered_by_host_id(struct cmd_context *cmd, struct volume_group *vg, int host_id, uint64_t *key, uint32_t *gen, int *partial)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t found_key = 0;
	uint64_t first_key = 0;
	uint32_t found_gen = 0;
	uint32_t first_gen = 0;
	int y = 0, n = 0, errors = 0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		found_key = 0;
		found_gen = 0;

		if (!dev_find_key(cmd, dev, 0, 0, NULL, host_id, &found_key, 0, NULL, NULL)) {
			log_error("Failed to read persistent reservation key on %s", dev_name(dev));
			errors++;
			continue;
		}

		if (!found_key) {
			n++;
			continue;
		}

		y++;

		/* verify the generation number matches on all devices */

		found_gen = (found_key & 0xFFFFFF0000) >> 16;

		if (!first_key) {
			first_key = found_key;
			first_gen = found_gen;
			continue;
		}

		if (first_key == found_key)
			continue;

		log_warn("WARNING: inconsistent reservation keys for host_id %d: 0x%llx 0x%llx (generation %u %u)",
			 host_id, (unsigned long long)first_key, (unsigned long long)found_key,
			 first_gen, found_gen);
		errors++;
	}

	if (y && n && partial)
		*partial = 1;
	if (errors && partial)
		*partial = 1;
	if (y && key)
		*key = first_key;
	if (y && gen)
		*gen = first_gen;
	return y;
}

static int vg_is_registered(struct cmd_context *cmd, struct volume_group *vg, uint64_t *our_key_ret, int *partial_ret)
{
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	uint64_t our_key_val = 0;
	uint64_t found_key = 0;
	uint32_t found_gen = 0;
	char *local_key = (char *)find_config_tree_str(cmd, local_pr_key_CFG, NULL);
	int local_host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);
	int partial = 0;

	if (!local_key && local_host_id && vg->lock_type && !strcmp(vg->lock_type, "sanlock")) {
		if (!vg_is_registered_by_host_id(cmd, vg, local_host_id, &found_key, &found_gen, &partial))
			return_0;

		if (found_key && our_key_ret)
			*our_key_ret = found_key;
		if (partial_ret)
			*partial_ret = partial;
		return 1;
	} else {
		if (!get_our_key(cmd, vg, local_key, local_host_id, our_key_buf, &our_key_val))
			return_0;

		if (!vg_is_registered_by_key(cmd, vg, our_key_val, &partial))
			return_0;

		if (our_key_ret)
			*our_key_ret = our_key_val;
		if (partial_ret)
			*partial_ret = partial;
		return 1;
	}
}

int vg_is_reserved(struct cmd_context *cmd, struct volume_group *vg, uint64_t *holder_ret, int *prtype_ret, int *partial_ret)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t holder_first = 0;
	uint64_t holder;
	int prtype_first = 0;
	int prtype;
	int y = 0, n = 0, errors = 0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		prtype = 0;
		holder = 0;

		if (!dev_read_reservation(cmd, dev, &holder, &prtype)) {
			log_error("Failed to read persistent reservation on %s", dev_name(dev));
			errors++;
			continue;
		}

		if (!prtype) {
			n++;
		} else if ((prtype == PR_TYPE_WE) && !holder) {
			log_debug("ignore prtype WE with no holder on %s", dev_name(dev));
			n++;
		} else {
			y++;
		}

		if (prtype && !prtype_first) {
			prtype_first = prtype;
		} else if (!prtype && prtype_first) {
			log_error("VG %s missing prtype on %s (other 0x%x)",
				  vg->name, dev_name(dev), prtype_first);
			errors++;
		} else if (prtype && prtype_first && (prtype != prtype_first)) {
			log_error("VG %s inconsistent prtype on %s (found 0x%x other 0x%x)",
				  vg->name, dev_name(dev), prtype, prtype_first);
			errors++;
		}

		if (holder && !holder_first) {
			holder_first = holder;
		} else if (!holder && holder_first) {
			log_error("VG %s missing reservation holder on %s (other 0x%llx)",
				  vg->name, dev_name(dev), (unsigned long long)holder_first);
			errors++;
		} else if (holder && holder_first && (holder != holder_first)) {
			log_error("VG %s inconsistent reservation holder on %s (found 0x%llx other 0x%llx)",
				  vg->name, dev_name(dev),
				  (unsigned long long)holder,
				  (unsigned long long)holder_first);
			errors++;
		}
	}

	if (prtype_ret)
		*prtype_ret = prtype_first;
	if (holder_ret)
		*holder_ret = holder_first;
	if (y && n && partial_ret)
		*partial_ret = 1;
	if (errors && partial_ret)
		*partial_ret = 1;
	return y;
}

int persist_is_started(struct cmd_context *cmd, struct volume_group *vg)
{
	uint64_t our_key_val = 0;
	uint64_t holder = 0;
	int partial_reg = 0;
	int partial_res = 0;
	int prtype = 0;
	int need_prtype = vg_is_shared(vg) ? PR_TYPE_WEAR : PR_TYPE_WE;
	int ret = 1;

	if (!vg_is_registered(cmd, vg, &our_key_val, &partial_reg)) {
		log_error("persistent reservation is not started.");
		return 0;
	}

	if (!vg_is_reserved(cmd, vg, &holder, &prtype, &partial_res)) {
		log_error("persistent reservation is not started.");
		return 0;
	}

	if (partial_reg) {
		log_error("persistent reservation key is partially registered, run vgchange --persist start %s.", vg->name);
		ret = 0;
	}

	if (partial_res) {
		log_error("persistent reservation is partially held, run vgchange --persist start %s.", vg->name);
		ret = 0;
	}

	if (prtype && (prtype != need_prtype)) {
		log_error("persistent reservation type is incorrect (found %s need %s).",
			  prtype_to_str(prtype), prtype_to_str(need_prtype));
		ret = 0;
	}

	if ((prtype == PR_TYPE_WE) && (holder != our_key_val)) {
		log_error("persistent reservation holder is not local key (found 0x%llx local 0x%llx).",
			  (unsigned long long)holder, (unsigned long long)our_key_val);
		ret = 0;
	}

	return ret;
}

static int get_our_key(struct cmd_context *cmd, struct volume_group *vg,
		       char *local_key, int local_host_id,
		       char *ret_key_buf, uint64_t *ret_key_val)
{
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	uint64_t our_key_val = 0;

	if (!local_key && local_host_id && vg->lock_type && !strcmp(vg->lock_type, "sanlock")) {
		int last_host_id = 0;
		uint32_t last_gen = 0;

		/*
		 * persist_start saves the key it uses to a local file.
		 * This provides a shortcut to let us avoid searching all
		 * the keys on devices to find our key when we need it
		 * for persist_stop, or other commands.  Without the
		 * shortcut we fall back to reading keys from devs to
		 * find the local key (one containing our host_id.)
		 */

		if (!read_key_file(cmd, vg, our_key_buf, &our_key_val, &last_host_id, &last_gen)) {
			log_debug("last key from file: none");
			goto read_keys;
		}

		if (last_host_id != local_host_id) {
			log_debug("last key from file: wrong host_id %d vs local %d", last_host_id, local_host_id);
			persist_key_file_remove(cmd, vg);
			goto read_keys;
		}

		log_debug("our key from file: 0x%llx", (unsigned long long)our_key_val);
		goto done;

 read_keys:
		/* read keys from device, looking for one with our host_id */

		memset(our_key_buf, 0, sizeof(our_key_buf));
		our_key_val = 0;

		log_debug("reading keys to find local host_id %d", local_host_id);

		if (!vg_is_registered_by_host_id(cmd, vg, local_host_id, &our_key_val, &last_gen, NULL)) {
			log_error("No registered key found for local host.");
			return 0;
		}

		if (dm_snprintf(our_key_buf, PR_KEY_BUF_SIZE-1, "0x100000%06x%04x", last_gen, local_host_id) != 18) {
			log_error("Failed to format key string for host_id %d gen %u", local_host_id, last_gen);
			return 0;
		}

		log_debug("our key from device: 0x%llx", (unsigned long long)our_key_val);

	} else if (local_key) {
		if (!parse_prkey(local_key, &our_key_val)) {
			log_error("Failed to parse local key %s", local_key);
			return 0;
		}
		if (dm_snprintf(our_key_buf, PR_KEY_BUF_SIZE-1, "0x%llx", (unsigned long long)our_key_val) < 0)
			return_0;

		log_debug("our key from arg: 0x%llx", (unsigned long long)our_key_val);

	} else if (local_host_id) {
		if (dm_snprintf(our_key_buf, PR_KEY_BUF_SIZE-1, "0x100000000000%04x", local_host_id) != 18) {
			log_error("Failed to format key string for host_id %d", local_host_id);
			return 0;
		}
		if (!parse_prkey(our_key_buf, &our_key_val)) {
			log_error("Failed to parse generated key %s", our_key_buf);
			return 0;
		}

		log_debug("our key from host_id %d: 0x%llx", local_host_id, (unsigned long long)our_key_val);
	}

 done:
	if (ret_key_buf)
		memcpy(ret_key_buf, our_key_buf, PR_KEY_BUF_SIZE);
	if (ret_key_val)
		*ret_key_val = our_key_val;
	return 1;
}

/*
 * This case of getting our key to start PR when used with a sanlock
 * shared VG is more complicated than other cases using get_our_key().
 *
 * . If the VG is already started (not expected to be the common case),
 *   then get the current generation number for the key.
 *
 * . Get the last key/gen we used from the file saved in /var/lib, or
 *   if that file is missing, look on devices to see if the last key
 *   we used is still registered there.
 *
 * . It's possible that no info is available about the last key or
 *   gen that we used, in which case we just use gen 1 in the key
 *   (which will be accurate if this is the first time joining.)
 *
 * . Create a new key using the current gen, or the last gen + 1.
 *
 * . After lockstart, the previous (and therefore the next) gen is known,
 *   so the end of lockstart checks that the correct gen was used in the
 *   key, and if not updates the key with the correct gen.
 */

static int get_our_key_sanlock_start(struct cmd_context *cmd, struct volume_group *vg, int local_host_id,
				     char *ret_key_buf, uint64_t *ret_key_val)
{
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	uint64_t our_key_val = 0;
	int last_host_id = 0;
	uint32_t cur_gen = 0;
	uint32_t last_gen = 0;
	uint32_t gen = 0;

	/*
	 * Check if the VG lockspace is already started, and if so then the
	 * current sanlock generation is already available.
	 */
	if (lockd_vg_is_started(cmd, vg, &cur_gen)) {
		log_debug("current host generation %u", cur_gen);
		last_gen = cur_gen - 1;
		goto done;
	}

	if (!read_key_file(cmd, vg, our_key_buf, &our_key_val, &last_host_id, &last_gen)) {
		log_debug("last key from file: none");
		goto read_keys;
	}

	if (last_host_id != local_host_id) {
		log_debug("last key from file: wrong host_id %d vs local %d", last_host_id, local_host_id);
		persist_key_file_remove(cmd, vg);
		goto read_keys;
	}

	log_debug("last key from file: 0x%llx gen %u", (unsigned long long)our_key_val, last_gen);
	goto done;

 read_keys:
	/* read keys from device, looking for one with our host_id */

	memset(our_key_buf, 0, sizeof(our_key_buf));
	our_key_val = 0;

	log_debug("reading keys to find local host_id %d", local_host_id);

	if (!vg_is_registered_by_host_id(cmd, vg, local_host_id, &our_key_val, &last_gen, NULL))
		last_gen = 0;

	log_debug("last key from device: 0x%llx gen %u", (unsigned long long)our_key_val, last_gen);

 done:
	/* create our key from host_id and the next generation number */

	memset(our_key_buf, 0, sizeof(our_key_buf));
	our_key_val = 0;

	gen = last_gen + 1;

	if (dm_snprintf(our_key_buf, PR_KEY_BUF_SIZE-1, "0x100000%06x%04x", gen, local_host_id) != 18) {
		log_error("Failed to format key string for host_id %d gen %u", local_host_id, gen);
		return 0;
	}

	if (!parse_prkey(our_key_buf, &our_key_val)) {
		log_error("Failed to parse generated key %s", our_key_buf);
		return 0;
	}

	log_debug("our key from host_id %d gen %u: 0x%llx",
		  local_host_id, gen, (unsigned long long)our_key_val);

	if (ret_key_buf)
		memcpy(ret_key_buf, our_key_buf, PR_KEY_BUF_SIZE);
	if (ret_key_val)
		*ret_key_val = our_key_val;
	return 1;
}

/*
 * Called after sanlock lockstart to check if a registered PR key contains the
 * latest generation number (from sanlock) for the host, and if not to update
 * the PR key.  The sanlock lockstart atually returns the previous generation
 * number that was used for this host_id in the lockspace, and we expect that
 * the next generation number just will be +1.
 *
 * In PR start, there may have been no info available about the prev key/gen,
 * in which case gen 1 was used in the key, and it likely needs to be updated
 * here.  (Generally, PR start is expected to happen before lockstart.)
 *
 * Keeping the PR key in sync with the current sanlock generation number is
 * a pain, but we do that to avoid problems from a potential race condition.
 * The common sequence for handling a host failure is expected to be:
 *
 * 1. host A fails, and begins rebooting
 * 2. host B removes A's PR key
 * 3. host A has rebooted and registers it's PR key again
 *
 * There is a potential race between steps 2 and 3.  After rebooting,
 * A may find that its key is still registered and do nothing, just
 * before host B removes A's key, which would leave A unregistered,
 * and failing to use the VG.  So, we include the host_id generation
 * number (from sanlock) in the key.  After each lockspace restart,
 * the host will have a new key value (containing the host_id and
 * generation number.)  The race is then harmless because B will
 * be removing the old key (with generation N) and A will be registering
 * its new key (with generation N+1).
 */

int persist_key_update(struct cmd_context *cmd, struct volume_group *vg, uint32_t prev_gen)
{
	struct pv_list *pvl;
	struct device *dev;
	char *local_key = (char *)find_config_tree_str(cmd, local_pr_key_CFG, NULL);
	int local_host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);
	char new_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	uint64_t our_key_val = 0;
	uint32_t key_gen = 0;
	uint32_t want_gen = prev_gen + 1;

	/*
	 * When using an explicit pr_key setting, there's
	 * not sanlock generation number that needs updating.
	 */
	if (local_key)
		return 1;

	/*
	 * Check if we are using PR on this VG.  We don't
	 * want to update our PR key if we are not already
	 * using PR for this VG.  (Could we just check for
	 * PR_REQUIRED? Or are there cases where REQUIRED
	 * is not set and we're still using PR and want to
	 * update the key here?)
	 *
	 * We are not using PR if there's no key file, which
	 * would have been created by persist_start().
	 * If there is a key file (perhaps an old one), and
	 * no PR exists on the device(s) for our host_id,
	 * then we're not using PR, and don't do a key update.
	 */

	if (!key_file_exists(cmd, vg)) {
		/* not using PR, nothing to update */
		return 1;
	}

	/*
	 * In case a previous VG with the same name left
	 * a key file behind.
	 */
	if (!strcmp(cmd->name, "vgcreate")) {
		persist_key_file_remove(cmd, vg);
		return 1;
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		/* may_fail=1 avoids errors when PR is not in use an not supported by the device */

		if (!dev_find_key(cmd, dev, 1, 0, NULL, local_host_id, &our_key_val, 0, NULL, NULL)) {
			/* not using PR, nothing to update */
			return 1;
		}

		if (!our_key_val) {
			/* not using PR, nothing to update */
			return 1;
		}

		key_gen = (our_key_val & 0xFFFFFF0000) >> 16;

		log_debug("persist_key_update found local_host_id %d key 0x%llx gen %u",
			  local_host_id, (unsigned long long)our_key_val, key_gen);
		break;
	}

	if (want_gen == key_gen) {
		/* Common case when using PR with shared VG. */
		log_debug("persist_key_update: 0x%llx already contains gen %u",
			  (unsigned long long)our_key_val, want_gen);
		return 1;
	}

	if (dm_snprintf(new_key_buf, PR_KEY_BUF_SIZE-1, "0x100000%06x%04x", want_gen, local_host_id) != 18) {
		log_error("Failed to format key string for host_id %d gen %u", local_host_id, want_gen);
		return 0;
	}

	if (!persist_start(cmd, vg, 0, new_key_buf, 0, NULL)) {
		log_error("Failed to update persistent reservation key to %s.", new_key_buf);
		return 0;
	}

	log_debug("persist_key_update: updated 0x%llx to %s", (unsigned long long)our_key_val, new_key_buf);
	return 1;
}

int persist_read(struct cmd_context *cmd, struct volume_group *vg)
{
	struct pv_list *pvl;
	struct device *dev;
	const char *devname;
	const char **argv;
	int args = 0;
	int pv_count = 0;
	int status;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		pv_count++;
	}
	if (!pv_count)
		return_0;

	if (!(argv = dm_pool_alloc(cmd->mem, (4 + pv_count*2) * sizeof(char *))))
		return_0;

	argv[0] = LVMPERSIST_PATH;
	argv[++args] = "read";
	argv[++args] = "--vg";
	argv[++args] = vg->name;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		if (!(devname = dm_pool_strdup(cmd->mem, dev_name(dev))))
			return_0;
		argv[++args] = "--device";
		argv[++args] = devname;
	}

	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("persistent reservation read failed: lvmpersist command error");
		return 0;
	}

	return 1;
}

static int _persist_check_local(struct cmd_context *cmd, struct volume_group *vg,
				char *local_key, int local_host_id)
{
	uint64_t our_key = 0;
	uint64_t holder = 0;
	uint64_t file_key = 0;
	int prtype = 0;
	int need_prtype = vg_is_shared(vg) ? PR_TYPE_WEAR : PR_TYPE_WE;
	int found_reg = 0;
	int partial_reg = 0;
	int partial_res = 0;
	int other_type;
	int other_key;
	int ret = 1;

	if (vg_is_registered(cmd, vg, &our_key, &partial_reg)) {
		found_reg = 1;

		if (partial_reg) {
			log_print_unless_silent("key 0x%llx for local host is partially registered", (unsigned long long)our_key);
			ret = 0;
		} else {
			log_print_unless_silent("key 0x%llx for local host is registered", (unsigned long long)our_key);
		}
	} else {
		if (local_key)
			log_print_unless_silent("key %s for local host is not registered", local_key);
		else
			log_print_unless_silent("key for local host_id %d is not registered", local_host_id);
		ret = 0;
	}

	if (vg_is_reserved(cmd, vg, &holder, &prtype, &partial_res)) {

		if (need_prtype == PR_TYPE_WE) {
			/* local VG */

			other_type = (prtype != PR_TYPE_WE);
			other_key = (holder != our_key);

			if (!partial_res && !other_type && !other_key) {
				log_print_unless_silent("reservation %s is held by local key 0x%llx",
							prtype_to_str(prtype), (unsigned long long)holder);
			} else {
				if (other_type && other_key) {
					log_print_unless_silent("reservation %s (expect %s) is %s by other key 0x%llx",
								prtype_to_str(prtype), prtype_to_str(PR_TYPE_WE),
								partial_res ? "partially held" : "held",
								(unsigned long long)holder);
				} else if (other_type && !other_key) {
					log_print_unless_silent("reservation %s (expect %s) is %s by local key 0x%llx",
								prtype_to_str(prtype), prtype_to_str(PR_TYPE_WE),
								partial_res ? "partially held" : "held",
								(unsigned long long)holder);
				} else if (!other_type && other_key) {
					log_print_unless_silent("reservation %s is %s by other key 0x%llx",
								prtype_to_str(prtype),
								partial_res ? "partially held" : "held",
								(unsigned long long)holder);
				} else if (!other_type && !other_key && partial_res) {
					log_print_unless_silent("reservation %s is %s by local key 0x%llx",
								prtype_to_str(prtype),
								partial_res ? "partially held" : "held",
								(unsigned long long)holder);
				} else {
					log_print_unless_silent("reservation state not recognized");
				}
				ret = 0;
			}
		} else if (need_prtype == PR_TYPE_WEAR) {
			/* shared VG */

			other_type = (prtype != PR_TYPE_WEAR);
			other_key = (holder != 0);

			if (!partial_res && !other_type && !other_key) {
				log_print_unless_silent("reservation %s is held", prtype_to_str(prtype));
			} else {
				if (other_type && other_key) {
					log_print_unless_silent("reservation %s (expect %s) is %s by key 0x%llx",
								prtype_to_str(prtype), prtype_to_str(PR_TYPE_WEAR),
								partial_res ? "partially held" : "held",
								(unsigned long long)holder);
				} else if (other_type && !other_key) {
					log_print_unless_silent("reservation %s (expect %s) is %s",
								prtype_to_str(prtype), prtype_to_str(PR_TYPE_WEAR),
								partial_res ? "partially held" : "held");
				} else if (!other_type && other_key) {
					log_print_unless_silent("reservation %s is %s by key 0x%llx",
								prtype_to_str(prtype),
								partial_res ? "partially held" : "held",
								(unsigned long long)holder);
				} else if (!other_type && !other_key && partial_res) {
					log_print_unless_silent("reservation %s is %s",
								prtype_to_str(prtype),
								partial_res ? "partially held" : "held");
				} else {
					log_print_unless_silent("reservation state not recognized");
				}
				ret = 0;
			}
		} else {
			/* non-standard config */
			log_print_unless_silent("reservation %s is %s by key 0x%llx",
						prtype_to_str(prtype),
						partial_res ? "partially held" : "held",
						(unsigned long long)holder);
			ret = 0;
		}
	} else {
		log_print_unless_silent("no reservation");
		ret = 0;
	}

	/*
	 * If our key uses sanlock generation number, check
	 * that it matches the current sanlock generation.
	 */
	if (found_reg && !local_key && local_host_id && vg->lock_type && !strcmp(vg->lock_type, "sanlock")) {
		uint32_t cur_gen = 0;
		uint32_t reg_gen = 0;

		if (lockd_vg_is_started(cmd, vg, &cur_gen)) {
			reg_gen = (our_key & 0xFFFFFF0000) >> 16;
			if (reg_gen != cur_gen) {
				log_print_unless_silent("host_id %d has incorrect key generation %u (expect %u)",
							local_host_id, reg_gen, cur_gen);
				ret = 0;
			} else {
				log_print_unless_silent("host_id %d has key generation %u", local_host_id, reg_gen);
			}
		}

		/* key file is an optimization, not strictly required, so don't fail command here */
		if (!read_key_file(cmd, vg, NULL, &file_key, NULL, NULL) || (file_key != our_key)) {
			log_print_unless_silent("updating incorrect key file value 0x%llx to 0x%llx",
						(unsigned long long)file_key, (unsigned long long)our_key);
			if (!write_key_file(cmd, vg, our_key))
				log_warn("WARNING: failed to update key file.");
		}
	}

	if (!ret)
		log_error("VG %s is not started.", vg->name);
	else
		log_print_unless_silent("VG %s is started.", vg->name);

	return ret;
}

static int _persist_check_all(struct cmd_context *cmd, struct volume_group *vg,
			      char *local_key, int local_host_id)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t *found_keys = NULL;
	uint64_t *keys;
	int need_prtype = vg_is_shared(vg) ? PR_TYPE_WEAR : PR_TYPE_WE;
	int error_prtype = 0;
	int found_prtype = 0;
	int other_prtype = 0;
	int zero_prtype = 0;
	int error_count = 0;
	int found_count = 0;
	int other_count = 0;
	int zero_count = 0;
	int other_keys = 0;
	int prtype;
	int count;
	int found;
	int i, j;
	int ret = 1;

	/* compare reservation type on all devs */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		prtype = 0;

		if (!dev_read_reservation(cmd, dev, NULL, &prtype))
			error_prtype++;
		else if (!prtype)
			zero_prtype++;
		else if (!found_prtype)
			found_prtype = prtype;
		else if (found_prtype != prtype)
			other_prtype++;
	}

	if (error_prtype) {
		log_error("check_all: error reading reservations");
		ret = 0;
	}
	if (found_prtype && (found_prtype != need_prtype)) {
		log_error("check_all: incorrect prtype");
		ret = 0;
	}
	if (zero_prtype && found_prtype) {
		log_error("check_all: incomplete device reservations");
		ret = 0;
	}
	if (other_prtype) {
		log_error("check_all: differing prtypes");
		ret = 0;
	}

	if (zero_prtype && !found_prtype)
		log_print_unless_silent("check_all: no reservation");
	else if (found_prtype)
		log_print_unless_silent("check_all: reservation type %s", prtype_to_str(found_prtype));

	/* compare registered keys on all devs */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		count = 0;
		keys = NULL;

		if (!dev_find_key(cmd, dev, 0, 0, NULL, 0, NULL, 1, &count, &keys)) {
			error_count++;
			continue;
		}

		if (count && !keys) {
			error_count++;
			continue;
		}

		if (!count) {
			zero_count++;
			continue;
		}

		if (!found_count) {
			found_count = count;
			found_keys = keys;
			continue;
		}

		if (found_count != count) {
			other_count++;
			continue;
		}

		for (i = 0; i < found_count; i++) {
			found = 0;
			for (j = 0; j < found_count; j++) {
				if (found_keys[i] == keys[j]) {
					found = 1;
					break;
				}
			}
			if (!found) {
				other_keys++;
				break;
			}
		}
		dm_pool_free(cmd->mem, keys);
	}

	if (error_count) {
		log_error("check_all: error reading registrations");
		ret = 0;
	}
	if (zero_count && found_count) {
		log_error("check_all: incomplete device registrations");
		ret = 0;
	}
	if (other_count) {
		log_error("check_all: differing registered key counts");
		ret = 0;
	}
	if (other_keys) {
		log_error("check_all: differing registered keys");
		ret = 0;
	}
	if (zero_prtype && found_count) {
		log_error("check_all: registered keys with no reservation");
		ret = 0;
	}

	if (zero_count && !found_count)
		log_print_unless_silent("check_all: no registrations");
	else if (found_count)
		log_print_unless_silent("check_all: registered key count %d", found_count);

	if (found_keys)
		dm_pool_free(cmd->mem, found_keys);

	return ret;
}

int persist_check(struct cmd_context *cmd, struct volume_group *vg,
		  const char *op, char *local_key, int local_host_id)
{
	/* check if the local key and reservation exists on all devices */
	if (!strcmp(op, "check"))
		return _persist_check_local(cmd, vg, local_key, local_host_id);

	/* check if all keys and reservations match on all devices */
	if (!strcmp(op, "check_all"))
		return _persist_check_all(cmd, vg, local_key, local_host_id);

	log_error("unknown persist action");
	return 0;
}

static int _run_stop(struct cmd_context *cmd, struct volume_group *vg, char *our_key_str, int cleanup)
{
	struct pv_list *pvl;
	struct device *dev;
	const char *devname;
	const char **argv;
	int args = 0;
	int pv_count = 0;
	int status;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		pv_count++;
	}
	if (!pv_count)
		return_0;

	if (!(argv = dm_pool_alloc(cmd->mem, (7 + pv_count*2) * sizeof(char *))))
		return_0;

	argv[0] = LVMPERSIST_PATH;
	argv[++args] = "stop";
	argv[++args] = "--ourkey";
	argv[++args] = our_key_str;
	argv[++args] = "--vg";
	argv[++args] = vg->name;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		if (!(devname = dm_pool_strdup(cmd->mem, dev_name(dev))))
			return_0;
		argv[++args] = "--device";
		argv[++args] = devname;
	}

	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		if (!cleanup)
			log_error("persistent reservation stop failed: lvmpersist command error");
		return 0;
	}

	return 1;
}

int persist_stop(struct cmd_context *cmd, struct volume_group *vg)
{
	char *local_key = (char *)find_config_tree_str(cmd, local_pr_key_CFG, NULL);
	int local_host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	uint64_t our_key_val = 0;
	uint32_t cur_gen = 0;

	if (!local_key && !local_host_id)
		return 1;

	if (lockd_vg_is_started(cmd, vg, &cur_gen)) {
		log_error("VG %s locking should be stopped before PR (vgchange --lockstop)", vg->name);
		return 0;
	}

	if (!get_our_key(cmd, vg, local_key, local_host_id, our_key_buf, &our_key_val))
		return_0;

	if (!_run_stop(cmd, vg, our_key_buf, 0))
		return 0;

	return 1;
}

static int _persist_extend_shared(struct cmd_context *cmd, struct volume_group *vg,
			  	  uint64_t our_key_val, struct device *check_dev)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t *old_vals = NULL;
	uint64_t *new_vals;
	int old_count = 0;
	int new_count;
	int prtype = 0;
	int error = 0;
	int found;
	int i, j;

	/*
	 * All hosts using the shared VG need to start PR on the new devs, not
	 * just the host running vgextend.  For shared VGs, require the user to
	 * use lvmpersist to start PR on the new devices from all hosts before
	 * running vgextend.  Verify that has been done here, checking that all
	 * the new devs have registrations/reservations set up from the user
	 * running lvmpersist, and matching the PR found on an existing device.
	 * Return 1 if PR has been set up on the new devs to match the old devs,
	 * otherwise return 0 and fail to vgextend.
	 */

	/*
	 * Check for reservation on new devs.
	 */

	dm_list_iterate_items(pvl, &vg->pv_write_list) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		if (!dev_read_reservation(cmd, dev, NULL, &prtype)) {
			log_error("PR not found on %s", dev_name(dev));
			return 0;
		}

		if (!prtype) {
			log_error("PR is not started on %s.", dev_name(dev));
			log_error("(Use lvmpersist to start PR on new devices from all hosts, prior to vgextend.)");
			return 0;
		}

		if (prtype != PR_TYPE_WEAR) {
			log_error("PR type %s (expect WEAR) found on %s",
				  prtype_to_str(prtype), dev_name(dev));
			return 0;
		}
	}

	/*
	 * Get keys from an existing/old device to use for
	 * checking that the new devs have the same keys.
	 */

	if (!dev_find_key(cmd, check_dev, 0, 0, NULL, 0, NULL, 1, &old_count, &old_vals)) {
		log_error("PR keys not found on %s", dev_name(check_dev));
		return 0;
	}

	/*
	 * Check for registered keys on new devs.
	 */

	dm_list_iterate_items(pvl, &vg->pv_write_list) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		new_count = 0;
		new_vals = NULL;

		if (!dev_find_key(cmd, dev, 0, 0, NULL, 0, NULL, 1, &new_count, &new_vals)) {
			log_error("PR keys not found on %s", dev_name(dev));
			error = 1;
			goto next;
		}

		/*
		 * Check if our key is on the new device.
		 */
		found = 0;

		for (i = 0; i < new_count; i++) {
			if (new_vals[i] == our_key_val) {
				found = 1;
				break;
			}
		}
		if (!found) {
			log_error("Local PR key 0x%llx not found on %s",
				  (unsigned long long)our_key_val, dev_name(dev));
			error = 1;
			goto next;
		}

		if (new_count != old_count) {
			log_error("PR keys incomplete (found %d of %d) on %s",
				  new_count, old_count, dev_name(dev));
			error = 1;
			goto next;
		}

		log_debug("checking for %d PR keys on %s", new_count, dev_name(dev));

		for (i = 0; i < old_count; i++) {
			found = 0;
			for (j = 0; j < old_count; j++) {
				if (old_vals[i] == new_vals[j]) {
					found = 1;
					break;
				}
			}
			if (!found) {
				log_error("PR key 0x%llx not found on %s",
					  (unsigned long long)old_vals[i], dev_name(dev));
				error = 1;
			}
		}
 next:
		dm_pool_free(cmd->mem, new_vals);
	}

	log_debug("Found PR on all new devs");
	dm_pool_free(cmd->mem, old_vals);

	return error ? 0 : 1;
}

/*
 * Return 1:
 * if PR is not in use on existing PVs (so nothing to do here),
 * or if PR is already started on the new PVs,
 * or if this is successful at starting PR on new PVs.
 */

int persist_start_extend(struct cmd_context *cmd, struct volume_group *vg)
{
	char *local_key = (char *)find_config_tree_str(cmd, local_pr_key_CFG, NULL);
	int local_host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);
	struct pv_list *pvl;
	struct device *dev = NULL;
	struct device *check_dev = NULL;
	uint64_t our_key_val = 0;
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	const char *devname;
	const char **argv;
	int status;
	int args = 0;
	int pv_count = 0;
	int errors = 0;
	int found = 0;
	int y = 0;
	int n = 0;

	/*
	 * PR is not in use without pr_key or host_id set.
	 */
	if (!local_key && !local_host_id)
		return 1;

	/*
	 * If there is no valid PR key, then PR must not be in use.
	 */
	if (!get_our_key(cmd, vg, local_key, local_host_id, our_key_buf, &our_key_val))
		return 1;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		check_dev = dev;
		break;
	}
	if (!check_dev)
		return 1;

	/*
	 * If REQUIRE is set, then persist_is_started() has already run and
	 * verified that PR is started on existing devices, now do new devs.
	 */
	if (vg->pr & VG_PR_REQUIRE)
		goto do_extend;

	/*
	 * If REQUIRE is not set, PR could still be in use.  Check if our key
	 * is registered on any device.  If so, then PR is in use.  If not, PR
	 * not in use.
	 */
	if (!dev_find_key(cmd, check_dev, 0, our_key_val, &found, 0, NULL, 0, NULL, NULL))
		return 1;
	if (!found)
		return 1;

 do_extend:

	dm_list_iterate_items(pvl, &vg->pv_write_list) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (!dev_allow_pr(cmd, dev)) {
			log_error("persistent reservation not supported for device type %s", dev_name(dev));
			return 0;
		}
	}

	/*
	 * For local VGs, vgextend starts PR on the new devs (here.)
	 * For shared VGs, the user must start PR on the new devs using
	 * lvmpersist (from all hosts) before running vgextend.
	 */

	if (vg_is_shared(vg))
		return _persist_extend_shared(cmd, vg, our_key_val, check_dev);

	dm_list_iterate_items(pvl, &vg->pv_write_list) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		pv_count++;
	}
	if (!pv_count)
		return_0;

	log_debug("start PR on %d new devs with local key %llx", pv_count, (unsigned long long)our_key_val);

	args = 9 + pv_count*2;

	if (!(argv = dm_pool_alloc(cmd->mem, args * sizeof(char *))))
		return_0;

	args = 0;
	argv[0] = LVMPERSIST_PATH;
	argv[++args] = "start";
	argv[++args] = "--ourkey";
	argv[++args] = our_key_buf;
	argv[++args] = "--prtype";
	argv[++args] = prtype_to_str(PR_TYPE_WE);
	argv[++args] = "--vg";
	argv[++args] = vg->name;

	dm_list_iterate_items(pvl, &vg->pv_write_list) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		if (!(devname = dm_pool_strdup(cmd->mem, dev_name(dev))))
			return_0;
		argv[++args] = "--device";
		argv[++args] = devname;
	}

	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("persistent reservation start failed: lvmpersist command error");
		return 0;
	}

	dm_list_iterate_items(pvl, &vg->pv_write_list) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;

		found = 0;

		if (!dev_find_key(cmd, dev, 0, our_key_val, &found, 0, NULL, 0, NULL, NULL)) {
			log_error("Failed to read persistent reservation key on %s", dev_name(dev));
			errors++;
			continue;
		}

		if (found)
			y++;
		else
			n++;
	}

	if (n || errors)
		return 0;
	return 1;
}

int persist_start(struct cmd_context *cmd, struct volume_group *vg,
		  int prtype, char *local_key, int local_host_id, const char *remkey)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t our_key_val = 0;
	uint64_t rem_key_val = 0;
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	char rem_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	const char *devname;
	const char **argv;
	uint64_t holder = 0;
	int partial_reg = 0;
	int partial_res = 0;
	int found_prtype = 0;
	int args = 0;
	int pv_count = 0;
	int status;

	if (!local_key && !local_host_id) {
		log_error("No pr_key or host_id configured (see lvmlocal.conf).");
		return 0;
	}

	if (remkey) {
		if (!parse_prkey(remkey, &rem_key_val)) {
			log_error("Invalid removekey value: %s.", remkey);
			return 0;
		}
		if (dm_snprintf(rem_key_buf, PR_KEY_BUF_SIZE-1, "0x%llx", (unsigned long long)rem_key_val) < 0)
			return_0;
	}

	if (!prtype)
		prtype = vg_is_shared(vg) ? PR_TYPE_WEAR : PR_TYPE_WE;

	if (!local_key && local_host_id && vg->lock_type && !strcmp(vg->lock_type, "sanlock")) {
		if (!get_our_key_sanlock_start(cmd, vg, local_host_id, our_key_buf, &our_key_val)) {
			log_error("Failed to create a local key.");
			return 0;
		}
	} else {
		if (!get_our_key(cmd, vg, local_key, local_host_id, our_key_buf, &our_key_val)) {
			log_error("Failed to create a local key.");
			return 0;
		}
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (!dev_allow_pr(cmd, dev)) {
			log_error("persistent reservation not supported for device type %s", dev_name(dev));
			return 0;
		}
		pv_count++;
	}
	if (!pv_count)
		return_0;

	log_debug("start PR on %d devs with local key %llx", pv_count, (unsigned long long)our_key_val);

	args = 9 + pv_count*2;
	if (vg->pr & VG_PR_PTPL)
		args += 1;
	if (remkey)
		args += 2;

	if (!(argv = dm_pool_alloc(cmd->mem, args * sizeof(char *))))
		return_0;

	args = 0;
	argv[0] = LVMPERSIST_PATH;
	argv[++args] = "start";
	argv[++args] = "--ourkey";
	argv[++args] = our_key_buf;
	argv[++args] = "--prtype";
	argv[++args] = prtype_to_str(prtype);
	argv[++args] = "--vg";
	argv[++args] = vg->name;
	if (vg->pr & VG_PR_PTPL)
		argv[++args] = "--ptpl";
	if (remkey) {
		argv[++args] = "--removekey";
		argv[++args] = rem_key_buf;
	}

	/* 
	 * The list of devices is already known here, so by supplying them,
	 * lvmpersist can avoid running another lvm command to get the list
	 * from the VG name.  We still provide the VG name so that lvmpersist
	 * can use it in log messages.
	 */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		if (!(devname = dm_pool_strdup(cmd->mem, dev_name(dev))))
			return_0;
		argv[++args] = "--device";
		argv[++args] = devname;
	}

	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("persistent reservation start failed: lvmpersist command error");
		return 0;
	}

	/* Verify */

	if (!vg_is_registered_by_key(cmd, vg, our_key_val, &partial_reg)) {
		log_error("persistent reservation start failed: key not registered");
		goto out_stop;
	}

	if (partial_reg) {
		log_error("persistent reservation start failed: key not registered on all devices");
		goto out_stop;
	}

	if (!vg_is_reserved(cmd, vg, &holder, &found_prtype, &partial_res)) {
		log_error("persistent reservation start failed: reservation not found");
		goto out_stop;
	}

	if (partial_res) {
		log_error("persistent reservation start failed: reservation not found on all devices");
		goto out_stop;
	}

	if (found_prtype != prtype) {
		log_error("persistent reservation start failed: reservation type not correct %s expect %s",
			  prtype_to_str(found_prtype), prtype_to_str(prtype));
		goto out_stop;
	}

	if ((prtype == PR_TYPE_WE) && (holder != our_key_val)) {
		log_error("persistent reservation start failed: reservation holder 0x%llx is not local key 0x%llx",
			  (unsigned long long)holder, (unsigned long long)our_key_val);
		goto out_stop;
	}

	/* key file is an optimization, not an error condition */
	if (!write_key_file(cmd, vg, our_key_val))
		stack;

	return 1;

 out_stop:
	/* try to clean up any parts of start that were successful */
	_run_stop(cmd, vg, our_key_buf, 1);
	return_0;
}

int persist_remove(struct cmd_context *cmd, struct volume_group *vg,
		   char *local_key, int local_host_id, const char *remkey)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t our_key_val = 0;
	uint64_t rem_key_val = 0;
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	char rem_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	int prtype = vg_is_shared(vg) ? PR_TYPE_WEAR : PR_TYPE_WE;
	const char *devname;
	const char **argv;
	int args = 0;
	int pv_count = 0;
	int status;

	if (!remkey) {
		log_error("A key to remove is required (see --removekey).");
		return 0;
	}

	if (!parse_prkey(remkey, &rem_key_val)) {
		log_error("Invalid key value: %s.", remkey);
		return 0;
	}

	if (dm_snprintf(rem_key_buf, PR_KEY_BUF_SIZE-1, "0x%llx", (unsigned long long)rem_key_val) < 0)
		return_0;

	if (!get_our_key(cmd, vg, local_key, local_host_id, our_key_buf, &our_key_val))
		return_0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		pv_count++;
	}
	if (!pv_count)
		return_0;

	if (!(argv = dm_pool_alloc(cmd->mem, (11 + pv_count*2) * sizeof(char *))))
		return_0;

	argv[0] = LVMPERSIST_PATH;
	argv[++args] = "remove";
	argv[++args] = "--ourkey";
	argv[++args] = our_key_buf;
	argv[++args] = "--removekey";
	argv[++args] = rem_key_buf;
	argv[++args] = "--prtype";
	argv[++args] = prtype_to_str(prtype);
	argv[++args] = "--vg";
	argv[++args] = vg->name;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		if (!(devname = dm_pool_strdup(cmd->mem, dev_name(dev))))
			return_0;
		argv[++args] = "--device";
		argv[++args] = devname;
	}

	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("persistent reservation remove failed: lvmpersist command error");
		return 0;
	}

	/* lvmpersist remove verifies that the key was removed. */

	return 1;
}

int persist_clear(struct cmd_context *cmd, struct volume_group *vg,
		  char *local_key, int local_host_id)
{
	struct pv_list *pvl;
	struct device *dev;
	uint64_t our_key_val = 0;
	char our_key_buf[PR_KEY_BUF_SIZE] = { 0 };
	const char *devname;
	const char **argv;
	int args = 0;
	int pv_count = 0;
	int status;

	if (!get_our_key(cmd, vg, local_key, local_host_id, our_key_buf, &our_key_val))
		return_0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		pv_count++;
	}
	if (!pv_count)
		return_0;

	if (!(argv = dm_pool_alloc(cmd->mem, (7 + pv_count*2) * sizeof(char *))))
		return_0;

	argv[0] = LVMPERSIST_PATH;
	argv[++args] = "clear";
	argv[++args] = "--ourkey";
	argv[++args] = our_key_buf;
	argv[++args] = "--vg";
	argv[++args] = vg->name;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(dev = pvl->pv->dev))
			continue;
		if (dm_list_empty(&dev->aliases))
			continue;
		if (!(devname = dm_pool_strdup(cmd->mem, dev_name(dev))))
			return_0;
		argv[++args] = "--device";
		argv[++args] = devname;
	}

	argv[++args] = NULL;

	if (!exec_cmd(cmd, argv, &status, 1)) {
		log_error("persistent reservation clear failed: lvmpersist command error");
		return 0;
	}

	/* lvmpersist clear verifies that the reservation and keys are gone. */

	return 1;
}
