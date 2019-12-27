/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _FILE_OFFSET_BITS 64

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <fnmatch.h>
#include <inttypes.h>

#include "../overlay_partition.h"
#include "libfstools.h"
#include "volume.h"

#include <linux/loop.h>

#define ROOTDEV_OVERLAY_ALIGN	(64ULL * 1024ULL)

#ifdef OVL_F2FS_ENABLE
#define F2FS_MINSIZE		(100ULL * 1024ULL * 1024ULL)
#endif

static int rootdisk_volume_identify_p(const char *dev, uint64_t offset);

struct squashfs_super_block {
	uint32_t s_magic;
	uint32_t pad0[9];
	uint64_t bytes_used;
};

struct rootdev_volume {
	int no_overlay_create;
	struct volume v;
	uint64_t offset;
	char dev_name[32];
};

static const char *rootdev;

#ifdef OVL_ROOTDISK_PART_ENABLE

/* Minimum overlay size (1 MiB) */
#define OVL_MINSIZE (1ULL * 1024ULL * 1024ULL)

struct blkdev_diskpart_info {
	uint64_t start; /* start offset in bytes */
	uint64_t size;  /* size in bytes */
	int partition;  /* partition number (1...4) */
	int ro;         /* partition has ro flag setted? */
	int rootfs;     /* rootfs partition? */
	char dev[64];   /* partition device */
};

struct blkdev_disk_info {
	/* partitions */
	struct blkdev_diskpart_info partition[4];

	const char *dev;         /* device */
	const char *devname;     /* device name without "/dev/" prefix */
	uint64_t hw_sector_size; /* sector size */
	uint64_t size;           /* disk size in bytes */
	int partitioned;         /* device has partitions? */
	int rootpart;            /* rootfs partition number (1...4) */
	int ro;                  /* entire disk has ro flag setted? */
};

static struct blkdev_disk_info rootdisk_info;
static int ovl_partition = 0;

#endif

static struct driver rootdisk_driver;

static char *get_blockdev(dev_t dev)
{
	const char *dirname = "/dev";
	DIR *dir = opendir(dirname);
	struct dirent *d;
	struct stat st;
	static char buf[256];
	char *ret = NULL;

	if (!dir)
		return ret;

	while ((d = readdir(dir)) != NULL) {
		snprintf(buf, sizeof(buf), "%s/%s", dirname, d->d_name);

		if (lstat(buf, &st) != 0)
			continue;

		if (!S_ISBLK(st.st_mode))
			continue;

		if (st.st_rdev != dev)
			continue;

		ret = buf;
		break;
	}

	closedir(dir);
	return ret;
}

static char *get_rootdev(const char *dir)
{
	struct stat st;

	if (stat(dir, &st))
		return NULL;

	return get_blockdev(S_ISBLK(st.st_mode) ? st.st_rdev : st.st_dev);
}

static int get_squashfs(struct squashfs_super_block *sb)
{
	FILE *f;
	int len;

	f = fopen(rootdev, "r");
	if (!f)
		return -1;

	len = fread(sb, sizeof(*sb), 1, f);
	fclose(f);

	if (len != 1)
		return -1;

	return 0;
}

#ifdef OVL_F2FS_ENABLE
static bool rootdisk_use_f2fs(struct rootdev_volume *p)
{
	uint64_t size = 0;
	bool ret = false;
	int fd;

	fd = open((p->offset == 0) ? p->dev_name : rootdev, O_RDONLY);
	if (ioctl(fd, BLKGETSIZE64, &size) == 0)
		ret = size - p->offset > F2FS_MINSIZE;
	close(fd);

	return ret;
}
#endif

#ifdef OVL_ROOTDISK_PART_ENABLE
static size_t file_read(const char *path, void *buf, size_t size)
{
	size_t n;

	FILE *f;
	f = fopen(path, "r");
	if (!f)
		return 0;

	n = fread(buf, 1, size, f);
	if (ferror(f)) {
		fclose(f);
		return 0;
	}

	fclose(f);
	return n;
}

static int file_read_u64(const char *path, uint64_t *value)
{
	size_t n;
	char buf[32] = { 0 };

	n = file_read(path, buf, sizeof(buf) - 1);
	if (n == 0)
		return -1;

	buf[n] = 0;
	*value = strtoull(buf, NULL, 0);

	return 0;
}

