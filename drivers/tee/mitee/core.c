// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, Linaro Limited
 * Copyright (c) 2016, EPAM Systems
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <tee_drv.h>
#include "optee_private.h"

static int mitee_concurrency_proc_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t mitee_concurrency_proc_write(struct file *file,
					    const char __user *buf,
					    size_t count, loff_t *ppos)
{
	struct optee *optee = get_optee_drv_state();
	unsigned int enable;
	int rc;

	if (!optee || !buf || !count || !ppos || count >= 5)
		return -EINVAL;

	rc = kstrtouint_from_user(buf, count, 10, &enable);
	if (rc)
		return rc;

	switch (enable) {
	case 0:
		pr_info("disable concurrency\n");
		sema_init(&optee->concurrency, 1);
		break;
	case 1:
		pr_info("enable concurrency\n");
		sema_init(&optee->concurrency, MITEE_WORKER_COUNT);
		break;
	default:
		pr_err("invalid proc ops: %u\n", enable);
		return -EINVAL;
	}

	return count;
}

static const struct proc_ops mitee_concurrency_fops = {
	.proc_open = mitee_concurrency_proc_open,
	.proc_write = mitee_concurrency_proc_write,
};

int mitee_proc_init(void)
{
	if (!proc_create("mitee_concurrency", 0644, NULL,
			 &mitee_concurrency_fops)) {
		pr_err("failed to create mitee worker proc node\n");
		return -ENOMEM;
	}

	return 0;
}

#if 0
static void optee_bus_scan(struct work_struct *work)
{
	WARN_ON(optee_enumerate_devices(PTA_CMD_GET_DEVICES_SUPP));
}
#endif

int noinline optee_open_common(struct tee_context *ctx, bool cap_memref_null)
{
	struct optee_context_data *ctxdata;
	struct tee_device *teedev = ctx->teedev;
	struct optee *optee = tee_get_drvdata(teedev);

	ctxdata = kzalloc(sizeof(*ctxdata), GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	if (teedev == optee->supp_teedev) {
		bool busy = true;

		mutex_lock(&optee->supp.mutex);
		if (!optee->supp.ctx) {
			busy = false;
			optee->supp.ctx = ctx;
		}
		mutex_unlock(&optee->supp.mutex);
		if (busy) {
			kfree(ctxdata);
			return -EBUSY;
		}
#if 0
		if (!optee->scan_bus_done) {
			INIT_WORK(&optee->scan_bus_work, optee_bus_scan);
			optee->scan_bus_wq = create_workqueue("optee_bus_scan");
			if (!optee->scan_bus_wq) {
				kfree(ctxdata);
				return -ECHILD;
			}
			queue_work(optee->scan_bus_wq, &optee->scan_bus_work);
			optee->scan_bus_done = true;
		}
#endif
	}
	mutex_init(&ctxdata->mutex);
	INIT_LIST_HEAD(&ctxdata->sess_list);

	ctx->cap_memref_null = cap_memref_null;
	ctx->data = ctxdata;
	return 0;
}

static void optee_release_helper(struct tee_context *ctx,
				 int (*close_session)(struct tee_context *ctx,
						      u32 session))
{
	struct optee_context_data *ctxdata = ctx->data;
	struct optee_session *sess;
	struct optee_session *sess_tmp;

	if (!ctxdata)
		return;

	list_for_each_entry_safe (sess, sess_tmp, &ctxdata->sess_list,
				  list_node) {
		list_del(&sess->list_node);
		close_session(ctx, sess->session_id);
		kfree(sess);
	}
	kfree(ctxdata);
	ctx->data = NULL;
}

void optee_release(struct tee_context *ctx)
{
	optee_release_helper(ctx, optee_close_session_helper);
}

void optee_release_supp(struct tee_context *ctx)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);

	optee_release_helper(ctx, optee_close_session_helper);
	if (optee->scan_bus_wq) {
		destroy_workqueue(optee->scan_bus_wq);
		optee->scan_bus_wq = NULL;
	}
	optee_supp_release(&optee->supp);
}

void optee_remove_common(struct optee *optee)
{
#if MITEE_FEATURE_FW_NP_ENABLE
	/* Unregister OP-TEE specific client devices on TEE bus */
	optee_unregister_devices();
#endif
	/*
	 * The two devices have to be unregistered before we can free the
	 * other resources.
	 */
	tee_device_unregister(optee->supp_teedev);
	tee_device_unregister(optee->teedev);

	tee_shm_pool_free(optee->pool);
	optee_wait_queue_exit(&optee->wait_queue);
	optee_supp_uninit(&optee->supp);
	mutex_destroy(&optee->call_queue.mutex);
}

MODULE_AUTHOR("Linaro");
MODULE_DESCRIPTION("OP-TEE driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:optee");
