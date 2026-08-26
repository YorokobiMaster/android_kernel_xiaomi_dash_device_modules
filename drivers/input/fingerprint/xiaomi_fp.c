// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 YorokobiMaster (GitHub: @YorokobiMaster)
 *
 * Independent reimplementation of functionality present in Xiaomi
 * stock software.
 *
 * This implementation does not claim ownership of the original vendor
 * design, interfaces, protocols, firmware, or other third-party
 * intellectual property. Such rights remain with their respective owners.
 */

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netlink.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <net/sock.h>

#include "../../gpu/drm/mediatek/mediatek_v2/mi_disp/mi_disp_notifier.h"

#define FP_NAME                 "xiaomi-fp"
#define FP_CLASS_NAME           "xiaomi_fp"
#define FP_ID_NAME              "mifp_id"
#define FP_IPC_NAME             "qbt_ipc"
#define FP_MINOR_MAIN           0
#define FP_MINOR_ID             1
#define FP_MINOR_IPC            2
#define FP_MINOR_COUNT          3
#define FP_POWER_NAME_LEN       10
#define FP_IOC_MAGIC            0x67
#define FP_EVENT_IRQ            1
#define FP_EVENT_SCREEN_OFF     2
#define FP_EVENT_SCREEN_ON      3
#define FP_EVENT_IPC            3
#define FP_EVENT_FIFO_SIZE      32

/* Exact command encodings recovered from the stock GPL module. */
#define FP_IOC_INIT                     _IOR(FP_IOC_MAGIC, 0, __u8)
#define FP_IOC_EXIT                     _IO(FP_IOC_MAGIC, 1)
#define FP_IOC_RESET                    _IO(FP_IOC_MAGIC, 2)
#define FP_IOC_ENABLE_IRQ               _IO(FP_IOC_MAGIC, 3)
#define FP_IOC_DISABLE_IRQ              _IO(FP_IOC_MAGIC, 4)
#define FP_IOC_ENABLE_SPI_CLK_ALT       _IOW(FP_IOC_MAGIC, 5, __u32)
#define FP_IOC_DISABLE_SPI_CLK          _IO(FP_IOC_MAGIC, 6)
#define FP_IOC_ENABLE_POWER             _IOW(FP_IOC_MAGIC, 7, __u8)
#define FP_IOC_DISABLE_POWER            _IOW(FP_IOC_MAGIC, 8, __u8)
#define FP_IOC_INPUT_KEY_EVENT          _IOW(FP_IOC_MAGIC, 9, __u64)
#define FP_IOC_GET_FW_INFO              _IOR(FP_IOC_MAGIC, 11, __u8)
#define FP_IOC_ENTER_SLEEP_MODE         _IO(FP_IOC_MAGIC, 12)
#define FP_IOC_CHIP_INFO                _IOW(FP_IOC_MAGIC, 13, __u64)
#define FP_IOC_ENABLE_SPI_CLK           _IO(FP_IOC_MAGIC, 16)
#define FP_IOC_DISABLE_SPI_CLK_TA       _IO(FP_IOC_MAGIC, 17)
#define FP_IOC_ENABLE_IPC               _IO(FP_IOC_MAGIC, 22)
#define FP_IOC_DISABLE_IPC              _IO(FP_IOC_MAGIC, 23)
#define FP_IOC_ACQUIRE_WAKELOCK         _IO(FP_IOC_MAGIC, 27)
#define FP_IOC_RELEASE_WAKELOCK         _IO(FP_IOC_MAGIC, 28)
#define FP_IOC_REQUEST_IPC              _IO(FP_IOC_MAGIC, 32)
#define FP_IOC_FREE_IPC                 _IO(FP_IOC_MAGIC, 33)
#define FP_IOC_DISABLE_INTR2            _IO(FP_IOC_MAGIC, 36)
#define FP_IOC_REQUEST_RESOURCE         _IO(FP_IOC_MAGIC, 37)
#define FP_IOC_RELEASE_RESOURCE         _IO(FP_IOC_MAGIC, 38)
#define FP_IOC_REQUEST_INTR2            _IO(FP_IOC_MAGIC, 39)
#define FP_IOC_DEV_INFO                 _IOR(FP_IOC_MAGIC, 40, __u8)
#define FP_IOC_RESET_OUT_LOW            _IO(FP_IOC_MAGIC, 41)
#define FP_IOC_RESET_TIME_MS            _IOW(FP_IOC_MAGIC, 44, __u32)
#define FP_IOC_INTR2_SIDE_CONFIG        _IOR(FP_IOC_MAGIC, 46, __u32)
#define FP_IOC_SENSOR_LOC               _IOR(FP_IOC_MAGIC, 47, __u32)

