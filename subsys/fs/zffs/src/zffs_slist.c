/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/queue.h"
#include "zffs/object.h"

struct slist_node_disk_head {
	zffs_disk(u8_t, object_type);
	zffs_disk(u32_t, next);
};

struct slist_node_disk_tail {
	zffs_disk(u16_t, crc);
};

static int slist_node_new(struct zffs_data *zffs, struct zffs_slist_node *node,
			  const void *data, u32_t len)
{
	int rc;
	union zffs_addr addr;
	struct slist_node_disk_head head;
	struct slist_node_disk_tail tail;
	u16_t crc;

	rc = zffs_object_new(zffs, node->id, sizeof(head) + len + sizeof(tail),
			     &addr);
	if (rc) {
		return rc;
	}

	*head.object_type = ZFFS_OBJECT_TYPE_SLIST_NODE;
	sys_put_le32(node->next, head.next);
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

static int slist_node_update(struct zffs_data *zffs, u32_t id, u32_t *next,
			     const void *ex_data, u32_t update_ex_len)
{
	int rc;
	union zffs_addr from, to;
	struct slist_node_disk_head head;
	struct slist_node_disk_tail tail;
	u16_t crc;
	u32_t ex_len;

	rc = zffs_object_open(zffs, id, &from);
	if (rc < 0) {
		return rc;
	}

	if (rc < sizeof(head) + sizeof(tail)) {
		return -EIO;
	}

	ex_len = rc - sizeof(head) - sizeof(tail);
	if (!ex_data) {
		update_ex_len = 0;
	}

	rc = zffs_object_update(zffs, id, max(ex_len, update_ex_len) +
				sizeof(head) + sizeof(tail), &to);
	if (rc) {
		return rc;
	}

	rc = zffs_area_read(zffs, &from, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	if (*head.object_type != ZFFS_OBJECT_TYPE_SLIST_NODE) {
		return -EIO;
	}

	if (next) {
		sys_put_le32(*next, head.next);
	}
	crc = crc16_ccitt(0, (const void *)&head, sizeof(head));
	rc = zffs_area_write(zffs, &to, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	if (ex_data) {
		rc = zffs_area_write(zffs, &to, ex_data, update_ex_len);
		if (rc) {
			return rc;
		}
		crc = crc16_ccitt(crc, ex_data, update_ex_len);

		from.offset += update_ex_len;
	}

	if (update_ex_len < ex_len) {
		rc = zffs_area_copy_crc(zffs, &from, &to,
					ex_len - update_ex_len, &crc);
		if (rc) {
			return rc;
		}
	}

	sys_put_le16(crc, tail.crc);
	return zffs_area_write(zffs, &to, (const void *)&tail, sizeof(tail));
}

static int slist_node_load(struct zffs_data *zffs, struct zffs_slist_node *node,
			   union zffs_addr *addr)
{
	int rc;
	struct slist_node_disk_head head;
	u32_t ex_len;

	rc = zffs_object_open(zffs, node->id, addr);
	if (rc < 0) {
		return rc;
	}

	if (rc < sizeof(head) + sizeof(struct slist_node_disk_tail)) {
		return -EIO;
	}

	ex_len = rc - sizeof(head) - sizeof(struct slist_node_disk_tail);
	rc = zffs_area_read(zffs, addr, &head, sizeof(head));
	if (rc) {
		return rc;
	}
	if (*head.object_type != ZFFS_OBJECT_TYPE_SLIST_NODE) {
		return -EIO;
	}

	node->next = sys_get_le32(head.next);
	return ex_len;
}

int zffs_slist_open_ex(struct zffs_data *zffs, u32_t id, union zffs_addr *addr)
{
	struct zffs_slist_node node = { .id = id };

	return slist_node_load(zffs, &node, addr);
}

int zffs_slist_prepend(struct zffs_data *zffs, struct zffs_slist *slist,
		       struct zffs_slist_node *node,
		       const void *data, u32_t len)
{
	node->next = slist->head;

	slist->head = node->id;
	slist->wait_update = true;

	return slist_node_new(zffs, node, data, len);
}

int zffs_slist_head(struct zffs_data *zffs,
		    const struct zffs_slist *slist,
		    struct zffs_slist_node *node,
		    union zffs_addr *addr)
{
	if (zffs_slist_is_empty(slist)) {
		return -ENOENT;
	}

	node->id = slist->head;

	return slist_node_load(zffs, node, addr);
}

int zffs_slist_next(struct zffs_data *zffs, struct zffs_slist_node *node,
		    union zffs_addr *addr)
{
	if (zffs_slist_is_tail(node)) {
		return -ENOENT;
	}

	node->id = node->next;

	return slist_node_load(zffs, node, addr);
}

int zffs_slist_search(struct zffs_data *zffs, const struct zffs_slist *slist,
		      const void *data, zffs_node_compar_fn_t compar_fn,
		      struct zffs_slist_node *node, void *node_data)
{
	int rc;
	union zffs_addr addr;

	for (rc = zffs_slist_head(zffs, slist, node, &addr); rc >= 0;
	     rc = zffs_slist_next(zffs, node, &addr)) {
		if (compar_fn(zffs, &addr, data, node, node_data, rc) ==
		    0) {
			return 0;
		}
	}

	return rc;
}

int zffs_slist_remove(struct zffs_data *zffs, struct zffs_slist *slist,
		      struct zffs_slist_node *node)
{
	struct zffs_slist_node prev;
	union zffs_addr addr;
	int rc;

	if (slist->head == node->id) {
		slist->head = node->next;
		node->next = ZFFS_NULL;
		slist->wait_update = true;
		return 0;
	}

	for (rc = zffs_slist_head(zffs, slist, &prev, &addr); rc >= 0;
	     rc = zffs_slist_next(zffs, &prev, &addr)) {
		if (prev.next == node->id) {
			return slist_node_update(zffs, prev.id, &node->next,
						 NULL, 0);
		}
	}

	return rc;
}

int zffs_slist_updata_ex(struct zffs_data *zffs, u32_t id, const void *data,
			 u32_t len)
{
	return slist_node_update(zffs, id, NULL, data, len);
}
