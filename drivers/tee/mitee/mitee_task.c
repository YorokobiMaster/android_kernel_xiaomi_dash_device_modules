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
#include "rpc_callback.h"

static_assert(sizeof(struct mitee_msg_ring) == 0x18);
static_assert(sizeof(struct mitee_msg) == MITEE_MSG_SLOT_SIZE);
static_assert(offsetof(struct mitee_msg, payload) == 0x14);
static_assert(offsetof(struct mitee_msg, timestamp) == 0xf4);

static const char *mitee_msg_queue_state_name(enum mitee_msg_queue_state state)
{
	switch (state) {
	case MITEE_MSG_QUEUE_EMPTY:
		return "empty";
	case MITEE_MSG_QUEUE_LOCAL:
		return "local";
	case MITEE_MSG_QUEUE_PUBLISHED:
		return "published";
	case MITEE_MSG_QUEUE_REVOKED:
		return "revoked";
	case MITEE_MSG_QUEUE_RETAINED:
		return "retained";
	default:
		return "invalid";
	}
}

static int mitee_msg_enqueue(struct mitee_msg_queue *queue,
			     const struct mitee_msg *msg)
{
	struct mitee_msg_ring *ring = queue->tx.ring;
	u64 capacity;
	u64 head;
	u64 next;

	mutex_lock(&queue->tx.lock);
	capacity = READ_ONCE(ring->capacity);
	if (capacity != MITEE_MSG_SLOT_COUNT) {
		mutex_unlock(&queue->tx.lock);
		return -EPROTO;
	}
	head = READ_ONCE(ring->head);
	if (head >= capacity || READ_ONCE(ring->tail) >= capacity) {
		mutex_unlock(&queue->tx.lock);
		return -EPROTO;
	}
	next = (head + 1) % capacity;
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
	u64 capacity;
	u64 head;
	u64 tail;

	mutex_lock(&queue->rx.lock);
	capacity = READ_ONCE(ring->capacity);
	if (capacity != MITEE_MSG_SLOT_COUNT) {
		mutex_unlock(&queue->rx.lock);
		return -EPROTO;
	}
	/* Observe the secure producer index before consuming its slot. */
	head = smp_load_acquire(&ring->head);
	if (head >= capacity || READ_ONCE(ring->tail) >= capacity) {
		mutex_unlock(&queue->rx.lock);
		return -EPROTO;
	}
	if (READ_ONCE(ring->tail) == head) {
		pr_err("rx buf empty\n");
		mutex_unlock(&queue->rx.lock);
		return -EBUSY;
	}

	tail = READ_ONCE(ring->tail);
	memcpy(msg, &queue->rx.messages[tail], sizeof(*msg));
	memset(&queue->rx.messages[tail], 0, sizeof(*msg));
	smp_store_release(&ring->tail,
			  (tail + 1) % capacity);
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

	mutex_init(&queue->state_lock);
	queue->state = MITEE_MSG_QUEUE_EMPTY;
	va = alloc_pages_exact(MITEE_MSG_QUEUE_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!va) {
		pr_err("failed to allocate message queue\n");
		return -ENOMEM;
	}

	queue->pa = virt_to_phys(va);
	queue->size = MITEE_MSG_QUEUE_SIZE;
	queue->buf_size = MITEE_MSG_BUF_SIZE;
	queue->va = va;

	mutex_init(&queue->rx.lock);
	queue->rx.ring = va;
	queue->rx.messages = va + 0x1000;
	queue->rx.ring->capacity = MITEE_MSG_SLOT_COUNT;

	mutex_init(&queue->tx.lock);
	queue->tx.ring = va + 0x800;
	queue->tx.messages = va + 0x2000;
	queue->tx.ring->capacity = MITEE_MSG_SLOT_COUNT;
	queue->state = MITEE_MSG_QUEUE_LOCAL;

	pr_info("allocated message queue pa: %#llx va: %p\n",
		(unsigned long long)queue->pa, va);
	return 0;
}

