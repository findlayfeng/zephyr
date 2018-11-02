/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_ZFFS_PATH_
#define H_ZFFS_PATH_

#include "dir.h"
#include "area.h"
#include "zffs.h"

#ifdef __cplusplus
extern "C" {
#endif

int zffs_path_step(struct zffs_data *zffs, const char **path,
		   struct zffs_node_data *node_data, sys_snode_t **snode);

#ifdef __cplusplus
}
#endif

#endif
