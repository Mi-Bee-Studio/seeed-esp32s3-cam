/*
 * Copyright (C) 2024 MiBee Cam Authors
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

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 摄像头全局配置结构体，持久化到 NVS 闪存 */
typedef struct {
    char wifi_ssid[33];
    // WiFi 路由器 SSID 名称
    char wifi_pass[64];
    // WiFi 路由器密码
    bool webdav_enabled;
    char wifi_ssid_2[33];
    // WiFi 备用路由器 SSID 名称
    char wifi_pass_2[64];
    // WiFi 备用路由器密码
    bool allow_ap_fallback;
    // 是否允许退回到 AP 模式（默认 false，永不退回 AP）
    // 是否启用 WebDAV 上传
    char upload_base_path[128];  // 上传基础路径
    char webdav_url[128];       // WebDAV 服务器 URL
    char webdav_user[32];
    // WebDAV 登录用户名
    char webdav_pass[32];
    uint8_t resolution;    // 0=VGA, 1=SVGA, 2=XGA
    // 摄像头分辨率：0=VGA, 1=SVGA, 2=XGA
    uint8_t fps;            // 1-30
    // 帧率，范围 1-30
    uint16_t segment_sec;   // seconds per segment
    // 录像分段时长（秒）
    uint8_t jpeg_quality;   // 1-63
    // JPEG 压缩质量，范围 1-63
    bool vflip;             // 垂直翻转（上下翻转）
    bool hmirror;            // 水平镜像（左右翻转）
    char web_password[32];
    // Web 管理界面登录密码
    char device_name[32];
    // 设备名称
    char timezone[48];
    // 时区设置，POSIX格式（如 CST-8、UTC0、EST5EDT）
    uint8_t cleanup_low_pct;   // 存储空间清理下限百分比（低于此值开始清理）
    uint8_t cleanup_high_pct;  // 存储空间清理上限百分比（清理到此值停止）
    bool frame_drop_enabled;   // 写入积压时是否自动丢帧（默认 true）
    char alert_webhook_url[256];  // Webhook 告警通知 URL
    bool alert_webhook_enabled;   // 是否启用 Webhook 告警通知
    uint8_t timelapse_interval_sec; // Timelapse interval in seconds (0=disabled, >0=interval)
    uint8_t timelapse_mode;              // 0=continuous, 1=normal timelapse, 2=dynamic timelapse
    uint8_t motion_sensitivity;          // Motion sensitivity 0-100 (higher=more sensitive)
    uint8_t motion_active_interval_sec;  // Dynamic: interval when motion detected (1-30s)
    uint8_t motion_idle_interval_sec;    // Dynamic: interval when no motion (5-300s)
    bool video_record_to_sd;     // 是否录制视频到 SD 卡（默认 true）
    bool audio_record_to_sd;     // 是否录制音频到 SD 卡（默认 false，合并进视频音轨）
    bool sd_log_enabled;         // 是否启用 SD 卡日志（默认 false，/sdcard/logs/）
    uint8_t day_night_mode;   // 日夜模式：0=彩色, 1=黑白, 2=自动(预留)
} cam_config_t;

/** @brief 初始化配置模块，从 NVS 加载配置，无存储则使用默认值 */
esp_err_t config_init(void);         // Load from NVS, apply defaults if empty
/** @brief 获取当前配置指针（指向 PSRAM 中的全局配置） */
cam_config_t* config_get(void);      // Returns pointer to current config in PSRAM
/** @brief 将当前配置保存到 NVS 闪存 */
esp_err_t config_save(void);         // Save current config to NVS
/** @brief 恢复出厂默认配置并保存 */
esp_err_t config_reset(void);        // Restore factory defaults
/** @brief 从 SD 卡读取 wifi.txt、config.txt 和 nas.txt 配置文件并覆盖当前配置 */
esp_err_t config_load_from_sd(void); // Read config/wifi.txt + config/nas.txt from SD

/** @brief 锁定配置互斥体（在修改配置前使用） */
void config_lock(void);
/** @brief 解锁配置互斥体（在修改配置完成后使用） */
void config_unlock(void);

/** @brief 线程安全复制当前配置到 caller 提供的缓冲区 */
esp_err_t config_get_copy(cam_config_t *out);
/** @brief 验证配置结构体所有字段是否在合法范围内 */
bool config_validate(const cam_config_t *cfg);
/** @brief 根据拍摄间隔和录制模式计算最优片段时长和帧率，覆盖 fps 和 segment_sec 字段 */
void config_apply_optimal(cam_config_t *cfg);

#ifdef __cplusplus
}
#endif
