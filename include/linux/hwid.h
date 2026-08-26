/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 XiaoMi, Inc.
 *               2022 The LineageOS Project
 * Modifications Copyright (C) 2026 YorokobiMaster (GitHub: @YorokobiMaster)
 */

#ifndef __HWID_H__
#define __HWID_H__

#include <linux/types.h>

uint32_t get_hw_version_platform(void);
const char *product_name_get(void);
uint32_t get_hw_id_value(void);
uint32_t get_hw_country_version(void);
uint32_t get_hw_version_major(void);
uint32_t get_hw_version_minor(void);
uint32_t get_hw_version_build(void);

#endif /* __HWID_H__ */
