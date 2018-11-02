/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/path.h"
#include <string.h>

int zffs_path_step(struct zffs_data *zffs, const char **path,
		   struct zffs_node_data *node_data, sys_snode_t **snode)
{
	char name[ZFFS_CONFIG_NAME_MAX + 1];
	char *next_path = strchr(*path, '/');
	int rc, len;

	if (next_path) {
		len = next_path - *path;
		strncpy(name, *path, len);
		name[len] = '\0';
		next_path++;
	} else {
		strcpy(name, *path);
	}

	rc = zffs_dir_search_for_node_data(zffs, name, node_data, snode);
	if (rc) {
		return rc;
	}

	*path = next_path;
	return 0;
}
