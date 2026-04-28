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

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

/**
 * @brief 启动 SNTP 时间同步（需在 WiFi 连接后调用）
 * 阻塞等待最多 5 秒获取首次同步，超时后异步继续同步
 * 使用 cn.pool.ntp.org 和 ntp.aliyun.com 作为 NTP 服务器
 * @return ESP_OK 成功
 */
esp_err_t time_sync_init(void);

/** @brief 检查时间是否已通过 NTP 或手动设置成功同步 */
bool time_is_synced(void);

/** @brief 获取当前时间字符串，格式为 "YYYY-MM-DD HH:MM:SS" */
void time_get_str(char *buf, size_t len);

/** @brief 应用配置中的时区设置（通过 setenv TZ + tzset） */
void time_sync_apply_timezone(const char *tz);
/** @brief 手动设置系统时间（在无 NTP 服务时使用） */
esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec);
