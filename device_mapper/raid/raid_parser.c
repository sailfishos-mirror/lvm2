/*
 * Support counting number of failed device bits in dm-raid superblock bit arrays or clear them out.
 */

#include "device_mapper/misc/dmlib.h"
#include "device_mapper/all.h"
#include "device_mapper/raid/target.h"
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

/* Copied/derived from kernel's drivers/md/dm-raid.c so this is prone to out-of-sync (factor out to header file?). */
#define	MAX_RAID_DEVICES		253 /* md-raid kernel limit? */
#define UINT64_BITS			(sizeof(uint64_t) * 8)
#define DISKS_ARRAY_ELEMS		((MAX_RAID_DEVICES + (UINT64_BITS - 1)) / UINT64_BITS)
#define DM_RAID_SB_MAGIC		0x446D5264 /* "DmRd" */
#define	FEATURE_FLAG_SUPPORTS_V190	0x1 /* Supports extended superblock */

/* RAID superblock at beginning of rmeta SubLVs trimmed down to mandatory members. */
struct dm_raid_superblock {
	__le32 magic;		/* "DmRd" */
	__le32 compat_features;	/* Used to indicate compatible features (like 1.9.0 ondisk metadata extension) */
	__le32 dummy[4];
	__le64 failed_devices;	/* Pre 1.9.0 part of bit field of devices to */
				/* indicate device failures (see extension below) */
	__le32 dummy1[7];

	/********************************************************************
	 * BELOW FOLLOW V1.9.0 EXTENSIONS TO THE PRISTINE SUPERBLOCK FORMAT!!!
	 *
	 * FEATURE_FLAG_SUPPORTS_V190 in the compat_features member indicates that those exist
	 */
	__le32 flags; /* Flags defining array states for reshaping */
	__le32 dummy2[14];
	__le64 extended_failed_devices[DISKS_ARRAY_ELEMS - 1];

	__le32 dummy3;
	/* Always set rest up to logical block size to 0 when writing ... */
} __packed;
/* END: Copied from ... */

/* Superblock I/O buffer size to be able to Cope with 4K native devices... */
#define	SB_BUFSZ	4096

static size_t _get_sb_size(const struct dm_raid_superblock *sb)
{
	return (FEATURE_FLAG_SUPPORTS_V190 & le32toh(sb->compat_features)) ?
	       sizeof(*sb) : ((char *) &sb->flags - (char *) sb);
}

static uint32_t _hweight64(__le64 v)
{
	uint32_t r = 0;

	while (v) {
		r += v & 1;
		v >>= 1;
	}

	return r;
}

static uint32_t _hweight_failed(struct dm_raid_superblock *sb)
{
	size_t sz = _get_sb_size(sb);
	uint32_t r = _hweight64(sb->failed_devices);

	if (sz == sizeof(*sb)) {
		int i = sizeof(sb->extended_failed_devices) / sizeof(*sb->extended_failed_devices);

		while (i--) {
			uint32_t hw =  _hweight64(sb->extended_failed_devices[i]);

			if (hw > r)
				r = hw;
		}
	}
	
	return r;
}

static void _clear_failed_devices(struct dm_raid_superblock *sb)
{
	size_t sz = _get_sb_size(sb);

	sb->failed_devices = 0;

	if (sz == sizeof(*sb)) {
		int i = sizeof(sb->extended_failed_devices) / sizeof(*sb->extended_failed_devices);

		while (i--)
			sb->extended_failed_devices[i] = 0;
	}
}

static int _count_or_clear_failed_devices(const char *dev_path, bool clear, uint32_t *nr_failed)
{
	int fd, r;
	struct dm_raid_superblock *sb = NULL;

	if (posix_memalign((void *) &sb, SB_BUFSZ, SB_BUFSZ)) {
		log_error("Failed to allocate RAID superblock buffer for %s", dev_path);
		return 0;
	}

	fd = open(dev_path, O_EXCL | O_RDWR | O_DIRECT);
	if (fd < 0) {
		if (clear) {
			log_error("Failed to open hidden RAID metadata SubLV %s", dev_path);
			r = 0;
		} else
			r = 1;

		goto out;
	}

	if (read(fd, sb, SB_BUFSZ) == SB_BUFSZ) {
		/* FIXME: big endian??? */
		if (sb->magic != htobe32(DM_RAID_SB_MAGIC)) {
			log_error("No RAID signature on %s", dev_path);
			r = 0;
			goto out;
		}

		if (nr_failed)
{
			*nr_failed = _hweight_failed(sb);
log_print("%s[%u] nr_failed=%u", __func__, __LINE__, *nr_failed);
}

		if (clear) {
			r = (lseek(fd, 0, SEEK_SET) < 0) ? 0 : 1;
			if (r) {
				size_t sz = _get_sb_size(sb);

				memset((void *)((char *) sb + sz), 0, SB_BUFSZ - sz);
				_clear_failed_devices(sb);
				r = write(fd, sb, SB_BUFSZ) == SB_BUFSZ;
			}
		} else
			r = 1;
	} else
		r = !clear;
out:
	free(sb);
	close(fd);
	return r;
}

int dm_raid_count_failed_devices(const char *dev_path, uint32_t *nr_failed)
{
	return _count_or_clear_failed_devices(dev_path, false, nr_failed);
}

int dm_raid_clear_failed_devices(const char *dev_path, uint32_t *nr_failed)
{
	return _count_or_clear_failed_devices(dev_path, true, nr_failed);
}
//----------------------------------------------------------------
