// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/arm_ffa.h>
#include <linux/build_bug.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <tee_drv.h>
#include "mitee_task.h"
#include "optee_msg.h"
#include "optee_private.h"

static_assert(sizeof(struct mitee_msg_ring) == 0x18);
static_assert(sizeof(struct mitee_msg) == MITEE_MSG_SLOT_SIZE);
static_assert(offsetof(struct mitee_msg, payload) == 0x14);

static int mitee_ffa_call(struct optee *optee,
			  struct ffa_send_direct_data *data)
{
	struct ffa_device *ffa_dev = optee->ffa.ffa_dev;
	int rc;

	rc = ffa_dev->ops->msg_ops->sync_send_receive(ffa_dev, data);
	if (!rc)
		atomic_notifier_call_chain(&optee->notifier,
					   MITEE_CALL_RETURNED, NULL);
	return rc;
}

static int mitee_msg_enqueue(struct mitee_msg_queue *queue,
			     const struct mitee_msg *msg)
{
	struct mitee_msg_ring *ring = queue->tx.ring;
	u64 next;

	mutex_lock(&queue->tx.lock);
	next = (READ_ONCE(ring->head) + 1) % ring->capacity;
	if (next == READ_ONCE(ring->tail)) {
		mutex_unlock(&queue->tx.lock);
		return -EBUSY;
	}

	memcpy(&queue->tx.messages[ring->head], msg, sizeof(*msg));
	/* Publish the complete slot before advancing the producer index. */
	smp_wmb();
	WRITE_ONCE(ring->head, next);
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
		mutex_unlock(&queue->rx.lock);
		return -EBUSY;
	}

	tail = ring->tail;
	/* Consume the slot only after observing the secure producer index. */
	smp_rmb();
	memcpy(msg, &queue->rx.messages[tail], sizeof(*msg));
	memset(&queue->rx.messages[tail], 0, sizeof(*msg));
	WRITE_ONCE(ring->tail, (tail + 1) % ring->capacity);
	mutex_unlock(&queue->rx.lock);
	return 0;
}

int mitee_msg_queue_init(struct mitee_msg_queue *queue)
{
	void *va;

	if (!queue)
		return -EINVAL;

	va = alloc_pages_exact(MITEE_MSG_QUEUE_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!va)
		return -ENOMEM;

	queue->pa = virt_to_phys(va);
	queue->size = MITEE_MSG_QUEUE_SIZE;
	queue->slot_size = MITEE_MSG_SLOT_SIZE;
	queue->va = va;

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

void mitee_msg_queue_deinit(struct mitee_msg_queue *queue)
{
	if (!queue || !queue->va)
		return;

	free_pages_exact(queue->va, queue->size);
	queue->va = NULL;
}

int mitee_msg_queue_register(struct optee *optee)
{
	struct ffa_send_direct_data data = {
		.data0 = MITEE_FFA_MSG_QUEUE_REGISTER,
		.data1 = optee->msg_queue.pa,
		.data2 = optee->msg_queue.size,
	};

	return mitee_ffa_call(optee, &data);
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

static struct mitee_task *mitee_task_alloc(struct tee_context *ctx,
					    u32 command,
					    struct optee_msg_arg *arg)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct mitee_task *task;
	int id;

	if (command != MITEE_MSG_CMD_CALL)
		return ERR_PTR(-EINVAL);

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (!task)
		return ERR_PTR(-ENOMEM);

	task->ctx = ctx;
	INIT_LIST_HEAD(&task->node);
	init_completion(&task->completion);
	task->arg = arg;
	task->command = command;
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
		task->state = 3;
	}
	mutex_unlock(&optee->tasks.lock);
	return task;
}

static int mitee_msg_pack(struct mitee_msg *msg, int task_id, u32 command,
			  const struct optee_msg_arg *arg)
{
	size_t size = OPTEE_MSG_GET_ARG_SIZE(arg->num_params);

	if (size > sizeof(msg->payload))
		return -E2BIG;

	memset(msg, 0, sizeof(*msg));
	msg->magic = MITEE_MSG_MAGIC;
	msg->version = MITEE_MSG_VERSION;
	msg->task_id = task_id;
	msg->command = command;
	memcpy(msg->payload, arg, size);
	return 0;
}

