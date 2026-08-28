// SPDX-License-Identifier: GPL-2.0
/* Xiaomi Memory Debug Interface reconstructed from the dash stock module. */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/blk-mq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nls.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include <asm/unaligned.h>

#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_common.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <ufs/ufs.h>
#include <ufs/ufshcd.h>

#include "../../../ufs/ufs-mediatek-xiaomi.h"

#define MI_MEMORY_NAME		"mi_memory"
#define UFS_DESC_MAX		QUERY_DESC_MAX_SIZE
#define UFS_HR_SIZE		512
#define UFS_HR_TIMEOUT		7500
#define UFS_HR_RETRIES		3

struct dram_info {
	char vendor[8];
	u32 ddr_type;
	u32 ddr_size;
	u32 ddr_id;
	u32 support_ch_cnt;
	u32 ch_cnt;
	u32 rk_cnt;
	u32 mr_cnt;
	u32 freq_cnt;
	u32 *rk_size;
	u32 *freq_step;
	u32 reserved;
	u32 *mr_info;
};

struct ufs_info {
	u16 manufacturer_id;
	u16 reserved;
	u32 capacity;
	char *model;
	char *revision;
	struct scsi_device *sdev;
	struct ufs_hba *hba;
};

struct mi_memory_dev {
	struct class *class;
	struct device *device;
	int major;
	struct work_struct ufs_work;
	struct dram_info *dram;
	struct ufs_info *ufs;
	struct proc_dir_entry *proc;
	struct dentry *debugfs_link;
};

static struct mi_memory_dev *mi_memory;
static struct dram_info dram_info;
static struct ufs_info ufs_info;
static struct dram_info *g_dram_info;
static struct ufs_info *g_ufs_info;
static DEFINE_MUTEX(hr_lock);

static struct scsi_device *mi_scsi_device_lookup(u64 lun)
{
	struct Scsi_Host *host;
	struct scsi_device *sdev = NULL;

	host = scsi_host_lookup(0);
	if (!host)
		return NULL;
	sdev = scsi_device_lookup(host, 0, 0, lun);
	scsi_host_put(host);
	return sdev;
}

struct ufs_hba *get_ufs_hba_data(void)
{
	struct scsi_device *sdev;

	if (!g_ufs_info)
		return NULL;
	sdev = g_ufs_info->sdev;
	if (!sdev) {
		sdev = mi_scsi_device_lookup(0);
		g_ufs_info->sdev = sdev;
	}
	return sdev ? shost_priv(sdev->host) : NULL;
}
EXPORT_SYMBOL(get_ufs_hba_data);

static void free_dram_info(struct dram_info *info)
{
	if (!info)
		return;
	kfree(info->rk_size);
	kfree(info->freq_step);
	kfree(info->mr_info);
	memset(info, 0, sizeof(*info));
}

static struct dram_info *init_dram_info(void)
{
	static const struct {
		u8 id;
		const char *name;
	} vendors[] = {
		{ 0x01, "SAMSUNG" }, { 0x03, "ELPIDA" },
		{ 0x05, "NANYA" },   { 0x06, "SKhynix" },
		{ 0x0e, "INTEL" },   { 0x13, "CXMT" },
		{ 0xff, "MICRON" },
	};
	struct device_node *np;
	const char *vendor = "UNKNOWN";
	u32 i;
	int ret;

	free_dram_info(&dram_info);
	g_dram_info = &dram_info;
	np = of_find_node_by_name(NULL, "dramc");
	if (!np) {
		pr_err("%s: find dram node fail\n", __func__);
		goto fail;
	}

#define READ_DRAM_PROP(_name, _field) do { \
	ret = of_property_read_u32(np, (_name), &dram_info._field); \
	if (ret) { \
		pr_err("%s: get %s fail\n", __func__, (_name)); \
		goto fail_put; \
	} \
} while (0)
	READ_DRAM_PROP("dram_type", ddr_type);
	READ_DRAM_PROP("support_ch_cnt", support_ch_cnt);
	READ_DRAM_PROP("ch_cnt", ch_cnt);
	READ_DRAM_PROP("rk_cnt", rk_cnt);

	dram_info.rk_size = kcalloc(dram_info.rk_cnt, sizeof(u32), GFP_KERNEL);
	if (!dram_info.rk_size)
		goto fail_put;
	ret = of_property_read_u32_array(np, "rk_size", dram_info.rk_size,
					 dram_info.rk_cnt);
	if (ret)
		goto fail_put;
	for (i = 0; i < dram_info.rk_cnt; i++) {
		dram_info.ddr_size += (dram_info.rk_size[i] >> 3) & 0x3fffff;
		pr_info("%s: g_dram_info->rk_size[%u]=%u\n", __func__, i,
			dram_info.rk_size[i]);
	}

	READ_DRAM_PROP("mr_cnt", mr_cnt);
	dram_info.mr_info = kcalloc(dram_info.mr_cnt, 2 * sizeof(u32), GFP_KERNEL);
	if (!dram_info.mr_info)
		goto fail_put;
	ret = of_property_read_u32_array(np, "mr", dram_info.mr_info,
					 dram_info.mr_cnt * 2);
	if (ret)
		goto fail_put;
	for (i = 0; i < dram_info.mr_cnt; i++)
		pr_info("%s: mr%u(%x)\n", __func__, dram_info.mr_info[i * 2],
			dram_info.mr_info[i * 2 + 1]);

	READ_DRAM_PROP("freq_cnt", freq_cnt);
	dram_info.freq_step = kcalloc(dram_info.freq_cnt, sizeof(u32), GFP_KERNEL);
	if (!dram_info.freq_step)
		goto fail_put;
	ret = of_property_read_u32_array(np, "freq_step", dram_info.freq_step,
					 dram_info.freq_cnt);
	if (ret)
		goto fail_put;

	dram_info.ddr_id = dram_info.mr_info[1] & 0xff;
	for (i = 0; i < ARRAY_SIZE(vendors); i++)
		if (vendors[i].id == dram_info.ddr_id) {
			vendor = vendors[i].name;
			break;
		}
	strscpy(dram_info.vendor, vendor, sizeof(dram_info.vendor));
	of_node_put(np);
	pr_info("%s: ddr_vendor(%s),ddr_type(%u),ddr_id(%u),ddr_size(%u),support_ch_cnt(%u),ch_cnt(%u),rk_cnt(%u),mr_cnt(%u),freq_cnt(%u)\n",
		__func__, dram_info.vendor, dram_info.ddr_type, dram_info.ddr_id,
		dram_info.ddr_size, dram_info.support_ch_cnt, dram_info.ch_cnt,
		dram_info.rk_cnt, dram_info.mr_cnt, dram_info.freq_cnt);
	return &dram_info;

