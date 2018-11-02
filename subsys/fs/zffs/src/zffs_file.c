/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/block.h"
#include "zffs/file.h"
#include "zffs/misc.h"
#include "zffs/object.h"

#define file_is_write(__file) ((__file)->write.block.id != ZFFS_NULL)
#define file_write_free_size(__file) \
	(ZFFS_CONFIG_BLOCK_SIZE -    \
	 ((__file)->offset & (ZFFS_CONFIG_BLOCK_SIZE - 1)))

struct file_dlist_node_disk {
	zffs_disk(u32_t, block);
};

int zffs_file_make(struct zffs_data *zffs, struct zffs_dir *dir,
		   const struct zffs_node_data *node_data,
		   struct zffs_file *file)
{
	int rc;

	rc = zffs_dir_append(zffs, dir, node_data);
	if (rc) {
		return rc;
	}

	file->id = node_data->id;
	file->size = 0;
	file->offset = 0;
	file->next_id = node_data->file.next_id;
	zffs_dlist_init(&file->list);
	file->block.id = ZFFS_NULL;
	file->node.id = ZFFS_NULL;
	file->node.prev = ZFFS_NULL;
	file->node.next = ZFFS_NULL;
	file->write.block.id = ZFFS_NULL;

	sys_slist_append(&zffs->opened, (void *)file);
	return 0;
}

int zffs_file_open(struct zffs_data *zffs,
		   const struct zffs_node_data *node_data,
		   struct zffs_file *file)
{
	int rc;
	struct file_dlist_node_disk disk;
	union zffs_addr addr;

	if (node_data->type != ZFFS_TYPE_FILE) {
		return -EISDIR;
	}

	file->id = node_data->id;
	file->size = node_data->file.size;
	file->next_id = node_data->file.next_id;
	file->offset = 0;
	file->list.wait_update = false;
	file->list.head = node_data->file.head;
	file->list.tail = node_data->file.tail;
	file->write.block.id = ZFFS_NULL;

	if (zffs_dlist_is_empty(&file->list)) {
		file->block.id = ZFFS_NULL;
		file->node.id = ZFFS_NULL;
		file->node.prev = ZFFS_NULL;
		file->node.next = ZFFS_NULL;
		goto done;
	}

	rc = zffs_dlist_head(zffs, &file->list, &file->node, &addr);
	if (rc < 0) {
		return rc;
	}

	if (rc < sizeof(disk)) {
		return -EIO;
	}

	rc = zffs_area_read(zffs, &addr, &disk, sizeof(disk));
	if (rc) {
		return rc;
	}

	rc = zffs_block_open(zffs, sys_get_le32(disk.block), &file->block,
			     &addr);
	if (rc < 0) {
		return rc;
	}

	if (rc != ZFFS_CONFIG_BLOCK_SIZE) {
		return -EIO;
	}

	file->addr = addr;

done:
	sys_slist_append(&zffs->opened, (void *)file);

	return 0;
}

int zffs_file_sync(struct zffs_data *zffs, struct zffs_file *file)
{
	int rc;
	struct file_dlist_node_disk disk;

	zffs_disk(u16_t, crc);

	if (!file_is_write(file)) {
		return 0;
	}

	if (((file->offset & (ZFFS_CONFIG_BLOCK_SIZE - 1)) != 0)) {
		union zffs_addr addr = file->addr;
		u32_t copybytes = file_write_free_size(file);

		file->addr = file->write.addr;

		rc = file->block.id == ZFFS_NULL
		     ? zffs_area_crc(zffs, &file->write.addr,
				     copybytes, &file->write.crc)
		     : zffs_area_copy_crc(zffs, &addr, &file->write.addr,
					  copybytes, &file->write.crc);
		if (rc) {
			return rc;
		}
	}

	sys_put_le16(file->write.crc, crc);

	rc = zffs_area_write(zffs, &file->write.addr, crc, sizeof(crc));
	if (rc) {
		return rc;
	}

	sys_put_le32(file->write.block.id, disk.block);

	if (file->block.id == ZFFS_NULL) {
		file->node.id = zffs_misc_next_id(zffs, &file->next_id);
		rc = zffs_dlist_append(zffs, &file->list, &file->node,
				       &disk, sizeof(disk));
	} else {
		rc = zffs_dlist_updata_ex(zffs, file->node.id, &disk,
					  sizeof(disk));
	}

	if (rc) {
		return rc;
	}

	file->block = file->write.block;
	file->write.block.id = ZFFS_NULL;

	return 0;
}

int zffs_file_read(struct zffs_data *zffs, struct zffs_file *file, void *data,
		   u32_t len)
{
	int rlen = 0, rc;
	struct file_dlist_node_disk disk;
	u32_t readbytes;

	rc = zffs_file_sync(zffs, file);
	if (rc) {
		return rc;
	}

	len = min(file->size - file->offset, len);

	while (len > rlen) {
		readbytes = min(len - rlen, ZFFS_CONFIG_BLOCK_SIZE -
				file->offset % ZFFS_CONFIG_BLOCK_SIZE);

		rc = zffs_area_read(zffs, &file->addr, (u8_t *)data + rlen,
				    readbytes);
		if (rc) {
			return rc;
		}

		rlen += readbytes;
		file->offset += readbytes;

		if (file->offset & (ZFFS_CONFIG_BLOCK_SIZE - 1)) {
			continue;
		}

		rc = zffs_dlist_next(zffs, &file->node, &file->addr);
		if (rc < 0) {
			return rc;
		}

		if (rc < sizeof(disk)) {
			return -EIO;
		}

		rc = zffs_area_read(zffs, &file->addr, &disk, sizeof(disk));
		if (rc) {
			return rc;
		}

		rc = zffs_block_open(zffs, sys_get_le32(disk.block),
				     &file->block, &file->addr);
		if (rc) {
			return rc;
		}
	}

	return rlen;
}

