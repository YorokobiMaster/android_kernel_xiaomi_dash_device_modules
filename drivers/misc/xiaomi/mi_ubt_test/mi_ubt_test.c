// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reconstructed from the Redmi Turbo 5 Max stock kernel module.
 * Reconstruction by YorokobiMaster.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <mi_ubt/mi_ubt.h>

static int ubt_show(struct seq_file *m, void *v)
{
	dump_user_backtrace(NULL);
	return 0;
}

static int __init mi_ubt_test_init(void)
{
	if (!proc_create_single_data("mi_ubt_test", 0400, NULL, ubt_show, NULL)) {
		remove_proc_entry("mi_ubt_test", NULL);
		return -ENOMEM;
	}
	pr_err("mi_ubt_test: %s\n", __func__);
	return 0;
}

static void __exit mi_ubt_test_exit(void)
{
	remove_proc_entry("mi_ubt_test", NULL);
	pr_err("mi_ubt_test: %s\n", __func__);
}

module_init(mi_ubt_test_init);
module_exit(mi_ubt_test_exit);

MODULE_DESCRIPTION("Register Mi Ubt Test driver");
MODULE_LICENSE("GPL v2");
