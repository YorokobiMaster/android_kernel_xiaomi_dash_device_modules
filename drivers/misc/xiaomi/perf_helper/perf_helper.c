// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reimplemented for this project from the observable interfaces and
 * behavior of the Xiaomi stock perf_helper kernel module.
 */

#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/memcontrol.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_wakeup.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/rtc.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#include <trace/hooks/vmscan.h>

#define PERF_RECORD_COUNT	2048
#define PERF_RECORD_LEN		128
#define PERF_EXCEPTION_COUNT	128
#define PERF_EXCEPTION_LEN	1024
#define PERF_TRIGGER_COUNT	11
#define PERF_TRIGGER_LEN	64
#define PERF_RECLAIM_TYPE_BASE	10000000U
#define PERF_RECLAIM_RETRIES	10
#define PERF_RECLAIM_WAKE_MS	30000
/* Exact stock memory.reclaim_once option; bit 0 is unnamed in this kernel. */
#define PERF_STOCK_MEMCG_RECLAIM_OPTIONS	1U

struct perf_log_record {
	char text[PERF_RECORD_LEN];
	struct timespec64 timestamp;
};

struct perf_exception_record {
	char text[PERF_EXCEPTION_LEN];
	struct timespec64 timestamp;
	int repeats;
};

struct perf_trigger_event {
	struct list_head node;
	char text[PERF_TRIGGER_LEN];
};

struct perf_memcg_state {
	struct mem_cgroup *memcg;
	struct list_head node;
	bool foreground;
};

static struct task_struct *kswapd_task;
static struct task_struct *kcompactd_task;
static DEFINE_MUTEX(kswapd_lock);

static struct perf_log_record perflock_records[PERF_RECORD_COUNT];
static DEFINE_RAW_SPINLOCK(perflock_lock);
static unsigned int perflock_count;
static unsigned int perflock_next;

static struct perf_log_record mimd_records[PERF_RECORD_COUNT];
static DEFINE_RAW_SPINLOCK(mimd_log_lock);
static unsigned int mimd_count;
static unsigned int mimd_next;

static struct perf_exception_record exceptions[PERF_EXCEPTION_COUNT];
static DEFINE_RAW_SPINLOCK(exception_lock);
static unsigned int exception_count;
static unsigned int exception_next;

static LIST_HEAD(trigger_events);
static DEFINE_MUTEX(trigger_lock);
static unsigned int trigger_count;

static LIST_HEAD(memcg_states);
static DEFINE_MUTEX(memcg_lock);

static struct task_struct *reclaim_task;
static struct wakeup_source *reclaim_ws;
static atomic_t reclaim_pending = ATOMIC_INIT(0);
static u32 reclaim_target;
static int reclaim_scan_type;
static DEFINE_RAW_SPINLOCK(reclaim_status_lock);
static char reclaim_status[PERF_TRIGGER_LEN];

static char kdamond_cpu_set[PERF_TRIGGER_LEN];
static int kdamond_pid = -1;
static s64 mimd_notifier_data;
static struct kobject *mimd_kobj;

static struct proc_dir_entry *kswapd_affinity_entry;
static struct proc_dir_entry *perflock_records_entry;
static struct proc_dir_entry *mimd_log_entry;
static struct proc_dir_entry *global_reclaim_entry;
static struct proc_dir_entry *kswapd_pid_entry;
static struct proc_dir_entry *kdamond_cpuset_entry;
static struct proc_dir_entry *kcompactd_pid_entry;
static struct proc_dir_entry *exception_entry;
static bool vmscan_hook_registered;
static bool memcg_files_registered;

static int perf_copy_user_string(char *dst, size_t dst_size,
				 const char __user *src, size_t count)
{
	/* Safety fix vs stock: reserve and write a terminating NUL. */
	if (count >= dst_size)
		return -EINVAL;
	if (copy_from_user(dst, src, count))
		return -EFAULT;
	dst[count] = '\0';
	return 0;
}