int zffs_file_write(struct zffs_data *zffs, struct zffs_file *file,
		    const void *data, u32_t len)

{
	struct file_dlist_node_disk disk;
	union zffs_addr addr;
	u32_t rlen = 0;
	u32_t writebytes;
	int rc;

	while (rlen < len) {
		if (!file_is_write(file)) {
			rc = zffs_block_make(zffs,
					     zffs_misc_next_id(zffs,
							       &file->next_id),
					     ZFFS_CONFIG_BLOCK_SIZE,
					     &file->write.block, &addr,
					     &file->write.crc);

			if (rc) {
				return rc;
			}

			file->list.wait_update = true;
			file->write.addr = addr;
		}

		writebytes = min(file_write_free_size(file), len - rlen);
		rc = zffs_area_write(zffs, &file->write.addr,
				     (u8_t *)data + rlen, writebytes);
		if (rc) {
			return rc;
		}

		file->write.crc = crc16_ccitt(file->write.crc,
					      (u8_t *)data + rlen, writebytes);
		file->offset += writebytes;

		if (file->block.id != ZFFS_NULL) {
			file->addr.offset += writebytes;
		}

		rlen += writebytes;
		if (file->offset > file->size) {
			file->size = file->offset;
		}

		if (file->offset & (ZFFS_CONFIG_BLOCK_SIZE - 1)) {
			continue;
		}

		rc = zffs_file_sync(zffs, file);
		if (rc) {
			return rc;
		}

		rc = zffs_dlist_next(zffs, &file->node, &file->addr);
		if (rc == -ENOENT) {
			file->block.id = ZFFS_NULL;
			continue;
		}

		if (rc < 0) {
			return rc;
		}

		if (rc < sizeof(disk)) {
			return -EIO;
		}

		rc = zffs_area_read(zffs, &file->addr, &disk, sizeof(disk));
		if (rc) {
			return rc;
		}

		rc = zffs_block_open(zffs, sys_get_le32(disk.block),
				     &file->block, &file->addr);
		if (rc) {
			return rc;
		}
	}

	return rlen;
}

int zffs_file_close(struct zffs_data *zffs, struct zffs_file *file)
{
	int rc;

	rc = zffs_file_sync(zffs, file);

	if (rc) {
		return rc;
	}

	if (file->list.wait_update) {
		struct zffs_node_data node_data;

		node_data.type = ZFFS_TYPE_FILE;
		node_data.id = file->id;
		node_data.name = NULL;
		node_data.file.head = file->list.head;
		node_data.file.tail = file->list.tail;
		node_data.file.size = file->size;
		node_data.file.next_id = file->next_id;

		rc = zffs_dir_update_node(zffs, &node_data);

		if (rc) {
			return rc;
		}
	}

	sys_slist_find_and_remove(&zffs->opened, (void *)file);

	return 0;
}

int zffs_file_seek(struct zffs_data *zffs, struct zffs_file *file,
		   int whence, int offset)
{
	struct file_dlist_node_disk disk;
	int rc;

	switch (whence) {
	case ZFFS_FILE_SEEK_SET:
		break;
	case ZFFS_FILE_SEEK_CUR:
		offset += file->offset;
		break;
	case ZFFS_FILE_SEEK_END:
		offset += file->size;
		break;
	default:
		return -ESPIPE;
	}

	if (offset > file->size || offset < 0) {
		return -ESPIPE;
	}

	if (file_is_write(file)) {
		if (file->offset < offset &&
		    ((file->offset ^ offset) &
		     ~(ZFFS_CONFIG_BLOCK_SIZE - 1)) == 0) {
			rc = zffs_area_copy_crc(zffs, &file->addr,
						&file->write.addr,
						offset - file->offset,
						&file->write.crc);
			if (rc) {
				return rc;
			}

			file->offset = offset;
			return 0;
		}

		rc = zffs_file_sync(zffs, file);
		if (rc) {
			return rc;
		}
	}

	if (((file->offset ^ offset) & ~(ZFFS_CONFIG_BLOCK_SIZE - 1)) == 0) {
		goto done;
	}

	do {
		if (file->offset > offset) {
			rc = zffs_dlist_prev(zffs, &file->node, &file->addr);
			if (rc < 0) {
				return rc;
			}

			file->offset -= ZFFS_CONFIG_BLOCK_SIZE;
		} else {
			rc = zffs_dlist_next(zffs, &file->node, &file->addr);
			if (rc < 0) {
				return rc;
			}

			file->offset += ZFFS_CONFIG_BLOCK_SIZE;
		}

	} while ((file->offset ^ offset) & ~(ZFFS_CONFIG_BLOCK_SIZE - 1));

	if (rc < sizeof(disk)) {
		return -EIO;
	}

	rc = zffs_area_read(zffs, &file->addr, &disk, sizeof(disk));
	if (rc) {
		return rc;
	}

	rc = zffs_block_open(zffs, sys_get_le32(disk.block), &file->block,
			     &file->addr);
	if (rc < 0) {
		return rc;
	}

	if (rc != ZFFS_CONFIG_BLOCK_SIZE) {
		return -EIO;
	}

	file->offset &= ~(ZFFS_CONFIG_BLOCK_SIZE - 1);

done:

	file->addr.offset += offset - file->offset;
	file->offset = offset;

	return 0;
}
