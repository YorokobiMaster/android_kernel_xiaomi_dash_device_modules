// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/rtc.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/kthread.h>
#include <linux/power_supply.h>
#include <linux/types.h>
#include <net/sock.h>
#include <net/netlink.h>
#include "xiaomi_touch_common.h"
#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
#include "../../../gpu/drm/mediatek/mediatek_v2/mi_disp/mi_disp_notifier.h"
#endif
//#include "../tp_get_lcm_name/tp_get_lcd_name.h"
#define NETLINK_TEST 24
#define MAX_MSGSIZE 16
int stringlength(char *s);
void sendnlmsg(char message);
static int pid = -1;
struct sock *nl_sk;
static struct xiaomi_touch_pdata *touch_pdata;
static struct xiaomi_touch *xiaomi_touch_device;
static struct xiaomi_touch_panel_data touch_panel_data[XIAOMI_TOUCH_MAX_PANEL];
static common_data_t thp_ic_cmd_data_common_data;
static DEFINE_MUTEX(thp_ic_read_data_mutex);
static DEFINE_MUTEX(thp_ic_write_data_mutex);
static bool xiaomi_touch_probe_finished = false;
#define RAW_SIZE (PAGE_SIZE * 12)

static int temperature_charge_state = -1;
static int temperature_last = 1000;

int get_bms_temp_common(void)
{
	struct power_supply *bms;
	union power_supply_propval value = { 0 };

	bms = power_supply_get_by_name("bms");
	if (!bms || power_supply_get_property(bms, POWER_SUPPLY_PROP_TEMP, &value))
		return -1000;
	return value.intval;
}
EXPORT_SYMBOL_GPL(get_bms_temp_common);

void xiaomi_touch_set_temperature_charge_state(int charging)
{
	WRITE_ONCE(temperature_charge_state, charging);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_set_temperature_charge_state);

void enable_temperature_detection_func(bool enable)
{
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (!panel->registered || !panel->hardware_operation.set_thermal_temp)
		return;
	WRITE_ONCE(panel->temp_enabled, enable);
	wake_up_interruptible(&panel->temp_wait);
}
EXPORT_SYMBOL_GPL(enable_temperature_detection_func);

void stop_temperature_detection_func(void)
{
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (panel->temp_thread) {
		kthread_stop(panel->temp_thread);
		panel->temp_thread = NULL;
	}
}
EXPORT_SYMBOL_GPL(stop_temperature_detection_func);

static int xiaomi_touch_temp_thread_func(void *data)
{
	struct xiaomi_touch_panel_data *panel = data;
	int raw_temp;
	int temperature;
	unsigned int interval;

	while (!kthread_should_stop()) {
		wait_event_interruptible(panel->temp_wait,
			READ_ONCE(panel->temp_enabled) || kthread_should_stop());
		if (kthread_should_stop())
			break;
		if (!READ_ONCE(panel->temp_enabled))
			continue;
		raw_temp = get_bms_temp_common();
		temperature = (raw_temp + 5) / 10;
		if (abs(raw_temp) < 1000 &&
		    abs(temperature - temperature_last) >=
		    panel->hardware_param.temperature_change_threshold) {
			panel->hardware_operation.set_thermal_temp(temperature, false);
			/* Stock notifies and advances the cache even on IC failure. */
			add_common_data_to_buf(0, SET_CUR_VALUE, 0x446, 1, &temperature);
			temperature_last = temperature;
		}
		interval = READ_ONCE(temperature_charge_state) ? 5000 : 10000;
		wait_event_interruptible_timeout(panel->temp_wait,
			kthread_should_stop(), msecs_to_jiffies(interval));
	}
	return 0;
}

static_assert(sizeof(hardware_param_t) == 0xd6);
static_assert(offsetof(hardware_param_t, temperature_change_threshold) == 0xd5);

static_assert(sizeof(hardware_operation_t) == 31 * sizeof(void *));
static_assert(offsetof(hardware_operation_t, set_cur_value) ==
		4 * sizeof(void *));
static_assert(offsetof(hardware_operation_t, enable_touch_raw) == 0x60);
static_assert(offsetof(hardware_operation_t, touch_doze_analysis) == 0x90);
static_assert(offsetof(hardware_operation_t, display_suspend_ready) ==
		20 * sizeof(void *));
static_assert(offsetof(hardware_operation_t, resume_suspend) ==
		23 * sizeof(void *));

static void xiaomi_touch_resume_work(struct work_struct *work)
{
	struct xiaomi_touch_panel_data *panel = container_of(work,
			struct xiaomi_touch_panel_data, resume_work);
	int ret;

	mutex_lock(&panel->pm_lock);
	if (!panel->registered || !panel->panel_notifier_registered ||
	    !panel->suspended) {
		mutex_unlock(&panel->pm_lock);
		return;
	}

	if (!panel->hardware_operation.resume_suspend) {
		mutex_unlock(&panel->pm_lock);
		return;
	}

	ret = panel->hardware_operation.resume_suspend(1, 0);
	if (!ret) {
		panel->suspended = false;
		xiaomi_touch_set_suspend_state(XIAOMI_TOUCH_RESUME);
	} else {
		dev_err(panel->dev, "touch resume failed: %d\n", ret);
	}
	mutex_unlock(&panel->pm_lock);
}

#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
static int xiaomi_drm_panel_notifier_callback(struct notifier_block *nb,
		unsigned long event, void *data)
{
	struct xiaomi_touch_panel_data *panel = container_of(nb,
			struct xiaomi_touch_panel_data, panel_notifier);
	struct mi_disp_notifier *evdata = data;
	int blank;
	int ret;

	if (event != MI_DISP_DPMS_EVENT && event != MI_DISP_DPMS_EARLY_EVENT)
		return NOTIFY_DONE;
	if (!evdata || !evdata->data)
		return NOTIFY_DONE;

	blank = *(int *)evdata->data;
	if (blank == MI_DISP_DPMS_LP1 || blank == MI_DISP_DPMS_LP2 ||
	    blank == MI_DISP_DPMS_POWERDOWN) {
		/* Finish an older resume before entering the low-power state. */
		flush_workqueue(panel->pm_wq);

		mutex_lock(&panel->pm_lock);
		if (!panel->registered || !panel->panel_notifier_registered ||
		    panel->suspended) {
			mutex_unlock(&panel->pm_lock);
			return NOTIFY_OK;
		}
		if (!panel->hardware_operation.resume_suspend) {
			mutex_unlock(&panel->pm_lock);
			return NOTIFY_DONE;
		}

		ret = panel->hardware_operation.resume_suspend(0, 0);
		if (!ret) {
			panel->suspended = true;
			xiaomi_touch_set_suspend_state(XIAOMI_TOUCH_SUSPEND);
		} else {
			dev_err(panel->dev, "touch suspend failed: %d\n", ret);
		}
		mutex_unlock(&panel->pm_lock);
		return NOTIFY_OK;
	}

	if (event == MI_DISP_DPMS_EVENT && blank == MI_DISP_DPMS_ON) {
		queue_work(panel->pm_wq, &panel->resume_work);
		return NOTIFY_OK;
	}

	if (event == MI_DISP_DPMS_EARLY_EVENT &&
	    blank == MI_DISP_DPMS_OFF) {
		mutex_lock(&panel->pm_lock);
		if (panel->registered && panel->panel_notifier_registered &&
		    panel->hardware_operation.display_suspend_ready)
			panel->hardware_operation.display_suspend_ready();
		mutex_unlock(&panel->pm_lock);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}
#endif

int xiaomi_register_panel_notifier_common(struct device *dev, int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	int ret;

	if (!dev || touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return -EINVAL;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return -ENODEV;
	if (panel->panel_notifier_registered)
		return -EBUSY;

#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
	panel->pm_wq = alloc_ordered_workqueue("xiaomi_touch_pm_%d",
			WQ_MEM_RECLAIM, touch_id);
	if (!panel->pm_wq)
		return -ENOMEM;

	panel->dev = dev;
	panel->suspended = false;
	INIT_WORK(&panel->resume_work, xiaomi_touch_resume_work);
	panel->panel_notifier.notifier_call =
		xiaomi_drm_panel_notifier_callback;
	ret = mi_disp_register_client(&panel->panel_notifier);
	if (ret) {
		destroy_workqueue(panel->pm_wq);
		panel->pm_wq = NULL;
		panel->dev = NULL;
		return ret;
	}
	panel->panel_notifier_registered = true;
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}
EXPORT_SYMBOL_GPL(xiaomi_register_panel_notifier_common);

void xiaomi_unregister_panel_notifier_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->panel_notifier_registered)
		return;

#if IS_ENABLED(CONFIG_MI_DISP_NOTIFIER)
	if (mi_disp_unregister_client(&panel->panel_notifier))
		dev_err(panel->dev, "failed to unregister display notifier\n");
#endif
	cancel_work_sync(&panel->resume_work);
	destroy_workqueue(panel->pm_wq);
	panel->pm_wq = NULL;

	mutex_lock(&panel->pm_lock);
	panel->panel_notifier_registered = false;
	panel->suspended = false;
	panel->dev = NULL;
	mutex_unlock(&panel->pm_lock);
}
EXPORT_SYMBOL_GPL(xiaomi_unregister_panel_notifier_common);

void sendnlmsg(char message)//char *message
{
	struct sk_buff *skb_1;
	struct nlmsghdr *nlh;
	int len = NLMSG_SPACE(MAX_MSGSIZE);
	int slen = 0;
	int ret = 0;
	if (!nl_sk || !pid) {
		return;
	}
	skb_1 = alloc_skb(len, GFP_KERNEL);
	if (!skb_1) {
		mi_ts_err("alloc_skb error\n");
		return;
	}
	slen = sizeof(message);
	nlh = nlmsg_put(skb_1, 0, 0, 0, MAX_MSGSIZE, 0);
	NETLINK_CB(skb_1).portid = 0;
	NETLINK_CB(skb_1).dst_group = 0;
//	message[slen] = '\0';
	memcpy(NLMSG_DATA(nlh), &message, slen);
	ret = netlink_unicast(nl_sk, skb_1, pid, MSG_DONTWAIT);
	if (!ret) {
		/*kfree_skb(skb_1); */
		mi_ts_err("send msg from kernel to usespace failed ret 0x%x\n",
		       ret);
	}
}
void nl_data_ready(struct sk_buff *__skb)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	char str[100];
	skb = skb_get(__skb);
	if (skb->len >= NLMSG_SPACE(0)) {
		nlh = nlmsg_hdr(skb);
		memcpy(str, NLMSG_DATA(nlh), sizeof(str));
		pid = nlh->nlmsg_pid;
		kfree_skb(skb);
	}
	mi_ts_info("netlink socket, pid =%d, msg = %d\n", pid, str[0]);
}
int netlink_init(void)
{
	struct netlink_kernel_cfg netlink_cfg;
	memset(&netlink_cfg, 0, sizeof(struct netlink_kernel_cfg));
	netlink_cfg.groups = 0;
	netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;
	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST, &netlink_cfg);
	if (!nl_sk) {
		mi_ts_err("create netlink socket error\n");
		return 1;
	}
	return 0;
}
void netlink_exit(void)
{
	if (nl_sk != NULL) {
		netlink_kernel_release(nl_sk);
		nl_sk = NULL;
	}
	mi_ts_info("self module exited\n");
}

static void xiaomi_touch_format_info(struct xiaomi_touch_panel_data *panel,
				     const char *name)
{
	hardware_operation_t *ops = &panel->hardware_operation;
	hardware_param_t *param = &panel->hardware_param;
	char *buf = panel->info_result;
	size_t size = sizeof(panel->info_result);
	int len = 0;
	int i;

	if (!strncmp(name, "tp_fw_version", 13)) {
		if (ops->get_fw_version)
			ops->get_fw_version(param->fw_version);
		len = snprintf(buf, size, "fw version: %s\ndriver version: %s\nhal version: %s\n"
			   "xiaomi-touch version: %s\n", param->fw_version,
			   param->driver_version, panel->hal_version, "2025.04.30-01");
		/* The CSV-backed limit callback remains absent until A42. */
		if (ops->limit_version_read) {
			ops->limit_version_read(panel->limit_version);
			len += snprintf(buf + len, size - len, "limit csv version: %s\n",
					panel->limit_version);
		}
	} else if (!strncmp(name, "tp_lockdown_info", 16)) {
		if (ops->lockdown_info_read) {
			ops->lockdown_info_read(param->lockdown_info);
			for (i = 0; i < ARRAY_SIZE(param->lockdown_info); i++)
				len += snprintf(buf + len, size - len, "0x%02X%s", param->lockdown_info[i],
					   i == 7 ? "\n" : ",");
		} else {
			len = snprintf(buf, size, "null");
		}
	} else {
		len = snprintf(buf, size, "%d\n", panel->self_test_result);
	}
	panel->info_result_len = len;
}

