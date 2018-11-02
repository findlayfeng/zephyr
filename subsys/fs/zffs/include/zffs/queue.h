/*
 * Copyright (c) 2018 Findlay Feng
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef H_ZFFS_QUEUE_
#define H_ZFFS_QUEUE_

#include "area.h"
#include "zffs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*zffs_node_compar_fn_t)(struct zffs_data *zffs,
				     union zffs_addr *addr, const void *data,
				     const void *node, void *node_data,
				     u32_t len);

struct zffs_slist_node {
	u32_t id;
	u32_t next;
};

struct zffs_slist {
	bool wait_update;
	u32_t head;
};

#define zffs_slist_init(_slist)	\
	(*(_slist) =		\
		 (struct zffs_slist){.head = ZFFS_NULL, .wait_update = true })

#define zffs_slist_is_tail(_node) ((_node)->next == ZFFS_NULL)
#define zffs_slist_is_empty(_slist) ((_slist)->head == ZFFS_NULL)

int zffs_slist_prepend(struct zffs_data *zffs, struct zffs_slist *slist,
		       struct zffs_slist_node *node,
		       const void *data, u32_t len);

int zffs_slist_head(struct zffs_data *zffs, const struct zffs_slist *slist,
		    struct zffs_slist_node *node, union zffs_addr *addr);

int zffs_slist_next(struct zffs_data *zffs, struct zffs_slist_node *node,
		    union zffs_addr *addr);

int zffs_slist_search(struct zffs_data *zffs, const struct zffs_slist *slist,
		      const void *data, zffs_node_compar_fn_t compar_fn,
		      struct zffs_slist_node *node, void *node_data);

int zffs_slist_remove(struct zffs_data *zffs, struct zffs_slist *slist,
		      struct zffs_slist_node *node);

int zffs_slist_open_ex(struct zffs_data *zffs, u32_t id, union zffs_addr *addr);

int zffs_slist_updata_ex(struct zffs_data *zffs, u32_t id, const void *data,
			 u32_t len);

struct zffs_dlist_node {
	u32_t id;
	u32_t prev;
	u32_t next;
};

struct zffs_dlist {
	bool wait_update;
	u32_t head;
	u32_t tail;
};

#define zffs_dlist_init(_dlist)		  \
	(*(_dlist) = (struct zffs_dlist){ \
		.head = ZFFS_NULL, .tail = ZFFS_NULL, .wait_update = true })
#define zffs_dlist_is_empty(_dlist) ((_dlist)->head == ZFFS_NULL)
#define zffs_dlist_is_tail(_node) ((_node)->next == ZFFS_NULL)
#define zffs_dlist_is_head(_node) ((_node)->prev == ZFFS_NULL)

int zffs_dlist_updata_ex(struct zffs_data *zffs, u32_t id, const void *data,
			 u32_t len);

int zffs_dlist_append(struct zffs_data *zffs, struct zffs_dlist *dlist,
		      struct zffs_dlist_node *node, const void *data,
		      u32_t len);

int zffs_dlist_head(struct zffs_data *zffs, struct zffs_dlist *dlist,
		    struct zffs_dlist_node *node, union zffs_addr *addr);

int zffs_dlist_tail(struct zffs_data *zffs, struct zffs_dlist *dlist,
		    struct zffs_dlist_node *node, union zffs_addr *addr);

int zffs_dlist_next(struct zffs_data *zffs, struct zffs_dlist_node *node,
		    union zffs_addr *addr);

int zffs_dlist_prev(struct zffs_data *zffs, struct zffs_dlist_node *node,
		    union zffs_addr *addr);

#ifdef __cplusplus
}
#endif

#endif
