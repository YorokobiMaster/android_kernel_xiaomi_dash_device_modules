// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 XiaoMi, Inc.
 *               2022 The LineageOS Project
 * Modifications Copyright (C) 2026 YorokobiMaster (GitHub: @YorokobiMaster)
 */

#include <linux/hwid.h>
#include <linux/init.h>
#include <linux/module.h>

#define HW_MAJOR_VERSION_SHIFT		16
#define HW_MINOR_VERSION_SHIFT		0
#define HW_COUNTRY_VERSION_SHIFT	20
#define HW_BUILD_VERSION_SHIFT		16
#define HW_MAJOR_VERSION_MASK		0xffff0000
#define HW_MINOR_VERSION_MASK		0x0000ffff
#define HW_COUNTRY_VERSION_MASK		0xfff00000
#define HW_BUILD_VERSION_MASK		0x000f0000

static uint hwid_value;
module_param(hwid_value, uint, 0444);
MODULE_PARM_DESC(hwid_value,
		 "xiaomi hwid value correspondingly different build");

static uint project;
module_param(project, uint, 0444);
MODULE_PARM_DESC(project, "xiaomi project serial num predefine");

static char *project_name;
module_param(project_name, charp, 0444);
MODULE_PARM_DESC(project_name, "xiaomi project name predefine");

static uint build_adc;
module_param(build_adc, uint, 0444);
MODULE_PARM_DESC(build_adc, "xiaomi adc value of build resistance");

static uint project_adc;
module_param(project_adc, uint, 0444);
MODULE_PARM_DESC(project_adc, "xiaomi adc value of project resistance");

uint32_t get_hw_version_platform(void)
{
	return project;
}
EXPORT_SYMBOL(get_hw_version_platform);

const char *product_name_get(void)
{
	return project_name;
}
EXPORT_SYMBOL(product_name_get);

uint32_t get_hw_id_value(void)
{
	return hwid_value;
}
EXPORT_SYMBOL(get_hw_id_value);

uint32_t get_hw_country_version(void)
{
	return (hwid_value & HW_COUNTRY_VERSION_MASK) >>
		HW_COUNTRY_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_country_version);

uint32_t get_hw_version_major(void)
{
	return (hwid_value & HW_MAJOR_VERSION_MASK) >> HW_MAJOR_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_major);

uint32_t get_hw_version_minor(void)
{
	return (hwid_value & HW_MINOR_VERSION_MASK) >> HW_MINOR_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_minor);

uint32_t get_hw_version_build(void)
{
	return (hwid_value & HW_BUILD_VERSION_MASK) >> HW_BUILD_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_build);

static int __init hwid_init(void)
{
	return 0;
}

static void __exit hwid_exit(void)
{
}

module_init(hwid_init);
module_exit(hwid_exit);

MODULE_AUTHOR("weixiaotian1@xiaomi.com");
MODULE_DESCRIPTION("Hwid Module Driver for Xiaomi Corporation");
MODULE_LICENSE("GPL v2");