static ssize_t xiaomi_touch_info_read(struct file *file, char __user *buf,
				      size_t count, loff_t *pos)
{
	struct xiaomi_touch_panel_data *panel = pde_data(file_inode(file));
	ssize_t ret;

	mutex_lock(&panel->proc_lock);
	/* Stock shares the result buffer across info nodes on one panel. */
	if (!*pos)
		xiaomi_touch_format_info(panel, file->f_path.dentry->d_name.name);
	ret = simple_read_from_buffer(buf, count, pos, panel->info_result,
				      panel->info_result_len);
	mutex_unlock(&panel->proc_lock);
	return ret;
}

static ssize_t xiaomi_touch_info_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *pos)
{
	struct xiaomi_touch_panel_data *panel = pde_data(file_inode(file));
	const char *name = file->f_path.dentry->d_name.name;
	char command[64] = { 0 };
	int retry;
	ssize_t ret = count;

	mutex_lock(&panel->proc_lock);
	if (!strncmp(name, "tp_fw_version", 13)) {
		memset(panel->hal_version, 0, sizeof(panel->hal_version));
		if (copy_from_user(panel->hal_version, buf,
				   min(count, sizeof(panel->hal_version) - 1)))
			ret = -EFAULT;
	} else if (!strncmp(name, "tp_selftest", 11)) {
		if (copy_from_user(command, buf, min(count, sizeof(command) - 1))) {
			ret = -EFAULT;
			goto out;
		}
		if (panel->hardware_operation.self_test) {
			/* Stock calls once, then performs up to five checked attempts. */
			panel->hardware_operation.self_test(command, &panel->self_test_result);
			for (retry = 0; retry < 5; retry++) {
				panel->hardware_operation.self_test(command, &panel->self_test_result);
				if (panel->self_test_result == 2)
					break;
			}
		}
	}
out:
	mutex_unlock(&panel->proc_lock);
	return ret;
}

static const struct proc_ops xiaomi_touch_info_ops = {
	.proc_read = xiaomi_touch_info_read,
	.proc_write = xiaomi_touch_info_write,
	.proc_lseek = default_llseek,
};

static int xiaomi_touch_create_info_proc(struct xiaomi_touch_panel_data *panel,
				       int touch_id)
{
	static const char * const names[] = {
		"tp_fw_version", "tp_lockdown_info", "tp_selftest",
	};
	char name[64];
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (touch_id)
			snprintf(name, sizeof(name), "%s_%d", names[i], touch_id);
		else
			strscpy(name, names[i], sizeof(name));
		panel->info_proc[i] = proc_create_data(name, 0644, NULL,
						     &xiaomi_touch_info_ops, panel);
		if (!panel->info_proc[i]) {
			while (i--) {
				proc_remove(panel->info_proc[i]);
				panel->info_proc[i] = NULL;
			}
			return -ENOMEM;
		}
	}
	return 0;
}

int register_touch_panel_common(struct device *dev, int touch_id,
		const hardware_param_t *hardware_param,
		const hardware_operation_t *hardware_operation)
{
	struct xiaomi_touch_panel_data *panel;
	size_t frame_total;
	size_t raw_total;

	(void)dev;
	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL || !hardware_param)
		return -EINVAL;

	panel = &touch_panel_data[touch_id];
	if (panel->registered)
		return -EBUSY;

	if (!hardware_param->frame_data_page_size ||
	    !hardware_param->frame_data_buf_size ||
	    !hardware_param->raw_data_page_size ||
	    !hardware_param->raw_data_buf_size)
		return -EINVAL;

	frame_total = (size_t)hardware_param->frame_data_page_size * PAGE_SIZE *
		      hardware_param->frame_data_buf_size;
	raw_total = (size_t)hardware_param->raw_data_page_size * PAGE_SIZE *
		    hardware_param->raw_data_buf_size;

	panel->frame_data = kzalloc(frame_total, GFP_KERNEL);
	if (!panel->frame_data)
		return -ENOMEM;

	panel->raw_data = kzalloc(raw_total, GFP_KERNEL);
	if (!panel->raw_data) {
		kfree(panel->frame_data);
		panel->frame_data = NULL;
		return -ENOMEM;
	}

	memcpy(&panel->hardware_param, hardware_param, sizeof(*hardware_param));
	memset(&panel->hardware_operation, 0, sizeof(panel->hardware_operation));
	if (hardware_operation)
		memcpy(&panel->hardware_operation, hardware_operation,
		       sizeof(panel->hardware_operation));
	panel->frame_data_size =
		(size_t)hardware_param->frame_data_page_size * PAGE_SIZE;
	panel->frame_data_buf_size = hardware_param->frame_data_buf_size;
	panel->frame_data_phys = virt_to_phys(panel->frame_data);
	panel->raw_data_size =
		(size_t)hardware_param->raw_data_page_size * PAGE_SIZE;
	panel->raw_data_buf_size = hardware_param->raw_data_buf_size;
	panel->raw_data_phys = virt_to_phys(panel->raw_data);
	atomic_set(&panel->frame_data_index, 0);
	atomic_set(&panel->raw_data_index, 0);
	atomic_set(&panel->common_data_index, 0);
	INIT_LIST_HEAD(&panel->client_list);
	spin_lock_init(&panel->client_lock);
	mutex_init(&panel->common_data_lock);
	mutex_init(&panel->pm_lock);
	mutex_init(&panel->proc_lock);
	memset(panel->hal_version, 0, sizeof(panel->hal_version));
	memset(panel->limit_version, 0, sizeof(panel->limit_version));
	panel->info_result_len = 0;
	panel->self_test_result = 0;
	panel->registered = true;
	init_waitqueue_head(&panel->temp_wait);
	panel->temp_enabled = true;
	if (xiaomi_touch_create_info_proc(panel, touch_id)) {
		unregister_touch_panel_common(touch_id);
		return -ENOMEM;
	}
	if (touch_id == 0 && panel->hardware_operation.set_thermal_temp) {
		panel->temp_thread = kthread_run(xiaomi_touch_temp_thread_func,
					       panel, "xiaomi_touch_temp_thread");
		if (IS_ERR(panel->temp_thread)) {
			int ret = PTR_ERR(panel->temp_thread);

			panel->temp_thread = NULL;
			unregister_touch_panel_common(touch_id);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(register_touch_panel_common);

void xiaomi_touch_remove_info_proc_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	int i;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;
	panel = &touch_panel_data[touch_id];
	for (i = 0; i < ARRAY_SIZE(panel->info_proc); i++) {
		proc_remove(panel->info_proc[i]);
		panel->info_proc[i] = NULL;
	}
}
EXPORT_SYMBOL_GPL(xiaomi_touch_remove_info_proc_common);

void unregister_touch_panel_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_client *client;
	struct xiaomi_touch_client *next;
	unsigned long flags;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return;
	xiaomi_touch_remove_info_proc_common(touch_id);
	xiaomi_unregister_panel_notifier_common(touch_id);
	if (touch_id == 0)
		stop_temperature_detection_func();
	spin_lock_irqsave(&panel->client_lock, flags);
	list_for_each_entry_safe(client, next, &panel->client_list, node) {
		list_del_init(&client->node);
		client->touch_id = -1;
		wake_up_all(&client->poll_wait_queue_head);
	}
	panel->registered = false;
	spin_unlock_irqrestore(&panel->client_lock, flags);

	kfree(panel->raw_data);
	kfree(panel->frame_data);
	panel->raw_data = NULL;
	panel->frame_data = NULL;
	memset(&panel->hardware_operation, 0, sizeof(panel->hardware_operation));
}
EXPORT_SYMBOL_GPL(unregister_touch_panel_common);

void *get_raw_data_base_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return NULL;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return NULL;

	return (u8 *)panel->frame_data + atomic_read(&panel->frame_data_index) *
		panel->frame_data_size;
}
EXPORT_SYMBOL_GPL(get_raw_data_base_common);

void notify_raw_data_update_common(int touch_id)
{
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_client *client;
	unsigned long flags;
	int index;

	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return;

	smp_wmb();
	index = atomic_inc_return(&panel->frame_data_index);
	if (index >= panel->frame_data_buf_size)
		atomic_set(&panel->frame_data_index, 0);

	spin_lock_irqsave(&panel->client_lock, flags);
	list_for_each_entry(client, &panel->client_list, node)
		wake_up_all(&client->poll_wait_queue_head);
	spin_unlock_irqrestore(&panel->client_lock, flags);
}
EXPORT_SYMBOL_GPL(notify_raw_data_update_common);