static void perf_print_timestamped(struct seq_file *m,
				   const struct perf_log_record *record)
{
	struct rtc_time tm;

	rtc_time64_to_tm(record->timestamp.tv_sec, &tm);
	seq_printf(m, "%d-%02d-%02d %02d:%02d:%02d UTC { %s }\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec, record->text);
}

static int perf_log_show(struct seq_file *m, struct perf_log_record *records,
			 unsigned int count, unsigned int next)
{
	unsigned int i;
	unsigned int index = count == PERF_RECORD_COUNT ? next : 0;

	for (i = 0; i < count; i++) {
		perf_print_timestamped(m, &records[index]);
		index = (index + 1) % PERF_RECORD_COUNT;
	}

	return 0;
}

static ssize_t perf_log_write(struct perf_log_record *records,
			      raw_spinlock_t *lock, unsigned int *count,
			      unsigned int *next, const char __user *buffer,
			      size_t size)
{
	char text[PERF_RECORD_LEN + 1] = { 0 };
	struct perf_log_record *record;
	int ret;

	ret = perf_copy_user_string(text, sizeof(text), buffer, size);
	if (ret)
		return ret;

	/* Stock treats logging as best-effort and silently drops on contention. */
	if (!raw_spin_trylock(lock))
		return size;

	record = &records[*next];
	ktime_get_real_ts64(&record->timestamp);
	snprintf(record->text, sizeof(record->text), "%s", text);
	*next = (*next + 1) % PERF_RECORD_COUNT;
	if (*count < PERF_RECORD_COUNT)
		(*count)++;

	raw_spin_unlock(lock);
	return size;
}

static struct task_struct *perf_find_kernel_task(const char *name, size_t len)
{
	struct task_struct *task;
	struct task_struct *found = NULL;

	read_lock(&tasklist_lock);
	for_each_process(task) {
		if (!task->mm && !strncmp(task->comm, name, len)) {
			found = task;
			break;
		}
	}
	read_unlock(&tasklist_lock);

	return found;
}

static int kswapd_affinity_show(struct seq_file *m, void *unused)
{
	return 0;
}

static int kswapd_affinity_open(struct inode *inode, struct file *file)
{
	return single_open(file, kswapd_affinity_show, NULL);
}

static ssize_t kswapd_affinity_write(struct file *file,
				     const char __user *buffer, size_t size,
				     loff_t *offset)
{
	char input[PERF_TRIGGER_LEN + 1] = { 0 };
	cpumask_t mask;
	int packed_value;
	int ret;

	ret = perf_copy_user_string(input, sizeof(input), buffer, size);
	if (ret)
		return ret;
	if (sscanf(input, "%d", &packed_value) != 1) {
		pr_err("input param error, can not prase param\n");
		return -EINVAL;
	}

	cpumask_clear(&mask);
	cpumask_bits(&mask)[0] = (u8)(packed_value >> 8);
	pr_info("set kswapd affinity = %u\n", (u8)(packed_value >> 8));

	mutex_lock(&kswapd_lock);
	if (kswapd_task) {
		ret = set_cpus_allowed_ptr(kswapd_task, &mask);
		if (ret)
			pr_err("bind kswapd fail\n");
	}
	if (kcompactd_task) {
		ret = set_cpus_allowed_ptr(kcompactd_task, &mask);
		if (ret)
			pr_err("bind kcompactd fail\n");
	}
	mutex_unlock(&kswapd_lock);

	return size;
}

static const struct proc_ops kswapd_affinity_ops = {
	.proc_open = kswapd_affinity_open,
	.proc_read = seq_read,
	.proc_write = kswapd_affinity_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int perflock_records_show(struct seq_file *m, void *unused)
{
	int ret;

	raw_spin_lock(&perflock_lock);
	ret = perf_log_show(m, perflock_records, perflock_count, perflock_next);
	raw_spin_unlock(&perflock_lock);
	return ret;
}

static int perflock_records_open(struct inode *inode, struct file *file)
{
	return single_open(file, perflock_records_show, NULL);
}

static ssize_t perflock_records_write(struct file *file,
				      const char __user *buffer, size_t size,
				      loff_t *offset)
{
	return perf_log_write(perflock_records, &perflock_lock,
			      &perflock_count, &perflock_next, buffer, size);
}

static const struct proc_ops perflock_records_ops = {
	.proc_open = perflock_records_open,
	.proc_read = seq_read,
	.proc_write = perflock_records_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int mimd_log_show(struct seq_file *m, void *unused)
{
	int ret;

	raw_spin_lock(&mimd_log_lock);
	/* Safety fix vs stock: use the MIMD ring's own wrap index. */
	ret = perf_log_show(m, mimd_records, mimd_count, mimd_next);
	raw_spin_unlock(&mimd_log_lock);
	return ret;
}

static int mimd_log_open(struct inode *inode, struct file *file)
{
	return single_open(file, mimd_log_show, NULL);
}

static ssize_t mimd_log_write(struct file *file, const char __user *buffer,
			      size_t size, loff_t *offset)
{
	return perf_log_write(mimd_records, &mimd_log_lock,
			      &mimd_count, &mimd_next, buffer, size);
}

static const struct proc_ops mimd_log_ops = {
	.proc_open = mimd_log_open,
	.proc_read = seq_read,
	.proc_write = mimd_log_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static unsigned long perf_reclaim_pages(struct mem_cgroup *memcg,
					unsigned long target,
					unsigned int reclaim_options,
					bool stop_on_no_progress,
					bool stop_on_foreground,
					int per_attempt_scan_type)
{
	unsigned long reclaimed = 0;
	unsigned long remaining = target;
	unsigned long current_reclaimed;
	unsigned int attempt;

	for (attempt = 0; attempt < PERF_RECLAIM_RETRIES && remaining; attempt++) {
		if (per_attempt_scan_type >= 0)
			WRITE_ONCE(reclaim_scan_type, per_attempt_scan_type);
		current_reclaimed = try_to_free_mem_cgroup_pages(
			memcg, remaining, GFP_KERNEL, reclaim_options);
		if (per_attempt_scan_type >= 0)
			WRITE_ONCE(reclaim_scan_type, 0);
		reclaimed += current_reclaimed;

		if (stop_on_no_progress && !current_reclaimed)
			break;
		if (current_reclaimed >= remaining)
			break;
		remaining -= current_reclaimed;

		if (stop_on_foreground) {
			struct perf_memcg_state *state;
			bool foreground = false;

			mutex_lock(&memcg_lock);
			list_for_each_entry(state, &memcg_states, node) {
				if (state->memcg == memcg) {
					foreground = state->foreground;
					break;
				}
			}
			mutex_unlock(&memcg_lock);
			if (foreground) {
				pr_err("perf_helper, memcg reclaim once stop!\n");
				break;
			}
		}
	}

	return reclaimed;
}

static int perf_decode_scan_type(u64 packed_target)
{
	u64 type = packed_target / PERF_RECLAIM_TYPE_BASE;

	return type == 2 || type == 3 ? (int)type : 0;
}

static void perf_set_scan_type(u64 packed_target)
{
	WRITE_ONCE(reclaim_scan_type, perf_decode_scan_type(packed_target));
}

static void perf_tune_scan_type(void *unused, enum scan_balance *balance)
{
	char comm[TASK_COMM_LEN] = { 0 };
	size_t len;
	int type = READ_ONCE(reclaim_scan_type);

	if ((current->flags & PF_KSWAPD) || !type)
		return;

	get_task_comm(comm, current);
	if (strstr(comm, "mimd"))
		goto apply;

	/* Preserve the stock predicate, including its broad fallback. */
	len = strnlen(comm, TASK_COMM_LEN);
	if (strncmp(comm, "g_reclaim_thread", len) ||
	    strncmp(comm, "sh", len))
		goto apply;
	return;

apply:
	WRITE_ONCE(*(int *)balance, type);
}

static int global_reclaim_thread(void *unused)
{
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!atomic_read(&reclaim_pending))
			schedule();
		__set_current_state(TASK_RUNNING);
		if (kthread_should_stop())
			break;

		while (atomic_read(&reclaim_pending) > 0) {
			u32 target = READ_ONCE(reclaim_target);
			unsigned long pages = target % PERF_RECLAIM_TYPE_BASE;
			unsigned long reclaimed;

			__pm_wakeup_event(reclaim_ws, PERF_RECLAIM_WAKE_MS);
			perf_set_scan_type(target);
			reclaimed = perf_reclaim_pages(
				NULL, pages, MEMCG_RECLAIM_MAY_SWAP,
				false, false, -1);
			/*
			 * Stock performs an eleventh global reclaim when ten counted
			 * attempts fall short, but does not include its result in the
			 * reported total.
			 */
			if (reclaimed < pages)
				try_to_free_mem_cgroup_pages(
					NULL, pages - reclaimed, GFP_KERNEL,
					MEMCG_RECLAIM_MAY_SWAP);
			WRITE_ONCE(reclaim_scan_type, 0);
			pr_err("perf_helper %s reclaimed: %lu kbytes\n",
			       __func__, reclaimed << (PAGE_SHIFT - 10));
			__pm_relax(reclaim_ws);

			if (atomic_dec_return(&reclaim_pending) <= 0) {
				raw_spin_lock(&reclaim_status_lock);
				snprintf(reclaim_status, sizeof(reclaim_status),
					 "reclaim %lu pages", reclaimed);
				raw_spin_unlock(&reclaim_status_lock);
				break;
			}
		}
	}

	WRITE_ONCE(reclaim_scan_type, 0);
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int global_reclaim_show(struct seq_file *m, void *unused)
{
	/* Safety fix vs stock: pair the reader with the status writer's lock. */
	raw_spin_lock(&reclaim_status_lock);
	seq_printf(m, "%s", reclaim_status);
	raw_spin_unlock(&reclaim_status_lock);
	return 0;
}

static int global_reclaim_open(struct inode *inode, struct file *file)
{
	return single_open(file, global_reclaim_show, NULL);
}

static ssize_t global_reclaim_write(struct file *file,
				    const char __user *buffer, size_t size,
				    loff_t *offset)
{
	char input[PERF_TRIGGER_LEN + 1] = { 0 };
	u64 target;
	int ret;

	ret = perf_copy_user_string(input, sizeof(input), buffer, size);
	if (ret)
		return ret;
	ret = kstrtoull(input, 10, &target);
	if (ret)
		return ret;
	if (!target)
		return 0;

	WRITE_ONCE(reclaim_target, (u32)target);
	atomic_inc(&reclaim_pending);
	if (!IS_ERR_OR_NULL(reclaim_task))
		wake_up_process(reclaim_task);
	return size;
}

static const struct proc_ops global_reclaim_ops = {
	.proc_open = global_reclaim_open,
	.proc_read = seq_read,
	.proc_write = global_reclaim_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int kswapd_pid_show(struct seq_file *m, void *unused)
{
	struct task_struct *task = perf_find_kernel_task("kswapd0", 7);

	seq_printf(m, "%d\n", task ? task_pid_nr(task) : 0);
	return 0;
}

static int kswapd_pid_open(struct inode *inode, struct file *file)
{
	return single_open(file, kswapd_pid_show, NULL);
}

static const struct proc_ops kswapd_pid_ops = {
	.proc_open = kswapd_pid_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int kcompactd_pid_show(struct seq_file *m, void *unused)
{
	struct task_struct *task = perf_find_kernel_task("kcompactd0", 10);

	seq_printf(m, "%d\n", task ? task_pid_nr(task) : 0);
	return 0;
}

static int kcompactd_pid_open(struct inode *inode, struct file *file)
{
	return single_open(file, kcompactd_pid_show, NULL);
}

static const struct proc_ops kcompactd_pid_ops = {
	.proc_open = kcompactd_pid_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int kdamond_cpuset_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "kdamond pid:%d cpu_set:%s\n",
		   kdamond_pid, kdamond_cpu_set);
	return 0;
}

static int kdamond_cpuset_open(struct inode *inode, struct file *file)
{
	return single_open(file, kdamond_cpuset_show, NULL);
}

static ssize_t kdamond_cpuset_write(struct file *file,
				    const char __user *buffer, size_t size,
				    loff_t *offset)
{
	char input[PERF_TRIGGER_LEN + 1] = { 0 };
	char *allocation;
	char *cursor;
	char *cpu_list;
	char *pid_text;
	char *token;
	cpumask_t mask;
	struct task_struct *task;
	int cpu;
	int pid;
	int ret;

	ret = perf_copy_user_string(input, sizeof(input), buffer, size);
	if (ret)
		return ret;
	strscpy(kdamond_cpu_set, input, sizeof(kdamond_cpu_set));

	/* Stock parses only the first 32 bytes of this otherwise 64-byte ABI. */
	allocation = kstrndup(input, 32, GFP_KERNEL);
	if (!allocation)
		return -ENOMEM;
	/* Safety fix vs stock: keep the allocation base separate from strsep. */
	cursor = allocation;
	cpu_list = strsep(&cursor, ";");
	pid_text = strsep(&cursor, ";");
	if (!cpu_list || !pid_text) {
		ret = -EINVAL;
		goto out_free;
	}

	cpumask_clear(&mask);
	while ((token = strsep(&cpu_list, ",")) != NULL) {
		ret = kstrtoint(token, 10, &cpu);
		/* Safety fix vs stock: reject CPU IDs outside cpumask storage. */
		if (ret || cpu < 0 || cpu >= nr_cpumask_bits) {
			pr_err("Failed to convert string to integer\n");
			ret = -EINVAL;
			goto out_free;
		}
		cpumask_set_cpu(cpu, &mask);
	}

	ret = kstrtoint(pid_text, 10, &pid);
	if (ret || pid <= 0) {
		ret = -EINVAL;
		goto out_free;
	}
	kdamond_pid = pid;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	/* Safety fix vs stock: keep the task alive after dropping RCU. */
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task) {
		ret = -EINVAL;
		goto out_free;
	}
	ret = set_cpus_allowed_ptr(task, &mask);
	put_task_struct(task);
	if (!ret)
		ret = size;

out_free:
	kfree(allocation);
	if (ret < 0)
		pr_err("kdamond_cpuset_input_parse: param err(%s)\n", input);
	return ret;
}

static const struct proc_ops kdamond_cpuset_ops = {
	.proc_open = kdamond_cpuset_open,
	.proc_read = seq_read,
	.proc_write = kdamond_cpuset_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t mimd_trigger_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buffer)
{
	struct perf_trigger_event *event;
	ssize_t ret = 0;

	mutex_lock(&trigger_lock);
	if (!list_empty(&trigger_events)) {
		event = list_first_entry(&trigger_events,
					 struct perf_trigger_event, node);
		ret = snprintf(buffer, PERF_TRIGGER_LEN, "%s\n", event->text);
		list_del(&event->node);
		kfree(event);
		trigger_count--;
	}
	mutex_unlock(&trigger_lock);
	return ret;
}

static ssize_t mimd_trigger_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buffer, size_t size)
{
	struct perf_trigger_event *event;

	mutex_lock(&trigger_lock);
	if (trigger_count >= PERF_TRIGGER_COUNT) {
		mutex_unlock(&trigger_lock);
		pr_err("mimdtrigger: trigger_event so manay that discard\n");
		return size;
	}

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		mutex_unlock(&trigger_lock);
		pr_err("mimdtrigger: kmalloc struct trigger_event failed\n");
		return size;
	}
	strscpy(event->text, buffer, sizeof(event->text));
	list_add_tail(&event->node, &trigger_events);
	trigger_count++;
	mutex_unlock(&trigger_lock);
	return size;
}

static ssize_t mimd_notifier_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buffer)
{
	return snprintf(buffer, PAGE_SIZE, "%lld\n", mimd_notifier_data);
}