#define FP_INTR2_NONE           0
#define FP_INTR2_AP             1
#define FP_INTR2_TIC            2

extern void spi_enable_fingerprint_clk(void);
extern void spi_disable_fingerprint_clk(void);

struct fp_key_event {
	__u32 key;
	__u32 value;
};

struct fp_chip_info {
	__u8 vendor_id;
	__u8 mode;
	__u8 operation;
	__u8 reserved[5];
};

struct fp_device {
	struct device *dev;
	struct class *class;
	struct cdev main_cdev;
	struct cdev id_cdev;
	struct cdev ipc_cdev;
	dev_t devt;
	struct device *main_device;
	struct device *id_device;
	struct device *ipc_device;
	struct input_dev *input;
	struct notifier_block display_notifier;
	struct sock *netlink;
	struct wakeup_source *wakeup;
	struct regulator *vreg_3v3;
	struct regulator *vreg_1v8;
	struct pinctrl *pinctrl;
	struct pinctrl_state *spi_gpio;
	struct pinctrl_state *spi_mode;
	struct pinctrl_state *eint_default;
	struct pinctrl_state *eint_pulldown;
	struct pinctrl_state *reset_high;
	struct pinctrl_state *reset_low;
	struct pinctrl_state *intr2_high;
	struct pinctrl_state *intr2_low;
	struct mutex lock;
	spinlock_t fifo_lock;
	DECLARE_KFIFO(events, __u32, FP_EVENT_FIFO_SIZE);
	wait_queue_head_t event_wait;
	char vendor[30];
	__u32 sensor_loc[5];
	__u32 intr2_electrical[5];
	__u32 reset_high_ms;
	__u32 reset_low_ms;
	__u32 intr2_side;
	__u32 netlink_family;
	__u32 netlink_pid;
	__u32 fw_info;
	int irq_gpio;
	int ipc_gpio;
	int power_gpio;
	int irq;
	int ipc_irq;
	int clk_users;
	int wake_users;
	bool irq_requested;
	bool irq_enabled;
	bool ipc_requested;
	bool ipc_enabled;
	bool vreg_3v3_enabled;
	bool vreg_1v8_enabled;
	bool power_gpio_enabled;
	bool intr2_output;
	bool fingerdown;
};

static struct fp_device *fp_dev;

static struct pinctrl_state *fp_lookup_state(struct fp_device *fp,
					     const char *name)
{
	struct pinctrl_state *state = pinctrl_lookup_state(fp->pinctrl, name);

	return IS_ERR(state) ? NULL : state;
}

static void fp_queue_event(struct fp_device *fp, __u32 event)
{
	unsigned long flags;

	spin_lock_irqsave(&fp->fifo_lock, flags);
	if (kfifo_is_full(&fp->events))
		kfifo_skip(&fp->events);
	kfifo_in(&fp->events, &event, 1);
	spin_unlock_irqrestore(&fp->fifo_lock, flags);
	wake_up_interruptible(&fp->event_wait);
}

static void fp_netlink_send(struct fp_device *fp, __u8 event)
{
	struct nlmsghdr *nlh;
	struct sk_buff *skb;

	if (!fp->netlink || !fp->netlink_pid)
		return;

	skb = nlmsg_new(1, GFP_ATOMIC);
	if (!skb)
		return;
	nlh = nlmsg_put(skb, 0, 0, 0, 1, 0);
	if (!nlh) {
		kfree_skb(skb);
		return;
	}
	*(__u8 *)nlmsg_data(nlh) = event;
	netlink_unicast(fp->netlink, skb, fp->netlink_pid, MSG_DONTWAIT);
}

static void fp_netlink_recv(struct sk_buff *skb)
{
	struct nlmsghdr *nlh;

	if (!fp_dev || skb->len < NLMSG_HDRLEN)
		return;
	nlh = nlmsg_hdr(skb);
	fp_dev->netlink_pid = nlh->nlmsg_pid;
}

static int fp_netlink_init(struct fp_device *fp)
{
	struct netlink_kernel_cfg cfg = { .input = fp_netlink_recv };

	if (fp->netlink)
		return 0;
	if (!fp->netlink_family)
		return -EINVAL;
	fp->netlink = netlink_kernel_create(&init_net, fp->netlink_family, &cfg);
	return fp->netlink ? 0 : -ENOMEM;
}

static void fp_netlink_destroy(struct fp_device *fp)
{
	if (fp->netlink)
		netlink_kernel_release(fp->netlink);
	fp->netlink = NULL;
	fp->netlink_pid = 0;
}

static void fp_select_active_pins(struct fp_device *fp)
{
	if (fp->reset_high)
		pinctrl_select_state(fp->pinctrl, fp->reset_high);
	if (fp->spi_mode)
		pinctrl_select_state(fp->pinctrl, fp->spi_mode);
	if (fp->eint_default)
		pinctrl_select_state(fp->pinctrl, fp->eint_default);
}