static int xiaomi_touch_dev_open(struct inode *inode, struct file *file)
{
	struct xiaomi_touch_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	INIT_LIST_HEAD(&client->node);
	client->touch_id = -1;
	init_waitqueue_head(&client->poll_wait_queue_head);
	file->private_data = client;
	return 0;
}
static ssize_t xiaomi_touch_dev_read(struct file *file, char __user *buf,
				size_t count, loff_t *pos)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	int index;

	if (!client || client->touch_id < 0 || count != sizeof(common_data_t))
		return -EINVAL;

	panel = &touch_panel_data[client->touch_id];
	if (!panel->registered)
		return -ENODEV;

	mutex_lock(&panel->common_data_lock);
	index = atomic_read(&client->common_data_index);
	if (index == atomic_read(&panel->common_data_index)) {
		mutex_unlock(&panel->common_data_lock);
		return 0;
	}
	if (copy_to_user(buf, &panel->common_data_buf[index], count)) {
		mutex_unlock(&panel->common_data_lock);
		return -EFAULT;
	}
	if (++index >= COMMON_DATA_BUF_SIZE)
		index = 0;
	atomic_set(&client->common_data_index, index);
	mutex_unlock(&panel->common_data_lock);

	return count;
}
static ssize_t xiaomi_touch_dev_write(struct file *file,
				const char __user *buf, size_t count, loff_t *pos)
{
	return count;
}
static unsigned int xiaomi_touch_dev_poll(struct file *file,
		poll_table *wait)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	unsigned int mask = 0;

	if (!client || client->touch_id < 0)
		return POLLERR;

	panel = &touch_panel_data[client->touch_id];
	if (!panel->registered)
		return POLLERR;

	poll_wait(file, &client->poll_wait_queue_head, wait);
	if (atomic_read(&client->common_data_index) !=
	    atomic_read(&panel->common_data_index))
		mask |= POLLRDNORM;
	if (atomic_read(&client->frame_data_index) !=
	    atomic_read(&panel->frame_data_index))
		mask |= POLLPRI;
	if (atomic_read(&client->raw_data_index) !=
	    atomic_read(&panel->raw_data_index))
		mask |= POLLRDBAND;

	return mask;
}
void notify_xiaomi_touch(struct xiaomi_touch_interface *touch_data)
{
	struct xiaomi_touch_pdata *client_private_data = NULL;
	if (!touch_data)
		return;
	spin_lock(&touch_data->private_data_lock);
	list_for_each_entry_rcu(client_private_data, &touch_data->private_data_list, node) {
		mi_ts_info("notify xiaomi-touch data update, client private data is %p", client_private_data);
		wake_up_all(&client_private_data->poll_wait_queue_head);
	}
	spin_unlock(&touch_data->private_data_lock);
}
void add_common_data_to_buf(s8 touch_id, enum common_data_cmd cmd, enum common_data_mode mode, int length, int *data)
{
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_client *client;
	common_data_t *common_data = NULL;
	unsigned long flags;
	int index;

	if (!xiaomi_touch_probe_finished) {
		return;
	}
	if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL ||
	    length < 0 || length > CMD_DATA_BUF_SIZE || (length && !data))
		return;

	panel = &touch_panel_data[touch_id];
	if (!panel->registered)
		return;

	mutex_lock(&panel->common_data_lock);
	index = atomic_read(&panel->common_data_index);
	common_data = &panel->common_data_buf[index];
	memset(common_data, 0, sizeof(*common_data));
	common_data->touch_id = touch_id;
	common_data->cmd = cmd;
	common_data->mode = mode;
	common_data->data_len = length;
	if (length)
		memcpy(common_data->data_buf, data, length * sizeof(s32));
	if (++index >= COMMON_DATA_BUF_SIZE)
		index = 0;
	atomic_set(&panel->common_data_index, index);
	mutex_unlock(&panel->common_data_lock);

	spin_lock_irqsave(&panel->client_lock, flags);
	list_for_each_entry(client, &panel->client_list, node)
		wake_up_all(&client->poll_wait_queue_head);
	spin_unlock_irqrestore(&panel->client_lock, flags);
}
EXPORT_SYMBOL_GPL(add_common_data_to_buf);
static long xiaomi_touch_dev_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	struct xiaomi_touch_interface *touch_data;
	common_data_t common_data;
	void __user *argp = (void __user *)arg;
	unsigned long flags;
	int touch_id;
	int index;
	int ret = 0;

	if (!client || _IOC_TYPE(cmd) != 'T')
		return -EINVAL;

	switch (_IOC_NR(cmd)) {
	case COMMON_DATA_CMD:
		if (_IOC_SIZE(cmd) != sizeof(common_data_t) ||
		    copy_from_user(&common_data, argp, sizeof(common_data)))
			return -EFAULT;
		touch_id = common_data.touch_id;
		if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL ||
		    !touch_pdata || !touch_pdata->touch_data[touch_id] ||
		    client->touch_id != touch_id ||
		    common_data.data_len > CMD_DATA_BUF_SIZE)
			return -EINVAL;
		touch_data = touch_pdata->touch_data[touch_id];
		panel = &touch_panel_data[touch_id];

		mutex_lock(&touch_pdata->device->mutex);
		switch (common_data.cmd) {
		case SET_CUR_VALUE:
			if (!common_data.data_len) {
				ret = -EOPNOTSUPP;
				break;
			}
			memset(&common_data.data_buf[common_data.data_len], 0,
			       sizeof(common_data.data_buf) -
			       common_data.data_len * sizeof(common_data.data_buf[0]));
			if (common_data.mode < Touch_Mode_NUM) {
				if (!touch_data->setModeValue) {
					ret = -EOPNOTSUPP;
					break;
				}
				common_data.data_buf[0] =
					touch_data->setModeValue(common_data.mode,
						common_data.data_buf[0]);
			} else {
				if (!panel->registered ||
				    !panel->hardware_operation.set_cur_value) {
					ret = -EOPNOTSUPP;
					break;
				}
				common_data.data_buf[0] =
					panel->hardware_operation.set_cur_value(
						common_data.mode,
						common_data.data_buf);
			}
			break;
		case GET_CUR_VALUE:
		case GET_DEF_VALUE:
		case GET_MIN_VALUE:
		case GET_MAX_VALUE:
			if (!touch_data->getModeValue) {
				ret = -EOPNOTSUPP;
				break;
			}
			common_data.data_buf[0] =
				touch_data->getModeValue(common_data.mode,
					common_data.cmd);
			common_data.data_len = 1;
			break;
		case GET_MODE_VALUE:
			if (!touch_data->getModeAll) {
				ret = -EOPNOTSUPP;
				break;
			}
			ret = touch_data->getModeAll(common_data.mode,
					common_data.data_buf);
			if (!ret)
				common_data.data_len = VALUE_TYPE_SIZE;
			break;
		case RESET_MODE:
			if (!touch_data->resetMode) {
				ret = -EOPNOTSUPP;
				break;
			}
			common_data.data_buf[0] =
				touch_data->resetMode(common_data.mode);
			common_data.data_len = 1;
			break;
		case SET_LONG_VALUE:
			if (!touch_data->setModeLongValue) {
				ret = -EOPNOTSUPP;
				break;
			}
			ret = touch_data->setModeLongValue(common_data.mode,
					common_data.data_len, common_data.data_buf);
			break;
		case SET_THP_IC_CUR_VALUE:
			if (!panel->hardware_operation.htc_ic_set_mode_value) {
				ret = -EOPNOTSUPP;
				break;
			}
			ret = panel->hardware_operation.htc_ic_set_mode_value(&common_data);
			break;
		case GET_THP_IC_CUR_VALUE:
			if (!panel->hardware_operation.htc_ic_get_mode_value) {
				ret = -EOPNOTSUPP;
				break;
			}
			ret = panel->hardware_operation.htc_ic_get_mode_value(&common_data);
			break;
		case SET_CMD_FOR_THP:
			add_common_data_to_buf(touch_id, SET_CUR_VALUE,
					       common_data.mode, common_data.data_len,
					       common_data.data_buf);
			break;
		case SET_CMD_FOR_DRIVER:
			if (common_data.mode == Touch_Palm_Sensor &&
			    common_data.data_len)
				update_palm_sensor_value(common_data.data_buf[0]);
			break;
		case GET_DUMP_PARAM:
			if (!touch_data->getModeValue) {
				ret = -EOPNOTSUPP;
				break;
			}
			common_data.data_buf[0] =
				touch_data->getModeValue(Touch_THP_Dump_Size, 0);
			common_data.data_len = 1;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		if (!ret && (common_data.mode == THP_HAL_INIT_READY ||
			     common_data.mode == Touch_Suspend))
			add_common_data_to_buf(touch_id, common_data.cmd,
					       common_data.mode, common_data.data_len,
					       common_data.data_buf);
		mutex_unlock(&touch_pdata->device->mutex);
		if (ret < 0)
			return ret;
		if (copy_to_user(argp, &common_data, sizeof(common_data)))
			return -EFAULT;
		return 0;

	case HARDWARE_PARAM_CMD:
		if (_IOC_SIZE(cmd) != sizeof(hardware_param_t))
			return -EINVAL;
		touch_id = client->touch_id;
		if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
			return -EINVAL;
		panel = &touch_panel_data[touch_id];
		if (!panel->registered)
			return -ENODEV;
		if (copy_to_user(argp, &panel->hardware_param,
				 sizeof(panel->hardware_param)))
			return -EFAULT;
		return 0;

	case SELECT_MMAP_CMD:
		if (arg != XIAOMI_TOUCH_MMAP_FRAME &&
		    arg != XIAOMI_TOUCH_MMAP_RAW)
			return -EINVAL;
		client->mmap_area = arg;
		return 0;

	case SELECT_TOUCH_ID:
		touch_id = arg;
		if (touch_id < 0 || touch_id >= XIAOMI_TOUCH_MAX_PANEL)
			return -EINVAL;
		panel = &touch_panel_data[touch_id];
		if (!panel->registered)
			return -ENODEV;
		if (client->touch_id >= 0)
			return client->touch_id == touch_id ? 0 : -EBUSY;

		client->touch_id = touch_id;
		atomic_set(&client->common_data_index,
			   atomic_read(&panel->common_data_index));
		atomic_set(&client->frame_data_index,
			   atomic_read(&panel->frame_data_index));
		atomic_set(&client->raw_data_index,
			   atomic_read(&panel->raw_data_index));
		spin_lock_irqsave(&panel->client_lock, flags);
		list_add_tail(&client->node, &panel->client_list);
		spin_unlock_irqrestore(&panel->client_lock, flags);
		return 0;

	case GET_FRAME_DATA_INDEX:
		touch_id = client->touch_id;
		if (touch_id < 0)
			return -EINVAL;
		panel = &touch_panel_data[touch_id];
		index = atomic_read(&client->frame_data_index);
		if (index == atomic_read(&panel->frame_data_index))
			return -1;
		ret = index;
		if (++index >= panel->frame_data_buf_size)
			index = 0;
		atomic_set(&client->frame_data_index, index);
		return ret;

	case RAW_DATA_INDEX:
		touch_id = client->touch_id;
		if (touch_id < 0)
			return -EINVAL;
		panel = &touch_panel_data[touch_id];
		if (_IOC_DIR(cmd) == _IOC_READ) {
			index = atomic_read(&client->raw_data_index);
			if (index == atomic_read(&panel->raw_data_index))
				return -EAGAIN;
			ret = index;
			if (++index >= panel->raw_data_buf_size)
				index = 0;
			atomic_set(&client->raw_data_index, index);
			return ret;
		}
		if (_IOC_DIR(cmd) == _IOC_WRITE) {
			smp_wmb();
			index = atomic_inc_return(&panel->raw_data_index);
			if (index >= panel->raw_data_buf_size)
				atomic_set(&panel->raw_data_index, 0);
			spin_lock_irqsave(&panel->client_lock, flags);
			list_for_each_entry(client, &panel->client_list, node)
				wake_up_all(&client->poll_wait_queue_head);
			spin_unlock_irqrestore(&panel->client_lock, flags);
			return 0;
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}
static int xiaomi_touch_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	phys_addr_t phys;
	size_t total;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (!client || client->touch_id < 0)
		return -EINVAL;

	panel = &touch_panel_data[client->touch_id];
	if (!panel->registered)
		return -ENODEV;

	switch (client->mmap_area) {
	case XIAOMI_TOUCH_MMAP_FRAME:
		phys = panel->frame_data_phys;
		total = panel->frame_data_size * panel->frame_data_buf_size;
		break;
	case XIAOMI_TOUCH_MMAP_RAW:
		phys = panel->raw_data_phys;
		total = panel->raw_data_size * panel->raw_data_buf_size;
		break;
	default:
		return -EINVAL;
	}

	if (offset > total || size > total - offset)
		return -EINVAL;

	if (remap_pfn_range(vma, vma->vm_start,
			    (phys + offset) >> PAGE_SHIFT, size,
			    PAGE_SHARED))
		return -EAGAIN;

	return 0;
}
static int xiaomi_touch_dev_release(struct inode *inode, struct file *file)
{
	struct xiaomi_touch_client *client = file->private_data;
	struct xiaomi_touch_panel_data *panel;
	unsigned long flags;

	if (!client)
		return 0;

	if (client->touch_id >= 0) {
		panel = &touch_panel_data[client->touch_id];
		spin_lock_irqsave(&panel->client_lock, flags);
		if (!list_empty(&client->node))
			list_del_init(&client->node);
		spin_unlock_irqrestore(&panel->client_lock, flags);
	}

	kfree(client);
	file->private_data = NULL;
	return 0;
}
static const struct file_operations xiaomitouch_dev_fops = {
	.owner = THIS_MODULE,
	.open = xiaomi_touch_dev_open,
	.read = xiaomi_touch_dev_read,
	.write = xiaomi_touch_dev_write,
	.poll = xiaomi_touch_dev_poll,
	.mmap = xiaomi_touch_dev_mmap,
	.unlocked_ioctl = xiaomi_touch_dev_ioctl,
	.compat_ioctl = xiaomi_touch_dev_ioctl,
	.release = xiaomi_touch_dev_release,
	.llseek	= no_llseek,
};
static struct xiaomi_touch xiaomi_touch_dev = {
	.misc_dev = {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "xiaomi-touch",
		.fops = &xiaomitouch_dev_fops,
		.parent = NULL,
	},
	.mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.mutex),
	.palm_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.palm_mutex),
	.prox_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.prox_mutex),
	.wait_queue = __WAIT_QUEUE_HEAD_INITIALIZER(xiaomi_touch_dev.wait_queue),
#ifdef	TOUCH_FOD_SUPPORT
	.fod_press_status_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.fod_press_status_mutex),