static ssize_t mimd_notifier_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buffer, size_t size)
{
	s64 value;
	int ret = kstrtoll(buffer, 10, &value);

	if (ret)
		return ret;
	WRITE_ONCE(mimd_notifier_data, value);
	return size;
}

static struct kobj_attribute mimd_trigger_attr =
	__ATTR(mimdtrigger, 0664, mimd_trigger_show, mimd_trigger_store);
static struct kobj_attribute mimd_notifier_attr =
	__ATTR(mimdnotifier, 0664, mimd_notifier_show, mimd_notifier_store);

static struct attribute *mimd_attrs[] = {
	&mimd_trigger_attr.attr,
	&mimd_notifier_attr.attr,
	NULL,
};

static const struct attribute_group mimd_attr_group = {
	.attrs = mimd_attrs,
};

static int exception_show(struct seq_file *m, void *unused)
{
	struct rtc_time tm;
	unsigned int index;
	unsigned int i;

	raw_spin_lock(&exception_lock);
	index = exception_count == PERF_EXCEPTION_COUNT ? exception_next : 0;
	for (i = 0; i < exception_count; i++) {
		struct perf_exception_record *record = &exceptions[index];

		rtc_time64_to_tm(record->timestamp.tv_sec, &tm);
		seq_printf(m,
			   "%d-%02d-%02d %02d:%02d:%02d UTC { %s count = %d }\n",
			   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			   tm.tm_hour, tm.tm_min, tm.tm_sec, record->text,
			   record->repeats + 1);
		index = (index + 1) % PERF_EXCEPTION_COUNT;
	}
	raw_spin_unlock(&exception_lock);
	return 0;
}

