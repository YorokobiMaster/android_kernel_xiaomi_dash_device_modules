// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi UFS compatibility hooks used by the vendor storage diagnostics.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/stddef.h>

#include <miev/mievent.h>

#include "ufshcd-priv.h"
#include "ufs-mediatek-xiaomi.h"

static struct ufs_xiaomi ufs_xiaomi = {
	.ufs_xiaomi_comp = COMPLETION_INITIALIZER(ufs_xiaomi.ufs_xiaomi_comp),
};

#define UFS_MIEVENT_CODE	0x363db24a

static void ufs_xiaomi_report_error(const char *reason, const char *event_info)
{
	struct misight_mievent *event;
	int i;
	int ret;

	for (i = ARRAY_SIZE(ufs_xiaomi.err_reason) - 1; i > 0; i--)
		memcpy(ufs_xiaomi.err_reason[i], ufs_xiaomi.err_reason[i - 1],
		       sizeof(ufs_xiaomi.err_reason[i]));
	strscpy(ufs_xiaomi.err_reason[0], reason,
		sizeof(ufs_xiaomi.err_reason[0]));
	ufs_xiaomi.err_state++;

	dump_stack();
	event = cdev_tevent_alloc(UFS_MIEVENT_CODE);
	ret = cdev_tevent_add_str(event, "errInfo", event_info);
	if (ret != -1)
		cdev_tevent_write(event);
	cdev_tevent_destroy(event);
}

void ufshcd_update_err_state(u32 evt, u32 val)
{
	switch (evt) {
	case UFS_EVT_AUTO_HIBERN8_ERR:
		ufs_xiaomi_report_error("auto_hibern8_err\n", "autoh8");
		break;
	case UFS_EVT_FATAL_ERR:
		ufs_xiaomi_report_error("fatal_err\n", "fatal");
		break;
	case UFS_EVT_LINK_STARTUP_FAIL:
		if (!val)
			ufs_xiaomi_report_error("link_startup_fail_not_present\n",
						"link_startup_npresent");
		else
			ufs_xiaomi_report_error("link_startup_fail\n",
						"link_startup");
		break;
	case UFS_EVT_RESUME_ERR:
		ufs_xiaomi_report_error("ufs_resume_fail\n", "resume");
		break;
	case UFS_EVT_SUSPEND_ERR:
		ufs_xiaomi_report_error("ufs_suspend_fail\n", "suspend");
		break;
	case UFS_EVT_WL_SUSP_ERR:
		ufs_xiaomi_report_error("wlun_suspend_fail\n", "lun_suspend");
		break;
	case UFS_EVT_WL_RES_ERR:
		ufs_xiaomi_report_error("wlun_resume_fail\n", "lun_resume");
		break;
	case UFS_EVT_HOST_RESET:
		ufs_xiaomi_report_error("host_reset\n", "host_reset");
		break;
	case UFS_EVT_ABORT:
		ufs_xiaomi_report_error("task_abort\n", "task_abort");
		break;
	case 0x1e:
		ufs_xiaomi_report_error("wait_uic_cmd_fail\n", "uic");
		break;
	case 0x1f:
		ufs_xiaomi_report_error("wait_dev_cmd_fail\n", "dev");
		break;
	case 0x20:
		ufs_xiaomi_report_error("uic_pwr_ctrl_fail\n", "pwr_ctrl");
		break;
	case 0x21:
		ufs_xiaomi_report_error("hibern8_enter_fail\n", "h8_enter");
		break;
	case 0x22:
		ufs_xiaomi_report_error("hibern8_exit_fail\n", "h8_exit");
		break;
	case 0x23:
		ufs_xiaomi_report_error("change_pwr_mode_fail\n", "pwr_mode");
		break;
	case 0x24:
		ufs_xiaomi_report_error("err_handler\n", "err_handler");
		break;
	case 0x25:
		ufs_xiaomi_report_error("try_to_abort_task\n", "try_abort_task");
		break;
	default:
		break;
	}
}