#endif
	.gesture_single_tap_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.gesture_single_tap_mutex),
	.abnormal_event_mutex = __MUTEX_INITIALIZER(xiaomi_touch_dev.abnormal_event_mutex),
};
struct xiaomi_touch *xiaomi_touch_dev_get(int minor)
{
	if (xiaomi_touch_dev.misc_dev.minor == minor)
		return &xiaomi_touch_dev;
	else
		return NULL;
}
struct class *get_xiaomi_touch_class(void)
{
	return xiaomi_touch_dev.class;
}
EXPORT_SYMBOL_GPL(get_xiaomi_touch_class);
struct device *get_xiaomi_touch_dev(void)
{
	return xiaomi_touch_dev.dev;
}
EXPORT_SYMBOL_GPL(get_xiaomi_touch_dev);
int xiaomitouch_register_modedata(int touchId, struct xiaomi_touch_interface *data)
{
	int ret = 0;
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata){
		mi_ts_info("touch_pdata is null\n");
		ret = -ENOMEM;
		return ret;
	}
	touch_data = touch_pdata->touch_data[touchId];
	mi_ts_info("enter\n");
	mutex_lock(&xiaomi_touch_dev.mutex);
	if (data->setModeValue)
		touch_data->setModeValue = data->setModeValue;
	if (data->getModeValue)
		touch_data->getModeValue = data->getModeValue;
	if (data->resetMode)
		touch_data->resetMode = data->resetMode;
	if (data->getModeAll)
		touch_data->getModeAll = data->getModeAll;
	if (data->palm_sensor_read)
		touch_data->palm_sensor_read = data->palm_sensor_read;
	if (data->palm_sensor_write)
		touch_data->palm_sensor_write = data->palm_sensor_write;
	if (data->prox_sensor_read)
		touch_data->prox_sensor_read = data->prox_sensor_read;
	if (data->prox_sensor_write)
		touch_data->prox_sensor_write = data->prox_sensor_write;
	if (data->panel_vendor_read)
		touch_data->panel_vendor_read = data->panel_vendor_read;
	if (data->panel_color_read)
		touch_data->panel_color_read = data->panel_color_read;
	if (data->panel_display_read)
		touch_data->panel_display_read = data->panel_display_read;
	if (data->touch_vendor_read)
		touch_data->touch_vendor_read = data->touch_vendor_read;
	if (data->setModeLongValue)
		touch_data->setModeLongValue = data->setModeLongValue;
	if (data->get_touch_rx_num)
		touch_data->get_touch_rx_num = data->get_touch_rx_num;
	if (data->get_touch_tx_num)
		touch_data->get_touch_tx_num = data->get_touch_tx_num;
	if (data->get_touch_freq_num)
		touch_data->get_touch_freq_num = data->get_touch_freq_num;
	if (data->get_touch_x_resolution)
		touch_data->get_touch_x_resolution = data->get_touch_x_resolution;
	if (data->get_touch_y_resolution)
		touch_data->get_touch_y_resolution = data->get_touch_y_resolution;
	if (data->enable_touch_raw)
		touch_data->enable_touch_raw = data->enable_touch_raw;
	if (data->enable_touch_delta)
		touch_data->enable_touch_delta = data->enable_touch_delta;
	if (data->enable_clicktouch_raw)
		touch_data->enable_clicktouch_raw = data->enable_clicktouch_raw;
	if (data->get_touch_super_resolution_factor)
		touch_data->get_touch_super_resolution_factor = data->get_touch_super_resolution_factor;
	if (data->set_up_interrupt_mode)
		touch_data->set_up_interrupt_mode = data->set_up_interrupt_mode;
	if (data->touch_doze_analysis)
		touch_data->touch_doze_analysis = data->touch_doze_analysis;
	if (data->get_touch_ic_buffer)
		touch_data->get_touch_ic_buffer = data->get_touch_ic_buffer;
	if (data->fod_test_store)
		touch_data->fod_test_store = data->fod_test_store;
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 start*/
	if (data->touch_dfs_test)
		touch_data->touch_dfs_test = data->touch_dfs_test;
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 end*/
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(xiaomitouch_register_modedata);
int update_palm_sensor_value(int value)
{
	struct xiaomi_touch *dev = NULL;
	mutex_lock(&xiaomi_touch_dev.palm_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.palm_mutex);
		return -ENODEV;
	}
	dev = touch_pdata->device;
	if (value != touch_pdata->palm_value) {
		mi_ts_info("value:%d\n", value);
		touch_pdata->palm_value = value;
		touch_pdata->palm_changed = true;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			     "palm_sensor");
	}
	mutex_unlock(&xiaomi_touch_dev.palm_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(update_palm_sensor_value);
static ssize_t palm_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	pdata->palm_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->palm_value);
}
static ssize_t palm_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	pdata->touch_data[0]->palm_sensor_onoff = input;
	if (pdata->touch_data[0]->palm_sensor_write)
		pdata->touch_data[0]->palm_sensor_write(!!input);
	else
		mi_ts_err("has not implement\n");
	mi_ts_info("value:%d\n", !!input);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "palm_sensor_data");
	return count;
}
static ssize_t palm_sensor_data_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	pdata->palm_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->palm_sensor_onoff);
}
static ssize_t palm_sensor_data_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	mutex_lock(&xiaomi_touch_dev.palm_mutex);
	if (sscanf(buf, "%d", &input) < 0) {
		mutex_unlock(&xiaomi_touch_dev.palm_mutex);
		return -EINVAL;
	}
	if (input != pdata->palm_value) {
		mi_ts_info("value:%d\n", input);
		pdata->palm_value = input;
		pdata->palm_changed = true;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			"palm_sensor");
	}
	mutex_unlock(&xiaomi_touch_dev.palm_mutex);
	return count;
}
static ssize_t touch_sensor_ctrl_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->touch_sensor_ctrl_value);
}
static ssize_t touch_sensor_ctrl_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		mi_ts_info("get input error\n");
		return -EINVAL;
	}
	pdata->touch_data[0]->touch_sensor_ctrl_value = input;
	mi_ts_info("touch sensor ctrl %d\n", input);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_sensor_ctrl");
	return count;
}
static ssize_t touch_sensor_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}
static ssize_t touch_sensor_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	if (input >= 0)
		thp_send_cmd_to_hal(THP_HAL_TOUCH_SENSOR, input);
	if (input == 0 && pdata->touch_data[0]->set_up_interrupt_mode)
		pdata->touch_data[0]->set_up_interrupt_mode(0);
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_testmode_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->thp_test_mode);
}
static ssize_t touch_testmode_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	pdata->touch_data[0]->thp_test_mode = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_testmode");
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_preset_point_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->thp_preset_point);
}
static ssize_t touch_testresult_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	pdata->touch_data[0]->thp_test_result = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_testresult");
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_testresult_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->thp_test_result);
}
static ssize_t touch_preset_point_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (sscanf(buf, "%d", &input) < 0) {
		return -EINVAL;
	}
	pdata->touch_data[0]->thp_preset_point = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_preset_point");
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t touch_thp_mem_notify_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}
static ssize_t touch_thp_mem_notify_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (!pdata)
		return -ENODEV;
	if (buf[0] == '1')
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_mem_notify");
	return count;
}
int update_prox_sensor_value(int value)
{
	struct xiaomi_touch *dev = NULL;
	mutex_lock(&xiaomi_touch_dev.prox_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.prox_mutex);
		return -ENODEV;
	}
	dev = touch_pdata->device;
	if (value != touch_pdata->prox_value) {
		mi_ts_info("value:%d\n", value);
		touch_pdata->prox_value = value;
		touch_pdata->prox_changed = true;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			     "prox_sensor");
	}
	mutex_unlock(&xiaomi_touch_dev.prox_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(update_prox_sensor_value);
static ssize_t prox_sensor_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	pdata->prox_changed = false;
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->prox_changed);
}
static ssize_t prox_sensor_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	if (pdata->touch_data[0]->prox_sensor_write)
		pdata->touch_data[0]->prox_sensor_write(!!input);
	else
		mi_ts_err("has not implement\n");
	mi_ts_info("value:%d\n", !!input);
	return count;
}
static ssize_t touch_ic_buffer_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	ssize_t count = 0;
	u8 *tmp_buf = NULL;
	if (pdata->touch_data[0]->get_touch_ic_buffer) {
		tmp_buf = pdata->touch_data[0]->get_touch_ic_buffer();
		count = snprintf(buf, PAGE_SIZE, "%s", tmp_buf);
	} else {
		mi_ts_info("get touch ic buffer wrong");
	}
	if (tmp_buf)
		vfree(tmp_buf);
	tmp_buf = NULL;
	return count;
}
static int touch_doze_analysis_result;

static ssize_t touch_doze_analysis_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", touch_doze_analysis_result);
}
static ssize_t touch_doze_analysis_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int input = 0, i;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(touch_panel_data); i++) {
		if (touch_panel_data[i].registered &&
		    touch_panel_data[i].hardware_operation.touch_doze_analysis)
			touch_doze_analysis_result =
				touch_panel_data[i].hardware_operation.touch_doze_analysis(input);
		else if (pdata->touch_data[i] && pdata->touch_data[i]->touch_doze_analysis)
			touch_doze_analysis_result = pdata->touch_data[i]->touch_doze_analysis(input);
	}
	mi_ts_info("value:%d\n", input);
	return count;
}
static ssize_t panel_vendor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->panel_vendor_read)
		return snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->panel_vendor_read());
	else
		return 0;
}
static ssize_t panel_color_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->panel_color_read)
		return snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->panel_color_read());
	else
		return 0;
}
static ssize_t panel_display_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->panel_display_read)
		return snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->panel_display_read());
	else
		return 0;
}
static ssize_t touch_vendor_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->touch_vendor_read)
		return snprintf(buf, PAGE_SIZE, "%c", pdata->touch_data[0]->touch_vendor_read());
	else
		return 0;
}
static ssize_t xiaomi_touch_tx_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->get_touch_tx_num)
		return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_tx_num());
	else
		return 0;
}
static ssize_t xiaomi_touch_rx_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->get_touch_rx_num)
		return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_rx_num());
	else
		return 0;
}
static ssize_t xiaomi_touch_freq_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->get_touch_freq_num)
		return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_freq_num());
	else
		return 0;
}
static ssize_t xiaomi_touch_x_resolution_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->get_touch_x_resolution)
		return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_x_resolution());
	else
		return 0;
}
static ssize_t xiaomi_touch_y_resolution_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (pdata->touch_data[0]->get_touch_y_resolution)
		return snprintf(buf, PAGE_SIZE, "%d\n", pdata->touch_data[0]->get_touch_y_resolution());
	else
		return 0;
}
int copy_touch_rawdata(char *raw_base,  int len)
{
	struct xiaomi_touch *dev = NULL;
	if (!touch_pdata)
		return -ENODEV;
	dev = touch_pdata->device;
#ifdef TOUCH_THP_SUPPORT
	memcpy((unsigned char *)touch_pdata->raw_buf[touch_pdata->raw_tail],
			(unsigned char *)raw_base, len);
	touch_pdata->raw_len = len;
	spin_lock(&touch_pdata->raw_lock);
	touch_pdata->raw_tail++;
	if (touch_pdata->raw_tail == RAW_BUF_NUM)
		touch_pdata->raw_tail = 0;
	spin_unlock(&touch_pdata->raw_lock);
#else
	// 使用共享内存touch_pdata->raw_data向touch tool apk传递数据
	if (!touch_pdata->raw_data)
		return -ENOMEM;
	memcpy((unsigned char *)touch_pdata->raw_data, (unsigned char *)raw_base, len);
#endif
	return 0;
}
EXPORT_SYMBOL_GPL(copy_touch_rawdata);
void get_current_timestamp(char* timebuf, int len)
{
	struct rtc_time tm;
	struct timespec64 tv = { 0 };
	/* android time */
	struct rtc_time tm_android;
	struct timespec64 tv_android = { 0 };
	ktime_get_real_ts64(&tv);
	tv_android = tv;
	rtc_time64_to_tm(tv.tv_sec, &tm);
	tv_android.tv_sec -= sys_tz.tz_minuteswest * 60;
	rtc_time64_to_tm(tv_android.tv_sec, &tm_android);
	if(len >= 24) {
		snprintf(timebuf, 24, "%04d-%02d-%02d-%02d-%02d-%02d.%03d",
			tm_android.tm_year + 1900,tm_android.tm_mon + 1,
			tm_android.tm_mday, tm_android.tm_hour,
			tm_android.tm_min, tm_android.tm_sec,
			(unsigned int)(tv_android.tv_nsec / 1000));
	} else {
		mi_ts_info("%02d-%02d-%02d %02d:%02d:%02d.%03d(android time)\n",
			tm_android.tm_year + 1900,tm_android.tm_mon + 1,
			tm_android.tm_mday, tm_android.tm_hour,
			tm_android.tm_min, tm_android.tm_sec,
			(unsigned int)(tv_android.tv_nsec / 1000));
	}
}
EXPORT_SYMBOL_GPL(get_current_timestamp);
int update_touch_rawdata(void)
{
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "update_rawdata");
	return 0;
}
EXPORT_SYMBOL_GPL(update_touch_rawdata);
static ssize_t enable_touchraw_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input = 0;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	if (touch_panel_data[0].registered &&
	    touch_panel_data[0].hardware_operation.enable_touch_raw)
		touch_panel_data[0].hardware_operation.enable_touch_raw(input);
	else if (touch_data->enable_touch_raw)
		touch_data->enable_touch_raw(!!input);
	touch_data->is_enable_touchraw = !!input;
	touch_pdata->raw_tail = 0;
	touch_pdata->raw_head = 0;
	return count;
}
static ssize_t enable_touchraw_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->is_enable_touchraw);
}
static ssize_t enable_touchdelta_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	if (touch_data->enable_touch_delta)
		touch_data->enable_touch_delta(!!input);
	touch_data->is_enable_touchdelta = !!input;
	return count;
}
static ssize_t enable_touchdelta_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->is_enable_touchdelta);
}
static ssize_t thp_cmd_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	mutex_lock(&dev->mutex);
	if (!touch_pdata) {
		mutex_unlock(&dev->mutex);
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_buf, touch_data->thp_cmd_size * sizeof(int));
	if (touch_pdata->param_head == touch_pdata->param_tail
		&& touch_pdata->param_flag == 0) {
		mi_ts_err("param buffer is empty!\n");
		mutex_unlock(&dev->mutex);
		return -EINVAL;
	}
	spin_lock(&touch_pdata->param_lock);
	memcpy(buf, touch_pdata->touch_cmd_data[touch_pdata->param_head]->param_buf,
		touch_pdata->touch_cmd_data[touch_pdata->param_head]->thp_cmd_size * sizeof(int));
	touch_pdata->param_head++;
	if (touch_pdata->param_head == PARAM_BUF_NUM)
		touch_pdata->param_head = 0;
	if (touch_pdata->param_head == touch_pdata->param_tail)
		touch_pdata->param_flag = 0;
	spin_unlock(&touch_pdata->param_lock);
	if (touch_pdata->param_head != touch_pdata->param_tail)
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_cmd");
#ifdef TOUCH_THP_SUPPORT
	touch_data->touch_event_status = 0;
	wake_up_all(&touch_data->wait_queue);