static int mitee_msg_unpack(const struct mitee_msg *msg,
			    struct optee_msg_arg **arg)
{
	if (msg->magic != MITEE_MSG_MAGIC ||
	    msg->version != MITEE_MSG_VERSION)
		return -EINVAL;

	*arg = (struct optee_msg_arg *)msg->payload;
	if (OPTEE_MSG_GET_ARG_SIZE((*arg)->num_params) > sizeof(msg->payload))
		return -E2BIG;
	return 0;
}

static void mitee_task_complete_error(struct mitee_worker *worker,
				      struct mitee_task *task)
{
	task->arg->ret = TEEC_ERROR_COMMUNICATION;
	task->arg->ret_origin = TEEC_ORIGIN_COMMS;
	task->state = 4;
	atomic_set(&worker->busy, 0);
	complete(&task->completion);
}

static int mitee_worker_fn(void *data)
{
	struct mitee_worker *worker = data;
	struct optee *optee = worker->optee;
	struct mitee_task *task;
	struct optee_msg_arg *arg;
	struct mitee_msg msg;
	struct ffa_send_direct_data ffa_data;
	int response_task_id;
	int rc;

	atomic_inc(&optee->workers_started);

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
			atomic_set(&worker->busy, 0);
			continue;
		}

		rc = mitee_msg_pack(&msg, task->id, task->command, task->arg);
		if (rc)
			goto task_error;

		for (;;) {
			rc = mitee_msg_enqueue(&optee->msg_queue, &msg);
			if (rc)
				goto task_error;

			ffa_data = (struct ffa_send_direct_data) {
				.data0 = MITEE_FFA_MSG_SEND,
			};
			rc = mitee_ffa_call(optee, &ffa_data);
			while (!rc && ffa_data.data0 == 2) {
				ffa_data = (struct ffa_send_direct_data) {
					.data0 = MITEE_FFA_MSG_RESUME,
				};
				rc = mitee_ffa_call(optee, &ffa_data);
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

			if (msg.command == MITEE_MSG_CMD_DONE) {
				struct mitee_task *done;

				mutex_lock(&optee->tasks.lock);
				done = idr_find(&optee->tasks.idr,
						response_task_id);
				if (done) {
					memcpy(done->arg, arg,
					       OPTEE_MSG_GET_ARG_SIZE(arg->num_params));
					done->state = 4;
				}
				atomic_set(&worker->busy, 0);
				if (done)
					complete(&done->completion);
				mutex_unlock(&optee->tasks.lock);
				if (!done)
					goto task_error;
				break;
			}

			if (msg.command != MITEE_MSG_CMD_RPC) {
				rc = -EINVAL;
				goto task_error;
			}

			while (!optee->supp.ctx && !kthread_should_stop())
				msleep_interruptible(100);
			if (kthread_should_stop()) {
				rc = -EINTR;
				goto task_error;
			}

			mitee_handle_rpc(optee->supp.ctx, arg);
			rc = mitee_msg_pack(&msg, response_task_id,
					    MITEE_MSG_CMD_RPC_REPLY, arg);
			if (rc)
				goto task_error;
		}
		continue;

task_error:
		pr_err("worker %u call failed: %d\n", worker->id, rc);
		mitee_task_complete_error(worker, task);
	}

	return 0;
}

int mitee_workers_init(struct optee *optee)
{
	unsigned int n;

	sema_init(&optee->concurrency, MITEE_WORKER_COUNT);
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
			mitee_workers_deinit(optee);
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

int mitee_do_call_with_arg(struct tee_context *ctx,
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
		rc = -EBUSY;
		goto out;
	}

	task = mitee_task_alloc(ctx, MITEE_MSG_CMD_CALL, arg);
	if (IS_ERR(task)) {
		rc = PTR_ERR(task);
		atomic_set(&worker->busy, 0);
		goto out;
	}

	complete(&worker->work);
	wait_for_completion(&task->completion);
	rc = task->arg->ret == TEEC_ERROR_COMMUNICATION ? -EIO : 0;
	task_id = task->id;
	mitee_task_free(optee, task_id);
out:
	set_cpus_allowed_ptr(current, &saved_mask);
	up(&optee->concurrency);
	return rc;
}