static void fp_select_inactive_pins(struct fp_device *fp)
{
	if (fp->spi_gpio)
		pinctrl_select_state(fp->pinctrl, fp->spi_gpio);
	if (fp->reset_low)
		pinctrl_select_state(fp->pinctrl, fp->reset_low);
	if (fp->eint_pulldown)
		pinctrl_select_state(fp->pinctrl, fp->eint_pulldown);
}

static int fp_set_power(struct fp_device *fp, const char *name, bool enable)
{
	struct regulator *regulator;
	bool *enabled;
	int ret;

	if (!strcmp(name, "vreg3v3")) {
		regulator = fp->vreg_3v3;
		enabled = &fp->vreg_3v3_enabled;
	} else if (!strcmp(name, "vreg1v8")) {
		regulator = fp->vreg_1v8;
		enabled = &fp->vreg_1v8_enabled;
	} else if (!strcmp(name, "pwr_gpio")) {
		if (!gpio_is_valid(fp->power_gpio))
			return -ENODEV;
		if (fp->power_gpio_enabled == enable)
			return 0;
		ret = gpiod_direction_output_raw(gpio_to_desc(fp->power_gpio),
						 enable);
		if (!ret) {
			fp->power_gpio_enabled = enable;
			usleep_range(1000, 1100);
		}
		return ret;
	} else {
		return -EINVAL;
	}

	if (!regulator)
		return -ENODEV;
	if (*enabled == enable)
		return 0;
	ret = enable ? regulator_enable(regulator) : regulator_disable(regulator);
	if (!ret) {
		*enabled = enable;
		usleep_range(1000, 1100);
	}
	return ret;
}

static void fp_reset(struct fp_device *fp, unsigned int low_ms,
			     unsigned int high_ms)
{
	if (fp->reset_low)
		pinctrl_select_state(fp->pinctrl, fp->reset_low);
	if (low_ms)
		msleep(low_ms);
	if (fp->reset_high)
		pinctrl_select_state(fp->pinctrl, fp->reset_high);
	if (high_ms)
		msleep(high_ms);
}

static void fp_clk_enable(struct fp_device *fp)
{
	if (!fp->clk_users++)
		spi_enable_fingerprint_clk();
}

static void fp_clk_disable(struct fp_device *fp)
{
	if (fp->clk_users > 0 && !--fp->clk_users)
		spi_disable_fingerprint_clk();
}

static irqreturn_t fp_irq_thread(int irq, void *data)
{
	struct fp_device *fp = data;

	pm_wakeup_ws_event(fp->wakeup, 2000, false);
	fp_netlink_send(fp, FP_EVENT_IRQ);
	return IRQ_HANDLED;
}

static irqreturn_t fp_ipc_irq_thread(int irq, void *data)
{
	struct fp_device *fp = data;

	pm_stay_awake(fp->dev);
	fp_queue_event(fp, FP_EVENT_IPC);
	pm_relax(fp->dev);
	return IRQ_HANDLED;
}

static int fp_request_main_irq(struct fp_device *fp)
{
	int ret;

	if (fp->irq_requested)
		return 0;
	ret = gpiod_direction_input(gpio_to_desc(fp->irq_gpio));
	if (ret)
		return ret;
	ret = request_threaded_irq(fp->irq, NULL, fp_irq_thread,
				   IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				   "xiaomi_fp_irq", fp);
	if (ret)
		return ret;
	fp->irq_requested = true;
	fp->irq_enabled = true;
	return 0;
}

static void fp_free_main_irq(struct fp_device *fp)
{
	if (!fp->irq_requested)
		return;
	free_irq(fp->irq, fp);
	fp->irq_requested = false;
	fp->irq_enabled = false;
}

static int fp_request_ipc_irq(struct fp_device *fp)
{
	int ret;

	if (fp->ipc_requested)
		return 0;
	ret = gpiod_direction_input(gpio_to_desc(fp->ipc_gpio));
	if (ret)
		return ret;
	ret = request_threaded_irq(fp->ipc_irq, NULL, fp_ipc_irq_thread,
				   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "qbt_ipc", fp);
	if (ret)
		return ret;
	fp->ipc_requested = true;
	fp->ipc_enabled = true;
	return 0;
}

static void fp_free_ipc_irq(struct fp_device *fp)
{
	if (!fp->ipc_requested)
		return;
	free_irq(fp->ipc_irq, fp);
	fp->ipc_requested = false;
	fp->ipc_enabled = false;
}