#endif
	mutex_unlock(&dev->mutex);
	return touch_data->thp_cmd_size * sizeof(int);
}
static ssize_t thp_cmd_status_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input[MAX_BUF_SIZE];
	const char *p = buf;
	bool new_data = false;
	int para_cnt = 0;
	int i = 0;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	memset(input, 0x00, sizeof(int) * MAX_BUF_SIZE);
	for (p = buf; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			input[i] = input[i] * 10 + (*p - '0');
			if (!new_data) {
				new_data = true;
				para_cnt++;
			}
		} else if (*p == ' ' || *p == ',') {
			if (new_data) {
				i++;
				new_data = false;
			}
		} else {
			break;
		}
	}
	mi_ts_info("size:%d, cmd:%d, %d, %d, %d\n", para_cnt, input[0], input[1], input[2], input[3]);
	if (sizeof(int) * para_cnt < MAX_BUF_SIZE) {
		for (i = 0; i < para_cnt; i++) {
			touch_data->thp_cmd_buf[i] = input[i];
		}
		touch_data->thp_cmd_size = para_cnt;
		touch_data->touch_event_status = 1;
		if (touch_pdata->param_head == touch_pdata->param_tail
			&& touch_pdata->param_flag == 1) {
			mi_ts_err("param buffer is full!\n");
			return -ENOMEM;
		}
		spin_lock(&touch_pdata->param_lock);
		touch_pdata->touch_cmd_data[touch_pdata->param_tail]->thp_cmd_size = touch_data->thp_cmd_size;
		memcpy((unsigned char*)touch_pdata->touch_cmd_data[touch_pdata->param_tail]->param_buf,
			(unsigned char*)touch_data->thp_cmd_buf, sizeof(int) * touch_data->thp_cmd_size);
		touch_pdata->param_tail++;
		if (touch_pdata->param_tail == PARAM_BUF_NUM)
			touch_pdata->param_tail = 0;
		if (touch_pdata->param_head == touch_pdata->param_tail)
			touch_pdata->param_flag = 1;
		spin_unlock(&touch_pdata->param_lock);
		sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd");
#ifdef TOUCH_THP_SUPPORT
		if (wait_event_interruptible_timeout(touch_data->wait_queue,
				!touch_data->touch_event_status, msecs_to_jiffies(100)) <= 0) {
			mi_ts_err("thp read timeout, skip this event, status:%d\n", touch_data->touch_event_status);
		}
#endif
	} else {
		mi_ts_info("memory overlow\n");
	}
	return count;
}
static ssize_t thp_cmd_ready_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	mutex_lock(&dev->mutex);
	if (!touch_pdata) {
		mutex_unlock(&dev->mutex);
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_ready_buf, touch_data->thp_cmd_ready_size * sizeof(int));
#if 0
	touch_data->touch_event_ready_status = 0;
	wake_up_all(&touch_data->wait_queue_ready);
#endif
	mutex_unlock(&dev->mutex);
	return touch_data->thp_cmd_ready_size * sizeof(int);
}
static ssize_t thp_cmd_ready_status_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input[MAX_BUF_SIZE];
	const char *p = buf;
	bool new_data = false;
	int para_cnt = 0;
	int i = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memset(input, 0x00, sizeof(int) * MAX_BUF_SIZE);
	for (p = buf; *p != '\0'; p++) {
		if (*p >= '0' && *p <= '9') {
			input[i] = input[i] * 10 + (*p - '0');
			if (!new_data) {
				new_data = true;
				para_cnt++;
			}
		} else if (*p == ' ' || *p == ',') {
			if (new_data) {
				i++;
				new_data = false;
			}
		} else {
			break;
		}
	}
	mi_ts_info("size:%d, cmd:%d, %d, %d, %d\n",
				para_cnt, input[0], input[1], input[2], input[3]);
	if (sizeof(int) * para_cnt < MAX_BUF_SIZE) {
		for (i = 0; i < para_cnt; i++) {
			touch_data->thp_cmd_ready_buf[i] = input[i];
		}
		touch_data->thp_cmd_ready_size = para_cnt;
		touch_data->touch_event_ready_status = 1;
		sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd_ready");
#if 0
		if (wait_event_interruptible_timeout(touch_data->wait_queue_ready,
			!touch_data->touch_event_ready_status, msecs_to_jiffies(100)) <= 0) {
			mi_ts_err("%s thp read timeout, skip this event, status:%d\n", __func__,
						touch_data->touch_event_ready_status);
		}
#endif
	} else {
		mi_ts_info("memory overlow\n");
	}
	return count;
}
static ssize_t thp_cmd_data_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	memcpy(buf, touch_data->thp_cmd_data_buf, touch_data->thp_cmd_data_size);
	return touch_data->thp_cmd_data_size;
}
static ssize_t thp_cmd_data_store(struct device *dev,
				  struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (count > MAX_BUF_SIZE) {
		mi_ts_info("memory out of range:%d\n", (int)count);
		return count;
	}
	mi_ts_info("buf:%s, size:%d\n", buf, (int)count);
	memcpy(touch_data->thp_cmd_data_buf, buf, count);
	touch_data->thp_cmd_data_size = count;
	sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd_data");
	return count;
}
static ssize_t touch_thp_ic_cmd_data_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	u16 data_len;
	int i;

	mutex_lock(&thp_ic_read_data_mutex);
	if (thp_ic_cmd_data_common_data.cmd != GET_THP_IC_CUR_VALUE)
		goto out;

	data_len = thp_ic_cmd_data_common_data.data_len;
	if (data_len > sizeof(thp_ic_cmd_data_common_data.data_buf))
		goto out;

	if (thp_ic_cmd_data_common_data.mode < SET_IDLE_THD) {
		memcpy(buf, thp_ic_cmd_data_common_data.data_buf, data_len);
		count = data_len;
	} else {
		for (i = 0; i < data_len && count < PAGE_SIZE; i++)
			count += sysfs_emit_at(buf, count, "%x",
				((u8 *)thp_ic_cmd_data_common_data.data_buf)[i]);
		if (count < PAGE_SIZE)
			count += sysfs_emit_at(buf, count, "\n");
	}
out:
	mutex_unlock(&thp_ic_read_data_mutex);
	return count;
}
static ssize_t touch_thp_ic_cmd_data_store(struct device *dev,
				  struct device_attribute *attr, const char *buf,
				  size_t count)
{
	if (count > sizeof(thp_ic_cmd_data_common_data)) {
		mi_ts_err("memory out of range:%zu\n", count);
		return count;
	}

	mutex_lock(&thp_ic_write_data_mutex);
	memcpy(&thp_ic_cmd_data_common_data, buf, count);
	mutex_unlock(&thp_ic_write_data_mutex);
	return count;
}
void thp_send_cmd_to_hal(int cmd, int value)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	touch_data = touch_pdata->touch_data[0];
	mi_ts_info("cmd: %d, value: %d", cmd, value);
	if (!touch_data)
		return;
	mutex_lock(&xiaomi_touch_dev.mutex);
	touch_data->thp_cmd_buf[0] = SET_CUR_VALUE;
	touch_data->thp_cmd_buf[1] = 0;
	touch_data->thp_cmd_buf[2] = cmd;
	touch_data->thp_cmd_buf[3] = value;
	touch_data->thp_cmd_size = 4;
	touch_data->touch_event_status = 1;
	if (touch_pdata->param_head == touch_pdata->param_tail && touch_pdata->param_flag == 1) {
		mi_ts_err("param buffer is full!\n");
		mutex_unlock(&xiaomi_touch_dev.mutex);
		return;
	}
	spin_lock(&touch_pdata->param_lock);
	touch_pdata->touch_cmd_data[touch_pdata->param_tail]->thp_cmd_size = touch_data->thp_cmd_size;
	memcpy((unsigned char*)touch_pdata->touch_cmd_data[touch_pdata->param_tail]->param_buf,
		(unsigned char*)touch_data->thp_cmd_buf, sizeof(int) * touch_data->thp_cmd_size);
	touch_pdata->param_tail++;
	if (touch_pdata->param_tail == PARAM_BUF_NUM)
		touch_pdata->param_tail = 0;
	if (touch_pdata->param_head == touch_pdata->param_tail)
		touch_pdata->param_flag = 1;
	spin_unlock(&touch_pdata->param_lock);
	sysfs_notify(&xiaomi_touch_device->dev->kobj, NULL, "touch_thp_cmd");
#ifdef TOUCH_THP_SUPPORT
	if (wait_event_interruptible_timeout(touch_data->wait_queue,
			!touch_data->touch_event_status, msecs_to_jiffies(100)) <= 0) {
		mi_ts_err("thp read timeout, skip this event, status:%d\n", touch_data->touch_event_status);
	}