void mitee_msg_queue_deinit(struct mitee_msg_queue *queue, const char *reason)
{
	if (!queue)
		return;

	mutex_lock(&queue->state_lock);
	if (queue->state == MITEE_MSG_QUEUE_PUBLISHED) {
		queue->state = MITEE_MSG_QUEUE_RETAINED;
		pr_crit("retaining message queue pa: %#llx va: %p size: %#x state: %s reason: %s\n",
			(unsigned long long)queue->pa, queue->va, queue->size,
			mitee_msg_queue_state_name(queue->state), reason);
		goto out;
	}
	if (queue->state == MITEE_MSG_QUEUE_EMPTY ||
	    queue->state == MITEE_MSG_QUEUE_RETAINED)
		goto out;
	if (WARN_ON(queue->state != MITEE_MSG_QUEUE_LOCAL &&
		    queue->state != MITEE_MSG_QUEUE_REVOKED))
		goto out;

	free_pages_exact(queue->va, queue->size);
	queue->va = NULL;
	queue->pa = 0;
	queue->size = 0;
	queue->buf_size = 0;
	queue->tx.ring = NULL;
	queue->tx.messages = NULL;
	queue->rx.ring = NULL;
	queue->rx.messages = NULL;
	queue->state = MITEE_MSG_QUEUE_EMPTY;
	pr_info("message queue destroyed\n");
out:
	mutex_unlock(&queue->state_lock);
}

int mitee_msg_queue_register(struct optee *optee)
{
	struct mitee_msg_queue *queue;
	struct ffa_send_direct_data data = {};
	int rc;

	if (!optee) {
		pr_err("driver state is NULL\n");
		return -EINVAL;
	}
	queue = &optee->msg_queue;

	mutex_lock(&queue->state_lock);
	if (!queue->va || queue->state != MITEE_MSG_QUEUE_LOCAL) {
		pr_err("invalid message queue state: %s\n",
		       mitee_msg_queue_state_name(queue->state));
		rc = -EINVAL;
		goto out;
	}
	data.data0 = MITEE_FFA_MSG_QUEUE_REGISTER;
	data.data1 = queue->pa;
	data.data2 = queue->size;

	rc = optee->comm_ops->call(&data);
	if (rc)
		pr_err("failed to register message queue: %d\n", rc);
	else
		queue->state = MITEE_MSG_QUEUE_PUBLISHED;
out:
	mutex_unlock(&queue->state_lock);
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
	struct mitee_task *task;
	int id;

	mutex_lock(&tasks->lock);
	idr_for_each_entry(&tasks->idr, task, id) {
		pr_warn("discarding task %d during shutdown\n", id);
		list_del_init(&task->node);
		kfree(task);
	}
	mutex_unlock(&tasks->lock);
	idr_destroy(&tasks->idr);
}

static struct mitee_task *mitee_task_alloc(struct tee_context *ctx,
					   u32 command,
					   struct optee_msg_arg *arg)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct mitee_task *task;
	int id;

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (!task)
		return ERR_PTR(-ENOMEM);

	if (command != MITEE_MSG_CMD_CALL) {
		kfree(task);
		return ERR_PTR(-EINVAL);
	}

	task->ctx = ctx;
	INIT_LIST_HEAD(&task->node);
	init_completion(&task->completion);
	task->command = command;
	task->arg = arg;
	task->state = 1;

	mutex_lock(&optee->tasks.lock);
	id = idr_alloc(&optee->tasks.idr, task, 1, INT_MAX, GFP_KERNEL);
	if (id > 0) {
		task->id = id;
		list_add_tail(&task->node, &optee->tasks.pending);
	}
	mutex_unlock(&optee->tasks.lock);
	if (id < 0) {
		kfree(task);
		return ERR_PTR(id);
	}
	return task;
}

static void mitee_task_free(struct optee *optee, int id)
{
	struct mitee_task *task;

	mutex_lock(&optee->tasks.lock);
	task = idr_remove(&optee->tasks.idr, id);
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
		list_del_init(&task->node);
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
		if (size > sizeof(msg->payload))
			return -E2BIG;
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
	if (OPTEE_MSG_GET_ARG_SIZE((*arg)->num_params) > sizeof(msg->payload))
		return -E2BIG;
	return 0;
}