static int parse_cmdline()
{
	char buf[256] = { 0 };
	char *p;

	if (file_read("/proc/cmdline", buf, sizeof(buf) - 1) == 0)
		return 0;

	p = strstr(buf, "ovl-partition=");
	if (!p)
		p = strstr(buf, "ovl_partition=");
	if (p) {
		p = strstr(p, "=");
		ovl_partition = (int)strtol(p + 1, NULL, 0);
		if ((ovl_partition < 1) || (ovl_partition > 4)) {
			ovl_partition = 0;
			ULOG_ERR("wrong 'ovl-partition' value (should be in the range from 1 to 4)\n");
		} else {
			ULOG_INFO("try to use ovl-partition %d for overlay filesystem\n", ovl_partition);
		}
	}

	return 0;
}

static const char *blkdev_get_devname(const char *dev)
{
	const char *devname;

	if (!dev)
		return NULL;

	/* Get root device name without "/dev/" */
	devname = strrchr(dev, '/');
	if (devname)
		devname++;
	else
		devname = dev;

	return devname;
}

#define BLKDEV_PARENT_PREFIX      "/block/"
#define BLKDEV_PARENT_PREFIX_LEN  7

static char *blkdev_get_partition_disk_dev(const char *devname)
{
	static char buf[256];
	static char link[256];
	char *p1, *p2;

	snprintf(buf, sizeof(buf), "/sys/class/block/%s", devname);
	if (readlink(buf, link, sizeof(link)) == -1)
		return NULL;

	p1 = strstr(link, BLKDEV_PARENT_PREFIX);
	p2 = strstr(link, devname);

	if (!p1 || !p2)
		return NULL;

	if (p2 == p1 + BLKDEV_PARENT_PREFIX_LEN)
		return NULL;

	p2[-1] = 0;

	/* Add /dev/ prefix */
	p1 += (BLKDEV_PARENT_PREFIX_LEN - 5); /* 5 = length of "/dev/" */
	memcpy(p1, "/dev/", 5);

	return p1;
}

static int rootdisk_scan_partitions(
	struct blkdev_disk_info *info)
{
	DIR *dir;
	struct dirent *d;
	uint64_t partition;
	uint64_t u64;

	static char pattern[64];
	static char buf[256];

	dir = opendir("/dev");
	if (!dir)
		return -1;

	snprintf(pattern, sizeof(pattern), "%s*", info->devname);

	while ((d = readdir(dir)) != NULL) {
		if (fnmatch(pattern, d->d_name, 0) == 0) {
			struct blkdev_diskpart_info *part;

			snprintf(buf, sizeof(buf), "/sys/class/block/%s/partition", d->d_name);
			if (file_read_u64(buf, &partition) != 0)
				continue; /* device is not partition */

			if ((partition < 1) || (partition > 4))
				continue; /* invalid partition number */

			part = &info->partition[partition - 1];

			snprintf(part->dev, sizeof(part->dev), "/dev/%s", d->d_name);

			/* rootfs */
			if (strcmp(part->dev, rootdev) == 0) {
				part->rootfs = 1;
				info->rootpart = partition;
			}

			/* partition start offset */
			snprintf(buf, sizeof(buf), "/sys/class/block/%s/start", d->d_name);
			if (file_read_u64(buf, &u64) != 0)
				continue;

			part->start = u64 * info->hw_sector_size;

			/* partition size */
			snprintf(buf, sizeof(buf), "/sys/class/block/%s/size", d->d_name);
			if (file_read_u64(buf, &u64) != 0)
				continue;

			part->size = u64 * info->hw_sector_size;

			/* ro */
			snprintf(buf, sizeof(buf), "/sys/class/block/%s/ro", d->d_name);
			if (file_read_u64(buf, &u64) != 0)
				continue;

			part->ro = (int)u64;
			part->partition = partition;
			info->partitioned = 1;
		}
	}

	closedir(dir);
	return 0;
}


static int blkdev_get_disk_info(
	const char *rootdev,
	struct blkdev_disk_info *info)
{
	const char *rootdevname;
	static char buf[512];
	uint64_t u64;
	int part;

	if (!rootdev)
		return -1;

	rootdevname = blkdev_get_devname(rootdev);
	if (!rootdevname)
		return -1;

	memset(info, 0, sizeof(struct blkdev_disk_info));

