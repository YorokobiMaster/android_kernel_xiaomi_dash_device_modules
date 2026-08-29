/*
 * Copyright (C) 2015 Google, Inc.
 * Copyright (C) 2021 XiaoMi, Inc.
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
#include "mitee_memlog.h"

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <tee_drv.h>
#include "optee_ffa.h"
#include "optee_private.h"
#include "optee_smc.h"

#define SET_KEY_FILE "/data/vendor/mitee/key.log"

static int log_read_line(struct mitee_memlog_state *s, int put, int get)
{
	struct log_rb *log = s->log;
	int i;
	char c = '\0';
	size_t max_to_read =
		min((size_t)(put - get), sizeof(s->line_buffer) - 1);
	size_t mask = log->sz - 1;

	for (i = 0; i < max_to_read && c != '\n';)
		s->line_buffer[i++] = c = log->data[get++ & mask];
	s->line_buffer[i] = '\0';

	return i;
}

static int log_read_b_line(struct mitee_memlog_state *s, int put, int get)
{
	struct log_rb *log = s->log;
	int i;
	char c = '\0';
	size_t max_to_read =
		min((size_t)(put - get), sizeof(s->line_buffer) - 1);
	size_t mask = log->b_sz - 1;

	for (i = 0; i < max_to_read && c != '\n';)
		s->line_buffer[i++] = c = log->data[log->sz + (get++ & mask)];
	s->line_buffer[i] = '\0';
	return i;
}
#if 0
static void mitee_dump_logs(struct mitee_memlog_state *s)
{
	struct log_rb *log = s->log;
	uint32_t get, put, alloc;
	int read_chars;

	BUG_ON(!is_power_of_2(log->sz));

	/*
	 * For this ring buffer, at any given point, alloc >= put >= get.
	 * The producer side of the buffer is not locked, so the put and alloc
	 * pointers must be read in a defined order (put before alloc) so
	 * that the above condition is maintained. A read barrier is needed
	 * to make sure the hardware and compiler keep the reads ordered.
	 */
	get = s->get;
	while ((put = log->put) != get) {
		/* Make sure that the read of put occurs before the read of log data */
		rmb();

		/* Read a line from the log */
		read_chars = log_read_line(s, put, get);

		/* Force the loads from log_read_line to complete. */
		rmb();
		alloc = log->alloc;

		/*
		 * Discard the line that was just read if the data could
		 * have been corrupted by the producer.
		 */
		if (alloc - get > log->sz) {
			pr_err("mitee: log overflow.");
			get = alloc - log->sz;
			continue;
		}
		pr_info("mitee: %s", s->line_buffer);
		get += read_chars;
	}
	s->get = get;
}
#endif

static int mitee_memlog_call_notify(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	struct mitee_memlog_state *s;
	// unsigned long flags;

	if (action != MITEE_CALL_RETURNED)
		return NOTIFY_DONE;

	s = container_of(nb, struct mitee_memlog_state, call_notifier);
	// pr_err("ststest: start dump log\n");
	// spin_lock_irqsave(&s->lock, flags);
	// mitee_dump_logs(s);
	// spin_unlock_irqrestore(&s->lock, flags);
	atomic_inc(&s->mitee_log_event_count);
	wake_up_interruptible(&s->mitee_log_wq);
	// pr_err("ststest: end dump log\n");
	return NOTIFY_OK;
}

static int is_buf_empty(struct mitee_memlog_state *s)
{
	struct log_rb *log = s->log;
	uint32_t get, put;

	get = s->get;
	put = log->put;
	return (get == put);
}

static int is_b_buf_empty(struct mitee_memlog_state *s)
{
	struct log_rb *log = s->log;
	uint32_t get, put;

	get = s->b_get;
	put = log->b_put;
	return (get == put);
}

