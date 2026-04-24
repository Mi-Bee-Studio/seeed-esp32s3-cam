/*
 * Copyright (C) 2024 ParrotCam Authors
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

typedef enum {
    LED_STARTING,         // 系统启动中，常亮
    LED_AP_MODE,          // AP 模式，慢闪（1秒周期）
    LED_WIFI_CONNECTING,  // WiFi 连接中，快闪（200ms周期）
    LED_RUNNING,          // 正常运行，LED 熄灭
    LED_ERROR             // 错误状态，双闪
} led_status_t;

/**
 * @brief 初始化状态 LED GPIO 和软件定时器
 * 配置 GPIO21 为输出模式（低电平有效），创建 FreeRTOS 定时器
 * 启动时默认进入 LED_STARTING 状态（常亮）
 * @return ESP_OK 成功，ESP_FAIL 或 ESP_ERR_NO_MEM 失败
 */
esp_err_t led_init(void);
/**
 * @brief 设置 LED 状态显示模式
 * 根据不同状态控制 LED 亮灭和闪烁模式：
 *   - STARTING: 常亮
 *   - AP_MODE: 慢闪（1秒周期）
 *   - WIFI_CONNECTING: 快闪（200ms周期）
 *   - RUNNING: 熄灭
 *   - ERROR: 双闪模式
 * @param status 目标 LED 状态
 */
void led_set_status(led_status_t status);
