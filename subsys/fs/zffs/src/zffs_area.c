/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/area.h"
#include "zffs/misc.h"
#include <string.h>

struct zffs_disk_area_head {
	char fs_name[16];
	zffs_disk(u32_t, length);
	zffs_disk(u16_t, erase_seq);
	zffs_disk(u8_t, ver);
	zffs_disk(u16_t, crc);
};

struct zffs_disk_area_tail {
	zffs_disk(u8_t, id);
	zffs_disk(u8_t, gc_seq);
	zffs_disk(u16_t, crc);
};

struct zffs_disk_area {
	struct zffs_disk_area_head head;
	struct zffs_disk_area_tail tail;
};

#define area_list(__zffs, __id)		  \
	((__id)->is_gc ? (__zffs)->swap : \
	 (__id)->is_tree ? (__zffs)->tree_areas : (__zffs)->data_areas)

#define area_next(__zffs, __id)			\
	((__id)->is_gc ? &(__zffs)->next_swap :	\
	 (__id)->is_tree ? &(__zffs)->next_tree : &(__zffs)->next_data)

#define area_from_list(__list, __id) ((__list)[(__id)->num])

#define area_size(__area) ((__area)->length -  sizeof(struct zffs_disk_area))
#define area_offset(__addr) ((__addr)->offset + sizeof(struct zffs_disk_area))


#if ZFFS_CONFIG_AREA_ALIGNED_SIZE == 1

#define area_read(__fa, __off, __dst, __len) \
	flash_area_read((__fa), (__off), (__dst), (__len))
#define area_write(__fa, __off, __src, __len) \
	flash_area_write((__fa), (__off), (__src), (__len))

#else

#define ZFFS_CONFIG_AREA_ALIGNED_MASK (ZFFS_CONFIG_AREA_ALIGNED_SIZE - 1)

inline static int area_read(const struct flash_area *fa, off_t off, void *dst,
			    size_t len)
{
	u32_t buf;
	int rc;
	int byte_off, bytes;

	byte_off = off & ZFFS_CONFIG_AREA_ALIGNED_MASK;

	if (byte_off) {
		rc = flash_area_read(fa, off & ~ZFFS_CONFIG_AREA_ALIGNED_MASK,
				     &buf, ZFFS_CONFIG_AREA_ALIGNED_SIZE);
		if (rc) {
			return rc;
		}

		bytes = min(ZFFS_CONFIG_AREA_ALIGNED_SIZE - byte_off, len);

		memcpy(dst, ((u8_t *)&buf) + byte_off, bytes);
		off += bytes;
		dst = (u8_t *)dst + bytes;
		len -= bytes;
	}

	bytes = len & ~ZFFS_CONFIG_AREA_ALIGNED_MASK;

	if (bytes) {
		rc = flash_area_read(fa, off, dst, bytes);
		if (rc) {
			return rc;
		}

		off += bytes;
		dst = (u8_t *)dst + bytes;
		len -= bytes;
	}

	if (len) {
		rc = flash_area_read(fa, off, &buf,
				     ZFFS_CONFIG_AREA_ALIGNED_SIZE);
		if (rc) {
			return rc;
		}

		memcpy(dst, &buf, len);
	}

	return 0;
}

inline static int area_write(const struct flash_area *fa, off_t off,
			     const void *src, size_t len)
{
	u32_t buf;
	int rc;
	int byte_off, bytes;

	byte_off = off & ZFFS_CONFIG_AREA_ALIGNED_MASK;

	if (byte_off) {
		bytes = min(ZFFS_CONFIG_AREA_ALIGNED_SIZE - byte_off, len);
		buf = 0xffffffff;
		memcpy(((u8_t *)&buf) + byte_off, src, bytes);

		rc = flash_area_write(fa, off & ~ZFFS_CONFIG_AREA_ALIGNED_MASK,
				      &buf, ZFFS_CONFIG_AREA_ALIGNED_SIZE);
		if (rc) {
			return rc;
		}

		off += bytes;
		src = (const u8_t *)src + bytes;
		len -= bytes;
	}

	bytes = len & ~ZFFS_CONFIG_AREA_ALIGNED_MASK;

	if (bytes) {
		rc = flash_area_write(fa, off, src, bytes);
		if (rc) {
			return rc;
		}

		off += bytes;
		src = (u8_t *)src + bytes;
		len -= bytes;
	}

	if (len) {
		buf = 0xffffffff;
		memcpy(&buf, src, len);
		rc = flash_area_write(fa, off, &buf,
				      ZFFS_CONFIG_AREA_ALIGNED_SIZE);
		if (rc) {
			return rc;
		}
	}

	return 0;
}
#endif