static int exception_open(struct inode *inode, struct file *file)
{
	return single_open(file, exception_show, NULL);
}

static ssize_t exception_write(struct file *file, const char __user *buffer,
			       size_t size, loff_t *offset)
{
	char text[PERF_EXCEPTION_LEN + 1] = { 0 };
	struct perf_exception_record *record;
	unsigned int i;
	int ret;

	ret = perf_copy_user_string(text, sizeof(text), buffer, size);
	if (ret) {
		if (ret == -EINVAL)
			pr_err("perflock_exception userbuf size longer than 1024\n");
		else
			pr_err("perflock_exception cant copy_from_userbuf\n");
		return ret;
	}

	if (!raw_spin_trylock(&exception_lock))
		return size;

	for (i = 0; i < exception_count; i++) {
		record = &exceptions[i];
		if (!strcmp(text, record->text)) {
			/* Safety fix vs stock: saturate instead of duplicating at INT_MAX. */
			if (record->repeats < INT_MAX)
				record->repeats++;
			raw_spin_unlock(&exception_lock);
			return size;
		}
	}

	record = &exceptions[exception_next];
	ktime_get_real_ts64(&record->timestamp);
	snprintf(record->text, sizeof(record->text), "%s", text);
	record->repeats = 0;
	exception_next = (exception_next + 1) % PERF_EXCEPTION_COUNT;
	if (exception_count < PERF_EXCEPTION_COUNT)
		exception_count++;
	raw_spin_unlock(&exception_lock);
	return size;
}

