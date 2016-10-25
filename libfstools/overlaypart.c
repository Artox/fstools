/*
 * Copyright (C) 2016 Josua Mayer <josua.mayer97@gmail.com>
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

#include <asm/setup.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <string.h>

#include "libfstools.h"
#include "volume.h"

#define F2FS_MINSIZE	(100 * 1024 * 1024)

struct overlaypart_volume {
	struct volume v;
	// PATH_MAX would be appropriate here, but COMMAND_LINE_SIZE is sufficient
	int fstype;
};

static struct driver overlaypart_driver;

// look for overlay device node passed on cmdline
static struct volume *overlaypart_volume_find(char *name) {
	FILE *fp;
	char buffer_dev[COMMAND_LINE_SIZE] = {0};
	char buffer_fstype[COMMAND_LINE_SIZE] = {0};
	struct stat st;
	struct overlaypart_volume *p;

	// this driver is only for the overlay partition
	if (strcmp(name, "rootfs_data") != 0)
		return NULL;

	// read cmdline
	fp = fopen("/proc/cmdline", "r");
	if (!fp) {
		ULOG_WARN("Failed to open /proc/cmdline for reading\n");
		return NULL;
	}

	// looking for overlay=/dev/xyz [overlayfstype=xyz]
	while (!feof(fp)) {
		if (fscanf(fp, "overlay=%s", buffer_dev))
			break;

		fseek(fp, 1, SEEK_CUR);
	}
	fclose(fp);

	// check if overlay device was given
	if (!buffer_dev[0]) {
		return NULL;
	}

	// validate overlay device
	if (stat(buffer_dev, &st) != 0) {
		ULOG_WARN("Failed to stat overlay device\n");
		return NULL;
	}

	if (!st.st_rdev) {
		ULOG_WARN("overlay device is no device\n");
		return NULL;
	}
	
	// read filesystem type if iven
	int fstype = FS_NONE;
	if(buffer_fstype[0]) {
		if (strcmp(buffer_fstype, "ext4") == 0)
			fstype = FS_EXT4;
		else if (strcmp(buffer_fstype, "f2fs") == 0)
			fstype = FS_F2FS;
		else {
			ULOG_WARN("overlayfstype not recognized\n");
		}
	}

	p = calloc(1, sizeof(struct overlaypart_volume));
	p->v.drv = &overlaypart_driver;
	p->v.name = "rootfs_data";
	p->v.blk = calloc(strlen(buffer_dev), sizeof(char));
	strcpy(p->v.blk, buffer_dev);
	p->fstype = fstype;

	return &p->v;
}

// detect filesystem, if any
static int overlaypart_volume_identify(struct volume *v) {
	FILE *fp;
	uint32_t magic = 0;
	int fstype = FS_NONE;

	fp = fopen(v->blk, "r");
	if (!fp) {
		ULOG_WARN("Failed to open /proc/cmdline for reading\n");
		return FS_NONE;
	}

	// detect ext4
	if (fseek(fp, 0x400, SEEK_SET) == 0) {
		fread(&magic, sizeof(uint32_t), 1, fp);
		if (magic == cpu_to_le32(0xF2F52010))
			fstype = FS_F2FS;
	}

	//detect f2fs
	magic = 0;
	if (fseek(fp, 0x438, SEEK_SET) == 0) {
	
		fread(&magic, sizeof(uint32_t), 1, fp);
		if ((le32_to_cpu(magic) & 0xffff) == 0xef53)
			fstype = FS_EXT4;
	}

	// done
	fclose(fp);
	return fstype;
}

// create filesystem, if none
static int overlaypart_volume_init(struct volume *v) {
	struct overlaypart_volume *p;
	FILE *fp;
	uint64_t size = 0;
	int r = -1;

	if (overlaypart_volume_identify(v) != FS_NONE) {
		// nothing to do
		return 0;
	}
	ULOG_INFO("overlaypart filesystem has not been created yet\n");

	// lookup private data (container of volume struct)
	p = container_of(v, struct overlaypart_volume, v);

	// get size of block device
	fp = fopen(v->blk, "r");
	ioctl(fileno(fp), BLKGETSIZE64, &size);
	fclose(fp);

	// decide on filesystem to use
	int fstype = p->fstype;
	if(fstype == FS_NONE) {
		if(size >= F2FS_MINSIZE) {
			// use f2fs if there is enough space
			fstype = FS_F2FS;
		} else {
			// fall back to ext4
			fstype = FS_EXT4;
		}
	}

	// TODO: use PATH_MAX + ARG_MAX?
	char cmd[32+COMMAND_LINE_SIZE] = {0};
	switch(fstype) {
		case FS_EXT4:
			snprintf(cmd, sizeof(cmd), "mkfs.ext4 -L rootfs_data %s", v->blk);
			r = system(cmd);
			break;
		case FS_F2FS:
			snprintf(cmd, sizeof(cmd), "mkfs.f2fs -l rootfs_data %s", v->blk);
			r = system(cmd);
			break;
		default:
			ULOG_WARN("unexpected filesystem type encountered, aborting\n");
			r = -1;
	}

	return r;
}

static struct driver overlaypart_driver = {
	.name = "overlaypart",
	.find = overlaypart_volume_find,
	.init = overlaypart_volume_init,
	.identify = overlaypart_volume_identify,
};

DRIVER(overlaypart_driver);