static int fp_display_notifier(struct notifier_block *nb,
			       unsigned long event, void *data)
{
	struct fp_device *fp = container_of(nb, struct fp_device,
					   display_notifier);
	struct mi_disp_notifier *notify = data;
	int blank;

	if (!notify || notify->disp_id != MI_DISPLAY_PRIMARY || !notify->data)
		return NOTIFY_DONE;
	blank = *(int *)notify->data;
	if (event == MI_DISP_DPMS_EARLY_EVENT &&
	    (blank == MI_DISP_DPMS_POWERDOWN || blank == MI_DISP_DPMS_OFF))
		fp_netlink_send(fp, FP_EVENT_SCREEN_OFF);
	else if (event == MI_DISP_DPMS_EVENT && blank == MI_DISP_DPMS_ON)
		fp_netlink_send(fp, FP_EVENT_SCREEN_ON);
	return NOTIFY_OK;
}

static int fp_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);

	if (!fp_dev || minor >= FP_MINOR_COUNT)
		return -EINVAL;
	file->private_data = fp_dev;
	return nonseekable_open(inode, file);
}

static int fp_release(struct inode *inode, struct file *file)
{
	struct fp_device *fp = file->private_data;
	unsigned int minor = iminor(inode);

	if (!fp)
		return -EINVAL;
	if (minor == FP_MINOR_MAIN)
		fp_free_main_irq(fp);
	else if (minor == FP_MINOR_IPC) {
		fp_free_ipc_irq(fp);
		if (fp->wake_users > 0) {
			fp->wake_users = 0;
			pm_relax(fp->dev);
		}
	}
	return 0;
}

static ssize_t fp_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	struct fp_device *fp = file->private_data;
	unsigned long flags;
	__u32 event;
	int ret;

	if (iminor(file_inode(file)) != FP_MINOR_IPC)
		return -EINVAL;
	if (count < sizeof(event))
		return -EINVAL;
	if (kfifo_is_empty(&fp->events)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(fp->event_wait,
					       !kfifo_is_empty(&fp->events));
		if (ret)
			return ret;
	}
	spin_lock_irqsave(&fp->fifo_lock, flags);
	ret = kfifo_out(&fp->events, &event, 1);
	spin_unlock_irqrestore(&fp->fifo_lock, flags);
	if (ret != 1)
		return -EIO;
	/* The stock qbt_ipc ABI returns copy_to_user()'s status, not a byte count. */
	return copy_to_user(buf, &event, sizeof(event)) ? -EFAULT : 0;
}

static __poll_t fp_poll(struct file *file, poll_table *wait)
{
	struct fp_device *fp = file->private_data;

	if (iminor(file_inode(file)) != FP_MINOR_IPC)
		return -EINVAL;
	poll_wait(file, &fp->event_wait, wait);
	return kfifo_is_empty(&fp->events) ? 0 : EPOLLIN | EPOLLRDNORM;
}

