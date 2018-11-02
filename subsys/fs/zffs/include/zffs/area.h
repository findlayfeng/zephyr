/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_ZFFS_AREA_
#define H_ZFFS_AREA_

#include "zffs.h"
#include <flash_map.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZFFS_AREA_ID_NONE 0xff

size_t zffs_area_size(struct zffs_area *area);
size_t zffs_area_size_from_id(struct zffs_data *zffs, union zffs_area_id id);

int zffs_area_load(struct zffs_data *zffs, u32_t offset, u32_t *length);
int zffs_area_init(struct zffs_data *zffs, u32_t offset, u32_t length);
int zffs_area_erase(struct zffs_data *zffs, struct zffs_area *area);

int zffs_area_alloc(struct zffs_data *zffs, union zffs_area_id id,
		    union zffs_addr *addr, size_t size);
int zffs_area_read(struct zffs_data *zffs, union zffs_addr *addr,
		   void *data, size_t len);
int zffs_area_write(struct zffs_data *zffs, union zffs_addr *addr,
		    const void *data, size_t len);

bool zffs_area_is_empty(struct zffs_data *zffs, union zffs_addr addr,
			size_t len);

int zffs_area_crc(struct zffs_data *zffs, union zffs_addr *addr,
		  size_t len, u16_t *crc);
int zffs_area_copy_crc(struct zffs_data *zffs, union zffs_addr *from,
		       union zffs_addr *to, size_t len, u16_t *crc);
int zffs_area_copy(struct zffs_data *zffs, union zffs_addr *from,
		   union zffs_addr *to, size_t len);



#ifdef __cplusplus
}
#endif

#endif