static int do_mitee_memlog_read(struct mitee_memlog_state *s, char __user *buf,
				size_t size)
{
	struct log_rb *log = s->log;
	uint32_t get, put, alloc;
	int read_chars = 0, copy_chars = 0, tbuf_size, outbuf_size;
	char *psrc = NULL;

	WARN_ON(!is_power_of_2(log->sz));

	if (s->reading_boot_log) {
		/*
		* For this ring buffer, at any given point, alloc >= put >= get.
		* The producer side of the buffer is not locked, so the put and alloc
		* pointers must be read in a defined order (put before alloc) so
		* that the above condition is maintained. A read barrier is needed
		* to make sure the hardware and compiler keep the reads ordered.
		*/
		get = s->b_get;
		put = log->b_put;
		/* make sure the hardware and compiler reads the correct put & alloc*/
		rmb();
		alloc = log->b_alloc;

		if (alloc - get > log->b_sz) {
			pr_notice("mitee: log overflow, lose some msg.");
			get = alloc - log->b_sz;
		}

		if (get > put)
			return -EFAULT;

		if (is_b_buf_empty(s)) {
			s->reading_boot_log = false;
			return 0;
		}

		outbuf_size = min((int)(put - get), (int)size);

		tbuf_size = (outbuf_size / MITEE_LINE_BUFFER_SIZE + 1) *
			    MITEE_LINE_BUFFER_SIZE;

		/* tbuf_size >= outbuf_size >= size */
		psrc = kzalloc(tbuf_size, GFP_KERNEL);

		if (!psrc)
			return -ENOMEM;

		while (get != put) {
			read_chars = log_read_b_line(s, put, get);
			/* Force the loads from log_read_line to complete. */
			rmb();
			if (copy_chars + read_chars > outbuf_size)
				break;
			memcpy(psrc + copy_chars, s->line_buffer, read_chars);
			get += read_chars;
			copy_chars += read_chars;
		}

		if (copy_to_user(buf, psrc, copy_chars)) {
			kfree(psrc);
			return -EFAULT;
		}
		kfree(psrc);

		s->b_get = get;

		return copy_chars;
	} else {
		/*
		* For this ring buffer, at any given point, alloc >= put >= get.
		* The producer side of the buffer is not locked, so the put and alloc
		* pointers must be read in a defined order (put before alloc) so
		* that the above condition is maintained. A read barrier is needed
		* to make sure the hardware and compiler keep the reads ordered.
		*/
		get = s->get;
		put = log->put;
		/* make sure the hardware and compiler reads the correct put & alloc*/
		rmb();
		alloc = log->alloc;

		if (alloc - get > log->sz) {
			pr_notice("mitee: log overflow, lose some msg.");
			get = alloc - log->sz;
		}

		if (get > put)
			return -EFAULT;

		if (is_buf_empty(s))
			return 0;

		outbuf_size = min((int)(put - get), (int)size);

		tbuf_size = (outbuf_size / MITEE_LINE_BUFFER_SIZE + 1) *
			    MITEE_LINE_BUFFER_SIZE;

		/* tbuf_size >= outbuf_size >= size */
		psrc = kzalloc(tbuf_size, GFP_KERNEL);

		if (!psrc)
			return -ENOMEM;

		while (get != put) {
			read_chars = log_read_line(s, put, get);
			/* Force the loads from log_read_line to complete. */
			rmb();
			if (copy_chars + read_chars > outbuf_size)
				break;
			memcpy(psrc + copy_chars, s->line_buffer, read_chars);
			get += read_chars;
			copy_chars += read_chars;
		}

		if (copy_to_user(buf, psrc, copy_chars)) {
			kfree(psrc);
			return -EFAULT;
		}
		kfree(psrc);

		s->get = get;

		return copy_chars;
	}
}

static ssize_t mitee_memlog_read(struct file *file, char __user *buf,
				 size_t size, loff_t *ppos)
{
	struct mitee_memlog_state *s = pde_data(file_inode(file));
	int ret = 0;

	if (mutex_lock_interruptible(&s->read_lock))
		return -ERESTARTSYS;
	ret = do_mitee_memlog_read(s, buf, size);
	s->poll_event = atomic_read(&s->mitee_log_event_count);
	mutex_unlock(&s->read_lock);
	return ret;
}

