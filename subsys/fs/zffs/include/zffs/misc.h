/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_ZFFS_MISC_
#define H_ZFFS_MISC_

#include "zffs.h"
#include "area.h"

#ifdef __cplusplus
extern "C" {
#endif

void zffs_misc_lock(struct zffs_data *zffs);
void zffs_misc_unlock(struct zffs_data *zffs);
u32_t zffs_misc_get_id(struct zffs_data *zffs);
u32_t zffs_misc_next_id(struct zffs_data *zffs, u32_t *id);
int zffs_misc_restore(struct zffs_data *zffs);
int zffs_misc_load_node(struct zffs_data *zffs, union zffs_addr *addr,
			u32_t object_size, struct zffs_node_data *data,
			sys_snode_t **snode);

bool zffs_misc_mem_is_empty(const void *data, size_t size);

#define zffs_misc_crc_compute(__disk)			    \
	sys_put_le16(crc16_ccitt(0, (const void *)(__disk), \
				 sizeof(*(__disk)) - 2),    \
		     ((u8_t *)(__disk)) + sizeof(*(__disk)) - 2)

#define zffs_misc_crc_check(__disk) \
	(crc16_ccitt(0, (const void *)(__disk), sizeof(*(__disk))) != 0)

#define zffs_misc_disk_is_empty(__disk)	\
	zffs_misc_mem_is_empty((__disk), sizeof(*(__disk)))

#ifdef __cplusplus
}
#endif

#endif
