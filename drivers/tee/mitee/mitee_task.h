/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 YorokobiMaster (GitHub: @YorokobiMaster)
 *
 * Independent reimplementation of functionality present in Xiaomi
 * stock software.
 *
 * This implementation does not claim ownership of the original vendor
 * design, interfaces, protocols, firmware, or other third-party
 * intellectual property. Such rights remain with their respective owners.
 */

#ifndef MITEE_TASK_H
#define MITEE_TASK_H

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/types.h>

struct optee;
struct optee_msg_arg;
struct tee_context;

#define MITEE_WORKER_COUNT		2
#define MITEE_MSG_QUEUE_SIZE		0x3000
#define MITEE_MSG_SLOT_SIZE		0x100
#define MITEE_MSG_SLOT_COUNT		16
#define MITEE_MSG_PAYLOAD_SIZE		0xec

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
	u8 payload[MITEE_MSG_PAYLOAD_SIZE];
};

struct mitee_msg_buf {
	struct mutex lock;
	struct mitee_msg_ring *ring;
	struct mitee_msg *messages;
};

struct mitee_msg_queue {
	phys_addr_t pa;
	u32 size;
	u32 slot_size;
	void *va;
	struct mitee_msg_buf tx;
	struct mitee_msg_buf rx;
};

struct mitee_task {
	struct tee_context *ctx;
	struct list_head node;
	struct completion completion;
	struct optee_msg_arg *arg;
	int id;
	u32 command;
	u32 state;
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
void mitee_msg_queue_deinit(struct mitee_msg_queue *queue);
int mitee_msg_queue_register(struct optee *optee);

void mitee_task_list_init(struct mitee_task_list *tasks);
void mitee_task_list_deinit(struct mitee_task_list *tasks);

int mitee_workers_init(struct optee *optee);
void mitee_workers_deinit(struct optee *optee);
int mitee_do_call_with_arg(struct tee_context *ctx,
			   struct optee_msg_arg *arg);

#endif /* MITEE_TASK_H */