static const struct proc_ops exception_ops = {
	.proc_open = exception_open,
	.proc_read = seq_read,
	.proc_write = exception_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static ssize_t memcg_reclaim_once(struct kernfs_open_file *of, char *buffer,
				  size_t size, loff_t offset)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct perf_memcg_state *state;
	u64 packed_target;
	unsigned long pages;
	unsigned long reclaimed;
	int scan_type;
	int ret;

	ret = kstrtoull(strim(buffer), 10, &packed_target);
	if (ret)
		return ret;

	mutex_lock(&memcg_lock);
	list_for_each_entry(state, &memcg_states, node) {
		if (state->memcg == memcg) {
			state->foreground = false;
			break;
		}
	}
	mutex_unlock(&memcg_lock);

	pages = packed_target % PERF_RECLAIM_TYPE_BASE;
	scan_type = perf_decode_scan_type(packed_target);
	reclaimed = perf_reclaim_pages(
		memcg, pages, PERF_STOCK_MEMCG_RECLAIM_OPTIONS,
		true, true, scan_type);
	pr_err("perf_helper %s reclaimed: %lu kbytes\n",
	       __func__, reclaimed << (PAGE_SHIFT - 10));
	return size;
}

static ssize_t memcg_process_fg(struct kernfs_open_file *of, char *buffer,
				size_t size, loff_t offset)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct perf_memcg_state *state;
	int value;
	int ret;

	ret = kstrtoint(strim(buffer), 10, &value);
	if (ret)
		return ret;

	mutex_lock(&memcg_lock);
	list_for_each_entry(state, &memcg_states, node) {
		if (state->memcg == memcg) {
			state->foreground = value > 0;
			mutex_unlock(&memcg_lock);
			return size;
		}
	}
	mutex_unlock(&memcg_lock);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	/* Safety fix vs stock: handle allocation failure instead of dereferencing. */
	if (!state)
		return -ENOMEM;
	state->memcg = memcg;
	state->foreground = value > 0;

	mutex_lock(&memcg_lock);
	list_add_tail(&state->node, &memcg_states);
	mutex_unlock(&memcg_lock);
	return size;
}

