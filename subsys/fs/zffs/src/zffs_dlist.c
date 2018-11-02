/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/queue.h"
#include "zffs/object.h"

struct dlist_node_disk_head {
	zffs_disk(u8_t, object_type);
	zffs_disk(u32_t, prev);
	zffs_disk(u32_t, next);
};

struct dlist_node_disk_tail {
	zffs_disk(u16_t, crc);
};

static int dlist_node_new(struct zffs_data *zffs, struct zffs_dlist_node *node,
			  const void *data, u32_t len)
{
	int rc;
	union zffs_addr addr;
	struct dlist_node_disk_head head;
	struct dlist_node_disk_tail tail;
	u16_t crc;

	rc = zffs_object_new(zffs, node->id, sizeof(head) + len + sizeof(tail),
			     &addr);
	if (rc) {
		return rc;
	}

	*head.object_type = ZFFS_OBJECT_TYPE_DLIST_NODE;
	sys_put_le32(node->next, head.next);
	sys_put_le32(node->prev, head.prev);
	crc = crc16_ccitt(0, (const void *)&head, sizeof(head));
	rc = zffs_area_write(zffs, &addr, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	if (len) {
		crc = crc16_ccitt(crc, data, len);
		rc = zffs_area_write(zffs, &addr, data, len);
		if (rc) {
			return rc;
		}
	}

	sys_put_le16(crc, tail.crc);
	return zffs_area_write(zffs, &addr, (const void *)&tail, sizeof(tail));
}

static int dlist_node_update(struct zffs_data *zffs, u32_t id,
			     u32_t *prev, u32_t *next, const void *data,
			     u32_t update_len)
{
	int rc;
	union zffs_addr from, to;
	struct dlist_node_disk_head head;
	struct dlist_node_disk_tail tail;
	u16_t crc;
	u32_t len;

	rc = zffs_object_open(zffs, id, &from);
	if (rc < 0) {
		return rc;
	}

	if (rc < sizeof(head) + sizeof(tail)) {
		return -EIO;
	}

	len = rc - sizeof(head) - sizeof(tail);
	if (!data) {
		update_len = 0;
	}

	rc = zffs_object_update(zffs, id, max(len, update_len) +
				sizeof(head) + sizeof(tail), &to);
	if (rc) {
		return rc;
	}

	rc = zffs_area_read(zffs, &from, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	if (*head.object_type != ZFFS_OBJECT_TYPE_DLIST_NODE) {
		return -EIO;
	}

	if (next) {
		sys_put_le32(*next, head.next);
	}

	if (prev) {
		sys_put_le32(*prev, head.prev);
	}

	crc = crc16_ccitt(0, (const void *)&head, sizeof(head));
	rc = zffs_area_write(zffs, &to, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	if (data) {
		rc = zffs_area_write(zffs, &to, data, update_len);
		if (rc) {
			return rc;
		}
		crc = crc16_ccitt(crc, data, update_len);

		from.offset += update_len;
	}

	if (update_len < len) {
		rc = zffs_area_copy_crc(zffs, &from, &to, len - update_len,
					&crc);
		if (rc) {
			return rc;
		}
	}

	sys_put_le16(crc, tail.crc);
	return zffs_area_write(zffs, &to, (const void *)&tail, sizeof(tail));
}

static int dlist_node_load(struct zffs_data *zffs,
			   struct zffs_dlist_node *node,
			   union zffs_addr *addr)
{
	int rc;
	u32_t len;
	struct dlist_node_disk_head head;

	rc = zffs_object_open(zffs, node->id, addr);
	if (rc < 0) {
		return rc;
	}

	if (rc < sizeof(head) + sizeof(struct dlist_node_disk_tail)) {
		return -EIO;
	}

	len = rc - sizeof(head) + sizeof(struct dlist_node_disk_tail);

	rc = zffs_area_read(zffs, addr, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	if (*head.object_type != ZFFS_OBJECT_TYPE_DLIST_NODE) {
		return -EIO;
	}

	node->prev = sys_get_le32(head.prev);
	node->next = sys_get_le32(head.next);
	return len;
}

int zffs_dlist_append(struct zffs_data *zffs, struct zffs_dlist *dlist,
		      struct zffs_dlist_node *node, const void *data, u32_t len)
{
	node->prev = dlist->tail;
	node->next = ZFFS_NULL;

	if (zffs_dlist_is_empty(dlist)) {
		dlist->head = node->id;
	} else {
		int rc;
		rc = dlist_node_update(zffs, node->prev, NULL, &node->id,
				       NULL, 0);
		if (rc) {
			return rc;
		}
	}

	dlist->tail = node->id;
	dlist->wait_update = true;

	return dlist_node_new(zffs, node, data, len);
}

int zffs_dlist_head(struct zffs_data *zffs, struct zffs_dlist *dlist,
		    struct zffs_dlist_node *node, union zffs_addr *addr)
{
	if (zffs_dlist_is_empty(dlist)) {
		return -ENOENT;
	}

	node->id = dlist->head;

	return dlist_node_load(zffs, node, addr);
}

int zffs_dlist_tail(struct zffs_data *zffs, struct zffs_dlist *dlist,
		    struct zffs_dlist_node *node, union zffs_addr *addr)
{
	if (zffs_dlist_is_empty(dlist)) {
		return -ENOENT;
	}

	node->id = dlist->tail;

	return dlist_node_load(zffs, node, addr);
}

int zffs_dlist_next(struct zffs_data *zffs, struct zffs_dlist_node *node,
		    union zffs_addr *addr)
{
	if (zffs_dlist_is_tail(node)) {
		return -ENOENT;
	}

	node->id = node->next;

	return dlist_node_load(zffs, node, addr);
}

int zffs_dlist_prev(struct zffs_data *zffs, struct zffs_dlist_node *node,
		    union zffs_addr *addr)
{
	if (zffs_dlist_is_head(node)) {
		return -ENOENT;
	}

	node->id = node->prev;

	return dlist_node_load(zffs, node, addr);
}

int zffs_dlist_updata_ex(struct zffs_data *zffs, u32_t id, const void *data,
			 u32_t len)
{
	return dlist_node_update(zffs, id, NULL, NULL, data, len);
}
