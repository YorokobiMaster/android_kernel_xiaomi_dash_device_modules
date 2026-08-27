// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/uaccess.h>

struct proc_dir_entry *entry_debug_ext;
struct proc_dir_entry *entry_set_panic;

static int reboot_flag;
static int shutdown_flag;

static ssize_t reboot_panic_procs_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char kernel_buf[16] = { 0 };
	int ret;

	sprintf(kernel_buf, "%d\n", reboot_flag);
	ret = simple_read_from_buffer(buf, count, ppos, kernel_buf,
			strlen(kernel_buf));

	return ret;
}

static ssize_t reboot_panic_procs_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	char kernel_buf[16] = { 0 };
	int val = 0;

	if (copy_from_user(kernel_buf, buf, count))
		return -EFAULT;

	sscanf(kernel_buf, "%d", &val);
	reboot_flag = val > 0;

	return count;
}

static const struct proc_ops reboot_panic_ops = {
	.proc_read = reboot_panic_procs_read,
	.proc_write = reboot_panic_procs_write,
	.proc_lseek = default_llseek,
};

static ssize_t shutdown_panic_procs_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char kernel_buf[16] = { 0 };
	int ret;

	sprintf(kernel_buf, "%d\n", shutdown_flag);
	ret = simple_read_from_buffer(buf, count, ppos, kernel_buf,
			strlen(kernel_buf));

	return ret;
}

static ssize_t shutdown_panic_procs_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	char kernel_buf[16] = { 0 };
	int val = 0;

	if (copy_from_user(kernel_buf, buf, count))
		return -EFAULT;

	sscanf(kernel_buf, "%d", &val);
	shutdown_flag = val > 0;

	return count;
}

static const struct proc_ops shutdown_panic_ops = {
	.proc_read = shutdown_panic_procs_read,
	.proc_write = shutdown_panic_procs_write,
	.proc_lseek = default_llseek,
};

static ssize_t customized_panic_procs_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	char kernel_buf[16] = { 0 };
	size_t write = min(count, sizeof(kernel_buf));

	if (copy_from_user(kernel_buf, buf, write))
		return -EFAULT;

	kernel_buf[write - 1] = '\0';
	pr_err("panic_trigger: %s\n", kernel_buf);
	panic(kernel_buf);
}

static const struct proc_ops customized_panic_ops = {
	.proc_write = customized_panic_procs_write,
};

static int panic_trigger_notify(struct notifier_block *nb,
		unsigned long action, void *data)
{
	if (action == SYS_RESTART && reboot_flag)
		panic("Panic trigger come from SYS_RESTART\n");

	if (action == SYS_POWER_OFF && shutdown_flag)
		panic("Panic trigger come from SYS_POWER_OFF\n");

	return NOTIFY_DONE;
}

static struct notifier_block panic_trigger_nb = {
	.notifier_call = panic_trigger_notify,
};

int panic_trigger_init(struct proc_dir_entry *parent)
{
	int ret;

	entry_set_panic = proc_mkdir("panic_trigger", parent);
	if (!entry_set_panic) {
		pr_err("panic_trigger: %s: Can't create panic_trigger proc entry\n",
				__func__);
		goto err_out;
	}

	if (!proc_create("panic_on_reboot", 0x666, entry_set_panic,
			&reboot_panic_ops)) {
		pr_err("panic_trigger: %s: %d: Can't create panic_on_reboot proc entry!\n",
				__func__, 130);
		goto err_remove_dir;
	}

	if (!proc_create("panic_on_shutdown", 0x666, entry_set_panic,
			&shutdown_panic_ops)) {
		pr_err("panic_trigger: %s: %d: Can't create panic_on_shutdown proc entry!\n",
				__func__, 138);
		goto err_remove_reboot;
	}

	if (!proc_create("panic", 0222, entry_set_panic,
			&customized_panic_ops)) {
		pr_err("panic_trigger: %s: %d: Can't create panic proc entry!\n",
				__func__, 146);
		goto err_remove_shutdown;
	}

	ret = register_reboot_notifier(&panic_trigger_nb);
	if (ret) {
		pr_err("panic_trigger: Error: Can't register reboot notifier\n");
		goto err_remove_panic;
	}

	return 0;

err_remove_panic:
	remove_proc_entry("panic", entry_set_panic);
err_remove_shutdown:
	remove_proc_entry("panic_on_shutdown", entry_set_panic);
err_remove_reboot:
	remove_proc_entry("panic_on_reboot", entry_set_panic);
err_remove_dir:
	remove_proc_entry("panic_trigger", parent);
err_out:
	return -ENOMEM;
}

void panic_trigger_exit(struct proc_dir_entry *parent)
{
	remove_proc_entry("panic", entry_set_panic);
	remove_proc_entry("panic_on_shutdown", entry_set_panic);
	remove_proc_entry("panic_on_reboot", entry_set_panic);
	remove_proc_entry("panic_trigger", parent);
	unregister_reboot_notifier(&panic_trigger_nb);
}

static int __init debug_ext_init(void)
{
	int ret;

	entry_debug_ext = proc_mkdir("debug_ext", NULL);
	if (!entry_debug_ext) {
		pr_err("%s: Can't create debug_ext proc entry\n", __func__);
		return -ENOMEM;
	}

	ret = panic_trigger_init(entry_debug_ext);
	if (ret)
		pr_err("%s: %d: set_panic_init_notifier failed!\n",
				__func__, 33);
	else
		pr_info("%s: init done!", __func__);

	return ret;
}

static void __exit debug_ext_exit(void)
{
	panic_trigger_exit(entry_debug_ext);
	remove_proc_entry("debug_ext", NULL);
	entry_debug_ext = NULL;
}

module_init(debug_ext_init);
module_exit(debug_ext_exit);

MODULE_LICENSE("GPL v2");
