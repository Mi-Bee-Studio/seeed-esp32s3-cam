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
#include "cJSON.h"
#include "sdkconfig.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 家族配置契约 v1.0（docs/config-contract.md）。
 * 持久化 = mibee_cfg 命名空间逐键 NVS + schema_ver 版本键（见 config_manager.c）；
 * 本常量是家族 schema 版本，独立于本仓历史 schema v1/v2 编号（迁移后一律 =1）。 */
#define CONFIG_SCHEMA_VERSION 1

/* 契约 v1.1：家族统一默认管理密码（首次启动/空密码迁移/一次性种子共用；公开默认 mibeecam2026，本地可覆盖） */
#define CONFIG_DEFAULT_WEB_PASSWORD CONFIG_MIBEE_CAM_DEFAULT_WEB_PASSWORD

/** @brief 摄像头全局配置结构体，持久化到 NVS 闪存
 *
 * 字段命名与 docs/config-contract.md §3 对齐（JSON 名 = 结构体名，NVS 键见
 * config_manager.c 的 KEY_ASSERT 表）。分辨率刻度 = esp32-camera framesize_t
 * （VGA=10 … UXGA=15，契约 v1.3 §5）。
 */
typedef struct {
    char wifi_ssid[33];
    // WiFi 路由器 SSID 名称
    char wifi_pass[64];
    // WiFi 路由器密码
    char wifi_ssid_2[33];
    // WiFi 备用路由器 SSID 名称
    char wifi_pass_2[64];
    // WiFi 备用路由器密码
    bool allow_ap_fallback;
    // 是否允许退回到 AP 模式（板级覆盖默认 false，永不退回 AP）
    char device_name[32];
    // 设备名称（家族默认 MiBeeCam）
    char timezone[48];
    // 时区设置，POSIX格式（如 CST-8、UTC0、EST5EDT）
    char web_password[32];
    // Web 管理界面登录密码
    char rtsp_user[33];
    // RTSP digest 鉴权用户名（契约 §3.2 rtsp 组，默认 admin）
    char rtsp_pass[64];
    // RTSP digest 鉴权密码（敏感字段，GET 掩码；存量迁移一次性种子 = web_password）
    uint8_t cam_framesize;  // framesize_t 刻度：10=VGA,11=SVGA,12=XGA,13=HD,14=SXGA,15=UXGA
    uint8_t cam_fps;        // 1-30（板级覆盖默认 12）
    uint8_t cam_quality;    // 10-63（板级覆盖默认 18）
    uint16_t segment_sec;   // seconds per segment
    bool cam_vflip;          // 垂直翻转（上下翻转）
    bool cam_hmirror;        // 水平镜像（左右翻转）
    int8_t cam_brightness;   // 摄像头亮度：-2..+2
    int8_t cam_contrast;     // 摄像头对比度：-2..+2
    int8_t cam_saturation;   // 摄像头饱和度：-2..+2
    int8_t cam_sharpness;    // 摄像头锐度：-2..+2
    uint8_t day_night_mode;   // 日夜模式：0=彩色, 1=黑白, 2=自动(预留)
    uint8_t onvif_enable;     /* 契约核心字段：0=关闭 ONVIF SOAP 注册+WS-Discovery（重启生效），默认 1 */
    uint8_t xclk_freq_mhz;    /* 摄像头 XCLK 频率 MHz，可选 10/16/20，默认 16（板值） */
    int8_t  wifi_roam_rssi;   /* WiFi 漫游 RSSI 阈值，板级覆盖默认 -75，0=禁用 */
    uint8_t wifi_roam_gap_s;  /* 漫游切换信号差值 dBm，5-15，默认 10 */
    uint8_t cleanup_low_pct;   // 存储空间清理下限百分比（空闲%低于此值开始清理）
    uint8_t cleanup_high_pct;  // 存储空间清理上限百分比（清理到空闲%高于此值停止）
    bool frame_drop_enabled;   // 写入积压时是否自动丢帧（默认 true）
    bool video_record_to_sd;     // 是否录制视频到 SD 卡（默认 true）
    bool record_on_boot;         // 开机自动录像（默认 true，家族传统）；false=完全手动
    bool audio_record_to_sd;     // 是否录制音频到 SD 卡（默认 false，合并进视频音轨）
    bool sd_log_enabled;         // 是否启用 SD 卡日志（默认 false，/sdcard/logs/）
    bool webdav_enabled;         // 是否启用 WebDAV 上传
    char webdav_url[128];       // WebDAV 服务器 URL
    char webdav_user[32];
    // WebDAV 登录用户名
    char webdav_pass[64];
    char webdav_base_path[128];  // 上传基础路径（拒绝 ..）
    char alert_webhook_url[256];  // Webhook 告警通知 URL
    bool alert_webhook_enabled;   // 是否启用 Webhook 告警通知
    /* 家族 motion 超集模型（契约 §3.2） */
    bool motion_enabled;             // 移动侦测总开关（默认 1；0=不出 WS 触发事件）
    uint8_t motion_sensitivity;      // 0-100（越大越灵敏）
    uint16_t motion_cooldown_s;      // 1-300，两次触发最小间隔（原 motion_idle_interval_sec 直迁）
    uint8_t motion_active_interval_s; // 1-30，持续活动期再触发间隔
    /* 家族延时摄影动态模型 8 字段（契约 §3.2，运行时见 video_recorder.c） */
    bool timelapse_enabled;           // 延时录像总开关（0=连续录像）
    uint16_t timelapse_interval_s;    // 静态模式固定间隔（1-255）
    uint8_t timelapse_burst_count;    // 突发连拍张数（保留字段，本板录像模式暂不使用）
    uint8_t timelapse_mode;           // 0=静态（固定间隔） 1=动态（随运动衰减）
    uint16_t timelapse_min_interval_s;  // 动态模式：有运动时间隔（默认 3）
    uint16_t timelapse_max_interval_s;  // 动态模式：无运动衰减上限（默认 300）
    uint8_t timelapse_decay_factor;     // 每衰减周期间隔倍率（默认 2）
    uint16_t timelapse_decay_period_s;  // 无运动多少秒后衰减一步（默认 10）
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
/** @brief 获取 Web 管理界面登录密码 */
const char *config_get_web_password(void);
/** @brief 生成当前配置的 JSON 对象（调用方负责 cJSON_Delete 释放） */
cJSON *config_get_json(void);
#ifdef __cplusplus
}
#endif
