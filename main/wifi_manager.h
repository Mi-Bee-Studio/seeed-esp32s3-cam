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
#include <stdint.h>

/** @brief WiFi 热点扫描结果信息结构体 */
typedef struct {
    char ssid[33];
    // WiFi SSID 名称
    int8_t rssi;
    // WiFi 信号强度
    uint8_t auth_mode;  // wifi_auth_mode_t value
    // 认证模式（wifi_auth_mode_t 枚举值）
} wifi_ap_info_t;

/** @brief WiFi 连接状态枚举 */
typedef enum {
    WIFI_STATE_AP,                  // AP mode active (no STA config)
                              // AP 热点模式已启动（无 STA 配置）
    WIFI_STATE_STA_CONNECTING,
                              // STA 正在连接中
    WIFI_STATE_STA_CONNECTED,
                              // STA 已连接并获得 IP
    WIFI_STATE_STA_DISCONNECTED,
                              // STA 连接断开
} wifi_state_t;

/** @brief WiFi 模块初始化，根据配置自动选择 AP 或 STA 模式 */
esp_err_t wifi_init(void);          // Decides AP/STA based on config
/** @brief 获取当前 WiFi 连接状态 */
wifi_state_t wifi_get_state(void);
/** @brief 获取当前 IP 地址字符串（静态缓冲区） */
char *wifi_get_ip_str(void);        // Returns current IP string (static buffer)
/** @brief 启动 WiFi AP 热点模式 */
esp_err_t wifi_start_ap(void);
/** @brief 启动 WiFi STA 客户端模式连接路由器 */
esp_err_t wifi_start_sta(void);
/** @brief 扫描周围 WiFi 热点，返回找到的数量，失败返回 -1 */
int wifi_scan(wifi_ap_info_t *aps, int max_count);   // Returns number of APs found, or -1 on error
/** @brief 判断当前是否处于 STA 模式（正在连接或已连接） */
bool wifi_is_sta(void);             // true if in STA mode (or trying)