static struct cftype memcg_ctrl_files[] = {
	{
		.name = "reclaim_once",
		.write = memcg_reclaim_once,
	},
	{
		.name = "process_fg",
		.write = memcg_process_fg,
	},
	{ }
};

static void perf_remove_proc_entries(void)
{
	proc_remove(exception_entry);
	proc_remove(kcompactd_pid_entry);
	proc_remove(kdamond_cpuset_entry);
	proc_remove(kswapd_pid_entry);
	proc_remove(global_reclaim_entry);
	proc_remove(mimd_log_entry);
	proc_remove(perflock_records_entry);
	proc_remove(kswapd_affinity_entry);

	exception_entry = NULL;
	kcompactd_pid_entry = NULL;
	kdamond_cpuset_entry = NULL;
	kswapd_pid_entry = NULL;
	global_reclaim_entry = NULL;
	mimd_log_entry = NULL;
	perflock_records_entry = NULL;
	kswapd_affinity_entry = NULL;
}

static void perf_free_lists(void)
{
	struct perf_trigger_event *event;
	struct perf_trigger_event *event_tmp;
	struct perf_memcg_state *state;
	struct perf_memcg_state *state_tmp;

	mutex_lock(&trigger_lock);
	list_for_each_entry_safe(event, event_tmp, &trigger_events, node) {
		list_del(&event->node);
		kfree(event);
	}
	trigger_count = 0;
	mutex_unlock(&trigger_lock);

	mutex_lock(&memcg_lock);
	list_for_each_entry_safe(state, state_tmp, &memcg_states, node) {
		list_del(&state->node);
		kfree(state);
	}
	mutex_unlock(&memcg_lock);
}

