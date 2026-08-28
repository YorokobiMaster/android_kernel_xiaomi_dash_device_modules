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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/arm_ffa.h>
#include <linux/build_bug.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include <tee_drv.h>
#include "mitee_task.h"
#include "optee_msg.h"
#include "optee_private.h"
#include "optee_rpc_cmd.h"

static_assert(sizeof(struct mitee_msg_ring) == 0x18);
static_assert(sizeof(struct mitee_msg) == MITEE_MSG_SLOT_SIZE);
static_assert(offsetof(struct mitee_msg, payload) == 0x14);
static_assert(offsetof(struct mitee_msg, timestamp) == 0xf4);

static int mitee_msg_enqueue(struct mitee_msg_queue *queue,
			     const struct mitee_msg *msg)
{
	struct mitee_msg_ring *ring = queue->tx.ring;
	u64 head;
	u64 next;

	mutex_lock(&queue->tx.lock);
	head = READ_ONCE(ring->head);
	next = (head + 1) % READ_ONCE(ring->capacity);
	if (next == READ_ONCE(ring->tail)) {
		pr_err("tx buf full\n");
		mutex_unlock(&queue->tx.lock);
		return -EBUSY;
	}

	memcpy(&queue->tx.messages[head], msg, sizeof(*msg));
	smp_store_release(&ring->head, next);
	mutex_unlock(&queue->tx.lock);
	return 0;
}

static int mitee_msg_dequeue(struct mitee_msg_queue *queue,
			     struct mitee_msg *msg)
{
	struct mitee_msg_ring *ring = queue->rx.ring;
	u64 tail;

	mutex_lock(&queue->rx.lock);
	if (READ_ONCE(ring->tail) == READ_ONCE(ring->head)) {
		pr_err("rx buf empty\n");
		mutex_unlock(&queue->rx.lock);
		return -EBUSY;
	}

	tail = READ_ONCE(ring->tail);
	memcpy(msg, &queue->rx.messages[tail], sizeof(*msg));
	memset(&queue->rx.messages[tail], 0, sizeof(*msg));
	smp_store_release(&ring->tail,
			  (tail + 1) % READ_ONCE(ring->capacity));
	mutex_unlock(&queue->rx.lock);
	return 0;
}

int mitee_msg_queue_init(struct mitee_msg_queue *queue)
{
	void *va;

	if (!queue) {
		pr_err("invalid message queue\n");
		return -EINVAL;
	}

	va = alloc_pages_exact(MITEE_MSG_QUEUE_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!va) {
		pr_err("failed to allocate message queue\n");
		return -ENOMEM;
	}

	queue->pa = virt_to_phys(va);
	queue->size = MITEE_MSG_QUEUE_SIZE;
	queue->buf_size = MITEE_MSG_BUF_SIZE;

	mutex_init(&queue->rx.lock);
	queue->rx.ring = va;
	queue->rx.messages = va + 0x1000;
	queue->rx.ring->capacity = MITEE_MSG_SLOT_COUNT;

	mutex_init(&queue->tx.lock);
	queue->tx.ring = va + 0x800;
	queue->tx.messages = va + 0x2000;
	queue->tx.ring->capacity = MITEE_MSG_SLOT_COUNT;

	pr_info("allocated message queue pa: %#llx va: %p\n",
		(unsigned long long)queue->pa, va);
	return 0;
}

int mitee_msg_queue_deinit(struct mitee_msg_queue *queue)
{
	if (!queue) {
		pr_err("invalid message queue\n");
		return -EINVAL;
	}

	free_pages_exact(phys_to_virt(queue->pa), queue->size);
	pr_info("message queue destroyed\n");
	return 0;
}

int mitee_msg_queue_register(void)
{
	struct optee *optee = get_optee_drv_state();
	struct ffa_send_direct_data data = {
		.data0 = MITEE_FFA_MSG_QUEUE_REGISTER,
		.data1 = optee->msg_queue.pa,
		.data2 = optee->msg_queue.size,
	};
	int rc;

	if (!optee) {
		pr_err("driver state is NULL\n");
		return -EINVAL;
	}

	rc = optee->comm_ops->call(&data);
	if (rc)
		pr_err("failed to register message queue: %d\n", rc);
	return rc;
}

void mitee_task_list_init(struct mitee_task_list *tasks)
{
	mutex_init(&tasks->lock);
	idr_init(&tasks->idr);
	INIT_LIST_HEAD(&tasks->pending);
}

void mitee_task_list_deinit(struct mitee_task_list *tasks)
{
	idr_destroy(&tasks->idr);
}

struct mitee_task *mitee_task_alloc(struct tee_context *ctx, u32 command,
				    struct optee_msg_arg *arg)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct mitee_task *task;
	int id;

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (!task)
		return ERR_PTR(-ENOMEM);

	task->ctx = ctx;
	INIT_LIST_HEAD(&task->node);
	init_completion(&task->completion);
	task->command = command;
	if (command != MITEE_MSG_CMD_CALL) {
		pr_err("invalid task command: %u\n", command);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&optee->tasks.lock);
	id = idr_alloc(&optee->tasks.idr, task, 1, INT_MAX, GFP_KERNEL);
	task->id = id;
	if (id <= 0) {
		mutex_unlock(&optee->tasks.lock);
		return ERR_PTR(-EINVAL);
	}
	task->arg = arg;
	task->state = 1;
	list_add_tail(&task->node, &optee->tasks.pending);
	mutex_unlock(&optee->tasks.lock);
	return task;
}