fail_put:
	of_node_put(np);
fail:
	free_dram_info(&dram_info);
	g_dram_info = NULL;
	return NULL;
#undef READ_DRAM_PROP
}

static int ufs_read_desc_param(struct ufs_hba *hba, enum desc_idn idn,
			       u8 offset, u8 size, u64 *value)
{
	u8 data[8] = { 0 };
	int ret;

	if (!hba || !size || size > sizeof(data))
		return -EINVAL;
	ret = pm_runtime_resume_and_get(hba->dev);
	if (ret < 0)
		return ret;
	ret = ufshcd_read_desc_param_sel(hba, idn, 0, 0, offset, data, size);
	pm_runtime_put(hba->dev);
	if (ret)
		return ret;

	switch (size) {
	case 1: *value = data[0]; break;
	case 2: *value = get_unaligned_be16(data); break;
	case 4: *value = get_unaligned_be32(data); break;
	case 8: *value = get_unaligned_be64(data); break;
	default: *value = data[0]; break;
	}
	return 0;
}

static int ufs_get_string_desc(struct ufs_hba *hba, char **result,
			       u8 device_offset, bool ascii)
{
	u8 *device_desc = NULL, *string_desc = NULL;
	u8 string_index, desc_len;
	char *out = NULL;
	int buf_len, chars, ret;

	*result = NULL;
	device_desc = kzalloc(UFS_DESC_MAX, GFP_KERNEL);
	string_desc = kzalloc(UFS_DESC_MAX, GFP_KERNEL);
	if (!device_desc || !string_desc) {
		ret = -ENOMEM;
		goto out;
	}
	ret = pm_runtime_resume_and_get(hba->dev);
	if (ret < 0)
		goto out;
	buf_len = UFS_DESC_MAX;
	ret = ufshcd_query_descriptor_retry_xm(hba, UPIU_QUERY_OPCODE_READ_DESC,
		QUERY_DESC_IDN_DEVICE, 0, 0, device_desc, &buf_len);
	if (ret) {
		ret = -EINVAL;
		goto out_pm;
	}
	string_index = device_desc[device_offset];
	buf_len = UFS_DESC_MAX;
	ret = ufshcd_read_desc_param_sel(hba, QUERY_DESC_IDN_STRING,
		string_index, 0, 0, string_desc, UFS_DESC_MAX);
	if (ret) {
		dev_err(hba->dev,
			"Reading String Desc failed after %d retries. err = %d\n",
			UFS_HR_RETRIES, ret);
		dev_err(hba->dev, "Reserve ret and change to -EINVAL\n");
		ret = -EINVAL;
		goto out_pm;
	}
	desc_len = string_desc[QUERY_DESC_LENGTH_OFFSET];
	if (desc_len < 3) {
		ret = 0;
		goto out_pm;
	}
	if (!ascii) {
		out = kmemdup(string_desc, desc_len, GFP_KERNEL);
		ret = out ? desc_len : -ENOMEM;
		goto done;
	}
	out = kzalloc((desc_len - 2) / 2 + 1, GFP_KERNEL);
	if (!out) {
		ret = -ENOMEM;
		goto out_pm;
	}
	chars = utf16s_to_utf8s((wchar_t *)(string_desc + 2), (desc_len - 2) / 2,
				UTF16_BIG_ENDIAN, out, (desc_len - 2) / 2);
	for (buf_len = 0; buf_len < chars; buf_len++)
		if (out[buf_len] < 0x20 || out[buf_len] > 0x7e)
			out[buf_len] = ' ';
	out[chars] = '\0';
	ret = chars + 1;
done:
	*result = out;
out_pm:
	pm_runtime_put(hba->dev);
out:
	if (ret < 0)
		kfree(out);
	kfree(device_desc);
	kfree(string_desc);
	return ret;
}

struct ufs_info *init_ufs_info(void)
{
	struct ufs_hba *hba;
	u64 capacity = 0, manufacturer = 0;
	u32 rounded = 0;