static long fp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fp_device *fp = file->private_data;
	void __user *up = (void __user *)arg;
	struct fp_chip_info chip_info;
	struct fp_key_event key;
	char power_name[FP_POWER_NAME_LEN + 1] = {};
	__u32 reset_time[2];
	__u32 value32;
	__u8 value8 = 0;
	unsigned int key_code;
	int ret = 0;

	if (!fp || _IOC_TYPE(cmd) != FP_IOC_MAGIC)
		return -EINVAL;

	mutex_lock(&fp->lock);
	switch (cmd) {
	case FP_IOC_INIT:
		ret = fp_netlink_init(fp);
		value8 = fp->netlink_family;
		if (!ret && copy_to_user(up, &value8, sizeof(value8)))
			ret = -EFAULT;
		break;
	case FP_IOC_EXIT:
		if (fp->irq_requested && fp->irq_enabled) {
			disable_irq(fp->irq);
			fp->irq_enabled = false;
		}
		fp_netlink_destroy(fp);
		break;
	case FP_IOC_RESET:
		fp_reset(fp, fp->reset_low_ms, fp->reset_high_ms);
		break;
	case FP_IOC_ENABLE_IRQ:
		if (fp->irq_requested && !fp->irq_enabled) {
			enable_irq(fp->irq);
			fp->irq_enabled = true;
		}
		break;
	case FP_IOC_DISABLE_IRQ:
		if (fp->irq_requested && fp->irq_enabled) {
			disable_irq(fp->irq);
			fp->irq_enabled = false;
		}
		break;
	case FP_IOC_ENABLE_SPI_CLK_ALT:
	case FP_IOC_ENABLE_SPI_CLK:
		fp_clk_enable(fp);
		break;
	case FP_IOC_DISABLE_SPI_CLK:
	case FP_IOC_DISABLE_SPI_CLK_TA:
		fp_clk_disable(fp);
		break;
	case FP_IOC_ENABLE_POWER:
		if (copy_from_user(power_name, up, FP_POWER_NAME_LEN)) {
			ret = -EFAULT;
			break;
		}
		ret = fp_set_power(fp, power_name, true);
		if (!ret) {
			msleep(10);
			fp_select_active_pins(fp);
		}
		break;
	case FP_IOC_DISABLE_POWER:
		if (copy_from_user(power_name, up, FP_POWER_NAME_LEN)) {
			ret = -EFAULT;
			break;
		}
		fp_select_inactive_pins(fp);
		ret = fp_set_power(fp, power_name, false);
		break;
	case FP_IOC_RESET_TIME_MS:
		if (copy_from_user(reset_time, up, sizeof(reset_time)))
			ret = -EFAULT;
		else if (!reset_time[0] || !reset_time[1])
			ret = -EINVAL;
		else
			fp_reset(fp, reset_time[0], reset_time[1]);
		break;
	case FP_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&key, up, sizeof(key))) {
			ret = -EFAULT;
			break;
		}
		switch (key.key) {
		case 1:
			key_code = KEY_HOME;
			break;
		case 2:
			key_code = KEY_POWER;
			break;
		case 5:
			key_code = KEY_CAMERA;
			break;
		case 6:
			key_code = BTN_C;
			break;
		default:
			key_code = key.key;
			break;
		}
		if ((key.key == 2 || key.key == 5) && key.value == 1) {
			input_report_key(fp->input, key_code, 1);
			input_sync(fp->input);
			input_report_key(fp->input, key_code, 0);
			input_sync(fp->input);
		} else if (key.key == 6) {
			input_report_key(fp->input, key_code, !!key.value);
			input_sync(fp->input);
		}
		break;
	case FP_IOC_RESET_OUT_LOW:
		if (fp->reset_low)
			ret = pinctrl_select_state(fp->pinctrl, fp->reset_low);
		break;
	case FP_IOC_GET_FW_INFO:
		if (copy_to_user(up, &value8, sizeof(value8)))
			ret = -EFAULT;
		break;
	case FP_IOC_ENTER_SLEEP_MODE:
		break;
	case FP_IOC_CHIP_INFO:
		if (copy_from_user(&chip_info, up, sizeof(chip_info)))
			ret = -EFAULT;
		break;
	case FP_IOC_ENABLE_IPC:
		if (!fp->ipc_requested) {
			ret = -EINVAL;
		} else if (!fp->ipc_enabled) {
			enable_irq(fp->ipc_irq);
			fp->ipc_enabled = true;
		}
		break;
	case FP_IOC_DISABLE_IPC:
		if (fp->ipc_requested && fp->ipc_enabled) {
			disable_irq(fp->ipc_irq);
			fp->ipc_enabled = false;
		}
		break;
	case FP_IOC_ACQUIRE_WAKELOCK:
		if (!fp->wake_users++)
			pm_stay_awake(fp->dev);
		break;
	case FP_IOC_RELEASE_WAKELOCK:
		if (fp->wake_users > 0 && !--fp->wake_users)
			pm_relax(fp->dev);
		break;
	case FP_IOC_REQUEST_IPC:
		ret = fp_request_ipc_irq(fp);
		if (!ret && fp->ipc_enabled) {
			disable_irq(fp->ipc_irq);
			fp->ipc_enabled = false;
		}
		break;
	case FP_IOC_FREE_IPC:
		fp_free_ipc_irq(fp);
		break;
	case FP_IOC_DISABLE_INTR2:
		if (fp->intr2_side == FP_INTR2_AP && fp->intr2_low)
			ret = pinctrl_select_state(fp->pinctrl, fp->intr2_low);
		fp->intr2_output = false;
		break;
	case FP_IOC_REQUEST_RESOURCE:
		ret = fp_request_main_irq(fp);
		break;
	case FP_IOC_RELEASE_RESOURCE:
		fp_free_main_irq(fp);
		break;
	case FP_IOC_REQUEST_INTR2:
		if (fp->intr2_side != FP_INTR2_AP || !fp->intr2_low) {
			ret = -EINVAL;
			break;
		}
		ret = pinctrl_select_state(fp->pinctrl, fp->intr2_low);
		fp->intr2_output = false;
		break;
	case FP_IOC_DEV_INFO:
		if (copy_to_user(up, fp->vendor, sizeof(fp->vendor)))
			ret = -EFAULT;
		break;
	case FP_IOC_INTR2_SIDE_CONFIG:
		value32 = fp->intr2_side;
		if (copy_to_user(up, &value32, sizeof(value32)))
			ret = -EFAULT;
		break;
	case FP_IOC_SENSOR_LOC:
		if (copy_to_user(up, fp->sensor_loc, sizeof(fp->sensor_loc)))
			ret = -EFAULT;
		break;
	default:
		/* Stock returns success for unsupported well-formed commands. */
		break;
	}
	mutex_unlock(&fp->lock);
	return ret;
}