	info->dev = blkdev_get_partition_disk_dev(rootdevname);
	if (info->dev) {
		/* rootfs is on partition */
		info->devname = blkdev_get_devname(info->dev);
		if (!info->devname)
			return -1;

		snprintf(buf, sizeof(buf), "/sys/class/block/%s/partition", info->devname);
		if (file_read_u64(buf, &u64) == 0)
			return -1; /* rootdisk device is partition */
	} else {
		/* rootfs is on entrire device */
		info->dev     = rootdev;
		info->devname = rootdevname;
	}

	snprintf(buf, sizeof(buf), "/sys/class/block/%s/queue/hw_sector_size", info->devname);
	if (file_read_u64(buf, &info->hw_sector_size))
		return -1;

	snprintf(buf, sizeof(buf), "/sys/class/block/%s/size", info->devname);
	if (file_read_u64(buf, &info->size))
		return -1;

	info->size = info->size * info->hw_sector_size;

	snprintf(buf, sizeof(buf), "/sys/class/block/%s/ro", info->devname);
	if (file_read_u64(buf, &u64) == 0)
		info->ro = (int)u64;

	if (rootdisk_scan_partitions(info) != 0)
		return -1;

	/* Output partition table */
	for (part = 0; part < 4; part++) {
		if (info->partition[part].partition != (part + 1))
			continue;

		ULOG_INFO("%s: p%d, %s, start %"PRIu64", size %"PRIu64"%s%s\n",
			info->partition[part].dev[0]
				? info->partition[part].dev : info->dev,
			part + 1,
			info->partition[part].ro ? "ro" : "rw",
			info->partition[part].start,
			info->partition[part].size,
			info->partition[part].rootfs ? " [rootfs]" : "",
			(ovl_partition == (part + 1)) ? " [overlayfs]" : ""
		);
	}

	return 0;
}

static int rootdisk_volume_cmp_partition_label(
	const char *dev, uint64_t offset, const char *name)
{
	int id;

	char volume_name[128] = { 0 };
	size_t n;
	FILE *f;

	id = rootdisk_volume_identify_p(dev, offset);

	f = fopen(dev, "r");
	if (!f)
		return -1;

	/* read volume name */
	if (id == FS_EXT4) {
		fseeko(f, offset + 0x478, SEEK_SET);
		n = fread(volume_name, 16, 1, f);
		if (n != 1) {
			fclose(f);
			return -1;
		}
		volume_name[16] = 0;
	}
#ifdef OVL_F2FS_ENABLE
	else if (id == FS_F2FS) {
		fseeko(f, offset + 0x47c, SEEK_SET);
		n = fread(volume_name, 127, 1, f);
		if (n != 1) {
			fclose(f);
			return -1;
		}
		volume_name[127] = 0;
	}
#endif

	fclose(f);
	return strcmp(volume_name, name);
}

static int rootdisk_volume_find_partition(
	const char *name,
	struct rootdev_volume *p
)
{
	int part;

	for (part = 0; part < 4; part++) {
		if (!rootdisk_info.partition[part].partition)
			continue;

		if (rootdisk_volume_cmp_partition_label(
			rootdisk_info.partition[part].dev, 0ULL, name) == 0) {
			ULOG_INFO("founded suitable overlay partition %s\n",
				rootdisk_info.partition[part].dev);
			strcpy(p->dev_name, rootdisk_info.partition[part].dev);
			p->offset = 0ULL;
			return 0;
		}
	}

	return -1;
}

#endif /* #ifdef OVL_ROOTDISK_PART_ENABLE */

static struct volume *rootdisk_volume_find(char *name)
{
	struct squashfs_super_block sb;
	struct rootdev_volume *p;

	if (strcmp(name, get_overlay_partition()) != 0)
		return NULL;

	if (!rootdev)
		rootdev = get_rootdev("/");
	if (!rootdev)
		rootdev = get_rootdev("/rom");
	if (!rootdev)
		return NULL;

	if (strstr(rootdev, "mtdblock") ||
	    strstr(rootdev, "ubiblock"))
		return NULL;

	if (get_squashfs(&sb))
		return NULL;

	if (memcmp(&sb.s_magic, "hsqs", sizeof(sb.s_magic)) != 0)
		return NULL;