	g_ufs_info = &ufs_info;
	hba = get_ufs_hba_data();
	if (!hba) {
		pr_err("%s: ufs info is NULL\n", __func__);
		g_ufs_info = NULL;
		return NULL;
	}
	ufs_info.hba = hba;
	ufs_get_string_desc(hba, &ufs_info.model, 0x15, true);
	ufs_get_string_desc(hba, &ufs_info.revision, 0x2a, true);
	if (!ufs_read_desc_param(hba, QUERY_DESC_IDN_DEVICE, 0x18, 2,
				 &manufacturer))
		ufs_info.manufacturer_id = manufacturer;
	if (ufs_read_desc_param(hba, QUERY_DESC_IDN_GEOMETRY, 0x04, 8,
				&capacity)) {
		ufs_info.capacity = 0;
		pr_info("mv unkonwn ufs size %u\n", 0);
		return g_ufs_info;
	}
	capacity = (capacity >> 21) & 0x3ffffffffULL;
	if (capacity >= 9 && capacity <= 16) rounded = 8;
	else if (capacity <= 32 && capacity >= 17) rounded = 32;
	else if (capacity <= 64 && capacity >= 33) rounded = 64;
	else if (capacity <= 128 && capacity >= 65) rounded = 128;
	else if (capacity <= 256 && capacity >= 129) rounded = 256;
	else if (capacity <= 512 && capacity >= 257) rounded = 512;
	else if (capacity <= 1024 && capacity >= 513) rounded = 1024;
	else if (capacity >= 1025) rounded = 512;
	else pr_info("mv unkonwn ufs size %llu\n", capacity);
	ufs_info.capacity = rounded;
	return g_ufs_info;
}
EXPORT_SYMBOL(init_ufs_info);

u16 get_ufs_id(void)
{
	return g_ufs_info ? g_ufs_info->manufacturer_id : 0xffff;
}
EXPORT_SYMBOL(get_ufs_id);

static void ufs_info_init_work(struct work_struct *work)
{
	struct ufs_xiaomi *xiaomi = get_ufs_xiaomi();

	if (!xiaomi)
		return;
	wait_for_completion(&xiaomi->ufs_xiaomi_comp);
	mi_memory->ufs = init_ufs_info();
}

static int mv_proc_show(struct seq_file *m, void *unused)
{
	if (!mi_memory->dram)
		seq_printf(m, "%s: mi memory dram info not ready!\n", __func__);
	else
		seq_printf(m, "D: 0x%02x %d\n", mi_memory->dram->ddr_id,
			   mi_memory->dram->ddr_size);
	if (!mi_memory->ufs)
		seq_printf(m, "%s: mi memory ufs info not ready!\n", __func__);
	else
		seq_printf(m, "U: 0x%04x %d %s %s\n",
			   mi_memory->ufs->manufacturer_id, mi_memory->ufs->capacity,
			   mi_memory->ufs->model ?: "", mi_memory->ufs->revision ?: "");
	return 0;
}

static int mv_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mv_proc_show, NULL);
}

static const struct proc_ops memory_procfs_fops = {
	.proc_open = mv_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

#define DRAM_SHOW(_name, _fmt, _field) \
static ssize_t _name##_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	if (!g_dram_info) \
		return sysfs_emit(buf, "dram_info is NULL\n"); \
	return sysfs_emit(buf, _fmt "\n", g_dram_info->_field); \
}
DRAM_SHOW(ddr_size, "%u", ddr_size)
DRAM_SHOW(ddr_id, "0x%02x", ddr_id)
DRAM_SHOW(ddr_vendor, "%s", vendor)
DRAM_SHOW(ddr_type, "0x%0x", ddr_type)
static DEVICE_ATTR_RO(ddr_size);
static DEVICE_ATTR_RO(ddr_id);
static DEVICE_ATTR_RO(ddr_vendor);
static DEVICE_ATTR_RO(ddr_type);

struct desc_field { const char *name; u8 offset; u8 size; };

static int read_descriptor(struct ufs_hba *hba, enum desc_idn idn,
			   u8 *buf, u8 len)
{
	int ret;

	ret = pm_runtime_resume_and_get(hba->dev);
	if (ret < 0)
		return ret;
	ret = ufshcd_read_desc_param_sel(hba, idn, 0, 0, 0, buf, len);
	pm_runtime_put(hba->dev);
	return ret;
}

static ssize_t dump_health_desc_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	static const struct desc_field health_fields[] = {
		{ "bLength", 0, 1 },
		{ "bDescriptorType", 1, 1 },
		{ "bPreEOLInfo", 2, 1 },
		{ "bDeviceLifeTimeEstA", 3, 1 },
		{ "bDeviceLifeTimeEstB", 4, 1 },
	};
	u64 desc_len = 0;
	u8 *desc;
	ssize_t len = 0;
	int ret;
	int i;

	if (!g_ufs_info || !g_ufs_info->hba) {
		pr_err("%s: ufs info is NULL\n", __func__);
		return -1;
	}
	ufs_read_desc_param(g_ufs_info->hba, QUERY_DESC_IDN_HEALTH, 0, 1,
			    &desc_len);
	desc = kzalloc((u8)desc_len, GFP_KERNEL);
	if (!desc)
		return sysfs_emit(buf, "get health info fail\n");
	ret = read_descriptor(g_ufs_info->hba, QUERY_DESC_IDN_HEALTH, desc,
			      (u8)desc_len);
	if (ret) {
		len = sysfs_emit(buf, "ufshcd_read_desc fail, err = %d\n", ret);
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(health_fields); i++) {
		const struct desc_field *f = &health_fields[i];

		if (f->offset >= desc_len)
			break;
		len += sysfs_emit_at(buf, len,
			"Device Descriptor[Byte offset 0x%x]: %s = 0x%x\n",
			f->offset, f->name, desc[f->offset]);
	}