#endif
	mutex_unlock(&xiaomi_touch_dev.mutex);
	return;
}
EXPORT_SYMBOL_GPL(thp_send_cmd_to_hal);
static ssize_t thp_downthreshold_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_downthreshold);
}
static ssize_t thp_downthreshold_store(struct device *dev,
				       struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_downthreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_downthd");
	return count;
}
static ssize_t thp_upthreshold_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_upthreshold);
}
static ssize_t thp_upthreshold_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_upthreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_upthd");
	return count;
}
static ssize_t thp_movethreshold_store(struct device *dev,
				       struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_movethreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_movethd");
	return count;
}
static ssize_t thp_movethreshold_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_movethreshold);
}
static ssize_t thp_islandthreshold_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_islandthreshold = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_islandthd");
	return count;
}
static ssize_t thp_islandthreshold_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_islandthreshold);
}
static ssize_t thp_noisefilter_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_noisefilter = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_noisefilter");
	return count;
}
static ssize_t thp_noisefilter_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_noisefilter);
}
static ssize_t thp_smooth_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_smooth = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_smooth");
	return count;
}
static ssize_t thp_smooth_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_smooth);
}
static ssize_t thp_dump_frame_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	touch_data->thp_dump_raw = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_thp_dump");
	return count;
}
static ssize_t thp_dump_frame_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_dump_raw);
}
static ssize_t thp_disconnect_result_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("value:%d\n", input);
	touch_data->thp_disconnect_result = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_disconnect_result");
	return count;
}
static ssize_t thp_disconnect_result_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_disconnect_result);
}
static ssize_t thp_disconnect_type_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (count > MAX_BUF_SIZE) {
		mi_ts_err("memory out of range:%d\n", (int)count);
		return count;
	}
	mi_ts_info("buf:%s, size:%d\n", buf, (int)count);
	memcpy(touch_data->thp_disconnect_type, buf, count);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_disconnect_type");
	return count;
}
static ssize_t thp_disconnect_type_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int len;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	len = strlen(touch_data->thp_disconnect_type);
	memcpy(buf, touch_data->thp_disconnect_type, len);
	return len;
}
static ssize_t thp_disconnect_detect_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("value:%d\n", input);
	touch_data->thp_disconnect_detect = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "touch_thp_disconnect_detect");
	return count;
}
static ssize_t thp_disconnect_detect_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->thp_disconnect_detect);
}
static ssize_t update_rawdata_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
#ifdef TOUCH_THP_SUPPORT
	int remaining = 0;
	if (!touch_pdata->raw_data)
		return -ENOMEM;
	if (touch_pdata->raw_head == touch_pdata->raw_tail)
		return snprintf(buf, PAGE_SIZE, "%s\n", "0");
	else {
		if (touch_pdata->raw_head < touch_pdata->raw_tail)
			remaining = touch_pdata->raw_tail - touch_pdata->raw_head;
		else
			remaining = RAW_BUF_NUM - touch_pdata->raw_head + touch_pdata->raw_tail;
		memcpy((unsigned char *)touch_pdata->raw_data, (unsigned char *)touch_pdata->raw_buf[touch_pdata->raw_head],
			touch_pdata->raw_len);
		spin_lock(&touch_pdata->raw_lock);
		touch_pdata->raw_head++;
		if (touch_pdata->raw_head == RAW_BUF_NUM)
			touch_pdata->raw_head = 0;
		spin_unlock(&touch_pdata->raw_lock);
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", remaining);
#else
	int len;
	char buf0[10] = "update";
	// mi_ts_info("%s in\n", __func__);
	len = strlen(buf0);
	memcpy(buf, buf0, len);
	return len;
#endif
}
static ssize_t update_rawdata_store(struct device *dev,
				    struct device_attribute *attr, const char *buf, size_t count)
{
	if (!touch_pdata->raw_data)
		return -ENOMEM;
	if (touch_pdata->raw_head != touch_pdata->raw_tail)
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "update_rawdata");
	// mi_ts_info("%s notify buf\n", __func__);
	return count;
}
static ssize_t enable_clicktouch_store(struct device *dev,
				       struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata)
		return -ENOMEM;
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	mi_ts_info("%d\n", input);
	if (touch_data->enable_clicktouch_raw)
		touch_data->enable_clicktouch_raw(input);
	return count;
}
static ssize_t enable_clicktouch_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", "1");
}
int update_clicktouch_raw(void)
{
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "clicktouch_raw");
	return 0;
}
EXPORT_SYMBOL_GPL(update_clicktouch_raw);
int xiaomi_touch_set_suspend_state(int state)
{
	if (!touch_pdata)
		return -ENODEV;
	touch_pdata->suspend_state = state;
#ifndef TOUCH_THP_SUPPORT
	add_common_data_to_buf(0, SET_CUR_VALUE, Touch_Suspend, 1, &(touch_pdata->suspend_state));
#endif
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "suspend_state");
	return 0;
}
EXPORT_SYMBOL_GPL(xiaomi_touch_set_suspend_state);
static ssize_t xiaomi_touch_suspend_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (!touch_pdata)
		return -ENODEV;
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_pdata->suspend_state);
}
#ifdef	TOUCH_FOD_SUPPORT
int update_fod_press_status(int value)
{
	struct xiaomi_touch *dev = NULL;
	mutex_lock(&xiaomi_touch_dev.fod_press_status_mutex);
	if (!touch_pdata) {
		mutex_unlock(&xiaomi_touch_dev.fod_press_status_mutex);
		return -ENODEV;
	}
	dev = touch_pdata->device;
	if (value != touch_pdata->fod_press_status_value) {
		mi_ts_info("value:%d\n", value);
		touch_pdata->fod_press_status_value = value;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			     "fod_press_status");
	}
	mutex_unlock(&xiaomi_touch_dev.fod_press_status_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(update_fod_press_status);
#endif
int notify_gesture_single_tap(void)
{
	mutex_lock(&xiaomi_touch_dev.gesture_single_tap_mutex);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
		     "gesture_single_tap_state");
	mutex_unlock(&xiaomi_touch_dev.gesture_single_tap_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(notify_gesture_single_tap);
static ssize_t gesture_single_tap_value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", 1);
}
#ifdef	TOUCH_FOD_SUPPORT
static ssize_t fod_press_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->fod_press_status_value);
}
#endif
void update_active_status(bool status)
{
	if (!touch_pdata || status == touch_pdata->touch_data[0]->active_status) {
		return;
	}
	touch_pdata->touch_data[0]->active_status = status;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			"touch_active_status");
	return;
}
EXPORT_SYMBOL_GPL(update_active_status);
static ssize_t touch_active_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->active_status);
}
void update_touch_irq_no(int irq_no)
{
	if (!touch_pdata) {
		return;
	}
	touch_pdata->touch_data[0]->irq_no = irq_no;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
			"touch_irq_no");
	return;
}
EXPORT_SYMBOL_GPL(update_touch_irq_no);
static ssize_t touch_finger_status_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	unsigned int input;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	if (sscanf(buf, "%d", &input) < 0)
			return -EINVAL;
	if (touch_data->finger_status == input)
		return count;
	touch_data->finger_status = input;
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,  "touch_finger_status");
	if (input != touch_pdata->touch_data[0]->active_status && input) {
		touch_pdata->touch_data[0]->active_status = input;
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL,
				"touch_active_status");
	}
	return count;
}
static ssize_t touch_finger_status_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->finger_status);
}
static ssize_t touch_irq_no_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	return snprintf(buf, PAGE_SIZE, "%d\n", touch_data->irq_no);
}
static ssize_t resolution_factor_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int factor = 1;
	if (!touch_pdata)
		return -ENODEV;
	if (touch_panel_data[0].registered) {
		factor = touch_panel_data[0].hardware_param.super_resolution_factor;
		mi_ts_info("enter resolution_factor_show factor %d", factor);
	} else if (touch_pdata->touch_data[0]->get_touch_super_resolution_factor){
		factor = touch_pdata->touch_data[0]->get_touch_super_resolution_factor();
		mi_ts_info("enter resolution_factor_show factor %d", factor);
	}
	return snprintf(buf, PAGE_SIZE, "%d", factor);
}
static ssize_t abnormal_event_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	int struct_abnormal_event_size = sizeof(struct abnormal_event);
	if (!pdata)
		return -ENODEV;
	mutex_lock(&pdata->device->abnormal_event_mutex);
	if (pdata->abnormal_event_buf.bottom == pdata->abnormal_event_buf.top &&
			!pdata->abnormal_event_buf.full_flag) {
		mi_ts_err("buf is empty\n");
		mutex_unlock(&pdata->device->abnormal_event_mutex);
		return -1;
	}
	memcpy(buf, &pdata->abnormal_event_buf.abnormal_event[pdata->abnormal_event_buf.bottom], struct_abnormal_event_size);
	pdata->abnormal_event_buf.bottom++;
	if (pdata->abnormal_event_buf.bottom >= SENSITIVE_EVENT_BUF_SIZE) {
		pdata->abnormal_event_buf.full_flag = false;
		pdata->abnormal_event_buf.bottom = 0;
	}
	if (pdata->abnormal_event_buf.top > pdata->abnormal_event_buf.bottom || pdata->abnormal_event_buf.full_flag)
		sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "abnormal_event");
	mutex_unlock(&pdata->device->abnormal_event_mutex);
	return struct_abnormal_event_size;
}
static ssize_t abnormal_event_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	struct abnormal_event *temp_event = (struct abnormal_event *)buf;
	int struct_abnormal_event_size = sizeof(struct abnormal_event);
	if (!pdata || count != struct_abnormal_event_size) {
		mi_ts_err("fail! pdata = %p, size = %zu, %d\n", (void *)pdata, count, struct_abnormal_event_size);
		return -ENODEV;
	}
	mutex_lock(&pdata->device->abnormal_event_mutex);
	memcpy(&pdata->abnormal_event_buf.abnormal_event[pdata->abnormal_event_buf.top], temp_event, struct_abnormal_event_size);
	pdata->abnormal_event_buf.top++;
	if (pdata->abnormal_event_buf.top >= SENSITIVE_EVENT_BUF_SIZE) {
		pdata->abnormal_event_buf.full_flag = true;
		pdata->abnormal_event_buf.top = 0;
	}
	mutex_unlock(&pdata->device->abnormal_event_mutex);
	sysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, "abnormal_event");
	return count;
}
static ssize_t fod_test_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int value = 0;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if ((!pdata) || (!pdata->touch_data[0])) {
		pr_err("%s touch_pdata is null!\n", __func__);
		return -ENODEV;
	}
	pr_info("%s,buf:%s,count:%zu\n", __func__, buf, count);
	if (kstrtoint(buf, FOD_TEST_BASE, &value))
		return -ENODEV;
	if (pdata->touch_data[0]->fod_test_store)
		pdata->touch_data[0]->fod_test_store(value);
	else {
		pr_err("%s has not implement\n", __func__);
	}
	pr_info("%s value:%d\n", __func__, value);
	return count;
}

static ssize_t fod_attn_test_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int value;
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (!panel->registered || !panel->hardware_operation.fod_attn_test)
		return -EOPNOTSUPP;
	if (kstrtoint(buf, 10, &value))
		return -EINVAL;

	panel->hardware_operation.fod_attn_test(value);
	return count;
}

static ssize_t fod_low_attn_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int value;
	struct xiaomi_touch_panel_data *panel = &touch_panel_data[0];

	if (!panel->registered || !panel->hardware_operation.fod_low_attn)
		return -EOPNOTSUPP;
	if (kstrtoint(buf, 10, &value))
		return -EINVAL;

	panel->hardware_operation.fod_low_attn(value);
	return count;
}
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 start*/
static ssize_t touch_dfs_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}
static ssize_t touch_dfs_test_store(struct device *dev,struct device_attribute *attr, const char *buf, size_t count)
{
	int input;
	struct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);
	if (sscanf(buf, "%d", &input) < 0)
		return -EINVAL;
	if (pdata->touch_data[0]->touch_dfs_test)
		pdata->touch_data[0]->touch_dfs_test(input);
	else {
		pr_err("%s has not touch_dfs_test\n", __func__);
	}
	pr_info("%s value:%d\n", __func__, input);
	return count;
}

#if IS_ENABLED(CONFIG_MIEV)
void xiaomi_touch_mievent_report_int(unsigned int code, int panel_id, const char *fault_name, const char *vendor_name, long error_code)
{
	struct misight_mievent *event;
#ifdef CONFIG_FACTORY_BUILD
	pr_info("factory build,return\n");
	return;
#endif
	pr_info("code:%d,fault_name:%s,panel_id:%d,vendor_name:%s,error_code:%ld\n", code, fault_name, panel_id, vendor_name, error_code);
	event = cdev_tevent_alloc(code);
	if (!event) {
		pr_err("misight event is error\n");
		return;
	}
	cdev_tevent_add_int(event, "panel_id", panel_id);
	cdev_tevent_add_str(event, "fault_name", fault_name);
	cdev_tevent_add_str(event, "vendor_name", vendor_name);
	cdev_tevent_add_int(event, "error_code", error_code);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_mievent_report_int);

void xiaomi_touch_mievent_report_str(unsigned int code, int panel_id, const char *fault_name, const char *vendor_name)
{
	struct misight_mievent *event;
#ifdef CONFIG_FACTORY_BUILD
	pr_info("factory build,return\n");
	return;
#endif
	pr_info("code:%d,fault_name:%s,panel_id:%d,vendor_name:%s\n", code, fault_name, panel_id, vendor_name);
	event = cdev_tevent_alloc(code);
	if (!event) {
		pr_err("misight event is error\n");
		return;
	}
	cdev_tevent_add_int(event, "panel_id", panel_id);
	cdev_tevent_add_str(event, "fault_name", fault_name);
	cdev_tevent_add_str(event, "vendor_name", vendor_name);
	cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}