int zffs_area_erase(struct zffs_data *zffs, struct zffs_area *area)
{
	struct zffs_disk_area_head head = { 0 };
	int rc;

	rc = flash_area_erase(zffs->flash, area->offset, area->length);
	if (rc) {
		return rc;
	}

	area->erase_seq += 1;
	memcpy(head.fs_name, ZFFS_NAME,
	       min(strlen(ZFFS_NAME), sizeof(head.fs_name)));
	sys_put_le32(area->length, head.length);
	sys_put_le16(area->erase_seq, head.erase_seq);
	*head.ver = ZFFS_VER;
	zffs_misc_crc_compute(&head);

	rc = area_write(zffs->flash, area->offset, &head, sizeof(head));
	if (rc) {
		return rc;
	}

	area->id.full = ZFFS_AREA_ID_NONE;

	return 0;
}

static void area_append(struct zffs_data *zffs, struct zffs_area *area)
{
	struct zffs_area **list;

	if (area->id.full == ZFFS_AREA_ID_NONE) {
		goto done;
	}

	list = area_list(zffs, &area->id);
	if (list[area->id.num] == NULL) {
		list[area->id.num] = &zffs->base_areas[zffs->area_num];
		goto done;
	}

	if (list[area->id.num]->gc_seq - area->gc_seq == 1) {
		list[area->id.num]->id.is_gc = 1;
		list[area->id.num] = &zffs->base_areas[zffs->area_num];
	} else {
		area->id.is_gc = 1;
	}
done:
	zffs->base_areas[zffs->area_num++] = *area;
}

static struct zffs_area *area_new(struct zffs_data *zffs, union zffs_area_id id)
{
	struct zffs_area *new = NULL, *old;
	struct zffs_disk_area_tail tail;

	for (int i = 0; i < zffs->area_num; i++) {
		if (zffs->base_areas[i].id.full != ZFFS_AREA_ID_NONE) {
			continue;
		}

		if (new == NULL ||
		    ((new->erase_seq > zffs->base_areas[i].erase_seq) ^
		     id.is_gc)) {
			new = &zffs->base_areas[i];
		}
	}

	if (new == NULL) {
		return new;
	}

	new->id = id;
	if (id.is_gc) {
		id.is_gc = 0;
		old = area_from_list(area_list(zffs, &id), &id);
		new->gc_seq = old != NULL ? old->gc_seq + 1 : 0;
	} else {
		new->gc_seq = 0;
	}

	*tail.id = id.full;
	*tail.gc_seq = new->gc_seq;
	zffs_misc_crc_compute(&tail);

	if (area_write(zffs->flash, new->offset +
		       sizeof(struct zffs_disk_area_head), &tail,
		       sizeof(tail))) {
		new->id.full = ZFFS_AREA_ID_NONE;
		return NULL;
	}

	return new;
}

int zffs_area_init(struct zffs_data *zffs, u32_t offset, u32_t length)
{
	struct zffs_area new = {
		.offset = offset,
		.length = length
	};
	int rc;

	rc = zffs_area_erase(zffs, &new);
	if (rc) {
		return rc;
	}

	area_append(zffs, &new);
	return 0;
}

int zffs_area_load(struct zffs_data *zffs, u32_t offset, u32_t *length)
{
	struct zffs_disk_area disk_area;
	struct zffs_area area;

	int rc = area_read(zffs->flash, offset, &disk_area, sizeof(disk_area));

	if (rc) {
		return rc;
	}

	if (strncmp(ZFFS_NAME, disk_area.head.fs_name,
		    sizeof(disk_area.head.fs_name))) {
		return -EFAULT;
	}

	if (zffs_misc_crc_check(&disk_area.head)) {
		return -EFAULT;
	}

	if (*disk_area.head.ver > ZFFS_VER) {
		return -ENOTSUP;
	}

	if (*disk_area.tail.id != ZFFS_AREA_ID_NONE &&
	    zffs_misc_crc_check(&disk_area.tail)) {
		return -EFAULT;
	}

	area.offset = offset;
	area.length = sys_get_le32(disk_area.head.length);
	area.erase_seq = sys_get_le16(disk_area.head.erase_seq);
	area.id.full = *disk_area.tail.id;
	area.gc_seq = *disk_area.tail.gc_seq;

	area_append(zffs, &area);
	if (length) {
		*length = area.length;
	}

	return 0;
}

int zffs_area_alloc(struct zffs_data *zffs, union zffs_area_id id,
		    union zffs_addr *addr, size_t size)
{
	struct zffs_area **list = area_list(zffs, &id);
	union zffs_addr *next = area_next(zffs, &id);
	struct zffs_area *area;

