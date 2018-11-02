/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zffs/tree.h"
#include "zffs/area.h"
#include "zffs/misc.h"
#include <string.h>

#define ZFFS_TREE_ADDR_WAIT_WRITE 0xffffff

struct zffs_tree_node {
	union zffs_addr disk_addr;
	u32_t key[2 * ZFFS_TREE_T - 1];
	u32_t value[2 * ZFFS_TREE_T - 1];
	ATOMIC_DEFINE(loadflag, 2 * ZFFS_TREE_T);
	union {
		union zffs_addr disk_child[2 * ZFFS_TREE_T];
		struct zffs_tree_node *child[2 * ZFFS_TREE_T];
	};
	struct zffs_tree_node *parent;
	u8_t root : 1;
	u8_t leaf : 1;
	u8_t n : 6;
};

struct zffs_tree_disk_node {
	u8_t root : 1;
	u8_t leaf : 1;
	u8_t n : 6;
	u8_t key[(2 * ZFFS_TREE_T - 1) * sizeof(u32_t)];
	u8_t value[(2 * ZFFS_TREE_T - 1) * sizeof(u32_t)];
	u8_t child[2 * ZFFS_TREE_T * sizeof(u32_t)];
	u8_t crc[2];
};

K_MEM_SLAB_DEFINE(zffs_tree_node_pool, sizeof(struct zffs_tree_node),
		  ZFFS_CONFIG_TREE_CACHE_NODE_MAX, sizeof(u32_t));

#define tree_node_is_full(__node) ((__node)->n == 2 * ZFFS_TREE_T - 1)

static struct zffs_tree_node *tree_node_alloc(void)
{
	void *node;

	if (k_mem_slab_alloc(&zffs_tree_node_pool, &node, K_NO_WAIT)) {
		node = NULL;
	}

	return node;
}

static void tree_node_free(struct zffs_tree_node *node)
{
	k_mem_slab_free(&zffs_tree_node_pool, (void **)&node);
}

static int tree_write_disk_node(struct zffs_data *zffs,
				union zffs_addr *node_addr,
				struct zffs_tree_disk_node *disk_node)
{
	union zffs_addr addr;
	int rc;

	rc = zffs_area_alloc(zffs, node_addr->id, &addr, sizeof(*disk_node));
	if (rc) {
		return rc;
	}
	zffs_misc_crc_compute(disk_node);
	*node_addr = addr;
	return zffs_area_write(zffs, &addr, disk_node, sizeof(*disk_node));
}

static int tree_read_disk_node(struct zffs_data *zffs, union zffs_addr addr,
			       struct zffs_tree_disk_node *disk_node)
{
	int rc = zffs_area_read(zffs, &addr, disk_node, sizeof(*disk_node));

	if (rc) {
		return rc;
	}

	return zffs_misc_crc_check(disk_node) ? -EIO : 0;
}

static int tree_load_node(struct zffs_data *zffs, union zffs_addr addr,
			  struct zffs_tree_node *node)
{
	struct zffs_tree_disk_node disk_node;
	int rc = tree_read_disk_node(zffs, addr, &disk_node);
	u8_t *child;

	if (rc) {
		return rc;
	}

	node->n = disk_node.n;
	node->leaf = disk_node.leaf;
	node->root = disk_node.root;
	node->disk_addr = addr;

	memcpy(node->key, disk_node.key, node->n * sizeof(node->key[0]));
	memcpy(node->value, disk_node.value, node->n * sizeof(node->value[0]));
	memset(node->loadflag, 0, sizeof(node->loadflag));

	if (node->leaf) {
		return 0;
	}

	child = disk_node.child;
	for (int i = 0; i < node->n + 1; i++, child += sizeof(u32_t)) {
		node->disk_child[i].full = sys_get_le32(child);
	}

	return 0;
}