out:
	kfree(desc);
	return len;
}
static DEVICE_ATTR_RO(dump_health_desc);

static ssize_t dump_string_desc_serial_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	char *desc;
	ssize_t len = 0;
	int size, i;

	if (!g_ufs_info || !g_ufs_info->hba) {
		pr_err("%s: ufs info is NULL\n", __func__);
		return -1;
	}
	size = ufs_get_string_desc(g_ufs_info->hba, &desc, 0x16, false);
	if (size < 0) {
		pr_err("%s, ufs_get_string_desc failed!, ret = %d\n",
		       __func__, size);
		return -1;
	}
	if (!desc) {
		dev_err(g_ufs_info->hba->dev, "serial_number is null.\n");
		return 0;
	}
	len += sysfs_emit_at(buf, len, "serial:");
	for (i = 2; i + 1 < (u8)desc[0]; i += 2)
		len += sysfs_emit_at(buf, len, "%02x%02x", (u8)desc[i], (u8)desc[i + 1]);
	len += sysfs_emit_at(buf, len, "\n");
	kfree(desc);
	return len;
}
static DEVICE_ATTR_RO(dump_string_desc_serial);

static const struct desc_field device_desc_fields[] = {
	{ "bLength", 0, 1 }, { "bDescriptorType", 1, 1 },
	{ "bDevice", 2, 1 }, { "bDeviceClass", 3, 1 },
	{ "bDeviceSubClass", 4, 1 }, { "bProtocol", 5, 1 },
	{ "bNumberLU", 6, 1 }, { "bNumberWLU", 7, 1 },
	{ "bBootEnable", 8, 1 }, { "bDescrAccessEn", 9, 1 },
	{ "bInitPowerMode", 10, 1 }, { "bHighPriorityLUN", 11, 1 },
	{ "bSecureRemovalType", 12, 1 }, { "bSecurityLU", 13, 1 },
	{ "Reserved", 14, 1 }, { "bInitActiveICCLevel", 15, 1 },
	{ "wSpecVersion", 16, 2 }, { "wManufactureDate", 18, 2 },
	{ "iManufactureName", 20, 1 }, { "iProductName", 21, 1 },
	{ "iSerialNumber", 22, 1 }, { "iOemID", 23, 1 },
	{ "wManufactureID", 24, 2 }, { "bUD0BaseOffset", 26, 1 },
	{ "bUDConfigPLength", 27, 1 }, { "bDeviceRTTCap", 28, 1 },
	{ "wPeriodicRTCUpdate", 29, 2 }, { "bUFSFeaturesSupport", 31, 1 },
	{ "bFFUTimeout", 32, 1 }, { "bQueueDepth", 33, 1 },
	{ "wDeviceVersion", 34, 2 }, { "bNumSecureWpArea", 36, 1 },
	{ "dPSAMaxDataSize", 37, 4 }, { "bPSAStateTimeout", 41, 1 },
	{ "iProductRevisionLevel", 42, 1 },
};

static ssize_t dump_device_desc_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 desc_len = 0;
	u8 *desc;
	ssize_t len = 0;
	u64 value;
	int ret, i;

	if (!g_ufs_info || !g_ufs_info->hba) {
		pr_err("%s: ufs info is NULL\n", __func__);
		return -1;
	}
	ret = ufs_read_desc_param(g_ufs_info->hba, QUERY_DESC_IDN_DEVICE,
				  0, 1, &desc_len);
	if (ret) {
		dev_err(g_ufs_info->hba->dev,
			"%s ufs_read_desc_param failed, err = %d\n",
			__func__, -EINVAL);
		return sysfs_emit(buf, "get desc info fail\n");
	}
	desc = kzalloc((u8)desc_len, GFP_KERNEL);
	if (!desc) {
		dev_err(g_ufs_info->hba->dev, "%s desc_buf malloc failed.\n",
			__func__);
		return 0;
	}
	ret = read_descriptor(g_ufs_info->hba, QUERY_DESC_IDN_DEVICE, desc,
			      (u8)desc_len);
	if (ret) {
		dev_err(g_ufs_info->hba->dev,
			"%s ufs_read_desc failed, err = %d\n", __func__, ret);
		len = sysfs_emit(buf, "ufshcd_read_desc fail, err = %d\n", ret);
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(device_desc_fields); i++) {
		const struct desc_field *f = &device_desc_fields[i];

		if (f->offset + f->size > desc_len)
			break;
		switch (f->size) {
		case 1: value = desc[f->offset]; break;
		case 2: value = get_unaligned_be16(desc + f->offset); break;
		case 4: value = get_unaligned_be32(desc + f->offset); break;
		default: continue;
		}
		len += sysfs_emit_at(buf, len,
			"Device Descriptor[Byte offset 0x%x]: %s = 0x%x\n",
			f->offset, f->name, (u32)value);
	}
out:
	kfree(desc);
	return len;
}
static DEVICE_ATTR_RO(dump_device_desc);

static ssize_t show_hba_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ufs_xiaomi *x = get_ufs_xiaomi();
	struct ufs_hba *hba;
	ssize_t len = 0;

	if (!g_ufs_info || !(hba = g_ufs_info->hba) || !x) {
		pr_err("%s: ufs info is NULL\n", __func__);
		return -1;
	}