static int mitee_memlog_open(struct inode *inode, struct file *file)
{
	struct mitee_memlog_state *s = pde_data(inode);
	int ret;

	ret = nonseekable_open(inode, file);
	if (unlikely(ret))
		return ret;
	s->poll_event = atomic_read(&s->mitee_log_event_count);
	return 0;
}

static int mitee_memlog_release(struct inode *inode, struct file *file)
{
	return 0;
}

static unsigned int mitee_memlog_poll(struct file *file,
				      struct poll_table_struct *wait)
{
	struct mitee_memlog_state *s = pde_data(file_inode(file));
	int mask = 0;

	if ((s->reading_boot_log && !is_b_buf_empty(s)) || !is_buf_empty(s))
		return POLLIN | POLLRDNORM;

	poll_wait(file, &s->mitee_log_wq, wait);

	if (s->poll_event != atomic_read(&s->mitee_log_event_count))
		mask |= POLLIN | POLLRDNORM;
	return mask;
}

static const struct proc_ops mitee_memlog_fops = {
	.proc_open = mitee_memlog_open,
	.proc_read = mitee_memlog_read,
	.proc_release = mitee_memlog_release,
	.proc_poll = mitee_memlog_poll,
};

static ssize_t mitee_memlog_key_read(struct file *file, char __user *buf,
				     size_t size, loff_t *ppos)
{
	struct mitee_memlog_state *s = pde_data(file_inode(file));
	char key[KEY_LENGTH];

	memcpy(key, (const void *)s->log->aeskey, sizeof(key));
	return simple_read_from_buffer(buf, size, ppos, key, sizeof(key));
}

static int mitee_memlog_key_open(struct inode *inode, struct file *file)
{
	int ret;
	ret = nonseekable_open(inode, file);
	if (unlikely(ret))
		return ret;
	return 0;
}

static int mitee_memlog_key_release(struct inode *inode, struct file *file)
{
	return 0;
}

static unsigned int mitee_memlog_key_poll(struct file *file,
					  struct poll_table_struct *wait)
{
	return 0;
}

static const struct proc_ops mitee_memlog_key_fops = {
	.proc_open = mitee_memlog_key_open,
	.proc_read = mitee_memlog_key_read,
	.proc_release = mitee_memlog_key_release,
	.proc_poll = mitee_memlog_key_poll,
};

static int mitee_call_notifier_register(struct optee *optee,
					struct notifier_block *n)
{
	return atomic_notifier_chain_register(&optee->notifier, n);
}

static int mitee_call_notifier_unregister(struct optee *optee,
					  struct notifier_block *n)
{
	return atomic_notifier_chain_unregister(&optee->notifier, n);
}