static int tree_load_child(struct zffs_data *zffs, struct zffs_tree_node *node,
			   int child)
{
	int rc;
	union zffs_addr addr;

	addr = node->disk_child[child];
	node->child[child] = tree_node_alloc();
	if (node->child[child] == NULL) {
		node->disk_child[child] = addr;
		return -ENOMEM;
	}

	rc = tree_load_node(zffs, addr, node->child[child]);

	if (rc) {
		tree_node_free(node->child[child]);
		node->child[child] = NULL;
		node->disk_child[child] = addr;
		return rc;
	}

	node->child[child]->parent = node;
	return 0;
}

static int tree_save_node(struct zffs_data *zffs, struct zffs_tree_node *node)
{
	struct zffs_tree_disk_node disk_node;
	u8_t *child;
	int rc;

	if (node->disk_addr.offset != ZFFS_TREE_ADDR_WAIT_WRITE) {
		return 0;
	}

	disk_node.n = node->n;
	disk_node.leaf = node->leaf;
	disk_node.root = node->root;
	memcpy(disk_node.key, node->key, node->n * sizeof(node->key[0]));
	memcpy(disk_node.value, node->value, node->n * sizeof(node->value[0]));
	if (node->leaf) {
		goto done;
	}

	child = disk_node.child;
	for (int i = 0; i < disk_node.n + 1; i++, child += sizeof(u32_t)) {
		if (!atomic_test_bit(node->loadflag, i)) {
			sys_put_le32(node->disk_child[i].full, child);
			continue;
		}
		if (node->child[i]->disk_addr.offset ==
		    ZFFS_TREE_ADDR_WAIT_WRITE) {
			return -ESPIPE;
		}
		sys_put_le32(node->child[i]->disk_addr.full, child);
	}

done:
	rc = tree_write_disk_node(zffs, &node->disk_addr, &disk_node);
	if (rc) {
		node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	} else if (node->parent) {
		node->parent->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	}

	return rc;
}

static int tree_find_child(struct zffs_tree_node *parent,
			   struct zffs_tree_node *child)
{
	if (parent == NULL) {
		return 0;
	}

	for (int i = 0; i <= parent->n; i++) {
		if (!atomic_test_bit(parent->loadflag, i)) {
			continue;
		}

		if (parent->child[i] == child) {
			return i;
		}
	}

	return -1;
}

typedef int (*tree_node_foreach_fun_t)(struct zffs_data *zffs,
				       struct zffs_tree_node *node, void *data);


static int tree_node_foreach(struct zffs_data *zffs,
			     struct zffs_tree_node *top_node,
			     void *data, bool is_load, bool is_free,
			     tree_node_foreach_fun_t callback)
{
	struct zffs_tree_node *node, *parent;
	int i, rc, c;

	i = 0;
	node = top_node;
	parent = node->parent;
	c = tree_find_child(parent, node);
	if (c < 0) {
		return -ESPIPE;
	}

	while (1) {
		if (node->leaf || i > node->n) {
			if (callback) {
				rc = callback(zffs, node, data);
				if (rc) {
					break;
				}
			}
		} else if (atomic_test_bit(node->loadflag, i)) {
foreach_in_child:
			parent = node;
			c = i;

			node = node->child[i];
			i = 0;
			continue;
		} else if (is_load) {
			rc = tree_load_child(zffs, node, i);
			if (rc) {
				break;
			}
			atomic_set_bit(node->loadflag, i);
			goto foreach_in_child;
		} else {
			i++;
			continue;
		}

		if (node == top_node) {
			rc = 0;
			break;
		}

		if (is_free) {
			if (node->disk_addr.offset ==
			    ZFFS_TREE_ADDR_WAIT_WRITE) {
				rc = tree_save_node(zffs, node);
				if (rc) {
					break;
				}
			}

			parent->disk_child[c] = node->disk_addr;
			atomic_clear_bit(parent->loadflag, c);
			tree_node_free(node);
		}

		i = c + 1;
		node = parent;
		parent = node->parent;
		c = tree_find_child(parent, node);
		if (c < 0) {
			rc = -ESPIPE;
			break;
		}
	}

	return rc;
}

struct tree_key_foreach_data {
	void *data;
	u32_t count;
	zffs_tree_foreach_fun_t callback;
};