void mitee_task_free(int id)
{
	struct optee *optee = get_optee_drv_state();
	struct mitee_task *task;

	mutex_lock(&optee->tasks.lock);
	task = idr_find(&optee->tasks.idr, id);
	if (!task)
		pr_warn("task id invalid: %d\n", id);
	idr_remove(&optee->tasks.idr, id);
	mutex_unlock(&optee->tasks.lock);
	kfree(task);
}

static struct mitee_task *mitee_task_take(struct optee *optee)
{
	struct mitee_task *task = NULL;

	mutex_lock(&optee->tasks.lock);
	if (!list_empty(&optee->tasks.pending)) {
		task = list_first_entry(&optee->tasks.pending,
					struct mitee_task, node);
		list_del(&task->node);
	}
	mutex_unlock(&optee->tasks.lock);
	return task;
}

static __always_inline int mitee_msg_pack(struct mitee_msg *msg, int task_id,
					  u32 command,
					  const struct optee_msg_arg *arg)
{
	size_t size = 0;
	struct timespec64 ts;

	memset(msg, 0, sizeof(*msg));
	msg->magic = MITEE_MSG_MAGIC;
	msg->version = MITEE_MSG_VERSION;
	msg->task_id = task_id;
	msg->command = command;
	if (arg) {
		size = OPTEE_MSG_GET_ARG_SIZE(arg->num_params);
		memcpy(msg->payload, arg, size);
	}
	ktime_get_real_ts64(&ts);
	msg->timestamp = ts.tv_sec;
	return 0;
}

static int mitee_msg_unpack(const struct mitee_msg *msg,
			    struct optee_msg_arg **arg)
{
	if (msg->magic != MITEE_MSG_MAGIC) {
		pr_err("invalid magic\n");
		return -EINVAL;
	}
	if (msg->version != MITEE_MSG_VERSION) {
		pr_err("invalid version\n");
		return -EINVAL;
	}

	*arg = (struct optee_msg_arg *)msg->payload;
	return 0;
}

static void handle_rpc_shm_alloc(struct tee_context *ctx,
				 struct optee_msg_arg *arg)
{
	struct tee_shm *shm;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT) {
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		shm = optee_rpc_cmd_alloc_suppl(ctx, arg->params[0].u.value.b);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
		shm = tee_shm_alloc(ctx, arg->params[0].u.value.b,
				    TEE_SHM_MAPPED | TEE_SHM_PRIV);
		break;
	case OPTEE_RPC_SHM_TYPE_GLOBAL:
		shm = tee_shm_alloc(ctx, arg->params[0].u.value.b,
				    TEE_SHM_MAPPED | TEE_SHM_PRIV |
				    TEE_SHM_DMA_BUF);
		break;
	default:
		arg->ret = TEEC_ERROR_BAD_PARAMETERS;
		return;
	}

	if (IS_ERR(shm)) {
		arg->ret = TEEC_ERROR_OUT_OF_MEMORY;
		return;
	}

	arg->params[0] = (struct optee_msg_param) {
		.attr = OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT,
		.u.tmem.buf_ptr = shm->paddr,
		.u.tmem.size = tee_shm_get_size(shm),
		.u.tmem.shm_ref = (unsigned long)shm,
	};
	arg->ret = TEEC_SUCCESS;
}

static void handle_rpc_shm_free(struct tee_context *ctx,
				struct optee_msg_arg *arg)
{
	struct tee_shm *shm;

	if (arg->num_params != 1 ||
	    arg->params[0].attr != OPTEE_MSG_ATTR_TYPE_VALUE_INPUT)
		goto bad;

	shm = (struct tee_shm *)arg->params[0].u.value.b;
	if (!shm)
		goto bad;

	switch (arg->params[0].u.value.a) {
	case OPTEE_RPC_SHM_TYPE_APPL:
		optee_rpc_cmd_free_suppl(ctx, shm);
		break;
	case OPTEE_RPC_SHM_TYPE_KERNEL:
	case OPTEE_RPC_SHM_TYPE_GLOBAL:
		tee_shm_free(shm);
		break;
	default:
		goto bad;
	}

	arg->ret = TEEC_SUCCESS;
	return;
bad:
	arg->ret = TEEC_ERROR_BAD_PARAMETERS;
}

static void handle_rpc(struct tee_context *ctx, struct optee_msg_arg *arg)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);

	arg->ret_origin = TEEC_ORIGIN_COMMS;
	switch (arg->cmd) {
	case OPTEE_RPC_CMD_SHM_ALLOC:
		handle_rpc_shm_alloc(ctx, arg);
		break;
	case OPTEE_RPC_CMD_SHM_FREE:
		handle_rpc_shm_free(ctx, arg);
		break;
	default:
		optee_rpc_cmd(ctx, optee, arg);
	}
}