int mitee_memlog_probe(struct platform_device *pdev, struct optee *optee)
{
	struct mitee_memlog_state *s;
	int result = 0;
	phys_addr_t pa;
	phys_addr_t begin;
	phys_addr_t end;
	phys_addr_t raw_end;
	size_t size;
	void *va;
	struct ffa_send_direct_data data = { OPTEE_FFA_GET_MITEE_LOG_BUFFER };
	int rc = 0;

	dev_printk(KERN_DEBUG, &pdev->dev, "%s\n", __func__);

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		result = -ENOMEM;
		goto error_alloc_state;
	}

	spin_lock_init(&s->lock);
	s->dev = &pdev->dev;
	s->mitee_dev = s->dev->parent;
	s->get = 0;
	s->b_get = 0;
	s->reading_boot_log = true;
	s->optee = optee;
	mutex_init(&s->read_lock);

	rc = optee->comm_ops->call(&data);
	if (rc) {
		pr_err("miteelog service not available %d\n", rc);
		result = -EIO;
		goto error_alloc_log;
	}
	//pr_err("miteelog start=%x, size=%zu, setting=%x\n", (uint64_t)res.result.start, res.result.size,res.result.settings);
	if (check_add_overflow((phys_addr_t)data.data0,
			       (phys_addr_t)data.data1, &raw_end)) {
		result = -EOVERFLOW;
		goto error_alloc_log;
	}
	begin = roundup((phys_addr_t)data.data0, PAGE_SIZE);
	end = rounddown(raw_end, PAGE_SIZE);
	if (end <= begin) {
		result = -EINVAL;
		goto error_alloc_log;
	}
	pa = begin;
	size = end - begin;

	pr_err("miteelog paddr=%#llx, size=%zu\n", (uint64_t)pa, size);
	va = memremap(pa, size, MEMREMAP_WB);
	if (!va) {
		pr_err("miteelog ioremap failed\n");
		result = -EFAULT;
		goto error_alloc_log;
	}
	s->log = va;
	s->mapped_size = size;
	if (size < offsetof(struct log_rb, data) ||
	    !is_power_of_2(s->log->sz) ||
	    !is_power_of_2(s->log->b_sz) ||
	    s->log->sz > size - offsetof(struct log_rb, data) ||
	    s->log->b_sz > size - offsetof(struct log_rb, data) - s->log->sz) {
		pr_err("invalid mitee log ring geometry\n");
		result = -EINVAL;
		goto error_unmap_log;
	}

	s->call_notifier.notifier_call = mitee_memlog_call_notify;
	result = mitee_call_notifier_register(optee, &s->call_notifier);
	if (result < 0) {
		dev_err(&pdev->dev, "failed to register mitee call notifier\n");
		goto error_call_notifier;
	}

	init_waitqueue_head(&s->mitee_log_wq);
	atomic_set(&s->mitee_log_event_count, 0);
	atomic_set(&s->readable, 1);
	platform_set_drvdata(pdev, s);

	/* create /proc/mitee_log */
	s->proc = proc_create_data("mitee_log", 0444, NULL, &mitee_memlog_fops,
				   s);
	if (!s->proc) {
		pr_info("mitee_log proc_create failed!\n");
		result = -ENOMEM;
		goto error_create_log_proc;
	}

	/* create /proc/mitee_log_key */
	s->proc_key = proc_create_data("mitee_log_key", 0444, NULL,
				       &mitee_memlog_key_fops, s);
	if (!s->proc_key) {
		pr_info("mitee_log proc_key_create failed!\n");
		result = -ENOMEM;
		goto error_create_key_proc;
	}

	return 0;

error_create_key_proc:
	proc_remove(s->proc);
error_create_log_proc:
	mitee_call_notifier_unregister(optee, &s->call_notifier);
error_call_notifier:
error_unmap_log:
	memunmap(s->log);
//TODO notify tee we failed register notifier
// trusty_std_call32(s->trusty_dev, SMC_SC_SHARED_LOG_RM,
// (u32)pa, (u32)((u64)pa >> 32), 0);
// error_std_call:
// __free_pages(s->log_pages, get_order(TRUSTY_LOG_SIZE));
error_alloc_log:
	kfree(s);
	s = NULL;
error_alloc_state:
	return result;
}

int mitee_memlog_remove(struct platform_device *pdev)
{
	// int result;
	struct mitee_memlog_state *s = platform_get_drvdata(pdev);
	// phys_addr_t pa = page_to_phys(s->log_pages);
	if (!s)
		return 0;

	dev_printk(KERN_DEBUG, &pdev->dev, "%s\n", __func__);
	proc_remove(s->proc);
	proc_remove(s->proc_key);
	mitee_call_notifier_unregister(s->optee, &s->call_notifier);
	//TODO notify tee we failed register notifier
	// result = trusty_std_call32(s->trusty_dev, SMC_SC_SHARED_LOG_RM,
	// (u32)pa, (u32)((u64)pa >> 32), 0);
	// if (result) {
	// 	pr_err("trusty std call (SMC_SC_SHARED_LOG_RM) failed: %d\n", result);
	// }
	// __free_pages(s->log_pages, get_order(TRUSTY_LOG_SIZE));
	memunmap(s->log);
	platform_set_drvdata(pdev, NULL);
	kfree(s);

	return 0;
}