static void mitee_task_complete(struct optee *optee, int task_id,
				const struct optee_msg_arg *arg, int error)
{
	struct mitee_task *task;

	mutex_lock(&optee->tasks.lock);
	task = idr_find(&optee->tasks.idr, task_id);
	if (task && task->state != 4) {
		task->result = error;
		if (error) {
			task->arg->ret = TEEC_ERROR_COMMUNICATION;
			task->arg->ret_origin = TEEC_ORIGIN_COMMS;
		} else {
			memcpy(task->arg, arg,
			       OPTEE_MSG_GET_ARG_SIZE(arg->num_params));
		}
		task->state = 4;
		complete(&task->completion);
	}
	mutex_unlock(&optee->tasks.lock);
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

static bool mitee_is_dynamic_mem_rpc(const struct optee_msg_arg *arg)
{
	u64 sub_cmd;

	if (arg->cmd != OPTEE_MSG_RPC_CMD_CALLBACK || !arg->num_params)
		return false;
	if ((u32)arg->params[0].u.value.b !=
	    REE_CALLBACK_MODULE_TEE_FRAMEWORK)
		return false;

	sub_cmd = arg->params[0].u.value.a;
	return sub_cmd == OPTEE_REE_CALLBACK_ALLOCATE_NONSECMEM ||
	       sub_cmd == OPTEE_REE_CALLBACK_FREE_NONSECMEM;
}

static bool supp_ready(struct optee *optee)
{
	return READ_ONCE(optee->supp.ctx) || mitee_lifecycle_is_shutting_down(optee);
}

static bool mitee_call_slot_ready(struct optee *optee)
{
	unsigned int n;

	if (mitee_lifecycle_is_shutting_down(optee))
		return true;
	if (READ_ONCE(optee->active_calls) >=
	    READ_ONCE(optee->concurrency_limit))
		return false;
	for (n = 0; n < MITEE_WORKER_COUNT; n++) {
		if (!atomic_read(&optee->workers[n].busy))
			return true;
	}
	return false;
}

static void mitee_worker_release(struct mitee_worker *worker)
{
	atomic_set(&worker->busy, 0);
	wake_up_all(&worker->optee->concurrency_wq);
}

int mitee_worker_fn(void *data)
{
	struct mitee_worker *worker = data;
	struct optee *optee = worker->optee;
	struct mitee_task *task;
	struct tee_context *rpc_ctx;
	wait_queue_head_t *supp_wq = &optee->supp_ctx_wq;
	struct optee_msg_arg *arg;
	struct mitee_msg msg;
	struct mitee_msg reply;
	struct ffa_send_direct_data ffa_data;
	int response_task_id;
	int request_task_id;
	int rc;

	atomic_inc(&optee->workers_started);
	if (WARN_ON(worker->id >= MITEE_WORKER_COUNT))
		return -EINVAL;

	while (!kthread_should_stop()) {
		wait_for_completion(&worker->work);
		if (kthread_should_stop())
			break;

		if (!(current->flags & PF_NO_SETAFFINITY)) {
			rc = set_cpus_allowed_ptr(current, &optee->cpus_allowed);
			if (rc)
				pr_warn("worker affinity failed: %d\n", rc);
		}

		task = mitee_task_take(optee);
		if (!task) {
			pr_err("task list empty\n");
			mitee_worker_release(worker);
			continue;
		}
		request_task_id = task->id;

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

			if (msg.command == MITEE_MSG_CMD_RPC) {
				if (mitee_is_dynamic_mem_rpc(arg)) {
					handle_rpc(task->ctx, arg);
				} else {
					for (;;) {
						rc = wait_event_interruptible(*supp_wq, supp_ready(optee));
						if (rc || mitee_lifecycle_is_shutting_down(optee)) {
							rc = rc ?: -ESHUTDOWN;
							goto task_error;
						}
						rpc_ctx = optee_supp_get_ctx(&optee->supp);
						if (rpc_ctx)
							break;
					}
					handle_rpc(rpc_ctx, arg);
					optee_supp_put_ctx(&optee->supp);
				}
				rc = mitee_msg_pack(&reply, response_task_id,
						    MITEE_MSG_CMD_RPC_REPLY, arg);
				if (rc)
					goto task_error;
				msg = reply;
				continue;
			}

			if (msg.command == MITEE_MSG_CMD_DONE) {
				bool found;

				mutex_lock(&optee->tasks.lock);
				found = idr_find(&optee->tasks.idr,
						 response_task_id) != NULL;
				mutex_unlock(&optee->tasks.lock);
				if (!found) {
					rc = -EINVAL;
					goto task_error;
				}
				mitee_task_complete(optee, response_task_id, arg, 0);
				mitee_worker_release(worker);
				break;
			}

			rc = -EINVAL;
			goto task_error;
		}
		continue;

task_error:
		pr_err("worker %u call failed: %d\n", worker->id, rc);
		mitee_worker_release(worker);
		mitee_task_complete(optee, request_task_id, NULL, rc);
	}

	mitee_worker_release(worker);
	return 0;
}