static int tree_key_foreach_cb(struct zffs_data *zffs,
			       struct zffs_tree_node *node,
			       struct tree_key_foreach_data *data)
{
	int rc;

	if (node->leaf) {
		for (int i = 0; i < node->n; i++) {
			rc = data->callback(zffs, node->key[i], node->value[i],
					    data->data);
			if (rc) {
				return rc;
			}
		}
	}

	if (node->parent) {
		int i = tree_find_child(node->parent, node);
		if (i < node->parent->n) {
			rc = data->callback(zffs, node->parent->key[i],
					    node->parent->value[i], data->data);
			if (rc) {
				return rc;
			}
		}
	}

	data->count++;

	return 0;
}

static int tree_key_foreach(struct zffs_data *zffs,
			    struct zffs_tree_node *top_node,
			    void *data, bool is_load, bool is_free,
			    zffs_tree_foreach_fun_t tree_key_cb)
{
	struct tree_key_foreach_data inode_data = { .data = data,
						    .count = 0,
						    .callback = tree_key_cb };
	int rc;

	rc = tree_node_foreach(zffs, top_node, &inode_data, is_load, is_free,
			       (void *)tree_key_foreach_cb);
	if (rc < 0) {
		return rc;
	}

	return inode_data.count;
}

static bool tree_node_is_in_path(struct zffs_tree_node *node,
				 struct zffs_tree_node *bottom_node)
{
	while (bottom_node) {
		if (bottom_node == node) {
			return true;
		}
		bottom_node = bottom_node->parent;
	}

	return false;
}

static int tree_node_free_other_path_cb(struct zffs_data *zffs,
					struct zffs_tree_node *node,
					void *bottom_node)
{
	struct zffs_tree_node *parent;
	int c, rc;

	if (tree_node_is_in_path(node, bottom_node)) {
		return 0;
	}

	parent = node->parent;

	if (parent == NULL) {
		return -ESPIPE;
	}

	c = tree_find_child(parent, node);
	if (c < 0) {
		return -ESPIPE;
	}

	if (node->disk_addr.offset == ZFFS_TREE_ADDR_WAIT_WRITE) {
		rc = tree_save_node(zffs, node);
		if (rc) {
			return rc;
		}
	}

	parent->disk_child[c] = node->disk_addr;
	atomic_clear_bit(parent->loadflag, c);
	tree_node_free(node);

	return 0;
}

static int tree_node_free_other_path(struct zffs_data *zffs,
				     struct zffs_tree_node *top_node,
				     struct zffs_tree_node *node)
{
	return tree_node_foreach(zffs, top_node, node, false, false,
				 tree_node_free_other_path_cb);
}

static int tree_load_child_confirmation(struct zffs_data *zffs,
					struct zffs_tree_node *node, int child)
{
	int rc;
	bool is_retry = false;

retry:
	if (atomic_test_bit(node->loadflag, child)) {
		return 0;
	}

	rc = tree_load_child(zffs, node, child);
	if (rc == -ENOMEM && !is_retry) {
		rc = tree_node_free_other_path(zffs, zffs->tree_root, node);
		if (rc) {
			return rc;
		}

		is_retry = true;
		goto retry;
	}

	if (rc) {
		return rc;
	}

	atomic_set_bit(node->loadflag, child);
	return 0;
}

static int tree_save_node_cb(struct zffs_data *zffs,
			     struct zffs_tree_node *node, void *data)
{
	return tree_save_node(zffs, node);
}

static int tree_save_node_recursive(struct zffs_data *zffs,
				    struct zffs_tree_node *node)
{
	return tree_node_foreach(zffs, node, NULL, false, false,
				 tree_save_node_cb);
}

// static bool tree_node_is_empty(struct zffs_data *zffs, u32_t addr)
// {
//      struct zffs_area_pointer pointer = zffs_swap_pointer(zffs);

//      zffs_area_pointer_set_addr(zffs, &pointer, addr);