int mitee_worker_fn(void *data)
{
	struct mitee_worker *worker = data;
	struct optee *optee = worker->optee;
	struct mitee_task *task;
	struct optee_msg_arg *arg;
	struct mitee_msg msg;
	struct mitee_msg reply;
	struct ffa_send_direct_data ffa_data;
	int response_task_id;
	int rc;

	atomic_inc(&optee->workers_started);
	BUG_ON(worker->id >= MITEE_WORKER_COUNT);

	for (;;) {
		wait_for_completion(&worker->work);

		if (!(current->flags & PF_NO_SETAFFINITY)) {
			rc = set_cpus_allowed_ptr(current, &optee->cpus_allowed);
			if (rc)
				pr_warn("worker affinity failed: %d\n", rc);
		}

		task = mitee_task_take(optee);
		if (!task) {
			pr_err("task list empty\n");
			atomic_set(&worker->busy, 0);
			continue;
		}

		rc = mitee_msg_pack(&msg, task->id, task->command, task->arg);
		if (rc)
			goto task_error;
		task->state = 3;

		for (;;) {
			rc = mitee_msg_enqueue(&optee->msg_queue, &msg);
			if (rc)
				goto task_error;

			ffa_data = (struct ffa_send_direct_data) {
				.data0 = MITEE_FFA_MSG_SEND,
			};
			rc = optee->comm_ops->call(&ffa_data);
			if (!rc)
				atomic_notifier_call_chain(&optee->notifier,
							   MITEE_CALL_RETURNED,
							   NULL);
			while (!rc && ffa_data.data0 == 2) {
				ffa_data = (struct ffa_send_direct_data) {
					.data0 = MITEE_FFA_MSG_RESUME,
				};
				rc = optee->comm_ops->call(&ffa_data);
				if (!rc)
					atomic_notifier_call_chain(&optee->notifier,
								   MITEE_CALL_RETURNED,
								   NULL);
			}
			if (rc)
				goto task_error;

			rc = mitee_msg_dequeue(&optee->msg_queue, &msg);
			if (rc)
				goto task_error;
			rc = mitee_msg_unpack(&msg, &arg);
			if (rc)
				goto task_error;
			response_task_id = msg.task_id;

			while (!READ_ONCE(optee->supp.ctx))
				msleep_interruptible(100);

			if (msg.command == MITEE_MSG_CMD_RPC) {
				handle_rpc(optee->supp.ctx, arg);
				rc = mitee_msg_pack(&reply, response_task_id,
						    MITEE_MSG_CMD_RPC_REPLY, arg);
				if (rc)
					goto task_error;
				msg = reply;
				continue;
			}

			if (msg.command == MITEE_MSG_CMD_DONE) {
				struct mitee_task *done;

				mutex_lock(&optee->tasks.lock);
				done = idr_find(&optee->tasks.idr,
						response_task_id);
				if (done) {
					memcpy(done->arg, arg,
					       OPTEE_MSG_GET_ARG_SIZE(arg->num_params));
					done->state = 4;
					atomic_set(&worker->busy, 0);
					complete(&done->completion);
				}
				mutex_unlock(&optee->tasks.lock);
				if (!done) {
					rc = -EINVAL;
					goto task_error;
				}
				break;
			}

			rc = -EINVAL;
			goto task_error;
		}
		continue;

task_error:
		pr_err("worker %u call failed: %d\n", worker->id, rc);
		atomic_set(&worker->busy, 0);
		BUG_ON(rc);
	}
}

int optee_do_call_with_arg(struct tee_context *ctx,
			   struct optee_msg_arg *arg)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct mitee_task *task;
	struct mitee_worker *worker = NULL;
	cpumask_t saved_mask;
	unsigned int n;
	int task_id;
	int rc;

	rc = down_interruptible(&optee->concurrency);
	if (rc)
		return rc;

	cpumask_copy(&saved_mask, &current->cpus_mask);
	if (!(current->flags & PF_NO_SETAFFINITY)) {
		rc = set_cpus_allowed_ptr(current, &optee->cpus_allowed);
		if (rc)
			pr_warn("caller affinity failed: %d\n", rc);
	}

	for (n = 0; n < MITEE_WORKER_COUNT; n++) {
		if (atomic_cmpxchg(&optee->workers[n].busy, 0, 1) == 0) {
			worker = &optee->workers[n];
			break;
		}
	}
	if (!worker) {
		pr_err("all workers are busy\n");
		rc = 0;
		goto out;
	}

	task = mitee_task_alloc(ctx, MITEE_MSG_CMD_CALL, arg);
	if (IS_ERR(task)) {
		pr_err("failed to allocate task: %ld\n", PTR_ERR(task));
		rc = -ENOMEM;
		goto out;
	}

	complete(&worker->work);
	wait_for_completion(&task->completion);
	rc = 0;
	task_id = task->id;
	mitee_task_free(task_id);
out:
	up(&optee->concurrency);
	set_cpus_allowed_ptr(current, &saved_mask);
	return rc;
}
