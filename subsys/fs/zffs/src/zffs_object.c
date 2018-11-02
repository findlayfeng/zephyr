/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/object.h"
#include "zffs/area.h"
#include "zffs/tree.h"
#include "zffs/misc.h"

struct zffs_object_disk {
	zffs_disk(u32_t, id);
	zffs_disk(u32_t, size);
	zffs_disk(u16_t, crc);
};

static int object_make(struct zffs_data *zffs, u32_t id, u32_t size,
		       bool is_update, union zffs_addr *addr)
{
	struct zffs_object_disk disk;
	int rc;
	u32_t offset;

	sys_put_le32(id, disk.id);
	sys_put_le32(size, disk.size);
	zffs_misc_crc_compute(&disk);

	rc = zffs_area_alloc(zffs, addr->id, addr, sizeof(disk) + size);
	if (rc) {
		return rc;
	}

	offset = addr->full;
	rc = zffs_area_write(zffs, addr, &disk, sizeof(disk));
	if (rc) {
		return rc;
	}

	rc = zffs_tree_insert(zffs, id, offset);
	if (is_update && rc == -EEXIST) {
		rc = zffs_tree_update(zffs, id, offset);
	}

	return rc;
}

int zffs_object_new(struct zffs_data *zffs, u32_t id, u32_t size,
		    union zffs_addr *addr)
{
	return object_make(zffs, id, size, false, addr);
}

int zffs_object_update(struct zffs_data *zffs, u32_t id, u32_t size,
		       union zffs_addr *addr)
{
	return object_make(zffs, id, size, true, addr);
}

static int object_open(struct zffs_data *zffs, union zffs_addr *addr, u32_t id)
{
	int rc;
	struct zffs_object_disk disk;

	rc = zffs_area_read(zffs, addr, &disk, sizeof(disk));
	if (rc) {
		return rc;
	}
	if (sys_get_le32(disk.id) != id || zffs_misc_crc_check(&disk)) {
		return -EIO;
	}

	return sys_get_le32(disk.size);
}

int zffs_object_open(struct zffs_data *zffs, u32_t id, union zffs_addr *addr)
{
	int rc;

	rc = zffs_tree_search(zffs, id, &addr->full);
	if (rc) {
		return rc;
	}

	rc = object_open(zffs, addr, id);
	if (rc < 0) {
		return rc;
	}

	return rc;
}

int zffs_object_check(struct zffs_data *zffs, union zffs_addr *addr, u32_t *id)
{
	struct zffs_object_disk disk;
	int rc;
	u16_t crc = 0;

	rc = zffs_area_read(zffs, addr, &disk, sizeof(disk));
	if (rc) {
		return rc;
	}

	if (zffs_misc_disk_is_empty(&disk)) {
		return -ENOENT;
	}

	if (zffs_misc_crc_check(&disk)) {
		return -EIO;
	}

	rc = zffs_area_crc(zffs, addr, sys_get_le32(disk.size), &crc);
	if (rc) {
		return rc;
	}

	if (id) {
		*id = sys_get_le32(disk.id);
	}

	if (crc) {
		rc = -EINVAL;
	}

	return rc;
}

struct object_foreach_data {
	void *data;
	zffs_object_callback_t callback;
};

static int object_foreach_cb(struct zffs_data *zffs, u32_t id,
			     union zffs_addr addr,
			     struct object_foreach_data *data)
{
	int rc = object_open(zffs, &addr, id);

	if (rc < 0) {
		return rc;
	}

	return data->callback(zffs, id, &addr, rc, data->data);
}

int zffs_object_foreach(struct zffs_data *zffs, void *data,
			zffs_object_callback_t object_cb)
{
	struct object_foreach_data cb_data = { .data = data,
					       .callback = object_cb };

	return zffs_tree_foreach(zffs, &cb_data,
				 (zffs_tree_foreach_fun_t)object_foreach_cb);
}