static int __init perf_helper_init(void)
{
	cpumask_t reclaim_mask;
	int ret;

	/* Safety fix vs stock: fail and unwind instead of leaving a partial ABI. */
	kswapd_task = perf_find_kernel_task("kswapd0", 7);
	kcompactd_task = perf_find_kernel_task("kcompactd0", 10);

	kswapd_affinity_entry = proc_create("kswapd_affinity", 0664, NULL,
					    &kswapd_affinity_ops);
	if (!kswapd_affinity_entry)
		return -ENOMEM;

	ret = register_trace_android_vh_tune_scan_type(perf_tune_scan_type, NULL);
	if (ret)
		goto err;
	vmscan_hook_registered = true;

	reclaim_task = kthread_create(global_reclaim_thread, NULL,
				      "g_reclaim_thread");
	if (IS_ERR(reclaim_task)) {
		ret = PTR_ERR(reclaim_task);
		reclaim_task = NULL;
		goto err;
	}
	cpumask_clear(&reclaim_mask);
	cpumask_set_cpu(0, &reclaim_mask);
	cpumask_set_cpu(1, &reclaim_mask);
	cpumask_set_cpu(2, &reclaim_mask);
	cpumask_set_cpu(3, &reclaim_mask);
	ret = set_cpus_allowed_ptr(reclaim_task, &reclaim_mask);
	if (ret)
		goto err;

	reclaim_ws = wakeup_source_register(NULL, "reclaim_wakeup_source");
	if (!reclaim_ws) {
		ret = -ENOMEM;
		goto err;
	}

	perflock_records_entry = proc_create("perflock_records", 0664, NULL,
					     &perflock_records_ops);
	mimd_log_entry = proc_create("mimdlog", 0664, NULL, &mimd_log_ops);
	global_reclaim_entry = proc_create("global_reclaim", 0664, NULL,
					   &global_reclaim_ops);
	kswapd_pid_entry = proc_create("kswapd_pid", 0440, NULL,
				       &kswapd_pid_ops);
	kdamond_cpuset_entry = proc_create("kdamond_cpuset", 0664, NULL,
					   &kdamond_cpuset_ops);
	kcompactd_pid_entry = proc_create("kcompactd_pid", 0664, NULL,
					  &kcompactd_pid_ops);
	exception_entry = proc_create("perflock_exception", 0664, NULL,
				      &exception_ops);
	if (!perflock_records_entry || !mimd_log_entry || !global_reclaim_entry ||
	    !kswapd_pid_entry || !kdamond_cpuset_entry ||
	    !kcompactd_pid_entry || !exception_entry) {
		ret = -ENOMEM;
		goto err;
	}

	mimd_kobj = kobject_create_and_add("mimd", &THIS_MODULE->mkobj.kobj);
	if (!mimd_kobj) {
		ret = -ENOMEM;
		goto err;
	}
	ret = sysfs_create_group(mimd_kobj, &mimd_attr_group);
	if (ret)
		goto err;

	ret = cgroup_add_legacy_cftypes(&memory_cgrp_subsys, memcg_ctrl_files);
	if (ret)
		goto err;
	memcg_files_registered = true;

	return 0;

err:
	if (mimd_kobj) {
		sysfs_remove_group(mimd_kobj, &mimd_attr_group);
		kobject_put(mimd_kobj);
		mimd_kobj = NULL;
	}
	perf_remove_proc_entries();
	if (reclaim_ws) {
		wakeup_source_unregister(reclaim_ws);
		reclaim_ws = NULL;
	}
	if (reclaim_task) {
		kthread_stop(reclaim_task);
		reclaim_task = NULL;
	}
	if (vmscan_hook_registered) {
		unregister_trace_android_vh_tune_scan_type(perf_tune_scan_type, NULL);
		vmscan_hook_registered = false;
	}
	return ret;
}

static void __exit perf_helper_exit(void)
{
	/* Safety fix vs stock: unregister every callback and exposed interface. */
	if (memcg_files_registered) {
		cgroup_rm_cftypes(memcg_ctrl_files);
		memcg_files_registered = false;
	}
	perf_remove_proc_entries();

	if (mimd_kobj) {
		sysfs_remove_group(mimd_kobj, &mimd_attr_group);
		kobject_put(mimd_kobj);
		mimd_kobj = NULL;
	}
	if (vmscan_hook_registered) {
		unregister_trace_android_vh_tune_scan_type(perf_tune_scan_type, NULL);
		vmscan_hook_registered = false;
	}
	if (reclaim_task) {
		kthread_stop(reclaim_task);
		reclaim_task = NULL;
	}
	if (reclaim_ws) {
		wakeup_source_unregister(reclaim_ws);
		reclaim_ws = NULL;
	}
	perf_free_lists();
}

module_init(perf_helper_init);
module_exit(perf_helper_exit);

MODULE_DESCRIPTION("Xiaomi performance and memory helper compatibility driver");
MODULE_LICENSE("GPL");