	p = calloc(1, sizeof(*p));
	p->v.drv = &rootdisk_driver;
	p->v.name = get_overlay_partition();

#ifdef OVL_ROOTDISK_PART_ENABLE
	parse_cmdline();

	if (blkdev_get_disk_info(rootdev, &rootdisk_info) != 0)
		return NULL;

	if (rootdisk_info.partitioned)
	{
		int ret;

		ULOG_INFO("root filesystem on the %s partition of %s (%s) device\n",
			rootdev, rootdisk_info.dev, rootdisk_info.ro ? "ro" : "rw");

		if (ovl_partition) {
			if (rootdisk_info.partition[ovl_partition - 1].partition)
				strcpy(p->dev_name, rootdisk_info.partition[ovl_partition - 1].dev);
			else {
				ULOG_ERR("ovl-partition %d is not exists, fallback to default behavior\n",
					ovl_partition);
				ovl_partition = 0;
			}
		}

		if (rootdisk_info.ro == 0) {
			/* try to find ext4/f2fs partition with label "<overlay_partition>" on rootdev */
			if (!ovl_partition)
				ret = rootdisk_volume_find_partition(name, p);
			else if (ovl_partition == rootdisk_info.rootpart)
				ret = -1;
			else {
				strcpy(p->dev_name, rootdisk_info.partition[ovl_partition - 1].dev);
				p->offset = 0ULL;
				ret = 0;
			}
				
			if (ret != 0) {
				int part;

				struct blkdev_diskpart_info *rootfs_part =
					&rootdisk_info.partition[rootdisk_info.rootpart - 1];

				if (rootfs_part->ro == 0) {
					/* root filesystem partition is rw */
					p->offset = le64_to_cpu(sb.bytes_used);
					p->offset = ((p->offset + (ROOTDEV_OVERLAY_ALIGN - 1)) &
						~(ROOTDEV_OVERLAY_ALIGN - 1));

					if ((p->offset + OVL_MINSIZE) <= rootfs_part->size) {
						/* rootfs partition has free space for overlay filesystem */
						/* try to create/find overlay fs after the rootfs partition */
						strcpy(p->dev_name, rootdev);
						return &p->v;
					}
				}

				if (ovl_partition) {
					ULOG_WARN("no space on %s partition for overlay filesystem\n",
						rootdisk_info.partition[ovl_partition - 1].dev);
					p->no_overlay_create = 1;
				} else {
					/* try to put overlayfs at end of the partitions on disk */
					for (part = rootdisk_info.rootpart; part <= 4; part++)
					{
						if (rootdisk_info.partition[part - 1].partition != part)
							continue;

						p->offset = rootdisk_info.partition[part - 1].start +
						            rootdisk_info.partition[part - 1].size;
					}

					p->offset = ((p->offset + (ROOTDEV_OVERLAY_ALIGN - 1)) &
						~(ROOTDEV_OVERLAY_ALIGN - 1));

					if ((p->offset + OVL_MINSIZE) <= rootdisk_info.size) {
						/* for properly loop device creation set rootdev to rootdiskdev */
						rootdev = rootdisk_info.dev;
					}
				}
			}
		} else {
			ULOG_WARN("root device %s is in read-only mode\n", rootdisk_info.dev);
			p->no_overlay_create = 1;
		}
	}
	else
	{
#endif
		ULOG_INFO("root filesystem on the %s device\n", rootdev);

		p->offset = le64_to_cpu(sb.bytes_used);
		p->offset = ((p->offset + (ROOTDEV_OVERLAY_ALIGN - 1)) &
			~(ROOTDEV_OVERLAY_ALIGN - 1));
#ifdef OVL_ROOTDISK_PART_ENABLE
	}

	if ((p->offset + OVL_MINSIZE) > rootdisk_info.size) {
		ULOG_WARN("no space on %s device for overlay filesystem\n",
			rootdisk_info.dev);
		strcpy(p->dev_name, rootdisk_info.dev);
		p->no_overlay_create = 1;
	}
#endif

	return &p->v;
}