//      return zffs_area_is_not_empty(zffs, &pointer,
//                                    sizeof(struct zffs_tree_disk_node)) !=
//             -ENOTEMPTY;
// }

static int tree_load_root(struct zffs_data *zffs, struct zffs_tree_node *node)
{
	int top, bottom;
	int rc;
	union zffs_addr addr;

	zffs->next_tree.id.is_tree = 1;
	zffs->next_tree.id.is_gc = 0;
	node->parent = NULL;

	if (zffs->tree_areas[0] == NULL) {
		goto empty;
	}

	for (int i = 1; i < zffs->area_num; i++) {
		if (zffs->tree_areas[i] == NULL) {
			break;
		}

		zffs->next_tree.id.num = i;
	}

	top = zffs_area_size_from_id(zffs, zffs->next_tree.id) /
	      sizeof(struct zffs_tree_disk_node);
	bottom = 0;

	node->parent = NULL;

	while (top - bottom > 1) {
		int n = (top + bottom) >> 1;

		zffs->next_tree.offset = n * sizeof(struct zffs_tree_disk_node);

		if (zffs_area_is_empty(zffs, zffs->next_tree,
				       sizeof(struct zffs_tree_disk_node))) {
			top = n;
		} else {
			bottom = n;
		}
	}

	addr = zffs->next_tree;
	while (addr.id.num || addr.offset) {

		if (addr.offset == 0) {
			addr.id.num--;
			addr.offset = zffs_area_size_from_id(zffs, addr.id) /
				      sizeof(struct zffs_tree_disk_node) *
				      sizeof(struct zffs_tree_disk_node);
		}

		addr.offset -=  sizeof(struct zffs_tree_disk_node);
		rc = tree_load_node(zffs, addr, node);
		if (rc) {
			return rc;
		}

		if (node->root) {
			return 0;
		}
	}

empty:
	node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	node->n = 0;
	node->leaf = 1;
	node->root = 1;
	memset(node->loadflag, 0x00, sizeof(node->loadflag));
	return 0;
}

static void tree_node_copy_key(struct zffs_tree_node *from, off_t from_off,
			       struct zffs_tree_node *to, off_t to_off)
{
	to->key[to_off] = from->key[from_off];
	to->value[to_off] = from->value[from_off];
}

static void tree_node_copy_child(struct zffs_tree_node *from, off_t from_off,
				 struct zffs_tree_node *to, off_t to_off)
{
	if (atomic_test_and_clear_bit(from->loadflag, from_off)) {
		to->child[to_off] = from->child[from_off];
		atomic_set_bit(to->loadflag, to_off);
		to->child[to_off]->parent = to;
	} else {
		to->disk_child[to_off] = from->disk_child[from_off];
	}
}

int zffs_tree_init(struct zffs_data *zffs)
{
	if (zffs->tree_root) {
		return 0;
	}

	k_mem_slab_init(&zffs_tree_node_pool,
			_k_mem_slab_buf_zffs_tree_node_pool,
			sizeof(struct zffs_tree_node),
			ZFFS_CONFIG_TREE_CACHE_NODE_MAX);
	zffs->tree_root = tree_node_alloc();

	if (zffs->tree_root == NULL) {
		return -ENOMEM;
	}

	return tree_load_root(zffs, zffs->tree_root);
}

int zffs_tree_search(struct zffs_data *zffs, u32_t key, u32_t *value)
{
	int i;
	int rc;
	struct zffs_tree_node *node = zffs->tree_root;

	while (true) {
		for (i = 0; i < node->n; i++) {
			if (node->key[i] == key) {
				*value = node->value[i];
				return 0;
			} else if (node->key[i] > key) {
				break;
			}
		}

		if (node->leaf) {
			return -ENOENT;
		}

		rc = tree_load_child_confirmation(zffs, node, i);
		if (rc) {
			return rc;
		}

		node = node->child[i];
	}
}

