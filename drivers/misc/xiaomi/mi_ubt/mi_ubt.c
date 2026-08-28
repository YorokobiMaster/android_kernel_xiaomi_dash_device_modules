// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reconstructed from the Redmi Turbo 5 Max stock kernel module.
 * Reconstruction by YorokobiMaster.
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/path.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/processor.h>
#include <asm/ptrace.h>

#define MI_UBT_DEPTH 64

struct mi_ubt_frame {
	unsigned long fp;
	unsigned long lr;
};

static bool mi_ubt_read_frame(struct task_struct *task, unsigned long fp,
		struct mi_ubt_frame *frame)
{
	unsigned long __user *user_frame = (unsigned long __user *)fp;
	bool success = false;

	pagefault_disable();
	if (task == current) {
		if (__get_user(frame->fp, &user_frame[0]))
			goto out;
		if (__get_user(frame->lr, &user_frame[1]))
			goto out;
	} else {
		if (access_process_vm(task, fp, &frame->fp,
				sizeof(frame->fp), 0) != sizeof(frame->fp))
			goto out;
		if (access_process_vm(task, fp + sizeof(frame->fp), &frame->lr,
				sizeof(frame->lr), 0) != sizeof(frame->lr))
			goto out;
	}

	success = true;
out:
	pagefault_enable();
	return success;
}

void dump_user_backtrace(struct task_struct *task)
{
	struct pt_regs *regs;
	struct task_struct *target = task ?: current;
	struct mi_ubt_frame frame;
	struct vm_area_struct *vma;
	unsigned long entries[MI_UBT_DEPTH] = { 0 };
	unsigned long stack_end;
	unsigned long fp;
	struct mm_struct *mm;
	unsigned int nr = 0;
	unsigned int i;

	get_task_struct(target);
	pr_err("mi_ubt: [%s, %d, %c, %llu] User backtrace:\n",
		target->comm, target->pid, task_state_to_char(target),
		(unsigned long long)target->sched_info.pcount);

	regs = task_pt_regs(target);
	if (!user_mode(regs))
		goto out;

	mm = target == current ? target->mm : get_task_mm(target);
	if (!mm)
		goto out;
	if (!mmap_read_trylock(mm))
		goto put_first_mm_failed_lock;
	{
		VMA_ITERATOR(vmi, mm, 0);

		for_each_vma(vmi, vma) {
			if (regs->sp >= vma->vm_start &&
					regs->sp <= vma->vm_end)
				break;
		}
	}
	if (!vma) {
		mmap_read_unlock(mm);
		goto put_first_mm;
	}
	stack_end = vma->vm_end;
	mmap_read_unlock(mm);
	if (target != current)
		mmput(mm);

	if (!regs->pc || !IS_ALIGNED(regs->pc, 4))
		goto out;
	entries[nr++] = regs->pc - 4;
	if (!regs->regs[30] || !IS_ALIGNED(regs->regs[30], 4))
		goto out;
	entries[nr++] = regs->regs[30] - 4;

	fp = regs->regs[29];
	while (fp > regs->sp && fp < stack_end && nr < MI_UBT_DEPTH) {
		if (!mi_ubt_read_frame(target, fp, &frame))
			goto out;
		if (!frame.lr || !IS_ALIGNED(frame.lr, 4))
			goto out;
		entries[nr++] = frame.lr - 4;
		if (frame.fp <= regs->sp || frame.fp >= stack_end)
			break;
		fp = frame.fp;
	}

	if (!nr)
		goto out;
	mm = target == current ? target->mm : get_task_mm(target);
	if (!mm)
		goto out;
	if (!mmap_read_trylock(mm))
		goto put_second_mm_failed_lock;
	for (i = 0; i < nr; i++) {
		char address[64] = { 0 };
		char *page;
		char *path = "?";

		if (!entries[i])
			continue;
		vma = find_vma(target->mm, entries[i]);
		if (!vma || !vma->vm_file)
			continue;

		page = (char *)__get_free_page(GFP_NOWAIT);
		if (!page)
			continue;
		path = file_path(vma->vm_file, page, PAGE_SIZE);
		if (IS_ERR(path))
			path = "?";
		snprintf(address, sizeof(address), "<0x%lx> in ", entries[i]);
		pr_err("mi_ubt: #%d %s%s[%lx-%lx]\n", i, address, path,
			vma->vm_start, vma->vm_end);
		free_page((unsigned long)page);
	}
	mmap_read_unlock(mm);
	if (target != current)
		mmput(mm);
	goto out;

put_second_mm_failed_lock:
	/* Match stock's failed-lock reference handling, including current. */
	mmput(mm);
	goto out;
put_first_mm_failed_lock:
	/* Match stock's failed-lock reference handling, including current. */
	mmput(mm);
	goto out;
put_first_mm:
	if (target != current)
		mmput(mm);
out:
	pr_err("mi_ubt: -----------User backtrace end-----------\n");
	put_task_struct(target);
}
EXPORT_SYMBOL_GPL(dump_user_backtrace);

static int mi_ubt_panic_notifier(struct notifier_block *nb,
		unsigned long action, void *data)
{
	dump_user_backtrace(NULL);
	return NOTIFY_DONE;
}

static struct notifier_block mi_ubt_panic_blk = {
	.notifier_call = mi_ubt_panic_notifier,
	.priority = INT_MAX,
};

static int __init user_backtrace_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &mi_ubt_panic_blk);
	pr_err("mi_ubt: %s successd\n", __func__);
	return 0;
}

static void __exit user_backtrace_exit(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list, &mi_ubt_panic_blk);
	pr_err("mi_ubt: %s\n", __func__);
}

module_init(user_backtrace_init);
module_exit(user_backtrace_exit);

MODULE_DESCRIPTION("Register user backtrace driver");
MODULE_LICENSE("GPL v2");
