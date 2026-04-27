/*
 * Copyright (C) 2024 MiBeeHomeCam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief 录像文件信息结构体，用于API返回文件列表
 *
 * name:     文件相对路径（如 2026-04/24/REC_xxx.avi）
 * size:     文件大小（字节）
 * time_str: 最后修改时间字符串（格式：YYYY-MM-DD HH:MM:SS）
 */
typedef struct {
    char name[64];
    uint32_t size;
    char time_str[32];
} file_info_t;

/**
 * @brief SD卡存储信息结构体
 *
 * total_bytes: SD卡总容量（字节）
 * free_bytes:  SD卡剩余空间（字节）
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
} storage_info_t;
/** @brief 初始化SD卡，配置1线SDMMC模式并挂载FAT文件系统 */
esp_err_t storage_init(void);
/** @brief 获取SD卡剩余空间占总空间的百分比 */
float storage_get_free_percent(void);
/** @brief 获取SD卡存储信息（总容量、剩余空间） */
esp_err_t storage_get_info(storage_info_t *info);
/** @brief 获取录像文件列表（从内存缓存，毫秒级响应） */
int storage_list_files(file_info_t *files, int max_count);
/** @brief 删除最旧的录像文件 */
esp_err_t storage_delete_oldest(void);
/** @brief 存储空间自动清理，低于20%时删除旧文件直到30%以上 */
esp_err_t storage_cleanup(void);
/** @brief 检查SD卡是否可用 */
bool storage_is_available(void);
/** @brief 检查SD卡是否仍然挂载 */
esp_err_t storage_check(void);           // Check SD still mounted
/** @brief 卸载并重新挂载SD卡（用于热插拔恢复） */
esp_err_t storage_remount(void);         // Unmount + remount SD card
/** @brief 格式化SD卡（擦除所有数据后重新创建FAT32文件系统） */
esp_err_t storage_format(void);
void storage_set_unavailable(void);      // Mark SD as removed
/** @brief 注册完成的录像文件到内存缓存（零 SD 卡 I/O） */
void storage_register_file(const char *filepath, size_t size);
/** @brief 从内存缓存移除已删除的文件 */
void storage_unregister_file(const char *name);