EXPORT_SYMBOL_GPL(xiaomi_touch_mievent_report_str);
#endif
/*P16 code for HQFEAT-89149 by xiongdejun at 2024/4/25 end*/
static DEVICE_ATTR(abnormal_event, (S_IRUGO | S_IWUSR | S_IWGRP),
			abnormal_event_show, abnormal_event_store);
static DEVICE_ATTR(touch_thp_cmd_data, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_cmd_data_show, thp_cmd_data_store);
static DEVICE_ATTR(touch_thp_ic_cmd_data, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_thp_ic_cmd_data_show, touch_thp_ic_cmd_data_store);
static DEVICE_ATTR(touch_thp_cmd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_cmd_status_show, thp_cmd_status_store);
static DEVICE_ATTR(touch_thp_cmd_ready, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_cmd_ready_status_show, thp_cmd_ready_status_store);
static DEVICE_ATTR(touch_thp_islandthd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_islandthreshold_show, thp_islandthreshold_store);
static DEVICE_ATTR(touch_thp_downthd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_downthreshold_show, thp_downthreshold_store);
static DEVICE_ATTR(touch_thp_upthd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_upthreshold_show, thp_upthreshold_store);
static DEVICE_ATTR(touch_thp_movethd, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_movethreshold_show, thp_movethreshold_store);
static DEVICE_ATTR(touch_thp_smooth, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_smooth_show, thp_smooth_store);
static DEVICE_ATTR(touch_thp_dump, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_dump_frame_show, thp_dump_frame_store);
static DEVICE_ATTR(touch_thp_disconnect_detect, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_disconnect_detect_show, thp_disconnect_detect_store);
static DEVICE_ATTR(touch_thp_disconnect_result, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_disconnect_result_show, thp_disconnect_result_store);
static DEVICE_ATTR(touch_thp_disconnect_type, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_disconnect_type_show, thp_disconnect_type_store);
static DEVICE_ATTR(touch_thp_noisefilter, (S_IRUGO | S_IWUSR | S_IWGRP),
			thp_noisefilter_show, thp_noisefilter_store);
static DEVICE_ATTR(enable_touch_raw, (S_IRUGO | S_IWUSR | S_IWGRP),
			enable_touchraw_show, enable_touchraw_store);
static DEVICE_ATTR(enable_touch_delta, (S_IRUGO | S_IWUSR | S_IWGRP),
			enable_touchdelta_show, enable_touchdelta_store);
static DEVICE_ATTR(palm_sensor, (S_IRUGO | S_IWUSR | S_IWGRP),
			palm_sensor_show, palm_sensor_store);
static DEVICE_ATTR(palm_sensor_data, (S_IRUGO | S_IWUSR | S_IWGRP),
			palm_sensor_data_show, palm_sensor_data_store);
static DEVICE_ATTR(prox_sensor, (S_IRUGO | S_IWUSR | S_IWGRP),
			prox_sensor_show, prox_sensor_store);
static DEVICE_ATTR(touch_doze_analysis, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_doze_analysis_show, touch_doze_analysis_store);
static DEVICE_ATTR(clicktouch_raw, (S_IRUGO | S_IWUSR | S_IWGRP),
			enable_clicktouch_show, enable_clicktouch_store);
static DEVICE_ATTR(panel_vendor, (S_IRUGO), panel_vendor_show, NULL);
static DEVICE_ATTR(panel_color, (S_IRUGO), panel_color_show, NULL);
static DEVICE_ATTR(panel_display, (S_IRUGO), panel_display_show, NULL);
static DEVICE_ATTR(touch_vendor, (S_IRUGO), touch_vendor_show, NULL);
static DEVICE_ATTR(touch_thp_tx_num, (S_IRUGO), xiaomi_touch_tx_num_show, NULL);
static DEVICE_ATTR(touch_thp_rx_num, (S_IRUGO), xiaomi_touch_rx_num_show, NULL);
static DEVICE_ATTR(touch_thp_freq_num, (S_IRUGO), xiaomi_touch_freq_num_show, NULL);
static DEVICE_ATTR(touch_sensor, (S_IRUGO | S_IWUSR | S_IWGRP), touch_sensor_show, touch_sensor_store);
static DEVICE_ATTR(touch_sensor_ctrl, (S_IRUGO | S_IWUSR | S_IWGRP), touch_sensor_ctrl_show, touch_sensor_ctrl_store);
static DEVICE_ATTR(touch_thp_mem_notify, (S_IRUGO | S_IWUSR | S_IWGRP),
			touch_thp_mem_notify_show, touch_thp_mem_notify_store);
static DEVICE_ATTR(touch_thp_x_resolution, (S_IRUGO), xiaomi_touch_x_resolution_show, NULL);
static DEVICE_ATTR(touch_thp_y_resolution, (S_IRUGO), xiaomi_touch_y_resolution_show, NULL);
static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);
static DEVICE_ATTR(update_rawdata, (0664), update_rawdata_show,
			update_rawdata_store);
#ifdef	TOUCH_FOD_SUPPORT
static DEVICE_ATTR(fod_press_status, (S_IRUGO | S_IWUSR | S_IWGRP), fod_press_status_show, NULL);
#endif
static DEVICE_ATTR(gesture_single_tap_state, (S_IRUGO | S_IWUSR | S_IWGRP), gesture_single_tap_value_show, NULL);
static DEVICE_ATTR(touch_active_status, (S_IRUGO | S_IWUSR | S_IWGRP), touch_active_status_show, NULL);
static DEVICE_ATTR(touch_irq_no, (S_IRUGO | S_IWUSR | S_IWGRP), touch_irq_no_show, NULL);
static DEVICE_ATTR(touch_finger_status, (S_IRUGO | S_IWUSR | S_IWGRP), touch_finger_status_show, touch_finger_status_store);
static DEVICE_ATTR(touch_thp_testmode, (S_IRUGO | S_IWUSR | S_IWGRP), touch_testmode_show, touch_testmode_store);
static DEVICE_ATTR(touch_thp_testresult, (S_IRUGO | S_IWUSR | S_IWGRP), touch_testresult_show, touch_testresult_store);
static DEVICE_ATTR(touch_thp_preset_point, (S_IRUGO | S_IWUSR | S_IWGRP), touch_preset_point_show, touch_preset_point_store);
static DEVICE_ATTR(resolution_factor, 0644, resolution_factor_show, NULL);
static DEVICE_ATTR(touch_ic_buffer, (S_IRUGO | S_IWUSR | S_IWGRP),touch_ic_buffer_show, NULL);
static DEVICE_ATTR(fod_test, (S_IRUGO | S_IWUSR | S_IWGRP), NULL, fod_test_store);
static DEVICE_ATTR(fod_attn_test, (S_IRUGO | S_IWUSR | S_IWGRP), NULL,
			fod_attn_test_store);
static DEVICE_ATTR(fod_low_attn, (S_IRUGO | S_IWUSR | S_IWGRP), NULL,
			fod_low_attn_store);