#define HBA_LINE(_fmt, ...) len += sysfs_emit_at(buf, len, _fmt "\n", __VA_ARGS__)
	HBA_LINE("hba->outstanding_tasks = 0x%x", (u32)hba->outstanding_tasks);
	HBA_LINE("hba->outstanding_reqs = 0x%x", (u32)hba->outstanding_reqs);
	HBA_LINE("hba->capabilities = 0x%x", hba->capabilities);
	HBA_LINE("hba->nutrs = %d", hba->nutrs);
	HBA_LINE("hba->nutmrs = %d", hba->nutmrs);
	HBA_LINE("hba->ufs_version = 0x%x", hba->ufs_version);
	HBA_LINE("hba->irq = 0x%x", hba->irq);
	HBA_LINE("hba->auto_bkops_enabled = %d", hba->auto_bkops_enabled);
	HBA_LINE("hba->ufshcd_state = 0x%x", hba->ufshcd_state);
	HBA_LINE("hba->clk_gating.state = 0x%x", hba->clk_gating.state);
	HBA_LINE("hba->eh_flags = 0x%x", hba->eh_flags);
	HBA_LINE("hba->intr_mask = 0x%x", hba->intr_mask);
	HBA_LINE("hba->ee_ctrl_mask = 0x%x", hba->ee_ctrl_mask);
	HBA_LINE("hba->errors = 0x%x", hba->errors);
	HBA_LINE("hba->uic_error = 0x%x", hba->uic_error);
	HBA_LINE("hba->saved_err = 0x%x", hba->saved_err);
	HBA_LINE("hba->saved_uic_err = 0x%x", hba->saved_uic_err);
	HBA_LINE("hibern8_exit_cnt = %d", hba->ufs_stats.hibern8_exit_cnt);
#define EVT_LINE(_name, _evt) HBA_LINE(_name " = 0x%llx", hba->ufs_stats.event[_evt].cnt)
	EVT_LINE("ufs_event_pa_error_cnt", UFS_EVT_PA_ERR);
	EVT_LINE("ufs_event_dl_error_cnt", UFS_EVT_DL_ERR);
	EVT_LINE("ufs_event_nl_error_cnt", UFS_EVT_NL_ERR);
	EVT_LINE("ufs_event_tl_error_cnt", UFS_EVT_TL_ERR);
	EVT_LINE("ufs_event_dme_error_cnt", UFS_EVT_DME_ERR);
	EVT_LINE("ufs_event_auto_hibern8_cnt", UFS_EVT_AUTO_HIBERN8_ERR);
	HBA_LINE("ufs_event_fatal_error_cnt= 0x%llx",
		 hba->ufs_stats.event[UFS_EVT_FATAL_ERR].cnt);
	EVT_LINE("ufs_event_link_startup_fail_cnt", UFS_EVT_LINK_STARTUP_FAIL);
	EVT_LINE("ufs_event_resume_error_cnt", UFS_EVT_RESUME_ERR);
	EVT_LINE("ufs_event_suspend_error_cnt", UFS_EVT_SUSPEND_ERR);
	EVT_LINE("ufs_event_device_reset_cnt", UFS_EVT_DEV_RESET);
	EVT_LINE("ufs_event_host_reset_cnt", UFS_EVT_HOST_RESET);
	EVT_LINE("ufs_event_abort_cnt", UFS_EVT_ABORT);
	HBA_LINE("pa_err_cnt_total = %d", x->pa_err_cnt_total);
	HBA_LINE("pa_lane_0_err_cnt = %d", x->pa_lane_err_cnt[0]);
	HBA_LINE("pa_lane_1_err_cnt = %d", x->pa_lane_err_cnt[1]);
	HBA_LINE("pa_lane_2_err_cnt = %d", x->pa_lane_err_cnt[2]);
	HBA_LINE("pa_lane_3_err_cnt = %d", x->pa_lane_err_cnt[3]);
	HBA_LINE("pa_line_reset_err_cnt = %d", x->pa_line_reset_err_cnt);
	HBA_LINE("dl_err_cnt_total = %d", x->dl_err_cnt_total);
	HBA_LINE("dl_nac_received_err_cnt = %d", x->dl_nac_received_err_cnt);
	HBA_LINE("dl_tcx_replay_timer_expired_err_cnt = %d", x->dl_tcx_replay_timer_expired_err_cnt);
	HBA_LINE("dl_afcx_request_timer_expired_err_cnt = %d", x->dl_afcx_request_timer_expired_err_cnt);
	HBA_LINE("dl_fcx_protection_timer_expired_err_cnt = %d", x->dl_fcx_protection_timer_expired_err_cnt);
	HBA_LINE("dl_crc_err_cnt = %d", x->dl_crc_err_cnt);
	HBA_LINE("dll_rx_buffer_overflow_err_cnt = %d", x->dl_rx_buffer_overflow_err_cnt);
	HBA_LINE("dl_max_frame_length_exceeded_err_cnt = %d", x->dl_max_frame_length_exceeded_err_cnt);
	HBA_LINE("dl_wrong_sequence_number_err_cnt = %d", x->dl_wrong_sequence_number_err_cnt);
	HBA_LINE("dl_afc_frame_syntax_err_cnt = %d", x->dl_afc_frame_syntax_err_cnt);
	HBA_LINE("dl_nac_frame_syntax_err_cnt = %d", x->dl_nac_frame_syntax_err_cnt);
	HBA_LINE("dl_eof_syntax_err_cnt = %d", x->dl_eof_syntax_err_cnt);
	HBA_LINE("dl_frame_syntax_err_cnt = %d", x->dl_frame_syntax_err_cnt);
	HBA_LINE("dl_bad_ctrl_symbol_type_err_cnt = %d", x->dl_bad_ctrl_symbol_type_err_cnt);
	HBA_LINE("dl_pa_init_err_cnt = %d", x->dl_pa_init_err_cnt);
	HBA_LINE("dl_pa_error_ind_received = %d", x->dl_pa_error_ind_received);
	HBA_LINE("dme_err_cnt = %d", x->dme_err_cnt);
