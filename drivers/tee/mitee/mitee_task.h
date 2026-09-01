// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 YorokobiMaster (GitHub: @YorokobiMaster)
 *
 * Reverse-engineered reimplementation of functionality present in Xiaomi
 * stock software, based on analysis of the shipped binary interfaces,
 * data structures, constants, behavior, and control semantics.
 *
 * No corresponding source implementation was available in the
 * vendor-published kernel sources.
 *
 * This file should not have been necessary if the corresponding source
 * had been available.
 *
 * This notice applies only to the newly written source code in this file
 * and does not claim ownership of vendor-originated interfaces, protocols,
 * firmware, behavior, or other third-party intellectual property.
 *
 * Provided without warranty.
 */

#ifndef MITEE_TASK_H
#define MITEE_TASK_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct optee;
struct optee_msg_arg;
struct tee_context;

#define MITEE_WORKER_COUNT		2
#define MITEE_MSG_QUEUE_SIZE		0x3000
#define MITEE_MSG_SLOT_SIZE		0x100
#define MITEE_MSG_BUF_SIZE		0x1000
#define MITEE_MSG_SLOT_COUNT		16
#define MITEE_MSG_ARG_SIZE		0xe0

#define MITEE_MSG_MAGIC			0x4d49
#define MITEE_MSG_VERSION		1
#define MITEE_MSG_CMD_CALL		1
#define MITEE_MSG_CMD_RPC_REPLY		2
#define MITEE_MSG_CMD_DONE		0x80000001
#define MITEE_MSG_CMD_RPC		0x80000002

#define MITEE_FFA_MSG_QUEUE_REGISTER	0x0c
#define MITEE_FFA_MSG_SEND		0x80000000
#define MITEE_FFA_MSG_RESUME		0x80000002

struct mitee_msg_ring {
	u64 head;
	u64 tail;
	u64 capacity;
};

struct mitee_msg {
	u32 magic;
	u32 version;
	u32 reserved;
	s32 task_id;
	u32 command;
	u8 payload[MITEE_MSG_ARG_SIZE];
	u64 timestamp;
	u32 timestamp_reserved;
} __packed;

struct mitee_msg_buf {
	struct mutex lock;
	struct mitee_msg_ring *ring;
	struct mitee_msg *messages;
};

enum mitee_msg_queue_state {
	MITEE_MSG_QUEUE_EMPTY,
	MITEE_MSG_QUEUE_LOCAL,
	MITEE_MSG_QUEUE_PUBLISHED,
	MITEE_MSG_QUEUE_REVOKED,
	MITEE_MSG_QUEUE_RETAINED,
};

struct mitee_msg_queue {
	phys_addr_t pa;
	u32 size;
	u32 buf_size;
	void *va;
	struct mitee_msg_buf tx;
	struct mitee_msg_buf rx;
	/* Serializes ownership state transitions. */
	struct mutex state_lock;
	enum mitee_msg_queue_state state;
};

struct mitee_task {
	struct tee_context *ctx;
	struct list_head node;
	struct completion completion;
	struct optee_msg_arg *arg;
	size_t arg_size;
	int id;
	u32 command;
	u32 state;
	int result;
};

struct mitee_task_list {
	struct mutex lock;
	struct idr idr;
	struct list_head pending;
};

struct mitee_worker {
	u32 id;
	struct task_struct *thread;
	atomic_t busy;
	struct completion work;
	struct optee *optee;
};

int mitee_msg_queue_init(struct mitee_msg_queue *queue);
void mitee_msg_queue_deinit(struct mitee_msg_queue *queue,
			    const char *reason);
int mitee_msg_queue_register(struct optee *optee);

void mitee_task_list_init(struct mitee_task_list *tasks);
void mitee_task_list_deinit(struct mitee_task_list *tasks);
int mitee_worker_fn(void *data);
int mitee_workers_init(struct optee *optee);
void mitee_workers_deinit(struct optee *optee);
int optee_do_call_with_arg(struct tee_context *ctx,
			   struct optee_msg_arg *arg);

#endif /* MITEE_TASK_H */
