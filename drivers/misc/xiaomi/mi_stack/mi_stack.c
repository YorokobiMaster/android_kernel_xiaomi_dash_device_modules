// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reconstructed from the Redmi Turbo 5 Max stock kernel module.
 * Reconstruction by YorokobiMaster.
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/uaccess.h>

#define MI_STACK_DEPTH 64
#define MI_STACK_PID_BUFFER_SIZE 128

static int selected_pid;
static char pid_buffer[MI_STACK_PID_BUFFER_SIZE];
static struct proc_dir_entry *mi_stack_dir;
static DEFINE_MUTEX(mi_stack_lock);

static int stack_show(struct seq_file *m, void *v)
{
	unsigned long *entries;
	struct task_struct *task;
	unsigned int nr_entries;
	unsigned int i;
	int pid;

	mutex_lock(&mi_stack_lock);
	pid = selected_pid;
	task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task) {
		mutex_unlock(&mi_stack_lock);
		pr_err("mi_stack: can not find pid %d\n", pid);
		return -ESRCH;
	}

	entries = kmalloc_array(MI_STACK_DEPTH, sizeof(*entries), GFP_KERNEL);
	if (!entries) {
		put_task_struct(task);
		mutex_unlock(&mi_stack_lock);
		return -ENOMEM;
	}

	nr_entries = stack_trace_save_tsk(task, entries, MI_STACK_DEPTH, 0);
	seq_printf(m, "[%d, %s] Kernel calltrace: \n", task->pid, task->comm);
	put_task_struct(task);
	mutex_unlock(&mi_stack_lock);

	for (i = 0; i < nr_entries; i++)
		seq_printf(m, "[<%d>] %pB\n", i, (void *)entries[i]);

	kfree(entries);
	return 0;
}

static ssize_t pid_read(struct file *file, char __user *buf, size_t count,
		loff_t *ppos)
{
	int len;

	mutex_lock(&mi_stack_lock);
	len = sprintf(pid_buffer, "%d\n", selected_pid);
	mutex_unlock(&mi_stack_lock);

	return simple_read_from_buffer(buf, count, ppos, pid_buffer, len);
}

static ssize_t pid_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	int pid = 0;
	ssize_t ret;

	if (count > sizeof(pid_buffer)) {
		pr_err("mi_stack: invalid len: len=%lu\n", count);
		return -EINVAL;
	}

	mutex_lock(&mi_stack_lock);
	ret = simple_write_to_buffer(pid_buffer, count, ppos, buf, count);
	pid_buffer[sizeof(pid_buffer) - 1] = '\0';
	if (sscanf(pid_buffer, "%d", &pid) != 1) {
		mutex_unlock(&mi_stack_lock);
		return -EINVAL;
	}
	if (pid < 0) {
		mutex_unlock(&mi_stack_lock);
		pr_err("mi_stack: pid %d is invalid\n", pid);
		return -EINVAL;
	}
	selected_pid = pid;
	mutex_unlock(&mi_stack_lock);

	pr_info("mi_stack: User set pid:%d\n", pid);
	return ret;
}

static const struct proc_ops pid_pops = {
	.proc_read = pid_read,
	.proc_write = pid_write,
};

static int __init mi_stack_init(void)
{
	struct proc_dir_entry *entry;

	mi_stack_dir = proc_mkdir("mi_stack", NULL);
	if (!mi_stack_dir)
		return -ENOMEM;

	entry = proc_create("pid", 0660, mi_stack_dir, &pid_pops);
	if (!entry)
		goto remove_dir;
	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

	entry = proc_create_single_data("stack", 0440, mi_stack_dir,
			stack_show, NULL);
	if (!entry)
		goto remove_pid;
	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

	pr_info("mi_stack: %s succeed\n", __func__);
	return 0;

remove_pid:
	remove_proc_entry("pid", mi_stack_dir);
remove_dir:
	remove_proc_entry("mi_stack", NULL);
	return -ENOMEM;
}

static void __exit mi_stack_exit(void)
{
	remove_proc_entry("stack", mi_stack_dir);
	remove_proc_entry("pid", mi_stack_dir);
	remove_proc_entry("mi_stack", NULL);
	pr_info("mi_stack: %s\n", __func__);
}

module_init(mi_stack_init);
module_exit(mi_stack_exit);

MODULE_AUTHOR("gaoxiang17 <gaoxiang17@xiaomi.com>");
MODULE_DESCRIPTION("Register Mi Stack driver");
MODULE_LICENSE("GPL v2");