#undef EVT_LINE
#undef HBA_LINE
	return len;
}
static DEVICE_ATTR_RO(show_hba);

static int mi_scsi_exec(struct scsi_device *sdev, const u8 *cdb,
			blk_opf_t op, void *data, unsigned int len,
			struct scsi_sense_hdr *sshdr, int *resid)
{
	struct request *req;
	struct scsi_cmnd *scmd;
	int ret;

	req = scsi_alloc_request(sdev->request_queue, op, BLK_MQ_REQ_PM);
	if (IS_ERR(req))
		return PTR_ERR(req);

	if (len) {
		ret = blk_rq_map_kern(sdev->request_queue, req, data, len,
				      GFP_NOIO);
		if (ret)
			goto out;
	}

	scmd = blk_mq_rq_to_pdu(req);
	scmd->cmd_len = COMMAND_SIZE(cdb[0]);
	if ((cdb[0] | 0x10) == 0xd0)
		scmd->cmd_len = 16;
	memcpy(scmd->cmnd, cdb, scmd->cmd_len);
	scmd->allowed = UFS_HR_RETRIES;
	req->timeout = UFS_HR_TIMEOUT;
	req->rq_flags |= RQF_PM | RQF_QUIET;

	blk_execute_rq(req, true);
	if (unlikely(scmd->resid_len > 0 && scmd->resid_len <= len))
		memset(data + len - scmd->resid_len, 0, scmd->resid_len);
	if (resid)
		*resid = scmd->resid_len;
	if (sshdr)
		scsi_normalize_sense(scmd->sense_buffer, scmd->sense_len,
				     sshdr);
	ret = scmd->result;
out:
	blk_mq_free_request(req);
	return ret;
}

int scsi_ymtc_hr(struct scsi_device *sdev, u8 *data, unsigned int len)
{
	struct scsi_sense_hdr sshdr = { 0 };
	u8 cdb[16] = { 0x3c, 0xc1, 0, 0, 0, 0, 0, 2, 0, 0, 2 };
	int ret;

	if (!data)
		return -EINVAL;

	ret = mi_scsi_exec(sdev, cdb, REQ_OP_DRV_IN, data, len, &sshdr,
			   NULL);
	if (!ret)
		return 0;

	pr_err("ufs: hr read buffer  error 0x%x\n", ret);
	pr_err("sense hr read key:0x%x; asc:0x%x; ascq:0x%x\n",
	       sshdr.sense_key, sshdr.asc, sshdr.ascq);
	return -EIO;
}

static int mi_hr_device_get(struct scsi_device *sdev)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(sdev->host->host_lock, flags);
	ret = scsi_device_get(sdev);
	if (!ret && sdev->sdev_state < 8 &&
	    (BIT(sdev->sdev_state) & 0xd0)) {
		scsi_device_put(sdev);
		pr_err("ufs: scsi device is not available\n");
		ret = -ENODEV;
	}
	spin_unlock_irqrestore(sdev->host->host_lock, flags);
	if (!ret)
		sdev->host->tmf_in_progress = 1;
	return ret;
}

static void mi_hr_device_put(struct scsi_device *sdev)
{
	sdev->host->tmf_in_progress = 0;
	scsi_device_put(sdev);
}

static int samsung_hr(struct scsi_device *sdev, u8 *data)
{
	u8 read_sdr[16] = { 0xc0, 0x40, 0, 0, 1, 0x0c, 0, 0, 0, 0, 0, 0x1c };
	u8 enter[16] = { 0xc0, 0, 0x5c, 0x38, 0x23, 0xae, 0x67, 0x68, 0x72 };
	u8 set_pwd[16] = { 0xc0, 3, 0x67, 0x68, 0x72 };
	u8 read_osv[16] = { 0xc0, 0x40, 0, 0, 1, 0x0a, 0, 0, 0, 0, 0, 0x4c };
	u8 exit[16] = { 0xc0, 1 };
	int ret;

	ret = mi_scsi_exec(sdev, read_sdr, REQ_OP_DRV_IN, data, 0x1c, NULL,
			   NULL);
	if (ret) {
		pr_err("ufs: get hr_inquiry result error 0x%x\n", ret);
		return -EIO;
	}
	ret = mi_scsi_exec(sdev, enter, REQ_OP_DRV_IN, NULL, 0, NULL, NULL);
	if (ret) {
		pr_err("ufs: enter vendor mode fail 0x%x\n", ret);
		pr_err("ufs: enter vendor mode fail, program key and try again\n");
		ret = mi_scsi_exec(sdev, set_pwd, REQ_OP_DRV_IN, NULL, 0, NULL,
				   NULL);
		if (ret) {
			pr_err("ufs: scsi_ss_enter_vendor_mode error 0x%x\n", ret);
			return 0;
		}
		ret = mi_scsi_exec(sdev, enter, REQ_OP_DRV_IN, NULL, 0, NULL,
				   NULL);
		if (ret) {
			pr_err("ufs: enter vendor mode fail 0x%x\n", ret);
			return 0;
		}
	}
	ret = mi_scsi_exec(sdev, read_osv, REQ_OP_DRV_IN, data + 0x80, 0x4c,
			   NULL, NULL);
	if (ret)
		pr_err("ufs: hr read buffer  error 0x%x\n", ret);
	ret = mi_scsi_exec(sdev, exit, REQ_OP_DRV_IN, NULL, 0, NULL, NULL);
	if (ret)
		pr_err("ufs: exit vendor mode fail 0x%x\n", ret);
	return 0;
}

