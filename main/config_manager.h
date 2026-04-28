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

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** @brief 摄像头全局配置结构体，持久化到 NVS 闪存 */
typedef struct {
    char wifi_ssid[33];
    // WiFi 路由器 SSID 名称
    char wifi_pass[64];
    // WiFi 路由器密码
    char ftp_host[64];
    // FTP 服务器地址
    uint16_t ftp_port;
    // FTP 服务器端口
    char ftp_user[32];
    // FTP 登录用户名
    char ftp_pass[32];
    // FTP 登录密码
    char ftp_path[128];
    // FTP 上传目录路径
    bool ftp_enabled;
    // 是否启用 FTP 上传
    char webdav_url[128];
    // WebDAV 服务器 URL
    char webdav_user[32];
    // WebDAV 登录用户名
    char webdav_pass[32];
    // WebDAV 登录密码
    bool webdav_enabled;
    // 是否启用 WebDAV 上传
    uint8_t resolution;    // 0=VGA, 1=SVGA, 2=XGA
    // 摄像头分辨率：0=VGA, 1=SVGA, 2=XGA
    uint8_t fps;            // 1-30
    // 帧率，范围 1-30
    uint16_t segment_sec;   // seconds per segment
    // 录像分段时长（秒）
    uint8_t jpeg_quality;   // 1-63
    // JPEG 压缩质量，范围 1-63
    char web_password[32];
    // Web 管理界面登录密码
    char device_name[32];
    // 设备名称
    char timezone[48];
    // 时区设置，POSIX格式（如 CST-8、UTC0、EST5EDT）
} cam_config_t;

/** @brief 初始化配置模块，从 NVS 加载配置，无存储则使用默认值 */
esp_err_t config_init(void);         // Load from NVS, apply defaults if empty
/** @brief 获取当前配置指针（指向 PSRAM 中的全局配置） */
cam_config_t* config_get(void);      // Returns pointer to current config in PSRAM
/** @brief 将当前配置保存到 NVS 闪存 */
esp_err_t config_save(void);         // Save current config to NVS
/** @brief 恢复出厂默认配置并保存 */
esp_err_t config_reset(void);        // Restore factory defaults
/** @brief 从 SD 卡读取 wifi.txt 和 nas.txt 配置文件并覆盖当前配置 */
esp_err_t config_load_from_sd(void); // Read config/wifi.txt + config/nas.txt from SD