int zffs_tree_update(struct zffs_data *zffs, u32_t key, u32_t value)
{
	int i;
	int rc;
	struct zffs_tree_node *node = zffs->tree_root;

	while (true) {
		for (i = 0; i < node->n; i++) {
			if (node->key[i] == key) {
				node->value[i] = value;
				node->disk_addr.offset =
					ZFFS_TREE_ADDR_WAIT_WRITE;
				return 0;
			} else if (node->key[i] > key) {
				break;
			}
		}

		if (node->leaf) {
			return -EBADF;
		}

		rc = tree_load_child_confirmation(zffs, node, i);
		if (rc) {
			return rc;
		}

		node = node->child[i];
	}
}

static int tree_node_insert(struct zffs_tree_node *node, u32_t key,
			    u32_t value)
{
	int i;
	int j;

	i = 0;
	while (i < node->n && node->key[i] < key) {
		i++;
	}

	for (j = node->n; j > i; j--) {
		tree_node_copy_key(node, j - 1, node, j);
		if (!node->leaf) {
			tree_node_copy_child(node, j, node, j + 1);
		}
	}

	node->n += 1;
	node->key[i] = key;
	node->value[i] = value;
	node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;

	return i;
}

static int tree_split_child(struct zffs_data *zffs, struct zffs_tree_node *node)
{
	struct zffs_tree_node *parent = node->parent;
	struct zffs_tree_node *brothers;
	int rc;
	bool is_retry = false;

	if (!tree_node_is_full(node)) {
		return -ESPIPE;
	}

retry:
	if (parent == NULL) {
		parent = tree_node_alloc();
		if (parent == NULL) {
			rc = -ENOMEM;
			goto done;
		}

		parent->disk_addr.id.is_gc = 0;
		parent->disk_addr.id.is_tree = 1;
		parent->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
		parent->parent = NULL;
		parent->n = 0;
		parent->leaf = 0;
		parent->root = 1;
		memset(parent->loadflag, 0x00, sizeof(parent->loadflag));
		atomic_set_bit(parent->loadflag, 0);
		parent->child[0] = node;
		node->root = 0;
		zffs->tree_root = parent;
		node->parent = parent;
	}

	brothers = tree_node_alloc();
	if (brothers == NULL) {
		rc = -ENOMEM;
		goto done;
	}

	rc = tree_node_insert(parent, node->key[ZFFS_TREE_T - 1],
			      node->value[ZFFS_TREE_T - 1]);
	if (rc < 0) {
		goto err;
	}

	brothers->disk_addr.id.is_gc = 1;
	brothers->disk_addr.id.is_tree = 1;
	brothers->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	brothers->parent = parent;
	brothers->leaf = node->leaf;
	brothers->root = 0;
	memset(brothers->loadflag, 0x00, sizeof(parent->loadflag));

	atomic_set_bit(parent->loadflag, rc + 1);
	parent->child[rc + 1] = brothers;

	for (int i = 0; i < ZFFS_TREE_T - 1; i++) {
		tree_node_copy_key(node, i + ZFFS_TREE_T, brothers, i);
	}

	if (!node->leaf) {
		for (int i = 0; i < ZFFS_TREE_T; i++) {
			tree_node_copy_child(node, i + ZFFS_TREE_T,
					     brothers, i);
		}
	}

	node->n = ZFFS_TREE_T - 1;
	brothers->n = ZFFS_TREE_T - 1;

	rc = 0;

done:
	if (rc == -ENOMEM && !is_retry) {
		rc = tree_node_free_other_path(zffs, zffs->tree_root, node);

		if (!rc) {
			is_retry = true;
			goto retry;
		}
	}

	return rc;

err:
	tree_node_free(brothers);
	return rc;
}