void ufshcd_update_uic_error_cnt(u32 evt, u32 val)
{
	unsigned long errors;
	unsigned int bit;

	switch (evt) {
	case UFS_EVT_PA_ERR:
		errors = val & 0x1f;
		for_each_set_bit(bit, &errors, 5) {
			ufs_xiaomi.pa_err_cnt_total++;
			if (bit < 4)
				ufs_xiaomi.pa_lane_err_cnt[bit]++;
			else
				ufs_xiaomi.pa_line_reset_err_cnt++;
		}
		break;
	case UFS_EVT_DL_ERR:
		errors = val & 0x7fff;
		for_each_set_bit(bit, &errors, 15) {
			u32 *detail = &ufs_xiaomi.dl_nac_received_err_cnt;

			ufs_xiaomi.dl_err_cnt_total++;
			detail[bit]++;
		}
		break;
	case UFS_EVT_DME_ERR:
		ufs_xiaomi.dme_err_cnt++;
		break;
	default:
		break;
	}
}

struct ufs_xiaomi *get_ufs_xiaomi(void)
{
	return &ufs_xiaomi;
}
EXPORT_SYMBOL(get_ufs_xiaomi);

void ufs_xiaomi_register_hba(struct ufs_hba *hba)
{
	BUILD_BUG_ON(offsetof(struct ufs_xiaomi, err_state) != 0x68);
	BUILD_BUG_ON(offsetof(struct ufs_xiaomi, err_reason) != 0x70);
	BUILD_BUG_ON(offsetof(struct ufs_xiaomi, ufs_xiaomi_comp) != 0x488);
	BUILD_BUG_ON(sizeof(struct ufs_xiaomi) != 0x4a8);

	reinit_completion(&ufs_xiaomi.ufs_xiaomi_comp);
	WRITE_ONCE(ufs_xiaomi.hba, hba);
}

void ufs_xiaomi_lun_configured(struct ufs_hba *hba, u64 lun)
{
	if (lun == 2 && READ_ONCE(ufs_xiaomi.hba) == hba)
		complete(&ufs_xiaomi.ufs_xiaomi_comp);
}

int ufshcd_query_descriptor_retry_xm(struct ufs_hba *hba,
				     enum query_opcode opcode,
				     enum desc_idn idn, u8 index,
				     u8 selector, u8 *desc_buf,
				     int *buf_len)
{
	return ufshcd_query_descriptor_retry(hba, opcode, idn, index, selector,
					     desc_buf, buf_len);
}
EXPORT_SYMBOL_GPL(ufshcd_query_descriptor_retry_xm);

int ufshcd_read_desc_param_sel(struct ufs_hba *hba, enum desc_idn desc_id,
			       int desc_index, u8 selector,
			       u8 param_offset, u8 *param_read_buf,
			       u8 param_size)
{
	int buff_len = QUERY_DESC_MAX_SIZE;
	bool is_kmalloc = true;
	u8 *desc_buf;
	int ret;

	if (desc_id >= QUERY_DESC_IDN_MAX || !param_size)
		return -EINVAL;

	if (param_offset != 0 || param_size < buff_len) {
		desc_buf = kzalloc(buff_len, GFP_KERNEL);
		if (!desc_buf)
			return -ENOMEM;
	} else {
		desc_buf = param_read_buf;
		is_kmalloc = false;
	}

	ret = ufshcd_query_descriptor_retry_xm(hba,
					      UPIU_QUERY_OPCODE_READ_DESC,
					      desc_id, desc_index, selector,
					      desc_buf, &buff_len);
	if (ret) {
		dev_err(hba->dev,
			"%s: Failed reading descriptor. desc_id %d, desc_index %d, param_offset %d, ret %d\n",
			__func__, desc_id, desc_index, param_offset, ret);
		goto out;
	}

	buff_len = desc_buf[QUERY_DESC_LENGTH_OFFSET];
	if (param_offset >= buff_len) {
		dev_err(hba->dev,
			"%s: Invalid offset 0x%x in descriptor IDN 0x%x, length 0x%x\n",
			__func__, param_offset, desc_id, buff_len);
		ret = -EINVAL;
		goto out;
	}

	if (desc_buf[QUERY_DESC_DESC_TYPE_OFFSET] != desc_id) {
		dev_err(hba->dev,
			"%s: invalid desc_id %d in descriptor header\n",
			__func__, desc_buf[QUERY_DESC_DESC_TYPE_OFFSET]);
		ret = -EINVAL;
		goto out;
	}

	if (is_kmalloc)
		memcpy(param_read_buf, &desc_buf[param_offset],
		       min_t(u32, param_size, buff_len - param_offset));
out:
	if (is_kmalloc)
		kfree(desc_buf);
	return ret;
}
EXPORT_SYMBOL_GPL(ufshcd_read_desc_param_sel);
