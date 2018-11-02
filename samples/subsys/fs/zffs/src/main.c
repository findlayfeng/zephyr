/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zffs/zffs.h>
#include <zffs/tree.h>
#include <fs.h>
#include <flash_map.h>
#include <misc/printk.h>
#include <zephyr.h>
#include <string.h>

#define ZFFS_ROOT_DIR "/zffs"

struct zffs_data zd;
struct fs_mount_t fs_mnt;

void main(void)
{
	// struct fs_dir_t zdp;
	int rc;

	if (flash_area_open(FLASH_AREA_EXTERNAL_ID,
			    (const struct flash_area **)&fs_mnt.storage_dev)) {
		return;
	}

	fs_mnt.type = FS_ZFFS;
	fs_mnt.mnt_point = ZFFS_ROOT_DIR;
	fs_mnt.fs_data = &zd;

	rc = fs_mount(&fs_mnt);
	if (rc) {
		flash_area_close(fs_mnt.storage_dev);
		return;
	}

	for (int i = 0; i < 10000; i++) {
		if (zffs_tree_insert(&zd, i, i << 1)) {
			while (true);
		}
	}

	fs_unmount(&fs_mnt);
	flash_area_close(fs_mnt.storage_dev);
}