static DEVICE_ATTR(touch_dfs_test, (0664), touch_dfs_test_show, touch_dfs_test_store);
static struct attribute *touch_attr_group[] = {
	&dev_attr_abnormal_event.attr,
	&dev_attr_enable_touch_raw.attr,
	&dev_attr_enable_touch_delta.attr,
	&dev_attr_touch_thp_cmd.attr,
	&dev_attr_touch_thp_cmd_data.attr,
	&dev_attr_touch_thp_ic_cmd_data.attr,
	&dev_attr_clicktouch_raw.attr,
	&dev_attr_touch_thp_tx_num.attr,
	&dev_attr_touch_thp_rx_num.attr,
	&dev_attr_touch_thp_freq_num.attr,
	&dev_attr_touch_thp_x_resolution.attr,
	&dev_attr_touch_thp_y_resolution.attr,
	&dev_attr_touch_thp_downthd.attr,
	&dev_attr_touch_thp_upthd.attr,
	&dev_attr_touch_thp_movethd.attr,
	&dev_attr_touch_thp_islandthd.attr,
	&dev_attr_touch_thp_smooth.attr,
	&dev_attr_touch_thp_dump.attr,
	&dev_attr_touch_thp_disconnect_detect.attr,
	&dev_attr_touch_thp_disconnect_result.attr,
	&dev_attr_touch_thp_disconnect_type.attr,
	&dev_attr_touch_thp_noisefilter.attr,
	&dev_attr_palm_sensor.attr,
	&dev_attr_palm_sensor_data.attr,
	&dev_attr_prox_sensor.attr,
	&dev_attr_panel_vendor.attr,
	&dev_attr_panel_color.attr,
	&dev_attr_panel_display.attr,
	&dev_attr_touch_vendor.attr,
	&dev_attr_update_rawdata.attr,
	&dev_attr_suspend_state.attr,
#ifdef	TOUCH_FOD_SUPPORT
	&dev_attr_fod_press_status.attr,
#endif
	&dev_attr_gesture_single_tap_state.attr,
	&dev_attr_touch_active_status.attr,
	&dev_attr_touch_irq_no.attr,
	&dev_attr_touch_finger_status.attr,
	&dev_attr_resolution_factor.attr,
	&dev_attr_fod_attn_test.attr,
	&dev_attr_fod_low_attn.attr,
	&dev_attr_touch_sensor.attr,
	&dev_attr_touch_sensor_ctrl.attr,
	&dev_attr_touch_thp_mem_notify.attr,
	&dev_attr_touch_thp_testmode.attr,
	&dev_attr_touch_thp_testresult.attr,
	&dev_attr_touch_thp_preset_point.attr,
	&dev_attr_touch_doze_analysis.attr,
	&dev_attr_touch_ic_buffer.attr,
	&dev_attr_touch_thp_cmd_ready.attr,
	&dev_attr_fod_test.attr,
	&dev_attr_touch_dfs_test.attr,
	NULL,
};
static void *event_start(struct seq_file *m, loff_t *p)
{
	int pos = 0;
	struct last_touch_event *event;
	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;
	event = touch_pdata->last_touch_events;
	if (*p >= LAST_TOUCH_EVENTS_MAX)
		return NULL;
	pos = (event->head + *p) & (LAST_TOUCH_EVENTS_MAX - 1);
	return event->touch_event_buf + pos;
}
static void *event_next(struct seq_file *m, void *v, loff_t *p)
{
	int pos = 0;
	struct last_touch_event *event;
	if (!touch_pdata || !touch_pdata->last_touch_events)
		return NULL;
	event = touch_pdata->last_touch_events;
	if (++*p >= LAST_TOUCH_EVENTS_MAX)
		return NULL;
	pos = (event->head + *p) & (LAST_TOUCH_EVENTS_MAX - 1);
	return event->touch_event_buf + pos;
}
static int32_t event_show(struct seq_file *m, void *v)
{
	struct touch_event *event_info;
	struct rtc_time tm;
	event_info = (struct touch_event *)v;
	if (event_info->state == EVENT_INIT)
		return 0;
	rtc_time64_to_tm(event_info->touch_time.tv_sec, &tm);
	seq_printf(m, "%d-%02d-%02d %02d:%02d:%02d.%09lu UTC Finger (%2d) %s\n",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec, event_info->touch_time.tv_nsec,
		   event_info->slot, event_info->state == EVENT_DOWN ? "P" : "R");
	return 0;
}
static void event_stop(struct seq_file *m, void *v)
{
}
const struct seq_operations last_touch_events_seq_ops = {
	.start = event_start,
	.next = event_next,
	.stop = event_stop,
	.show = event_show,
};
void last_touch_events_collect(int slot, int state)
{
	struct touch_event *event_info;
	struct last_touch_event *event;
	static int event_state[MAX_TOUCH_ID] = {0};
	if (!touch_pdata || !touch_pdata->last_touch_events || slot >= MAX_TOUCH_ID || event_state[slot] == state)
		return;
	event_state[slot] = state;
	event = touch_pdata->last_touch_events;
	event_info = &event->touch_event_buf[event->head];
	event_info->state = !!state ? EVENT_DOWN : EVENT_UP;
	event_info->slot = slot;
	ktime_get_real_ts64(&event_info->touch_time);
	event->head++;
	event->head &= LAST_TOUCH_EVENTS_MAX - 1;
}
EXPORT_SYMBOL_GPL(last_touch_events_collect);
static ssize_t tp_hal_version_read(struct file *file, char __user *buf,
			size_t count, loff_t *pos)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int cnt = 0;
	int ret = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	if (*pos != 0)
		return 0;
	touch_data = touch_pdata->touch_data[0];
	cnt = strlen(touch_data->tp_hal_version);
	if ((ret = copy_to_user(buf, touch_data->tp_hal_version, cnt))) {
		return -EFAULT;
	}
	*pos += cnt;
	if (ret != 0)
		return 0;
	else
		return cnt;
}
static ssize_t tp_hal_version_write(struct file *file, const char __user *buf,
			size_t count, loff_t *pos)
{
	struct xiaomi_touch_interface *touch_data = NULL;
	int retval = 0;
	if (!touch_pdata) {
		return -ENOMEM;
	}
	touch_data = touch_pdata->touch_data[0];
	memset(touch_data->tp_hal_version, 0x00, TP_VERSION_SIZE);
	if (count < TP_VERSION_SIZE && copy_from_user(touch_data->tp_hal_version, buf, count)) {
		retval = -EFAULT;
		goto out;
	}
out:
	if (retval >= 0)
		retval = count;
	return retval;
}
static const struct proc_ops tp_hal_version_ops = {
	.proc_read = tp_hal_version_read,
	.proc_write = tp_hal_version_write,
};
static const struct of_device_id xiaomi_touch_of_match[] = {
	{ .compatible = "xiaomi-touch", },
	{ },
};
static int xiaomi_touch_parse_dt(struct device *dev, struct xiaomi_touch_pdata *data)
{
	int ret;
	struct device_node *np;
	np = dev->of_node;
	if (!np)
		return -ENODEV;
	ret = of_property_read_string(np, "touch,name", &data->name);
	if (ret)
		return ret;
	mi_ts_info("touch,name:%s\n", data->name);
	return 0;
}
struct drm_panel *xiaomi_touch_check_panel(void)
{
	int i;
	int count;
	struct device_node *node;
	struct drm_panel *panel = NULL;
	if (!touch_pdata || !touch_pdata->of_node) {
		mi_ts_err("Invalid params");
		return NULL;
	}
	count = of_count_phandle_with_args(touch_pdata->of_node, "panel", NULL);
	if (count <= 0)
		return NULL;
	for (i = 0; i < count; i++) {
		node = of_parse_phandle(touch_pdata->of_node, "panel", i);
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (!IS_ERR(panel))
			break;
	}
	return panel;
}
EXPORT_SYMBOL_GPL(xiaomi_touch_check_panel);
static int xiaomi_touch_probe(struct platform_device *pdev)
{
	int ret = 0;
	int i = 0;
	struct device *dev = &pdev->dev;
	struct xiaomi_touch_pdata *pdata = NULL;
	mi_ts_info("enter\n");
	if (!dev || !dev->of_node) {
		mi_ts_err("Invalid touch device");
		return -ENODEV;
	}
	ret = knock_node_init();
	if (ret)
		goto parse_dt_err;
	pdata = devm_kzalloc(dev, sizeof(struct xiaomi_touch_pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;
	pdata->of_node = dev->of_node;
	pdata->raw_data = (unsigned int *)kzalloc(RAW_SIZE, GFP_KERNEL);
	if (!pdata->raw_data) {
		ret = -ENOMEM;
		mi_ts_err("fail alloc mem for raw data\n");
		goto parse_dt_err;
	}
	for (i = 0; i < PARAM_BUF_NUM; i++) {
		pdata->touch_cmd_data[i] = (struct touch_cmd_info *) kzalloc(sizeof(struct touch_cmd_info), GFP_KERNEL);
		if (!pdata->touch_cmd_data[i]) {
			ret = -ENOMEM;
			mi_ts_err("fail alloc mem for param buf data\n");
			goto alloc_param_data_err;
		}
	}
	pdata->param_head = 0;
	pdata->param_tail = 0;
	pdata->param_flag = 0;
	for (i = 0; i < RAW_BUF_NUM; i++) {
		pdata->raw_buf[i] = (unsigned int *)kzalloc(RAW_SIZE, GFP_KERNEL);
		if (!pdata->raw_buf[i]) {
			ret = -ENOMEM;
			mi_ts_err("fail alloc mem for raw buf data\n");
			goto parse_dt_err;
		}
	}
	pdata->raw_head = 0;
	pdata->raw_tail = 0;
	pdata->phy_base = virt_to_phys(pdata->raw_data);
	mi_ts_info("kernel base:%lud, phy base:%lud\n", (unsigned long)pdata->raw_data, (unsigned long)pdata->phy_base);
	spin_lock_init(&pdata->raw_lock);
	spin_lock_init(&pdata->param_lock);
	ret = xiaomi_touch_parse_dt(dev, pdata);
	if (ret < 0) {
		mi_ts_err("parse dt error:%d\n", ret);
		goto parse_dt_err;
	}
	ret = misc_register(&xiaomi_touch_dev.misc_dev);
	if (ret) {
		mi_ts_err("create misc device err:%d\n", ret);
		goto parse_dt_err;
	}
	xiaomi_touch_device = &xiaomi_touch_dev;
	if (!xiaomi_touch_dev.class)
		xiaomi_touch_dev.class = class_create("touch");
	if (!xiaomi_touch_dev.class) {
		mi_ts_err("create device class err\n");
		goto class_create_err;
	}
	xiaomi_touch_dev.dev = device_create(xiaomi_touch_dev.class, NULL, 'T', NULL, "touch_dev");
	if (!xiaomi_touch_dev.dev) {
		mi_ts_err("create device dev err\n");
		goto device_create_err;
	}
	pdata->touch_data[0] = (struct xiaomi_touch_interface *)kzalloc(sizeof(struct xiaomi_touch_interface), GFP_KERNEL);
	if (pdata->touch_data[0] == NULL) {
		ret = -ENOMEM;
		mi_ts_err("alloc mem for touch_data\n");
		goto data_mem_err;
	}
	pdata->touch_data[1] = (struct xiaomi_touch_interface *)kzalloc(sizeof(struct xiaomi_touch_interface), GFP_KERNEL);
	if (pdata->touch_data[1] == NULL) {
		ret = -ENOMEM;
		mi_ts_err("alloc mem for touch_data\n");
		goto sys_group_err;
	}
	pdata->last_touch_events = (struct last_touch_event *)kzalloc(sizeof(struct last_touch_event), GFP_KERNEL);
	if (pdata->last_touch_events == NULL) {
		ret = -ENOMEM;
		mi_ts_err("alloc mem for last touch evnets\n");
		goto sys_group_err;
	}
	pdata->device = &xiaomi_touch_dev;
	dev_set_drvdata(xiaomi_touch_dev.dev, pdata);
	init_waitqueue_head(&pdata->touch_data[0]->wait_queue);
	init_waitqueue_head(&pdata->touch_data[1]->wait_queue);
	init_waitqueue_head(&pdata->touch_data[0]->wait_queue_ready);
	init_waitqueue_head(&pdata->touch_data[1]->wait_queue_ready);
	init_waitqueue_head(&pdata->poll_wait_queue_head);
	INIT_LIST_HEAD(&pdata->touch_data[0]->private_data_list);
	INIT_LIST_HEAD(&pdata->touch_data[1]->private_data_list);
	spin_lock_init(&pdata->touch_data[0]->private_data_lock);
	spin_lock_init(&pdata->touch_data[1]->private_data_lock);
	mutex_init(&pdata->touch_data[0]->common_data_buf_lock);
	mutex_init(&pdata->touch_data[1]->common_data_buf_lock);
	touch_pdata = pdata;
	xiaomi_touch_dev.attrs.attrs = touch_attr_group;
	ret = sysfs_create_group(&xiaomi_touch_dev.dev->kobj, &xiaomi_touch_dev.attrs);
	if (ret) {
		mi_ts_err("ERROR: Cannot create sysfs structure!:%d\n", ret);
		ret = -ENODEV;
		goto sys_group_err;
	}
	pdata->last_touch_events_proc =
			proc_create_seq("last_touch_events", 0644, NULL, &last_touch_events_seq_ops);
	pdata->tp_hal_version_proc =
		proc_create("tp_hal_version", 0644, NULL, &tp_hal_version_ops);
#ifdef TOUCH_THP_SUPPORT
	mi_ts_err("disable thp by default");
	pdata->touch_data[0]->is_enable_touchraw = false;
#endif
	spin_lock(&touch_pdata->touch_data[0]->private_data_lock);
	list_add_tail_rcu(&touch_pdata->node, &touch_pdata->touch_data[0]->private_data_list);
	spin_unlock(&touch_pdata->touch_data[0]->private_data_lock);
	netlink_init();
	xiaomi_touch_probe_finished = true;
	mi_ts_info("over\n");
	return ret;
sys_group_err:
	if (pdata->touch_data[0]) {
		kfree(pdata->touch_data[0]);
		pdata->touch_data[0] = NULL;
	}
	if (pdata->touch_data[1]) {
		kfree(pdata->touch_data[1]);
		pdata->touch_data[1] = NULL;
	}
	if (pdata->last_touch_events) {
		kfree(pdata->last_touch_events);
		pdata->last_touch_events = NULL;
	}
data_mem_err:
	device_destroy(xiaomi_touch_dev.class, 'T');
device_create_err:
	class_destroy(xiaomi_touch_dev.class);
	xiaomi_touch_dev.class = NULL;
class_create_err:
	misc_deregister(&xiaomi_touch_dev.misc_dev);
	knock_node_release();
parse_dt_err:
	if (pdata->raw_data) {
		kfree(pdata->raw_data);
		pdata->raw_data = NULL;
	}
	for (i = 0; i < RAW_BUF_NUM; i++) {
		if (pdata->raw_buf[i]) {
			kfree(pdata->raw_buf[i]);
			pdata->raw_buf[i] = NULL;
		}
	}
alloc_param_data_err:
	for (i = 0; i < PARAM_BUF_NUM; i++) {
		if (pdata->touch_cmd_data[i]) {
			kfree(pdata->touch_cmd_data[i]);
			pdata->touch_cmd_data[i]= NULL;
		}
	}
	mi_ts_err("fail!\n");
	return ret;
}
static int xiaomi_touch_remove(struct platform_device *pdev)
{
	int i;
	xiaomi_touch_probe_finished = false;
	stop_temperature_detection_func();
	netlink_exit();
	sysfs_remove_group(&xiaomi_touch_dev.dev->kobj, &xiaomi_touch_dev.attrs);
	device_destroy(xiaomi_touch_dev.class, 'T');
	class_destroy(xiaomi_touch_dev.class);
	xiaomi_touch_dev.class = NULL;
	misc_deregister(&xiaomi_touch_dev.misc_dev);
	if (touch_pdata->raw_data) {
		kfree(touch_pdata->raw_data);
		touch_pdata->raw_data = NULL;
	}
	for (i = 0; i < RAW_BUF_NUM; i++) {
		if (touch_pdata->raw_buf[i]) {
			kfree(touch_pdata->raw_buf[i]);
			touch_pdata->raw_buf[i] = NULL;
		}
	}
	if (touch_pdata->last_touch_events) {
		kfree(touch_pdata->last_touch_events);
		touch_pdata->last_touch_events = NULL;
	}
	if (touch_pdata->last_touch_events_proc != NULL) {
		remove_proc_entry("last_touch_events", NULL);
		touch_pdata->last_touch_events_proc = NULL;
	}
	if (touch_pdata->tp_hal_version_proc != NULL) {
		remove_proc_entry("tp_hal_version", NULL);
		touch_pdata->tp_hal_version_proc = NULL;
	}
	if (touch_pdata->touch_data[0]) {
		kfree(touch_pdata->touch_data[0]);
		touch_pdata->touch_data[0] = NULL;
	}
	if (touch_pdata->touch_data[1]) {
		kfree(touch_pdata->touch_data[1]);
		touch_pdata->touch_data[1] = NULL;
	}
	knock_node_release();
	return 0;
}
static struct platform_driver xiaomi_touch_device_driver = {
	.probe		= xiaomi_touch_probe,
	.remove		= xiaomi_touch_remove,
	.driver		= {
		.name	= "xiaomi-touch",
		.of_match_table = of_match_ptr(xiaomi_touch_of_match),
	}
};
static int __init xiaomi_touch_init(void)
{
	return platform_driver_register(&xiaomi_touch_device_driver);
}
static void __exit xiaomi_touch_exit(void)
{
	platform_driver_unregister(&xiaomi_touch_device_driver);
}

MODULE_LICENSE("GPL");
module_init(xiaomi_touch_init);
module_exit(xiaomi_touch_exit);