static int tree_merge_child(struct zffs_data *zffs, struct zffs_tree_node *node,
			    int child)
{
	struct zffs_tree_node buf;
	int rc;

	rc = tree_load_child_confirmation(zffs, node, child);
	if (rc) {
		return rc;
	}

	if (node->child[child]->n >= ZFFS_TREE_T) {
		return 1;
	}

	rc = tree_load_child_confirmation(zffs, node, child + 1);
	if (rc) {
		return rc;
	}

	if (node->child[child]->n >= ZFFS_TREE_T) {
		return 2;
	}

	buf = *node->child[child + 1];
	tree_node_free(node->child[child + 1]);

	if (!atomic_test_and_set_bit(node->loadflag, child)) {
		rc = tree_load_child(zffs, node, child);
		if (rc) {
			return rc;
		}
	}

	tree_node_copy_key(node, child, node->child[child], ZFFS_TREE_T);
	for (int i = 0; i < ZFFS_TREE_T; i++) {
		tree_node_copy_key(&buf, i, node->child[child],
				   ZFFS_TREE_T + 1 + i);
	}

	for (int i = child + 1; i <= node->n; i++) {
		tree_node_copy_key(node, i, node, i - 1);
		tree_node_copy_child(node, i + 1, node, i);
	}

	if (!buf.leaf) {
		for (int i = 0; i < ZFFS_TREE_T; i++) {
			tree_node_copy_child(&buf, i, node->child[child],
					     ZFFS_TREE_T + i + 1);
		}
	}

	node->child[child]->n = 2 * ZFFS_TREE_T - 1;

	if (node->root && node->n == 0 && !node->leaf) {
		zffs->tree_root = node->child[0];
		tree_node_free(node);
	}

	return 0;
}

int zffs_tree_insert(struct zffs_data *zffs, u32_t key, u32_t value)
{
	int i;
	int rc;
	struct zffs_tree_node *node = zffs->tree_root;

	while (true) {
		if (tree_node_is_full(node)) {
			rc = tree_split_child(zffs, node);
			if (rc) {
				break;
			}
		}

		for (i = 0; i < node->n; i++) {
			if (node->key[i] == key) {
				return -EEXIST;
			} else if (node->key[i] > key) {
				break;
			}
		}

		if (node->leaf) {
			rc = tree_node_insert(node, key, value);
			if (rc < 0) {
				break;
			}
			rc = 0;
			break;
		}

		rc = tree_load_child_confirmation(zffs, node, i);
		if (rc) {
			break;
		}

		node = node->child[i];
	}

	return rc;
}

int zffs_tree_sync(struct zffs_data *zffs)
{
	return tree_save_node_recursive(zffs, zffs->tree_root);
}

static int tree_info_cb(struct zffs_data *zffs, u32_t key, u32_t value,
			struct zffs_tree_info *info)
{
	if (key > info->key_max) {
		info->key_max = key;
	}

	if (value < info->value_min) {
		info->value_min = value;
	}

	if (value > info->value_max) {
		info->value_max = value;
	}

	info->key_count++;
	return 0;
}

int zffs_tree_info(struct zffs_data *zffs, struct zffs_tree_info *info)
{
	int rc;

	info->key_count = 0;
	info->key_max = 0;
	info->value_min = 0xffffffff;
	info->value_max = 0;

	rc = tree_key_foreach(zffs, zffs->tree_root, info, true, true,
			      (zffs_tree_foreach_fun_t)tree_info_cb);
	if (rc < 0) {
		return rc;
	}
	info->node_count = rc;
	return 0;
}

int zffs_tree_foreach(struct zffs_data *zffs, void *data,
		      zffs_tree_foreach_fun_t callback)
{
	return tree_key_foreach(zffs, zffs->tree_root, data, true, true,
				callback);
}

static int tree_gc_cb(struct zffs_data *zffs,
		      struct zffs_tree_node *node,
		      void *data)
{
	node->disk_addr.id.is_gc = 1;

	if (node->leaf) {
		node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
		return 0;
	}

	for (int i = 0; i <= node->n; i++) {
		node->disk_addr.id.is_gc = 0;
	}

	return 0;
}