int mitee_workers_init(struct optee *optee)
{
	unsigned int n;

	atomic_set(&optee->workers_started, 0);
	for (n = 0; n < MITEE_WORKER_COUNT; n++) {
		struct mitee_worker *worker = &optee->workers[n];

		worker->id = n;
		worker->optee = optee;
		atomic_set(&worker->busy, 0);
		init_completion(&worker->work);
		worker->thread = kthread_run(mitee_worker_fn, worker,
					     "mitee_worker/%u", n);
		if (IS_ERR(worker->thread)) {
			int rc = PTR_ERR(worker->thread);

			worker->thread = NULL;
			return rc;
		}
	}
	return 0;
}

void mitee_workers_deinit(struct optee *optee)
{
	unsigned int n;

	for (n = 0; n < MITEE_WORKER_COUNT; n++) {
		struct mitee_worker *worker = &optee->workers[n];

		if (!worker->thread)
			continue;
		complete(&worker->work);
		kthread_stop(worker->thread);
		worker->thread = NULL;
	}
}

static int mitee_call_slot_get(struct optee *optee,
			       struct mitee_worker **worker)
{
	unsigned int n;
	int rc;
	wait_queue_head_t *call_wq = &optee->concurrency_wq;

	for (;;) {
		rc = wait_event_interruptible(*call_wq, mitee_call_slot_ready(optee));
		if (rc)
			return rc;

		mutex_lock(&optee->concurrency_lock);
		if (mitee_lifecycle_is_shutting_down_locked(optee)) {
			mutex_unlock(&optee->concurrency_lock);
			return -ESHUTDOWN;
		}
		if (optee->active_calls >= optee->concurrency_limit) {
			mutex_unlock(&optee->concurrency_lock);
			continue;
		}

		for (n = 0; n < MITEE_WORKER_COUNT; n++) {
			if (atomic_cmpxchg(&optee->workers[n].busy, 0, 1))
				continue;
			*worker = &optee->workers[n];
			optee->active_calls++;
			mutex_unlock(&optee->concurrency_lock);
			return 0;
		}
		mutex_unlock(&optee->concurrency_lock);
	}
}

static void mitee_call_slot_put(struct optee *optee)
{
	mutex_lock(&optee->concurrency_lock);
	if (WARN_ON(!optee->active_calls)) {
		mutex_unlock(&optee->concurrency_lock);
		return;
	}
	optee->active_calls--;
	mutex_unlock(&optee->concurrency_lock);
	wake_up(&optee->concurrency_wq);
}

int optee_do_call_with_arg(struct tee_context *ctx,
			   struct optee_msg_arg *arg)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct mitee_task *task;
	struct mitee_worker *worker = NULL;
	cpumask_t saved_mask;
	int task_id;
	int rc;

	rc = mitee_call_slot_get(optee, &worker);
	if (rc)
		return rc;

	cpumask_copy(&saved_mask, &current->cpus_mask);
	if (!(current->flags & PF_NO_SETAFFINITY)) {
		rc = set_cpus_allowed_ptr(current, &optee->cpus_allowed);
		if (rc)
			pr_warn("caller affinity failed: %d\n", rc);
	}

	task = mitee_task_alloc(ctx, MITEE_MSG_CMD_CALL, arg);
	if (IS_ERR(task)) {
		pr_err("failed to allocate task: %ld\n", PTR_ERR(task));
		rc = PTR_ERR(task);
		mitee_worker_release(worker);
		goto out;
	}

	complete(&worker->work);
	wait_for_completion(&task->completion);
	rc = task->result;
	task_id = task->id;
	mitee_task_free(optee, task_id);
out:
	mitee_call_slot_put(optee);
	if (!(current->flags & PF_NO_SETAFFINITY))
		set_cpus_allowed_ptr(current, &saved_mask);
	return rc;
}
