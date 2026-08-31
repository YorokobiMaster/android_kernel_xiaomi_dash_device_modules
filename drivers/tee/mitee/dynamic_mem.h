/*
 * Copyright (C) 2015 Google, Inc.
 * Copyright (C) 2024 XiaoMi, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __DYNAMIC_MEM_H
#define __DYNAMIC_MEM_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

struct mem_desc {
	struct sg_table *sgt;
	uint64_t global_id;
	uint32_t mem_size;
	struct list_head node;
};

struct mitee_dynamic_mem_queue {
	struct list_head mem_node;
	struct mutex mem_mut;
};

void mitee_dynamic_mem_add(struct mitee_dynamic_mem_queue *queue,
			   struct mem_desc *desc);
struct mem_desc *mitee_dynamic_mem_take(struct mitee_dynamic_mem_queue *queue,
					uint64_t mem_handle);
struct mem_desc *mitee_dynamic_mem_take_first(
				struct mitee_dynamic_mem_queue *queue);
void mitee_dynamic_mem_restore(struct mitee_dynamic_mem_queue *queue,
			       struct mem_desc *desc);
unsigned int mitee_dynamic_mem_count(struct mitee_dynamic_mem_queue *queue);

/* mem_size: [in] target memory size going to free
 * sgt: target memory assigned in sg_table to free
 * free memory that assigned in a sg_table with size of mem_size
 */
void mitee_free_memory_sgt(uint32_t mem_size, struct sg_table *sgt);

/* mem_size: [in] targe memory size want to allocate
 * out_sgt: [out] target memory sg_table will assigned in
 * allocate a large memory as size of mem_size and assigned to out_sgt
 */
int mitee_alloc_memory_sgt(uint32_t mem_size, struct sg_table **out_sgt);

void mitee_dynamic_mem_init(struct mitee_dynamic_mem_queue *queue);
unsigned int mitee_dynamic_mem_deinit(
				struct mitee_dynamic_mem_queue *queue);

#endif
