// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define CRASH_PAGE_SIZE 1024

struct proc_dir_entry *entry_crash_module;
static char page[CRASH_PAGE_SIZE];

static ssize_t crash_module_procs_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	int ret;

	ret = simple_read_from_buffer(buf, count, ppos, page,
				      strnlen(page, sizeof(page)));
	pr_err("%s: failed\npage:%s\n", __func__, page);
	return ret;
}

static ssize_t crash_module_procs_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	int ret;

	count = min_t(size_t, count, sizeof(page) - 1);
	memset(page, 0, sizeof(page));
	ret = simple_write_to_buffer(page, sizeof(page) - 1, ppos, buf, count);
	pr_err("%s:page:%s\n", __func__, page);
	return ret;
}

static const struct proc_ops module_ops = {
	.proc_read = crash_module_procs_read,
	.proc_write = crash_module_procs_write,
	.proc_lseek = default_llseek,
};

static int __init crash_module_init(void)
{
	entry_crash_module = proc_create("crash_module", 0666, NULL, &module_ops);
	if (!entry_crash_module) {
		pr_err("%s: Can't create crash_module proc entry\n", __func__);
		return -ENOMEM;
	}
	pr_info("%s: init done!\n", __func__);
	return 0;
}

static void __exit crash_module_exit(void)
{
	remove_proc_entry("crash_module", NULL);
	entry_crash_module = NULL;
}

module_init(crash_module_init);
module_exit(crash_module_exit);

MODULE_LICENSE("GPL v2");