static int rootdisk_volume_identify_p(const char *dev, uint64_t offset)
{
	int ret = FS_NONE;
	uint32_t magic = 0;
	size_t n;
	FILE *f;

	f = fopen(dev, "r");
	if (!f)
		return ret;

#ifdef OVL_F2FS_ENABLE
	fseeko(f, offset + 0x400, SEEK_SET);
	n = fread(&magic, sizeof(magic), 1, f);
	if (n != 1) {
		fclose(f);
		return -1;
	}

	if (magic == cpu_to_le32(0xF2F52010))
		ret = FS_F2FS;
#endif

	magic = 0;
	fseeko(f, offset + 0x438, SEEK_SET);
	n = fread(&magic, sizeof(magic), 1, f);
	if (n != 1) {
		fclose(f);
		return -1;
	}
	if ((le32_to_cpu(magic) & 0xffff) == 0xef53)
		ret = FS_EXT4;

	fclose(f);
	return ret;
}

static int rootdisk_volume_identify(struct volume *v)
{
	struct rootdev_volume *p = container_of(v, struct rootdev_volume, v);
	if (!v)
		return FS_NONE;
	return rootdisk_volume_identify_p((p->offset == 0) ? p->dev_name : rootdev, p->offset);
}

static int rootdisk_create_loop(struct rootdev_volume *p)
{
	struct loop_info64 info;
	int ret = -1;
	int fd = -1;
	int i, ffd;

	ffd = open(rootdev, O_RDWR);
	if (ffd < 0)
		return -1;

	for (i = 0; i < 8; i++) {
		snprintf(p->dev_name, sizeof(p->dev_name), "/dev/loop%d",
			 i);

		if (fd >= 0)
			close(fd);

		fd = open(p->dev_name, O_RDWR);
		if (fd < 0)
			continue;

		if (ioctl(fd, LOOP_GET_STATUS64, &info) == 0) {
			if (strcmp((char *) info.lo_file_name, rootdev) != 0)
				continue;
			if (info.lo_offset != p->offset)
				continue;
			ret = 0;
			break;
		}

		if (errno != ENXIO)
			continue;

		if (ioctl(fd, LOOP_SET_FD, ffd) != 0)
			continue;

		memset(&info, 0, sizeof(info));
		snprintf((char *) info.lo_file_name, sizeof(info.lo_file_name), "%s",
			 rootdev);
		info.lo_offset = p->offset;
		info.lo_flags |= LO_FLAGS_AUTOCLEAR;

		if (ioctl(fd, LOOP_SET_STATUS64, &info) != 0) {
			ioctl(fd, LOOP_CLR_FD, 0);
			continue;
		}

		/*
		 * Don't close fd. Leave it open until this process exits, to avoid
		 * the autoclear from happening too soon.
		 */
		fd = -1;

		ret = 0;
		break;
	}

	if (fd >= 0)
		close(fd);

	close(ffd);

	if (ret)
		p->dev_name[0] = 0;

	return ret;
}

static int rootdisk_volume_init(struct volume *v)
{
	struct rootdev_volume *p = container_of(v, struct rootdev_volume, v);
	char str[128];
	int ret = 0;

	if (!p->dev_name[0] && rootdisk_create_loop(p) != 0) {
		ULOG_ERR("unable to create loop device\n");
		return -1;
	}

	v->type = BLOCKDEV;
	v->blk = p->dev_name;

	if (p->no_overlay_create)
		return -1;

	switch (rootdisk_volume_identify(v)) {
	case FS_NONE:
		ULOG_INFO("rootdisk overlay filesystem has not been formatted yet\n");
#ifdef OVL_F2FS_ENABLE
		if (rootdisk_use_f2fs(p)) {
			ULOG_INFO("creating f2fs overlay filesystem (%s, offset %"PRIu64")...\n",
				v->blk, p->offset);
			snprintf(str, sizeof(str), "mkfs.f2fs -q -l %s %s", get_overlay_partition(), v->blk);
		}
		else
#endif
		{
			ULOG_INFO("creating ext4 overlay filesystem (%s, offset %"PRIu64")...\n",
				v->blk, p->offset);
			snprintf(str, sizeof(str), "mkfs.ext4 -q -L %s %s", get_overlay_partition(), v->blk);
		}
		ret = system(str);
		if (ret)
			ULOG_ERR("overlay filesystem creation failed with error %d\n", ret);
		break;
	default:
		break;
	}
	return ret;
}

static struct driver rootdisk_driver = {
	.name = "rootdisk",
	.find = rootdisk_volume_find,
	.init = rootdisk_volume_init,
	.identify = rootdisk_volume_identify,
};

DRIVER(rootdisk_driver);