static int read_vendor_hr(struct scsi_device *sdev, struct ufs_hba *hba, u8 *data)
{
	u8 cdb[16] = { 0 };
	u8 payload[44] = { 0xfe, 0x40, 0, 0x10, 1 };
	bool supported;
	int ret;

	supported = !strncmp(sdev->vendor, "WDC", 3) ||
		!strncmp(sdev->vendor, "TOSHIBA", 7) ||
		!strncmp(sdev->vendor, "KIOXIA", 6) ||
		!strncmp(sdev->vendor, "SAMSUNG", 7) ||
		!strncmp(sdev->vendor, "MICRON", 6) ||
		!strncmp(sdev->vendor, "SKhynix", 7) ||
		!strncmp(sdev->vendor, "XBSTOR", 6);
	if (!supported)
		return -EOPNOTSUPP;
	ret = mi_hr_device_get(sdev);
	if (ret)
		return ret;

	if (!strncmp(sdev->vendor, "WDC", 3)) {
		u8 wd_cdb[16] = { 0x3c, 1, 1, 0x7d, 0x9c, 0x69, 0, 2 };
		ret = mi_scsi_exec(sdev, wd_cdb, REQ_OP_DRV_IN, data,
				   UFS_HR_SIZE, NULL, NULL);
		if (ret)
			ret = -EIO;
		goto out;
	}
	if (!strncmp(sdev->vendor, "TOSHIBA", 7) ||
	    !strncmp(sdev->vendor, "KIOXIA", 6)) {
		u8 toshiba_cdb[16] = { 0x12, 0x69, 0xc0, 2 };
		ret = mi_scsi_exec(sdev, toshiba_cdb, REQ_OP_DRV_IN, data,
				   UFS_HR_SIZE, NULL, NULL);
		if (!ret && data[1] != 0xc0)
			pr_err("ufs: hr_inruiry data error\n");
		if (ret)
			ret = -EIO;
		goto out;
	}
	if (!strncmp(sdev->vendor, "SAMSUNG", 7)) {
		ret = samsung_hr(sdev, data);
		goto out;
	}
	if (!strncmp(sdev->vendor, "MICRON", 6)) {
		u8 write_cdb[16] = { 0x3b, 0xe1, 0, 0, 0, 0, 0, 0, 0x2c };
		u8 read_cdb[16] = { 0x3c, 0xc1, 0, 0, 0, 0, 0, 2 };
		ret = mi_scsi_exec(sdev, write_cdb, REQ_OP_DRV_OUT, payload,
				   sizeof(payload), NULL, NULL);
		if (!ret)
			ret = mi_scsi_exec(sdev, read_cdb, REQ_OP_DRV_IN, data,
					   UFS_HR_SIZE, NULL, NULL);
		if (ret)
			ret = -EIO;
		goto out;
	}
	if (!strncmp(sdev->vendor, "SKhynix", 7)) {
		ret = pm_runtime_resume_and_get(hba->dev);
		if (ret < 0)
			goto out;
		ret = ufshcd_read_desc_param_sel(hba, QUERY_DESC_IDN_HEALTH,
				0, 0, 0, data, 0x25);
		pm_runtime_put(hba->dev);
		if (ret)
			goto out;
		cdb[0] = 0xd0;
		cdb[1] = hba->dev_info.wspecversion < 0x400 ? 3 : 0x0f;
		cdb[2] = hba->dev_info.wspecversion < 0x400 ? 0x58 : 0x53;
		cdb[11] = hba->dev_info.wspecversion < 0x400 ? 0x52 : 0x6e;
		ret = mi_scsi_exec(sdev, cdb, REQ_OP_DRV_IN, data + 0x80,
				   cdb[11], NULL, NULL);
		if (ret) {
			pr_err("ufs: ger hr fail fail 0x%x\n", ret);
			ret = 0;
		}
		goto out;
	}
	if (!strncmp(sdev->vendor, "XBSTOR", 6)) {
		ret = scsi_ymtc_hr(sdev, data, UFS_HR_SIZE);
		goto out;
	}
out:
	mi_hr_device_put(sdev);
	return ret;
}

static ssize_t hr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct scsi_device *sdev;
	struct ufs_hba *hba;
	u8 *data;
	ssize_t len = 0;
	u64 lun;
	int ret, i;

	if (!g_ufs_info || !(hba = g_ufs_info->hba))
		return -ENODEV;
	lun = hba->dev_info.wmanufacturerid == 300 ? 2 : 0;
	sdev = mi_scsi_device_lookup(lun);
	if (!sdev)
		return -ENODEV;
	ret = scsi_autopm_get_device(sdev);
	if (ret)
		goto out_put;
	data = kzalloc(UFS_HR_SIZE, GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_pm;
	}
	mutex_lock(&hr_lock);
	ret = read_vendor_hr(sdev, hba, data);
	if (ret == -EOPNOTSUPP)
		len = sysfs_emit(buf, "NOT SUPPORTED %s\n", sdev->vendor);
	else if (ret)
		len = sysfs_emit(buf, "Fail to get hr, err is: %d\n", ret);
	else {
		for (i = 0; i < UFS_HR_SIZE; i++)
			len += sysfs_emit_at(buf, len, "%02x", data[i]);
		len += sysfs_emit_at(buf, len, "\n");
	}
	mutex_unlock(&hr_lock);
	kfree(data);