	if (list == NULL) {
		return -EIO;
	}

	area = area_from_list(list, &next->id);
	if (area == NULL) {
		goto new;
	}

	if (area_size(area) - area_offset(next) >= size) {
		goto done;
	}

	next->id.num++;
	next->offset = 0;

new:
	area = area_new(zffs, next->id);
	if (area == NULL) {
		return -ENOSPC;
	}
	list[next->id.num] = area;

done:

	*addr = *next;
	next->offset += size;
	return 0;
}

int zffs_area_write(struct zffs_data *zffs, union zffs_addr *addr,
		    const void *data, size_t len)
{
	struct zffs_area **list;
	struct zffs_area *area;

	list = area_list(zffs, &addr->id);

	if (list == NULL) {
		return -EIO;
	}

	area = area_from_list(list, &addr->id);
	if (area == NULL || area->length < addr->offset + len) {
		return -ENOSPC;
	}

	if (area_write(zffs->flash, area->offset + area_offset(addr),
		       data, len)) {
		return -EIO;
	}

	addr->offset += len;

	return 0;
}

int zffs_area_read(struct zffs_data *zffs, union zffs_addr *addr,
		   void *data, size_t len)
{
	struct zffs_area **list;
	struct zffs_area *area;

	list = area_list(zffs, &addr->id);

	if (list == NULL) {
		return -EIO;
	}

	area = area_from_list(list, &addr->id);
	if (area == NULL || area->length < addr->offset + len) {
		return -ENOSPC;
	}

	if (area_read(zffs->flash, area->offset + area_offset(addr),
		      data, len)) {
		return -EIO;
	}

	addr->offset += len;

	return 0;
}

bool zffs_area_is_empty(struct zffs_data *zffs, union zffs_addr addr,
			size_t len)
{
	u8_t buf[ZFFS_CONFIG_AREA_BUF_SIZE];
	size_t read_bytes;

	while (len > 0) {
		read_bytes = min(len, sizeof(buf));
		if (zffs_area_read(zffs, &addr, buf, read_bytes)) {
			return false;
		}

		if (!zffs_misc_mem_is_empty(buf, read_bytes)) {
			return false;
		}

		len -= read_bytes;
	}

	return true;
}

int zffs_area_copy_crc(struct zffs_data *zffs, union zffs_addr *from,
		       union zffs_addr *to, size_t len, u16_t *crc)
{
	u8_t buf[ZFFS_CONFIG_AREA_BUF_SIZE];
	size_t copy_bytes;
	int rc;

	while (len > 0) {
		copy_bytes = min(len, sizeof(buf));
		rc = zffs_area_read(zffs, from, buf, copy_bytes);
		if (rc) {
			return rc;
		}

		rc = zffs_area_write(zffs, to, buf, copy_bytes);
		if (rc) {
			return rc;
		}

		*crc = crc16_ccitt(*crc, buf, copy_bytes);

		len -= copy_bytes;
	}

	return 0;
}

int zffs_area_crc(struct zffs_data *zffs, union zffs_addr *addr,
		  size_t len, u16_t *crc)
{
	u8_t buf[ZFFS_CONFIG_AREA_BUF_SIZE];
	size_t read_bytes;
	int rc;

	while (len > 0) {
		read_bytes = min(len, sizeof(buf));
		rc = zffs_area_read(zffs, addr, buf, read_bytes);
		if (rc) {
			return rc;
		}

		*crc = crc16_ccitt(*crc, buf, read_bytes);

		len -= read_bytes;
	}

	return 0;
}

int zffs_area_copy(struct zffs_data *zffs, union zffs_addr *from,
		   union zffs_addr *to, size_t len)
{
	u8_t buf[ZFFS_CONFIG_AREA_BUF_SIZE];
	size_t copy_bytes;
	int rc;

	while (len > 0) {
		copy_bytes = min(len, sizeof(buf));
		rc = zffs_area_read(zffs, from, buf, copy_bytes);
		if (rc) {
			return rc;
		}

		rc = zffs_area_write(zffs, to, buf, copy_bytes);
		if (rc) {
			return rc;
		}

		len -= copy_bytes;
	}

	return 0;
}


size_t zffs_area_size(struct zffs_area *area)
{
	return area_size(area);
}

size_t zffs_area_size_from_id(struct zffs_data *zffs, union zffs_area_id id)
{
	struct zffs_area **list = area_list(zffs, &id);

	if (list == NULL) {
		return -1;
	}

	return area_size(area_from_list(list, &id));
}