int zffs_tree_gc(struct zffs_data *zffs)
{
	int rc;

	rc = zffs_tree_sync(zffs);
	if (rc < 0) {
		return rc;
	}

	zffs->swap = &zffs->tree_areas[zffs->next_tree.id.num + 1];
	zffs->next_swap.full = 0;
	zffs->next_swap.id.is_gc = 1;
	zffs->next_swap.id.is_tree = 1;

	rc = tree_node_foreach(zffs, zffs->tree_root, NULL, true, true,
			       tree_gc_cb);
	if (rc < 0) {
		for (int i = 0; i <= zffs->next_swap.id.num; i++) {
			zffs_area_erase(zffs, zffs->swap[i]);
			zffs->swap[i] = NULL;
		}
		return rc;
	}

	for (int i = 0; i <= zffs->next_tree.id.num; i++) {
		zffs_area_erase(zffs, zffs->tree_areas[i]);
		zffs->tree_areas[i] = NULL;
	}

	for (int i = 0; i <= zffs->next_swap.id.num; i++) {
		zffs->tree_areas[i] = zffs->swap[i];
		zffs->swap[i] = NULL;
	}

	zffs->swap = NULL;

	return zffs_tree_sync(zffs);
}


static int tree_node_left_rotate(struct zffs_data *zffs,
				 struct zffs_tree_node *node, int i)
{
	int rc;
	bool isload;
	u32_t key;
	u32_t value;
	struct zffs_tree_node *child;
	union zffs_addr disk_child;


	rc = tree_load_child_confirmation(zffs, node, i + 1);
	if (rc) {
		return rc;
	}

	key = node->key[i];
	value = node->value[i];
	tree_node_copy_key(node->child[i + 1], 0, node, i);
	for (int j = 1; j < node->child[i + 1]->n; j++) {
		tree_node_copy_key(node->child[i + 1], j, node->child[i + 1],
				   j - 1);
	}

	if (!node->child[i + 1]->leaf) {
		isload = atomic_test_bit(node->child[i + 1]->loadflag, 0);
		if (isload) {
			child = node->child[i + 1]->child[0];
		} else {
			disk_child = node->child[i + 1]->disk_child[0];
		}

		for (int j = 1; j <= node->child[i + 1]->n; j++) {
			tree_node_copy_child(node->child[i + 1], j,
					     node->child[i + 1], j - 1);
		}

	}

	node->child[i + 1]->n--;
	node->child[i + 1]->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	rc = tree_load_child_confirmation(zffs, node, i);
	if (rc) {
		return rc;
	}

	node->child[i]->key[node->child[i]->n] = key;
	node->child[i]->value[node->child[i]->n] = value;
	node->child[i]->n++;
	node->child[i]->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;

	if (node->child[i]->leaf) {
		return 0;
	}

	if (isload) {
		node->child[i]->child[node->child[i]->n] = child;
		atomic_set_bit(node->child[i]->loadflag, node->child[i]->n);
		child->parent = node->child[i];
	} else {
		node->child[i]->disk_child[node->child[i]->n] = disk_child;
		atomic_clear_bit(node->child[i]->loadflag, node->child[i]->n);
	}

	return 0;
}

static int tree_node_right_rotate(struct zffs_data *zffs,
				  struct zffs_tree_node *node, int i)
{
	int rc;
	bool isload;
	u32_t key;
	u32_t value;
	struct zffs_tree_node *child;
	union zffs_addr disk_child;

	rc = tree_load_child_confirmation(zffs, node, i);
	if (rc) {
		return rc;
	}

	key = node->key[i];
	value = node->value[i];
	tree_node_copy_key(node->child[i], node->child[i]->n - 1, node, i);

	if (!node->child[i]->leaf) {
		isload = atomic_test_bit(node->child[i]->loadflag,
					 node->child[i]->n);
		if (isload) {
			child = node->child[i]->child[node->child[i]->n];
		} else {
			disk_child =
				node->child[i]->disk_child[node->child[i]->n];
		}
	}

	node->child[i]->n--;
	node->child[i]->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
	rc = tree_load_child_confirmation(zffs, node, i);
	if (rc) {
		return rc;
	}