out_pm:
	scsi_autopm_put_device(sdev);
out_put:
	scsi_device_put(sdev);
	return ret < 0 && !len ? ret : len;
}
static DEVICE_ATTR_RO(hr);

static ssize_t err_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ufs_xiaomi *x = get_ufs_xiaomi();

	return g_ufs_info && x ? sysfs_emit(buf, "%llu\n", x->err_state) : 0;
}
static DEVICE_ATTR_RO(err_state);

static ssize_t err_reason_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct ufs_xiaomi *x = get_ufs_xiaomi();
	ssize_t len = 0;
	int i;

	if (!g_ufs_info || !x)
		return 0;
	for (i = 0; i < ARRAY_SIZE(x->err_reason); i++)
		len += sysfs_emit_at(buf, len, "%s", x->err_reason[i]);
	return len;
}
static DEVICE_ATTR_RO(err_reason);

static struct attribute *ddr_attrs[] = {
	&dev_attr_ddr_size.attr, &dev_attr_ddr_id.attr,
	&dev_attr_ddr_vendor.attr, &dev_attr_ddr_type.attr, NULL,
};
static const struct attribute_group ddr_group = { .name = "ddr", .attrs = ddr_attrs };
static struct attribute *ufshcd_attrs[] = {
	&dev_attr_dump_health_desc.attr, &dev_attr_dump_string_desc_serial.attr,
	&dev_attr_dump_device_desc.attr, &dev_attr_show_hba.attr,
	&dev_attr_hr.attr, &dev_attr_err_state.attr, &dev_attr_err_reason.attr, NULL,
};
static const struct attribute_group ufshcd_group = { .name = "ufshcd0", .attrs = ufshcd_attrs };
static const struct attribute_group *memory_sysfs_groups[] = {
	&ddr_group, &ufshcd_group, NULL,
};

static const struct file_operations mem_ops = { .owner = THIS_MODULE };

static int __init mi_memory_init(void)
{
	int ret;

	mi_memory = kzalloc(sizeof(*mi_memory), GFP_KERNEL);
	if (!mi_memory)
		return -ENOMEM;
	mi_memory->class = class_create(MI_MEMORY_NAME);
	if (IS_ERR(mi_memory->class)) {
		ret = PTR_ERR(mi_memory->class);
		goto free_dev;
	}
	mi_memory->major = register_chrdev(0, "mi_memory_module", &mem_ops);
	if (mi_memory->major < 0) {
		ret = mi_memory->major;
		goto destroy_class;
	}
	mi_memory->device = device_create(mi_memory->class, NULL,
		MKDEV(mi_memory->major, 1), NULL, "mi_memory_device");
	if (IS_ERR(mi_memory->device)) {
		ret = PTR_ERR(mi_memory->device);
		goto unregister_chrdev;
	}
	ret = sysfs_create_groups(&mi_memory->device->kobj, memory_sysfs_groups);
	if (ret)
		goto destroy_device;
	mi_memory->proc = proc_create("mv", 0555, NULL, &memory_procfs_fops);
	if (!mi_memory->proc) {
		ret = -ENOMEM;
		goto remove_groups;
	}
	mi_memory->dram = init_dram_info();
	g_ufs_info = &ufs_info;
	INIT_WORK(&mi_memory->ufs_work, ufs_info_init_work);
	schedule_work(&mi_memory->ufs_work);
	mi_memory->debugfs_link = debugfs_create_symlink("ufshcd0", NULL,
		"../../class/mi_memory/mi_memory_device/ufshcd0");
	return 0;
remove_groups:
	sysfs_remove_groups(&mi_memory->device->kobj, memory_sysfs_groups);
destroy_device:
	device_destroy(mi_memory->class, MKDEV(mi_memory->major, 1));
unregister_chrdev:
	unregister_chrdev(mi_memory->major, "mi_memory_module");
destroy_class:
	class_destroy(mi_memory->class);
free_dev:
	kfree(mi_memory);
	mi_memory = NULL;
	return ret;
}

static void __exit mi_memory_exit(void)
{
	struct ufs_xiaomi *xiaomi;

	if (!mi_memory)
		return;
	xiaomi = get_ufs_xiaomi();
	if (xiaomi)
		complete_all(&xiaomi->ufs_xiaomi_comp);
	cancel_work_sync(&mi_memory->ufs_work);
	debugfs_remove(mi_memory->debugfs_link);
	proc_remove(mi_memory->proc);
	sysfs_remove_groups(&mi_memory->device->kobj, memory_sysfs_groups);
	device_destroy(mi_memory->class, MKDEV(mi_memory->major, 1));
	unregister_chrdev(mi_memory->major, "mi_memory_module");
	class_destroy(mi_memory->class);
	if (ufs_info.sdev)
		scsi_device_put(ufs_info.sdev);
	kfree(ufs_info.model);
	kfree(ufs_info.revision);
	free_dram_info(&dram_info);
	kfree(mi_memory);
	mi_memory = NULL;
	g_ufs_info = NULL;
	g_dram_info = NULL;
}

module_init(mi_memory_init);
module_exit(mi_memory_exit);

MODULE_DESCRIPTION("Xiaomi Memory Debug Interface");
MODULE_LICENSE("GPL");