static long fp_compat_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	return fp_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}

static const struct file_operations fp_fops = {
	.owner = THIS_MODULE,
	.open = fp_open,
	.release = fp_release,
	.read = fp_read,
	.poll = fp_poll,
	.unlocked_ioctl = fp_ioctl,
	.compat_ioctl = fp_compat_ioctl,
	.llseek = no_llseek,
};

static ssize_t intr2_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct fp_device *fp = dev_get_drvdata(dev);

	if (fp->intr2_side != FP_INTR2_AP || !fp->intr2_high || !fp->intr2_low)
		return -EINVAL;
	return sysfs_emit(buf, "%u\n", fp->intr2_output);
}

static ssize_t intr2_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct fp_device *fp = dev_get_drvdata(dev);
	struct pinctrl_state *state;
	int ret;

	if (fp->intr2_side != FP_INTR2_AP || !fp->intr2_high || !fp->intr2_low)
		return -EINVAL;

	if (buf[0] == '0') {
		state = fp->intr2_low;
	} else if (buf[0] == '1') {
		state = fp->intr2_high;
	} else {
		return -EINVAL;
	}
	ret = pinctrl_select_state(fp->pinctrl, state);
	if (ret)
		return ret;
	fp->intr2_output = buf[0] == '1';
	return count;
}
static DEVICE_ATTR_RW(intr2);

static ssize_t fingerdown_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct fp_device *fp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", fp->fingerdown);
}

static ssize_t fingerdown_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fp_device *fp = dev_get_drvdata(dev);

	if (buf[0] != '0' && buf[0] != '1')
		return -EINVAL;
	fp->fingerdown = buf[0] == '1';
	if (fp->fingerdown)
		sysfs_notify(&dev->kobj, NULL, "fingerdown");
	return count;
}
static DEVICE_ATTR_RW(fingerdown);

static struct attribute *fp_attrs[] = {
	&dev_attr_intr2.attr,
	&dev_attr_fingerdown.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fp);

static int fp_parse_dt(struct fp_device *fp)
{
	struct device_node *np = fp->dev->of_node;
	const char *intr2_side;
	const char *vendor;
	int ret;

	fp->irq_gpio = of_get_named_gpio(np, "xiaomi,gpio_irq", 0);
	fp->ipc_gpio = of_get_named_gpio(np, "xiaomi,gpio_ipc", 0);
	if (!gpio_is_valid(fp->irq_gpio) || !gpio_is_valid(fp->ipc_gpio))
		return -EINVAL;
	fp->irq = gpiod_to_irq(gpio_to_desc(fp->irq_gpio));
	fp->ipc_irq = gpiod_to_irq(gpio_to_desc(fp->ipc_gpio));
	if (fp->irq < 0 || fp->ipc_irq < 0)
		return -EINVAL;

	of_property_read_u32(np, "netlink-event", &fp->netlink_family);
	if (!of_property_read_string(np, "xiaomi,vendor_names", &vendor))
		strscpy(fp->vendor, vendor, sizeof(fp->vendor));
	of_property_read_u32_array(np, "sensor-loc", fp->sensor_loc,
				   ARRAY_SIZE(fp->sensor_loc) - 1);
	of_property_read_u32_array(np, "intr2-default-electrical",
				   fp->intr2_electrical, 2);
	if (of_property_read_u32(np, "xiaomi,rst_high_time", &fp->reset_high_ms) ||
	    !fp->reset_high_ms)
		fp->reset_high_ms = 10;
	if (of_property_read_u32(np, "xiaomi,rst_low_time", &fp->reset_low_ms) ||
	    !fp->reset_low_ms)
		fp->reset_low_ms = 10;
	if (of_property_read_bool(np, "intr2-pin-enable") &&
	    !of_property_read_string(np, "intr2-side-config", &intr2_side)) {
		if (!strcmp(intr2_side, "AP"))
			fp->intr2_side = FP_INTR2_AP;
		else if (!strcmp(intr2_side, "TIC"))
			fp->intr2_side = FP_INTR2_TIC;
	}

	fp->pinctrl = devm_pinctrl_get(fp->dev);
	if (IS_ERR(fp->pinctrl))
		return PTR_ERR(fp->pinctrl);
	fp->spi_gpio = fp_lookup_state(fp, "spiio_gpio_mode");
	fp->spi_mode = fp_lookup_state(fp, "spiio_spi_mode");
	fp->eint_default = fp_lookup_state(fp, "eint_default");
	fp->eint_pulldown = fp_lookup_state(fp, "eint_pulldown");
	fp->reset_high = fp_lookup_state(fp, "reset_high");
	fp->reset_low = fp_lookup_state(fp, "reset_low");
	if (fp->intr2_side == FP_INTR2_AP) {
		fp->intr2_high = fp_lookup_state(fp, "intr2_high");
		fp->intr2_low = fp_lookup_state(fp, "intr2_low");
		if (!fp->intr2_high || !fp->intr2_low)
			fp->intr2_side = FP_INTR2_NONE;
		else
			pinctrl_select_state(fp->pinctrl, fp->intr2_low);
	}
	fp_select_inactive_pins(fp);

	fp->vreg_3v3 = devm_regulator_get_optional(fp->dev, "fp_3v3_vreg");
	if (IS_ERR(fp->vreg_3v3)) {
		ret = PTR_ERR(fp->vreg_3v3);
		if (ret == -EPROBE_DEFER)
			return ret;
		fp->vreg_3v3 = NULL;
	}
	fp->vreg_1v8 = devm_regulator_get_optional(fp->dev, "fp_1v8_vreg");
	if (IS_ERR(fp->vreg_1v8)) {
		ret = PTR_ERR(fp->vreg_1v8);
		if (ret == -EPROBE_DEFER)
			return ret;
		fp->vreg_1v8 = NULL;
	}
	fp->power_gpio = of_get_named_gpio(np, "xiaomi,gpio_pwr", 0);
	return 0;
}