	for (int j = node->child[i + 1]->n; j > 0; j--) {
		tree_node_copy_key(node->child[i + 1], j - 1,
				   node->child[i + 1], j);
	}
	node->child[i + 1]->n++;
	node->child[i + 1]->key[0] = key;
	node->child[i + 1]->value[0] = value;
	node->child[i + 1]->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;

	if (node->child[i + 1]->leaf) {
		return 0;
	}

	for (int j = node->child[i + 1]->n; j > 0; j--) {
		tree_node_copy_child(node->child[i + 1], j - 1,
				     node->child[i + 1], j);
	}

	if (isload) {
		node->child[i + 1]->child[0] = child;
		atomic_set_bit(node->child[i + 1]->loadflag, 0);
		child->parent = node->child[i + 1];
	} else {
		node->child[i + 1]->disk_child[0] = disk_child;
		atomic_clear_bit(node->child[i + 1]->loadflag, 0);
	}

	return 0;
}

static int tree_node_adj_child(struct zffs_data *zffs,
			       struct zffs_tree_node *node, int *i)
{
	int rc;

	rc = tree_load_child_confirmation(zffs, node, *i);
	if (rc) {
		return rc;
	}

	if (node->child[*i]->n >= ZFFS_TREE_T) {
		return 0;
	}

	if (*i != node->n) {
		rc = tree_load_child_confirmation(zffs, node, *i + 1);
		if (rc) {
			return rc;
		}

		if (node->child[*i + 1]->n < ZFFS_TREE_T) {
			rc = tree_merge_child(zffs, node, *i);
			if (rc < 0) {
				return rc;
			}
			if (rc) {
				return -EIO;
			}
			return 0;
		}

		return tree_node_right_rotate(zffs, node, *i);
	}

	if (*i != 0) {
		rc = tree_load_child_confirmation(zffs, node, *i - 1);
		if (rc) {
			return rc;
		}

		if (node->child[*i - 1]->n < ZFFS_TREE_T) {
			rc = tree_merge_child(zffs, node, *i - 1);
			if (rc < 0) {
				return rc;
			}
			if (rc) {
				return -EIO;
			}
			*i = *i - 1;
			return 0;
		}

		return tree_node_left_rotate(zffs, node, *i);
	}

	return -EIO;
}

static void tree_leaf_node_delete_from_off(struct zffs_tree_node *node, int i)
{
	node->n--;
	for (; i < node->n; i++) {
		tree_node_copy_key(node, i + 1, node, i);
	}

	node->disk_addr.offset = ZFFS_TREE_ADDR_WAIT_WRITE;
}

static void tree_leaf_node_delete(struct zffs_tree_node *node, u32_t key)
{
	for (int i = 0; i < node->n; i++) {
		if (node->key[i] == key) {
			tree_leaf_node_delete_from_off(node, i);
			return;
		}
		if (node->key[i] > key) {
			break;
		}
	}
}

int zffs_tree_delete(struct zffs_data *zffs, u32_t key)
{
	int i, _i;
	int rc;
	struct zffs_tree_node *node = zffs->tree_root, *_node;
	int flag = 0;

	while (true) {
		if (node->leaf) {
			tree_leaf_node_delete(node, key);
			return 0;
		}

		for (i = 0; i < node->n; i++) {
			if (node->key[i] == key) {
				rc = tree_merge_child(zffs, node, i);
				if (rc <= 0) {
					return rc;
				}
				flag = rc;
				_node = node;
				_i = i;
				i = flag == 1 ? i : i + 1;
				node = node->child[i];

				while (!node->leaf) {
					rc = tree_node_adj_child(zffs, node,
								 &i);
					if (rc) {
						return rc;
					}
					i = flag == 1 ? node->n : 0;
					node = node->child[i];
				}
				i = flag == 1 ? node->n : 0;
				_node->key[_i] = node->key[i];
				_node->value[_i] = node->value[i];
				tree_leaf_node_delete_from_off(node, i);

				return 0;
			}
			if (node->key[i] > key) {
				break;
			}
		}

		rc = tree_node_adj_child(zffs, node, &i);
		if (rc) {
			return rc;
		}

		node = node->child[i];
	}
}
