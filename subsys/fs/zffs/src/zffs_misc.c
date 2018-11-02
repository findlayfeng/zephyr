/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/misc.h"
#include "zffs/dir.h"
#include "zffs/file.h"
#include "zffs/object.h"
#include "zffs/path.h"
#include "zffs/tree.h"

void zffs_misc_lock(struct zffs_data *zffs)
{
	k_mutex_lock(&zffs->lock, K_FOREVER);
}

void zffs_misc_unlock(struct zffs_data *zffs)
{
	k_mutex_unlock(&zffs->lock);
}

u32_t zffs_misc_get_id(struct zffs_data *zffs)
{
	int id;

	if (zffs->next_id == ZFFS_ROOT_ID) {
		zffs->next_id += ZFFS_CONFIG_MISC_ID_STEP;
	}

	id = zffs->next_id;
	zffs->next_id += ZFFS_CONFIG_MISC_ID_STEP;

	return id;
}

u32_t zffs_misc_next_id(struct zffs_data *zffs, u32_t *id)
{
	if ((*id + 1) % ZFFS_CONFIG_MISC_ID_STEP == 0) {
		*id = zffs_misc_get_id(zffs);
	} else {
		(*id)++;
	}

	return *id;
}

int zffs_misc_restore(struct zffs_data *zffs)
{
	u32_t id;
	struct zffs_node_data node_data;
	struct zffs_tree_info tree_info;
	int rc;

	rc = zffs_tree_init(zffs);
	if (rc) {
		return rc;
	}

	rc = zffs_tree_info(zffs, &tree_info);
	if (rc) {
		return rc;
	}

	if (tree_info.key_count > 0) {
		zffs->next_data.full = tree_info.value_max;
		rc = zffs_object_check(zffs, &zffs->next_data, &id);
		if (rc) {
			tree_info.key_count--;
			// todo
			return rc;
		}
	} else {
		zffs->next_data.full = 0;
	}

	do {
		rc = zffs_object_check(zffs, &zffs->next_data, &id);
		if (rc) {
			continue;
		}

		rc = zffs_tree_insert(zffs, id, zffs->next_data.full);
		if (rc == -EEXIST) {
			rc = zffs_tree_update(zffs, id, zffs->next_data.full);
		}

		if (rc) {
			return rc;
		}

		if (id > tree_info.key_max) {
			tree_info.key_max = id;
		}

		tree_info.key_count++;
	} while (rc != -ENOENT && rc != -ENOSPC);

	if (tree_info.key_count) {
		zffs->next_id = (tree_info.key_max &
				 ~(ZFFS_CONFIG_MISC_ID_STEP - 1)) +
				ZFFS_CONFIG_MISC_ID_STEP;
	} else {
		zffs->next_id = 0;
	}

	rc = zffs_dir_search_for_node_data(zffs, "", &node_data, NULL);
	if (rc == -ENOENT) {
		node_data.type = ZFFS_TYPE_DIR;
		node_data.id = ZFFS_ROOT_ID;
		node_data.name = NULL;
		node_data.dir.head = ZFFS_NULL;

		rc = zffs_dir_update_node(zffs, &node_data);
	}

	return rc;
}

int zffs_misc_load_node(struct zffs_data *zffs, union zffs_addr *addr,
			u32_t object_size, struct zffs_node_data *data,
			sys_snode_t **snode)
{
	int rc = zffs_dir_load_node(zffs, addr, object_size, data);
	sys_snode_t *sn;

	if (!snode || rc) {
		return rc;
	}

	*snode = NULL;

	switch (data->type) {
	case ZFFS_TYPE_DIR:
		SYS_SLIST_FOR_EACH_NODE(&zffs->opened, sn) {
			struct zffs_dir *dir = (struct zffs_dir *)sn;

			if (dir->id == data->id) {
				data->dir.head = dir->list.head;
				*snode = sn;
				break;
			}
		}
		break;
	case ZFFS_TYPE_FILE:
		SYS_SLIST_FOR_EACH_NODE(&zffs->opened, sn) {
			struct zffs_file *file = (struct zffs_file *)sn;

			if (file->id == data->id) {
				data->file.head = file->list.head;
				data->file.tail = file->list.tail;
				data->file.tail = file->list.tail;
				data->file.size = file->size;
				data->file.size = file->next_id;
				*snode = sn;
				break;
			}
		}
		break;
	default:
		rc = -EIO;
	}

	return rc;
}

bool zffs_misc_mem_is_empty(const void *data, size_t size)
{
	const u8_t *buf = data;

	while (size--) {
		if (buf[size] != 0xff) {
			return false;
		}
	}
	return true;
}