static int fp_probe(struct platform_device *pdev)
{
	struct fp_device *fp;
	int ret;

	fp = devm_kzalloc(&pdev->dev, sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	fp->dev = &pdev->dev;
	mutex_init(&fp->lock);
	spin_lock_init(&fp->fifo_lock);
	INIT_KFIFO(fp->events);
	init_waitqueue_head(&fp->event_wait);
	ret = fp_parse_dt(fp);
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&fp->devt, 0, FP_MINOR_COUNT, FP_NAME);
	if (ret)
		return ret;
	cdev_init(&fp->main_cdev, &fp_fops);
	fp->main_cdev.owner = THIS_MODULE;
	ret = cdev_add(&fp->main_cdev, fp->devt + FP_MINOR_MAIN, 1);
	if (ret)
		goto err_region;
	cdev_init(&fp->id_cdev, &fp_fops);
	fp->id_cdev.owner = THIS_MODULE;
	ret = cdev_add(&fp->id_cdev, fp->devt + FP_MINOR_ID, 1);
	if (ret)
		goto err_main_cdev;
	cdev_init(&fp->ipc_cdev, &fp_fops);
	fp->ipc_cdev.owner = THIS_MODULE;
	ret = cdev_add(&fp->ipc_cdev, fp->devt + FP_MINOR_IPC, 1);
	if (ret)
		goto err_id_cdev;
	fp->class = class_create(FP_CLASS_NAME);
	if (IS_ERR(fp->class)) {
		ret = PTR_ERR(fp->class);
		goto err_ipc_cdev;
	}
	fp->main_device = device_create(fp->class, fp->dev,
					fp->devt + FP_MINOR_MAIN, fp, FP_NAME);
	if (IS_ERR(fp->main_device)) {
		ret = PTR_ERR(fp->main_device);
		goto err_class;
	}
	fp->id_device = device_create(fp->class, fp->dev,
				      fp->devt + FP_MINOR_ID, fp, FP_ID_NAME);
	if (IS_ERR(fp->id_device)) {
		ret = PTR_ERR(fp->id_device);
		goto err_main_device;
	}
	fp->ipc_device = device_create(fp->class, fp->dev,
				       fp->devt + FP_MINOR_IPC,
				       fp, FP_IPC_NAME);
	if (IS_ERR(fp->ipc_device)) {
		ret = PTR_ERR(fp->ipc_device);
		goto err_id_device;
	}

	fp->input = devm_input_allocate_device(fp->dev);
	if (!fp->input) {
		ret = -ENOMEM;
		goto err_ipc_device;
	}
	fp->input->name = "uinput-xiaomi";
	fp->input->id.bustype = BUS_HOST;
	fp->input->id.vendor = 0x666;
	fp->input->id.product = 0x888;
	input_set_capability(fp->input, EV_KEY, KEY_HOME);
	input_set_capability(fp->input, EV_KEY, KEY_MENU);
	input_set_capability(fp->input, EV_KEY, KEY_BACK);
	input_set_capability(fp->input, EV_KEY, KEY_POWER);
	input_set_capability(fp->input, EV_KEY, BTN_C);
	ret = input_register_device(fp->input);
	if (ret)
		goto err_ipc_device;

	fp->wakeup = wakeup_source_register(fp->dev, "fp_wakesrc");
	if (!fp->wakeup) {
		ret = -ENOMEM;
		goto err_input;
	}
	device_init_wakeup(fp->dev, true);
	fp->display_notifier.notifier_call = fp_display_notifier;
	ret = mi_disp_register_client(&fp->display_notifier);
	if (ret)
		goto err_wakeup;

	platform_set_drvdata(pdev, fp);
	fp_dev = fp;
	ret = sysfs_create_groups(&pdev->dev.kobj, fp_groups);
	if (ret)
		goto err_notifier;
	ret = fp_netlink_init(fp);
	if (ret)
		goto err_sysfs;
	dev_info(fp->dev, "xiaomi fingerprint transport registered\n");
	return 0;

err_sysfs:
	sysfs_remove_groups(&pdev->dev.kobj, fp_groups);
err_notifier:
	fp_dev = NULL;
	mi_disp_unregister_client(&fp->display_notifier);
err_wakeup:
	device_init_wakeup(fp->dev, false);
	wakeup_source_unregister(fp->wakeup);
err_input:
	input_unregister_device(fp->input);
	fp->input = NULL;
err_ipc_device:
	device_destroy(fp->class, fp->devt + FP_MINOR_IPC);
err_id_device:
	device_destroy(fp->class, fp->devt + FP_MINOR_ID);
err_main_device:
	device_destroy(fp->class, fp->devt + FP_MINOR_MAIN);
err_class:
	class_destroy(fp->class);
err_ipc_cdev:
	cdev_del(&fp->ipc_cdev);
err_id_cdev:
	cdev_del(&fp->id_cdev);
err_main_cdev:
	cdev_del(&fp->main_cdev);
err_region:
	unregister_chrdev_region(fp->devt, FP_MINOR_COUNT);
	return ret;
}

static int fp_remove(struct platform_device *pdev)
{
	struct fp_device *fp = platform_get_drvdata(pdev);

	fp_dev = NULL;
	sysfs_remove_groups(&pdev->dev.kobj, fp_groups);
	mi_disp_unregister_client(&fp->display_notifier);
	fp_free_ipc_irq(fp);
	fp_free_main_irq(fp);
	fp_netlink_destroy(fp);
	while (fp->clk_users > 0)
		fp_clk_disable(fp);
	fp_select_inactive_pins(fp);
	if (fp->vreg_1v8_enabled)
		fp_set_power(fp, "vreg1v8", false);
	if (fp->vreg_3v3_enabled)
		fp_set_power(fp, "vreg3v3", false);
	if (fp->power_gpio_enabled)
		fp_set_power(fp, "pwr_gpio", false);
	if (fp->wake_users > 0)
		pm_relax(fp->dev);
	device_init_wakeup(fp->dev, false);
	wakeup_source_unregister(fp->wakeup);
	input_unregister_device(fp->input);
	device_destroy(fp->class, fp->devt + FP_MINOR_IPC);
	device_destroy(fp->class, fp->devt + FP_MINOR_ID);
	device_destroy(fp->class, fp->devt + FP_MINOR_MAIN);
	class_destroy(fp->class);
	cdev_del(&fp->ipc_cdev);
	cdev_del(&fp->id_cdev);
	cdev_del(&fp->main_cdev);
	unregister_chrdev_region(fp->devt, FP_MINOR_COUNT);
	return 0;
}

static int fp_suspend(struct device *dev)
{
	struct fp_device *fp = dev_get_drvdata(dev);

	if (device_may_wakeup(dev) && fp->irq_requested)
		irq_set_irq_wake(fp->irq, 1);
	if (device_may_wakeup(dev) && fp->ipc_requested)
		irq_set_irq_wake(fp->ipc_irq, 1);
	return 0;
}

static int fp_resume(struct device *dev)
{
	struct fp_device *fp = dev_get_drvdata(dev);

	if (device_may_wakeup(dev) && fp->irq_requested)
		irq_set_irq_wake(fp->irq, 0);
	if (device_may_wakeup(dev) && fp->ipc_requested)
		irq_set_irq_wake(fp->ipc_irq, 0);
	return 0;
}

static const struct dev_pm_ops fp_pm_ops = {
	.suspend = fp_suspend,
	.resume = fp_resume,
};

static const struct of_device_id fp_of_match[] = {
	{ .compatible = "xiaomi,xiaomi-fp" },
	{}
};
MODULE_DEVICE_TABLE(of, fp_of_match);

static struct platform_driver fp_platform_driver = {
	.probe = fp_probe,
	.remove = fp_remove,
	.driver = {
		.name = FP_NAME,
		.of_match_table = fp_of_match,
		.pm = &fp_pm_ops,
	},
};
module_platform_driver(fp_platform_driver);

MODULE_ALIAS("xiaomi-fp");
MODULE_AUTHOR("Xiaomi");
MODULE_DESCRIPTION("Xiaomi Fingerprint driver");
MODULE_LICENSE("GPL");
