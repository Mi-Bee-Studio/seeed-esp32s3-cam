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

/*
 * config_manager.c — 家族配置契约 v1.0（docs/config-contract.md）
 *
 * 持久化：mibee_cfg 命名空间逐键 NVS + schema_ver 版本键（2026-09-05 起，
 * 取代旧的 cam_config 命名空间逐键方案）。单键自治：读缺键=默认值；写单键
 * 失败只 WARN 不中断（PIT-022）。所有 NVS 键 ≤15 字符，构建期断言。
 *
 * 迁移（契约 §6 seeed 行，幂等可重入）：mibee_cfg 无 schema_ver 且旧
 * cam_config 命名空间存在 → 读旧键（含 v3 字段改名、FTP→webdav、
 * timelapse_mode 兼容、密码一次性种子等全部历史迁移语义）→ 按契约映射
 * （分辨率刻度 0-5→framesize_t 10-15、motion/timelapse 模型收敛、
 * rtsp_pass 一次性种子=web_password）→ 写入 mibee_cfg。旧命名空间
 * 原样留存只读（回滚备份）。
 */

#include "config_manager.h"
#include "camera_driver.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";

#define NVS_NS         "mibee_cfg"    /* 家族统一命名空间（契约 §1） */
#define NVS_NS_LEGACY  "cam_config"   /* 本仓旧命名空间（迁移后留存只读） */
#define KEY_SCHEMA_VER "schema_ver"
#define KEY_PW_SEED    "pw_seed_v1"   /* 契约 v1.1 密码一次性种子标记（跨命名空间生效） */

static cam_config_t s_config;
static SemaphoreHandle_t s_config_mutex = NULL;

/* ──────────────────────────────────────────────────────────────────
 * NVS 键长度断言（契约 §1：≤15 字符；PIT-022 超长键教训）
 * ────────────────────────────────────────────────────────────────── */
#define KEY_ASSERT(k) _Static_assert(sizeof(k) <= 16, "NVS key too long: " k)
KEY_ASSERT("schema_ver");
KEY_ASSERT("device_name");
KEY_ASSERT("wifi_ssid");
KEY_ASSERT("wifi_pass");
KEY_ASSERT("wifi_ssid_2");
KEY_ASSERT("wifi_pass_2");
KEY_ASSERT("ap_fallback");
KEY_ASSERT("timezone");
KEY_ASSERT("web_password");
KEY_ASSERT("cam_framesize");
KEY_ASSERT("cam_fps");
KEY_ASSERT("cam_quality");
KEY_ASSERT("cam_vflip");
KEY_ASSERT("cam_hmirror");
KEY_ASSERT("cam_brightness");
KEY_ASSERT("cam_contrast");
KEY_ASSERT("cam_saturation");
KEY_ASSERT("cam_sharpness");
KEY_ASSERT("day_night");
KEY_ASSERT("onvif_enable");
KEY_ASSERT("rtsp_user");
KEY_ASSERT("rtsp_pass");
KEY_ASSERT("xclk_mhz");
KEY_ASSERT("wifi_roam_rssi");
KEY_ASSERT("wifi_roam_gap");
KEY_ASSERT("cleanup_low");
KEY_ASSERT("cleanup_high");
KEY_ASSERT("frame_drop_en");
KEY_ASSERT("segment_sec");
KEY_ASSERT("video_to_sd");
KEY_ASSERT("record_on_boot");
KEY_ASSERT("audio_to_sd");
KEY_ASSERT("sd_log_en");
KEY_ASSERT("webdav_en");
KEY_ASSERT("webdav_url");
KEY_ASSERT("webdav_user");
KEY_ASSERT("webdav_pass");
KEY_ASSERT("webdav_base");
KEY_ASSERT("webhook_en");
KEY_ASSERT("webhook_url");
KEY_ASSERT("motion_en");
KEY_ASSERT("motion_sens");
KEY_ASSERT("motion_cool_s");
/* 契约 §3.2 括注键 motion_act_int_s 实为 16 字符，超 NVS 15 字符上限
 * （PIT-022 同类），与 ai-thinker 参考实现一致改用 motion_act_s */
KEY_ASSERT("motion_act_s");
KEY_ASSERT("tl_en");
KEY_ASSERT("tl_int_s");
KEY_ASSERT("tl_burst");
KEY_ASSERT("tl_mode");
KEY_ASSERT("tl_min_int_s");
KEY_ASSERT("tl_max_int_s");
KEY_ASSERT("tl_decay_f");
KEY_ASSERT("tl_decay_p_s");
KEY_ASSERT("pw_seed_v1");

/* ──────────────────────────────────────────────────────────────────
 * 逐键读写辅助（缺键=保持默认；写失败只 WARN，PIT-022）
 * ────────────────────────────────────────────────────────────────── */
static bool rd_str(nvs_handle_t h, const char *key, char *out, size_t outsz)
{
    size_t len = outsz;
    return nvs_get_str(h, key, out, &len) == ESP_OK;
}
static bool rd_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    return nvs_get_u8(h, key, out) == ESP_OK;
}
static bool rd_u16(nvs_handle_t h, const char *key, uint16_t *out)
{
    return nvs_get_u16(h, key, out) == ESP_OK;
}
static bool rd_i8(nvs_handle_t h, const char *key, int8_t *out)
{
    return nvs_get_i8(h, key, out) == ESP_OK;
}
static void wr_str(nvs_handle_t h, const char *key, const char *val)
{
    esp_err_t ret = nvs_set_str(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(ret));
}
static void wr_u8(nvs_handle_t h, const char *key, uint8_t val)
{
    esp_err_t ret = nvs_set_u8(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_u8(%s) failed: %s", key, esp_err_to_name(ret));
}
static void wr_u16(nvs_handle_t h, const char *key, uint16_t val)
{
    esp_err_t ret = nvs_set_u16(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_u16(%s) failed: %s", key, esp_err_to_name(ret));
}
static void wr_i8(nvs_handle_t h, const char *key, int8_t val)
{
    esp_err_t ret = nvs_set_i8(h, key, val);
    if (ret != ESP_OK) ESP_LOGW(TAG, "nvs_set_i8(%s) failed: %s", key, esp_err_to_name(ret));
}

/* ──────────────────────────────────────────────────────────────────
 * 默认值（契约 §3 家族默认 + §5 seeed 板级覆盖）
 * ────────────────────────────────────────────────────────────────── */
static void apply_defaults(cam_config_t *cfg)
{
    memset(cfg, 0, sizeof(cam_config_t));
    strlcpy(cfg->device_name, "MiBeeCam", sizeof(cfg->device_name));
    cfg->wifi_ssid[0] = '\0';
    cfg->wifi_pass[0] = '\0';
    cfg->wifi_ssid_2[0] = '\0';
    cfg->wifi_pass_2[0] = '\0';
    cfg->allow_ap_fallback = false;      /* 板级覆盖（§5）：保留现行为，永不退 AP */
    strlcpy(cfg->timezone, "CST-8", sizeof(cfg->timezone));
    strlcpy(cfg->web_password, CONFIG_DEFAULT_WEB_PASSWORD, sizeof(cfg->web_password));
    strlcpy(cfg->rtsp_user, "admin", sizeof(cfg->rtsp_user));
    strlcpy(cfg->rtsp_pass, CONFIG_DEFAULT_WEB_PASSWORD, sizeof(cfg->rtsp_pass));
    /* 板级覆盖（§5，OV5640 热调优 PIT-016）：SVGA / fps12 / q18 */
    cfg->cam_framesize = CAMERA_RES_SVGA;
    cfg->cam_fps = 12;
    cfg->cam_quality = 18;
    cfg->cam_vflip = false;
    cfg->cam_hmirror = false;
    cfg->cam_brightness = 0;
    cfg->cam_contrast = 0;
    cfg->cam_saturation = 0;
    cfg->cam_sharpness = 0;
    cfg->day_night_mode = 0;
    cfg->onvif_enable = 1;               /* 契约核心字段；本板历史始终开启 */
    cfg->xclk_freq_mhz = 16;             /* 板值（§5） */
    cfg->wifi_roam_rssi = -75;           /* 板级覆盖（§5） */
    cfg->wifi_roam_gap_s = 10;
    cfg->cleanup_low_pct = 20;
    cfg->cleanup_high_pct = 30;
    cfg->frame_drop_enabled = true;
    cfg->segment_sec = 300;
    cfg->video_record_to_sd = true;
    cfg->record_on_boot = true;
    cfg->audio_record_to_sd = false;
    cfg->sd_log_enabled = false;
    cfg->webdav_enabled = false;
    cfg->webdav_url[0] = '\0';
    cfg->webdav_user[0] = '\0';
    cfg->webdav_pass[0] = '\0';
    strlcpy(cfg->webdav_base_path, "/MiBee Cam", sizeof(cfg->webdav_base_path));
    cfg->alert_webhook_enabled = false;
    cfg->alert_webhook_url[0] = '\0';
    /* 家族 motion 超集（契约 §3.2）；灵敏度保留本板历史默认 50 */
    cfg->motion_enabled = true;
    cfg->motion_sensitivity = 50;
    cfg->motion_cooldown_s = 30;
    cfg->motion_active_interval_s = 1;
    /* 家族延时摄影动态模型 8 字段（契约 §3.2，家族默认） */
    cfg->timelapse_enabled = false;      /* 默认连续录像 */
    cfg->timelapse_interval_s = 30;
    cfg->timelapse_burst_count = 3;
    cfg->timelapse_mode = 0;             /* 0=静态 1=动态 */
    cfg->timelapse_min_interval_s = 3;
    cfg->timelapse_max_interval_s = 300;
    cfg->timelapse_decay_factor = 2;
    cfg->timelapse_decay_period_s = 10;
}

/* ── 旧分辨率刻度 0-5 → 家族 framesize_t 刻度（契约 v1.3 §5 迁移表） ── */
static uint8_t legacy_scale_to_framesize(uint8_t legacy)
{
    /* 6/7（FHD/QXGA）为历史测试残留，本板热限 UXGA（PIT-016）→ 钳到 UXGA */
    if (legacy > 5) legacy = 5;
    switch (legacy) {
        case 0:  return 10;  /* VGA  */
        case 1:  return 11;  /* SVGA */
        case 2:  return 12;  /* XGA  */
        case 3:  return 13;  /* HD   */
        case 4:  return 14;  /* SXGA */
        case 5:  return 15;  /* UXGA */
        default: return 11;  /* SVGA（本板默认档） */
    }
}

/* ──────────────────────────────────────────────────────────────────
 * 旧命名空间（cam_config）读取 + 全部历史迁移语义 + 契约映射
 * ────────────────────────────────────────────────────────────────── */

/* 读取旧命名空间全部键值到契约结构体（含 v3 字段改名回退、FTP→webdav、
 * timelapse_mode 兼容、刻度翻译、motion/timelapse 模型收敛 §6.1）。
 * 旧命名空间保持只读，不做任何写入。 */
static void load_legacy_namespace(nvs_handle_t h, cam_config_t *cfg)
{
    uint8_t u8v = 0;
    uint16_t u16v = 0;

    /* ---- 身份 / WiFi ---- */
    rd_str(h, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    rd_str(h, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    rd_str(h, "wifi_ssid_2", cfg->wifi_ssid_2, sizeof(cfg->wifi_ssid_2));
    rd_str(h, "wifi_pass_2", cfg->wifi_pass_2, sizeof(cfg->wifi_pass_2));
    if (rd_u8(h, "allow_ap_fallback", &u8v)) cfg->allow_ap_fallback = u8v != 0;
    rd_str(h, "device_name", cfg->device_name, sizeof(cfg->device_name));
    rd_str(h, "timezone", cfg->timezone, sizeof(cfg->timezone));
    rd_str(h, "web_password", cfg->web_password, sizeof(cfg->web_password));

    /* ---- 相机（v3 改名回退：resolution/jpeg_quality/vflip/hmirror） ---- */
    if (rd_u8(h, "cam_framesize", &u8v) || rd_u8(h, "resolution", &u8v)) {
        cfg->cam_framesize = legacy_scale_to_framesize(u8v);
    }
    if (rd_u8(h, "cam_quality", &u8v) || rd_u8(h, "jpeg_quality", &u8v)) {
        cfg->cam_quality = u8v;
    }
    if (rd_u8(h, "cam_vflip", &u8v) || rd_u8(h, "vflip", &u8v)) {
        cfg->cam_vflip = u8v != 0;
    }
    if (rd_u8(h, "cam_hmirror", &u8v) || rd_u8(h, "hmirror", &u8v)) {
        cfg->cam_hmirror = u8v != 0;
    }
    if (rd_u8(h, "fps", &u8v)) cfg->cam_fps = u8v;
    rd_u8(h, "day_night", &cfg->day_night_mode);
    rd_u8(h, "xclk_freq_mhz", &cfg->xclk_freq_mhz);
    rd_i8(h, "wifi_roam_rssi", &cfg->wifi_roam_rssi);
    rd_u8(h, "wifi_roam_gap", &cfg->wifi_roam_gap_s);

    /* ---- 录像 / 存储 ---- */
    if (rd_u16(h, "segment_sec", &u16v)) cfg->segment_sec = u16v;
    if (rd_u8(h, "frame_drop", &u8v)) cfg->frame_drop_enabled = u8v != 0;
    rd_u8(h, "cleanup_low", &cfg->cleanup_low_pct);
    rd_u8(h, "cleanup_high", &cfg->cleanup_high_pct);
    if (rd_u8(h, "video_record_to_sd", &u8v)) cfg->video_record_to_sd = u8v != 0;
    if (rd_u8(h, "record_on_boot", &u8v)) cfg->record_on_boot = u8v != 0;
    if (rd_u8(h, "audio_record_to_sd", &u8v)) cfg->audio_record_to_sd = u8v != 0;
    if (rd_u8(h, "sd_log_enabled", &u8v)) cfg->sd_log_enabled = u8v != 0;

    /* ---- NAS / WebDAV（含 legacy FTP→webdav 迁移） ---- */
    {
        uint8_t ftp_enabled = 0;
        if (rd_u8(h, "ftp_enabled", &ftp_enabled) && ftp_enabled) {
            cfg->webdav_enabled = true;
        }
        if (rd_u8(h, "webdav_enabled", &u8v)) cfg->webdav_enabled = (u8v || ftp_enabled) != 0;
    }
    rd_str(h, "webdav_url", cfg->webdav_url, sizeof(cfg->webdav_url));
    rd_str(h, "webdav_user", cfg->webdav_user, sizeof(cfg->webdav_user));
    rd_str(h, "webdav_pass", cfg->webdav_pass, sizeof(cfg->webdav_pass));
    /* 旧键 upload_base_path 实为 16 字符（超 NVS 15 字符上限，历史上从未
     * 持久化成功过），读取仍尝试一次以兜底手工注入的 NVS，失败即用默认 */
    rd_str(h, "upload_base_path", cfg->webdav_base_path, sizeof(cfg->webdav_base_path));

    /* ---- Webhook ---- */
    rd_str(h, "alert_webhook_url", cfg->alert_webhook_url, sizeof(cfg->alert_webhook_url));
    if (rd_u8(h, "alert_webhook_enabled", &u8v)) cfg->alert_webhook_enabled = u8v != 0;

    /* ---- motion 模型收敛（契约 §6.1）：
     * motion_sensitivity 直迁；motion_idle_interval_sec → motion_cooldown_s；
     * motion_active_interval_sec → motion_active_interval_s；enabled 新增=1 ---- */
    rd_u8(h, "motion_sens", &cfg->motion_sensitivity);
    {
        uint8_t active = 0, idle = 0;
        if (rd_u8(h, "motion_active_int", &active)) {
            cfg->motion_active_interval_s = active ? active : 1;
        }
        if (rd_u8(h, "motion_idle_int", &idle)) {
            cfg->motion_cooldown_s = idle ? idle : 30;
        }
    }

    /* ---- 旧 schema v1→v2 迁移复刻：v1 设备没有 xclk/roam 键，
     * 0/越界值补种默认（与旧 config_init 行为一致） ---- */
    {
        uint8_t old_schema = 0;
        bool have_schema = rd_u8(h, "schema_version", &old_schema);
        if (!have_schema || old_schema < 2) {
            if (cfg->xclk_freq_mhz != 10 && cfg->xclk_freq_mhz != 16 && cfg->xclk_freq_mhz != 20) {
                cfg->xclk_freq_mhz = 16;
            }
            if (cfg->wifi_roam_rssi == 0) {
                cfg->wifi_roam_rssi = -75;
            }
            if (cfg->wifi_roam_gap_s == 0) {
                cfg->wifi_roam_gap_s = 10;
            }
        }
    }

    /* ---- timelapse 模型收敛（契约 §6.1）：2 字段 → 8 字段家族模型 ---- */
    {
        uint8_t old_interval = 0, old_mode = 0;
        rd_u8(h, "timelapse_interval", &old_interval);
        rd_u8(h, "timelapse_mode", &old_mode);
        /* 旧兼容：interval>0 而 mode=0（更早固件无 mode 字段）→ mode=1 */
        if (old_interval > 0 && old_mode == 0) old_mode = 1;
        /* 契约映射：mode 0→0（关闭，连续录像）；mode 1/2→动态 */
        cfg->timelapse_enabled = old_mode > 0;
        cfg->timelapse_mode = (old_mode > 0) ? 1 : 0;
        cfg->timelapse_interval_s = old_interval;
        if (cfg->timelapse_enabled && cfg->timelapse_interval_s == 0) {
            cfg->timelapse_interval_s = 30;   /* 动态模式不用固定间隔，静态模式兜底 */
        }
        /* min/max/decay/burst 取家族默认（apply_defaults 已就位） */
    }
}

/* ──────────────────────────────────────────────────────────────────
 * 新命名空间逐键读/写
 * ────────────────────────────────────────────────────────────────── */
static void load_keys_from_nvs(nvs_handle_t h, cam_config_t *cfg)
{
    /* 缺键 = apply_defaults 已就位的默认值（契约 §1 单键自治） */
    rd_str(h, "device_name", cfg->device_name, sizeof(cfg->device_name));
    rd_str(h, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    rd_str(h, "wifi_pass", cfg->wifi_pass, sizeof(cfg->wifi_pass));
    rd_str(h, "wifi_ssid_2", cfg->wifi_ssid_2, sizeof(cfg->wifi_ssid_2));
    rd_str(h, "wifi_pass_2", cfg->wifi_pass_2, sizeof(cfg->wifi_pass_2));
    rd_u8(h, "ap_fallback", (uint8_t *)&cfg->allow_ap_fallback);
    rd_str(h, "timezone", cfg->timezone, sizeof(cfg->timezone));
    rd_str(h, "web_password", cfg->web_password, sizeof(cfg->web_password));
    rd_str(h, "rtsp_user", cfg->rtsp_user, sizeof(cfg->rtsp_user));
    rd_str(h, "rtsp_pass", cfg->rtsp_pass, sizeof(cfg->rtsp_pass));
    rd_u8(h, "cam_framesize", &cfg->cam_framesize);
    rd_u8(h, "cam_fps", &cfg->cam_fps);
    rd_u8(h, "cam_quality", &cfg->cam_quality);
    rd_u8(h, "cam_vflip", (uint8_t *)&cfg->cam_vflip);
    rd_u8(h, "cam_hmirror", (uint8_t *)&cfg->cam_hmirror);
    rd_i8(h, "cam_brightness", &cfg->cam_brightness);
    rd_i8(h, "cam_contrast", &cfg->cam_contrast);
    rd_i8(h, "cam_saturation", &cfg->cam_saturation);
    rd_i8(h, "cam_sharpness", &cfg->cam_sharpness);
    rd_u8(h, "day_night", &cfg->day_night_mode);
    rd_u8(h, "onvif_enable", &cfg->onvif_enable);
    rd_u8(h, "xclk_mhz", &cfg->xclk_freq_mhz);
    rd_i8(h, "wifi_roam_rssi", &cfg->wifi_roam_rssi);
    rd_u8(h, "wifi_roam_gap", &cfg->wifi_roam_gap_s);
    rd_u8(h, "cleanup_low", &cfg->cleanup_low_pct);
    rd_u8(h, "cleanup_high", &cfg->cleanup_high_pct);
    rd_u8(h, "frame_drop_en", (uint8_t *)&cfg->frame_drop_enabled);
    rd_u16(h, "segment_sec", &cfg->segment_sec);
    rd_u8(h, "video_to_sd", (uint8_t *)&cfg->video_record_to_sd);
    rd_u8(h, "record_on_boot", (uint8_t *)&cfg->record_on_boot);
    rd_u8(h, "audio_to_sd", (uint8_t *)&cfg->audio_record_to_sd);
    rd_u8(h, "sd_log_en", (uint8_t *)&cfg->sd_log_enabled);
    rd_u8(h, "webdav_en", (uint8_t *)&cfg->webdav_enabled);
    rd_str(h, "webdav_url", cfg->webdav_url, sizeof(cfg->webdav_url));
    rd_str(h, "webdav_user", cfg->webdav_user, sizeof(cfg->webdav_user));
    rd_str(h, "webdav_pass", cfg->webdav_pass, sizeof(cfg->webdav_pass));
    rd_str(h, "webdav_base", cfg->webdav_base_path, sizeof(cfg->webdav_base_path));
    rd_u8(h, "webhook_en", (uint8_t *)&cfg->alert_webhook_enabled);
    rd_str(h, "webhook_url", cfg->alert_webhook_url, sizeof(cfg->alert_webhook_url));
    rd_u8(h, "motion_en", (uint8_t *)&cfg->motion_enabled);
    rd_u8(h, "motion_sens", &cfg->motion_sensitivity);
    rd_u16(h, "motion_cool_s", &cfg->motion_cooldown_s);
    rd_u8(h, "motion_act_s", &cfg->motion_active_interval_s);
    rd_u8(h, "tl_en", (uint8_t *)&cfg->timelapse_enabled);
    rd_u16(h, "tl_int_s", &cfg->timelapse_interval_s);
    rd_u8(h, "tl_burst", &cfg->timelapse_burst_count);
    rd_u8(h, "tl_mode", &cfg->timelapse_mode);
    rd_u16(h, "tl_min_int_s", &cfg->timelapse_min_interval_s);
    rd_u16(h, "tl_max_int_s", &cfg->timelapse_max_interval_s);
    rd_u8(h, "tl_decay_f", &cfg->timelapse_decay_factor);
    rd_u16(h, "tl_decay_p_s", &cfg->timelapse_decay_period_s);
}

static void write_keys_to_nvs(nvs_handle_t h, const cam_config_t *cfg)
{
    wr_str(h, "device_name", cfg->device_name);
    wr_str(h, "wifi_ssid", cfg->wifi_ssid);
    wr_str(h, "wifi_pass", cfg->wifi_pass);
    wr_str(h, "wifi_ssid_2", cfg->wifi_ssid_2);
    wr_str(h, "wifi_pass_2", cfg->wifi_pass_2);
    wr_u8(h, "ap_fallback", cfg->allow_ap_fallback ? 1 : 0);
    wr_str(h, "timezone", cfg->timezone);
    wr_str(h, "web_password", cfg->web_password);
    wr_str(h, "rtsp_user", cfg->rtsp_user);
    wr_str(h, "rtsp_pass", cfg->rtsp_pass);
    wr_u8(h, "cam_framesize", cfg->cam_framesize);
    wr_u8(h, "cam_fps", cfg->cam_fps);
    wr_u8(h, "cam_quality", cfg->cam_quality);
    wr_u8(h, "cam_vflip", cfg->cam_vflip ? 1 : 0);
    wr_u8(h, "cam_hmirror", cfg->cam_hmirror ? 1 : 0);
    wr_i8(h, "cam_brightness", cfg->cam_brightness);
    wr_i8(h, "cam_contrast", cfg->cam_contrast);
    wr_i8(h, "cam_saturation", cfg->cam_saturation);
    wr_i8(h, "cam_sharpness", cfg->cam_sharpness);
    wr_u8(h, "day_night", cfg->day_night_mode);
    wr_u8(h, "onvif_enable", cfg->onvif_enable);
    wr_u8(h, "xclk_mhz", cfg->xclk_freq_mhz);
    wr_i8(h, "wifi_roam_rssi", cfg->wifi_roam_rssi);
    wr_u8(h, "wifi_roam_gap", cfg->wifi_roam_gap_s);
    wr_u8(h, "cleanup_low", cfg->cleanup_low_pct);
    wr_u8(h, "cleanup_high", cfg->cleanup_high_pct);
    wr_u8(h, "frame_drop_en", cfg->frame_drop_enabled ? 1 : 0);
    wr_u16(h, "segment_sec", cfg->segment_sec);
    wr_u8(h, "video_to_sd", cfg->video_record_to_sd ? 1 : 0);
    wr_u8(h, "record_on_boot", cfg->record_on_boot ? 1 : 0);
    wr_u8(h, "audio_to_sd", cfg->audio_record_to_sd ? 1 : 0);
    wr_u8(h, "sd_log_en", cfg->sd_log_enabled ? 1 : 0);
    wr_u8(h, "webdav_en", cfg->webdav_enabled ? 1 : 0);
    wr_str(h, "webdav_url", cfg->webdav_url);
    wr_str(h, "webdav_user", cfg->webdav_user);
    wr_str(h, "webdav_pass", cfg->webdav_pass);
    wr_str(h, "webdav_base", cfg->webdav_base_path);
    wr_u8(h, "webhook_en", cfg->alert_webhook_enabled ? 1 : 0);
    wr_str(h, "webhook_url", cfg->alert_webhook_url);
    wr_u8(h, "motion_en", cfg->motion_enabled ? 1 : 0);
    wr_u8(h, "motion_sens", cfg->motion_sensitivity);
    wr_u16(h, "motion_cool_s", cfg->motion_cooldown_s);
    wr_u8(h, "motion_act_s", cfg->motion_active_interval_s);
    wr_u8(h, "tl_en", cfg->timelapse_enabled ? 1 : 0);
    wr_u16(h, "tl_int_s", cfg->timelapse_interval_s);
    wr_u8(h, "tl_burst", cfg->timelapse_burst_count);
    wr_u8(h, "tl_mode", cfg->timelapse_mode);
    wr_u16(h, "tl_min_int_s", cfg->timelapse_min_interval_s);
    wr_u16(h, "tl_max_int_s", cfg->timelapse_max_interval_s);
    wr_u8(h, "tl_decay_f", cfg->timelapse_decay_factor);
    wr_u16(h, "tl_decay_p_s", cfg->timelapse_decay_period_s);
    wr_u16(h, KEY_SCHEMA_VER, CONFIG_SCHEMA_VERSION);
}

/* ── cam_config（旧命名空间）→ mibee_cfg 一次性迁移（幂等） ── */
static bool migrate_legacy_namespace(void)
{
    /* 已迁移（schema_ver 在新命名空间）→ 跳过 */
    nvs_handle_t h;
    uint16_t schema_ver = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        bool have = nvs_get_u16(h, KEY_SCHEMA_VER, &schema_ver) == ESP_OK;
        nvs_close(h);
        if (have) {
            return false;
        }
    }

    /* 无旧命名空间 → 真空出厂，由 config_init 落默认值 */
    if (nvs_open(NVS_NS_LEGACY, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    cam_config_t migrated;
    apply_defaults(&migrated);
    load_legacy_namespace(h, &migrated);
    nvs_close(h);

    /* 密码一次性种子标记跨命名空间生效（已种过的设备不得重种，契约 v1.1） */
    bool pw_seeded = false;
    uint8_t flag = 0;
    if (nvs_open(NVS_NS_LEGACY, NVS_READONLY, &h) == ESP_OK) {
        pw_seeded = (nvs_get_u8(h, KEY_PW_SEED, &flag) == ESP_OK && flag == 1);
        nvs_close(h);
    }
    if (!pw_seeded && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        pw_seeded = (nvs_get_u8(h, KEY_PW_SEED, &flag) == ESP_OK && flag == 1);
        nvs_close(h);
    }
    if (!pw_seeded) {
        ESP_LOGW(TAG, "One-shot password seed: unifying web_password to family default");
        strlcpy(migrated.web_password, CONFIG_DEFAULT_WEB_PASSWORD,
                sizeof(migrated.web_password));
    }
    /* 空密码迁移到家族统一默认（契约 v1.1） */
    if (migrated.web_password[0] == '\0') {
        strlcpy(migrated.web_password, CONFIG_DEFAULT_WEB_PASSWORD,
                sizeof(migrated.web_password));
    }
    /* rtsp_pass 一次性种子 = 现 web_password（契约 §6：本板 RTSP 原用
     * web_password 做 digest 鉴权，拆分后首次迁移带上现值） */
    strlcpy(migrated.rtsp_pass, migrated.web_password, sizeof(migrated.rtsp_pass));

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s for migration", NVS_NS);
        return false;
    }
    write_keys_to_nvs(h, &migrated);
    if (!pw_seeded) {
        nvs_set_u8(h, KEY_PW_SEED, 1);
    }
    nvs_commit(h);
    nvs_close(h);

    /* 旧命名空间 cam_config 原样留存只读（回滚备份，契约 §6） */
    ESP_LOGI(TAG, "Legacy namespace '%s' migrated to '%s' (schema_ver=%d, "
             "framesize=%u, tl_enabled=%u tl_mode=%u)",
             NVS_NS_LEGACY, NVS_NS, CONFIG_SCHEMA_VERSION,
             migrated.cam_framesize, migrated.timelapse_enabled, migrated.timelapse_mode);
    return true;
}

/* ──────────────────────────────────────────────────────────────────
 * 最优延时录像参数映射（timelapse_enabled 时按模式调 segment/fps）
 * ────────────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t interval_sec;
    uint16_t segment_sec_optimal;
    uint8_t  fps_optimal;
} optimal_timelapse_entry_t;

/**
 * @brief 最优配置映射表：根据 timelapse_interval_s 查表得到最优 segment_sec 和 fps
 *
 * 设计原则：目标文件 20-40MB（SVGA@quality12，~65KB/帧），每日文件数 24-144
 * 连续模式不受影响，保留用户自定义 fps/segment_sec
 */
static const optimal_timelapse_entry_t s_optimal_timelapse[] = {
    {  1,   600,  15 },  /* 39MB/段, 144文件/天 */
    {  5,  1800,  15 },  /* 23MB/段,  48文件/天 */
    { 10,  3600,  15 },  /* 23MB/段,  24文件/天 */
    { 30,  3600,  15 },  /*  8MB/段,  24文件/天 */
    { 60,  3600,  15 },  /*  4MB/段,  24文件/天 */
    {120,  3600,  15 },  /*  2MB/段,  24文件/天 */
    {255,  3600,  15 },  /* 表尾（interval 1-255 最近邻匹配） */
};

#define OPTIMAL_TIMELAPSE_ENTRIES (sizeof(s_optimal_timelapse) / sizeof(s_optimal_timelapse[0]))

/** @brief 根据 timelapse_interval_s 查最优映射，返回最接近的索引 */
static int optimal_lookup(uint16_t interval_sec)
{
    for (int i = 0; i < (int)OPTIMAL_TIMELAPSE_ENTRIES; i++) {
        if (s_optimal_timelapse[i].interval_sec == interval_sec) {
            return i;
        }
    }
    /* Fallback：不在表中的间隔值，使用最近邻匹配 */
    int best = 0;
    int best_diff = 999;
    for (int i = 0; i < (int)OPTIMAL_TIMELAPSE_ENTRIES; i++) {
        int diff = abs((int)s_optimal_timelapse[i].interval_sec - (int)interval_sec);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    ESP_LOGW(TAG, "optimal_lookup: interval=%d not in table, nearest=%d",
             interval_sec, s_optimal_timelapse[best].interval_sec);
    return best;
}

/**
 * @brief 根据延时模式计算最优片段时长和帧率（家族 8 字段模型语义）
 *
 * - timelapse_enabled=0（连续录像）：不做任何修改，保留用户自定义值
 * - enabled + mode=0（静态延时）：根据 timelapse_interval_s 查表
 * - enabled + mode=1（动态延时）：固定值 1800s，fps=15
 */
void config_apply_optimal(cam_config_t *cfg)
{
    if (!cfg) return;
    if (!cfg->timelapse_enabled) return;

    if (cfg->timelapse_mode == 1) {
        cfg->segment_sec = 1800;
        cfg->cam_fps     = 15;
        ESP_LOGD(TAG, "optimal: dynamic timelapse -> segment_sec=%u, fps=%u",
                 cfg->segment_sec, cfg->cam_fps);
    } else {
        int idx = optimal_lookup(cfg->timelapse_interval_s);
        cfg->segment_sec = s_optimal_timelapse[idx].segment_sec_optimal;
        cfg->cam_fps     = s_optimal_timelapse[idx].fps_optimal;
        ESP_LOGD(TAG, "optimal: static timelapse interval=%u -> segment_sec=%u, fps=%u",
                 cfg->timelapse_interval_s, cfg->segment_sec, cfg->cam_fps);
    }
}

/* ──────────────────────────────────────────────────────────────────
 * SD 卡 provisioning（契约 §9：家族 KEY=VALUE 格式 + 旧格式兼容读）
 * ────────────────────────────────────────────────────────────────── */

/** @brief 解析 KEY=VALUE 格式的文本行，提取字符串值 */
static void parse_line(char *line, const char *key, char *dest, size_t dest_len)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return;
    if (line[klen] != '=') return;

    const char *val = line + klen + 1;
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n')) {
        vlen--;
    }
    if (vlen >= dest_len) vlen = dest_len - 1;
    memcpy(dest, val, vlen);
    dest[vlen] = '\0';
}

/** @brief 解析 KEY=VALUE 格式的文本行，提取布尔值 */
static void parse_bool(char *line, const char *key, bool *dest)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return;
    if (line[klen] != '=') return;
    char val[32];
    strncpy(val, line + klen + 1, sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n' || val[vlen - 1] == ' ')) {
        val[--vlen] = '\0';
    }
    *dest = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
}

/** @brief 文件首行是否为 one_time 清除标记（契约 §9；兼容行内 one_time=1） */
static bool sd_file_is_one_time(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[64];
    bool first = true, one_time = false, inline_flag = false;
    while (fgets(line, sizeof(line), f)) {
        char *end = line + strlen(line) - 1;
        while (end >= line && (*end == '\r' || *end == '\n' || *end == ' ')) { *end = '\0'; end--; }
        if (first) {
            first = false;
            if (strcmp(line, "one_time") == 0) one_time = true;
        }
        if (strncmp(line, "one_time=", 9) == 0) inline_flag = true;
    }
    fclose(f);
    return one_time || inline_flag;
}

/** @brief 从 SD 卡读取 wifi.txt（家族 wifi_ssid=/wifi_pass= 键 + 旧 SSID=/PASS= 兼容）。
 *
 * 安全红线（契约 §9）：仅在 NVS 无任何 WiFi 凭据时生效，绝不静默覆盖在线凭据。
 * wifi.txt 旧格式（SSID=/PASS=/SSID2=/PASS2=/ALLOW_AP_FALLBACK=/TIMEZONE=）保留兼容读。
 */
static void parse_wifi_txt(void)
{
    const char *path = "/sdcard/config/wifi.txt";
    FILE *f = fopen(path, "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found wifi.txt on SD card");
    char line[256];
    char ssid[33] = {0}, pass[65] = {0}, ssid2[33] = {0}, pass2[65] = {0}, tz[48] = {0};
    bool ap_fallback = s_config.allow_ap_fallback;
    bool have_creds = false;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_line(line, "wifi_ssid", ssid, sizeof(ssid));
        parse_line(line, "wifi_pass", pass, sizeof(pass));
        parse_line(line, "wifi_ssid_2", ssid2, sizeof(ssid2));
        parse_line(line, "wifi_pass_2", pass2, sizeof(pass2));
        /* 旧格式键（大写） */
        parse_line(line, "SSID", ssid, sizeof(ssid));
        parse_line(line, "PASS", pass, sizeof(pass));
        parse_line(line, "SSID2", ssid2, sizeof(ssid2));
        parse_line(line, "PASS2", pass2, sizeof(pass2));
        parse_bool(line, "ALLOW_AP_FALLBACK", &ap_fallback);
        parse_line(line, "TIMEZONE", tz, sizeof(tz));
    }
    fclose(f);
    have_creds = (ssid[0] != '\0' && pass[0] != '\0');

    bool one_time = sd_file_is_one_time(path);
    if (!have_creds) {
        ESP_LOGW(TAG, "wifi.txt has no complete credentials, ignoring");
        if (one_time) remove(path);
        return;
    }

    /* 安全红线：NVS 已有凭据时绝不覆盖（契约 §9） */
    if (s_config.wifi_ssid[0] != '\0' && s_config.wifi_pass[0] != '\0') {
        ESP_LOGI(TAG, "NVS WiFi already configured ('%s'), skipping SD override", s_config.wifi_ssid);
        if (one_time) remove(path);
        return;
    }

    strlcpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid));
    strlcpy(s_config.wifi_pass, pass, sizeof(s_config.wifi_pass));
    strlcpy(s_config.wifi_ssid_2, ssid2, sizeof(s_config.wifi_ssid_2));
    strlcpy(s_config.wifi_pass_2, pass2, sizeof(s_config.wifi_pass_2));
    s_config.allow_ap_fallback = ap_fallback;
    if (tz[0] != '\0') {
        strlcpy(s_config.timezone, tz, sizeof(s_config.timezone));
    }
    if (one_time) {
        remove(path);
        ESP_LOGW(TAG, "Deleted one-time config: %s", path);
    }
}

/** @brief 从 SD 卡读取 config/config.txt（契约 §9：通用键值，键名=契约 JSON 名）
 *         与根目录旧 config.txt（WIFI.SSID= 风格，兼容读一个版本）。 */
static void parse_config_txt(void)
{
    const char *path = "/sdcard/config/config.txt";
    FILE *f = fopen(path, "r");
    bool legacy_fmt = false;
    if (!f) {
        path = "/sdcard/config.txt";
        legacy_fmt = true;
        f = fopen(path, "r");
        if (!f) return;
    }

    ESP_LOGI(TAG, "Found config.txt on SD card (%s format)", legacy_fmt ? "legacy" : "family");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (legacy_fmt) {
            parse_line(line, "WIFI.SSID", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
            parse_line(line, "WIFI.PASS", s_config.wifi_pass, sizeof(s_config.wifi_pass));
            parse_line(line, "WIFI.SSID2", s_config.wifi_ssid_2, sizeof(s_config.wifi_ssid_2));
            parse_line(line, "WIFI.PASS2", s_config.wifi_pass_2, sizeof(s_config.wifi_pass_2));
            parse_bool(line, "ALLOW_AP_FALLBACK", &s_config.allow_ap_fallback);
            parse_bool(line, "VIDEO_RECORD_TO_SD", &s_config.video_record_to_sd);
            parse_bool(line, "AUDIO_RECORD_TO_SD", &s_config.audio_record_to_sd);
            parse_bool(line, "SD_LOG_ENABLED", &s_config.sd_log_enabled);
        } else {
            /* 契约键名（§9：键名 = 契约 JSON 字段名）。逐项套用 §4 校验域，
             * 越界行忽略（与 POST 同表，不静默截断）。 */
            char buf[128] = {0};
            parse_line(line, "timezone", buf, sizeof(buf));
            if (buf[0]) { strlcpy(s_config.timezone, buf, sizeof(s_config.timezone)); continue; }
            parse_line(line, "device_name", buf, sizeof(buf));
            if (buf[0]) { strlcpy(s_config.device_name, buf, sizeof(s_config.device_name)); continue; }
            parse_bool(line, "allow_ap_fallback", &s_config.allow_ap_fallback);
            parse_bool(line, "video_record_to_sd", &s_config.video_record_to_sd);
            parse_bool(line, "audio_record_to_sd", &s_config.audio_record_to_sd);
            parse_bool(line, "sd_log_enabled", &s_config.sd_log_enabled);
            parse_bool(line, "record_on_boot", &s_config.record_on_boot);
            parse_bool(line, "onvif_enable", (bool *)&s_config.onvif_enable);
        }
    }
    fclose(f);
    if (sd_file_is_one_time(path)) {
        remove(path);
        ESP_LOGW(TAG, "Deleted one-time config: %s", path);
    }
}

/** @brief 从 SD 卡读取 nas.txt 配置文件并覆盖 NAS 上传配置
 *         （契约 §9 NAS/WebDAV 字段；旧大写键名保留兼容读） */
static void parse_nas_txt(void)
{
    const char *path = "/sdcard/config/nas.txt";
    FILE *f = fopen(path, "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found nas.txt on SD card");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_line(line, "webdav_base_path", s_config.webdav_base_path, sizeof(s_config.webdav_base_path));
        parse_line(line, "webdav_url", s_config.webdav_url, sizeof(s_config.webdav_url));
        parse_line(line, "webdav_user", s_config.webdav_user, sizeof(s_config.webdav_user));
        parse_line(line, "webdav_pass", s_config.webdav_pass, sizeof(s_config.webdav_pass));
        parse_bool(line, "webdav_enabled", &s_config.webdav_enabled);
        /* 旧格式键（大写） */
        parse_line(line, "UPLOAD_BASE_PATH", s_config.webdav_base_path, sizeof(s_config.webdav_base_path));
        parse_line(line, "WEBDAV_URL", s_config.webdav_url, sizeof(s_config.webdav_url));
        parse_line(line, "WEBDAV_USER", s_config.webdav_user, sizeof(s_config.webdav_user));
        parse_line(line, "WEBDAV_PASS", s_config.webdav_pass, sizeof(s_config.webdav_pass));
        parse_bool(line, "WEBDAV_ENABLED", &s_config.webdav_enabled);
    }
    fclose(f);
    if (sd_file_is_one_time(path)) {
        remove(path);
        ESP_LOGW(TAG, "Deleted one-time config: %s", path);
    }
}

/* ──────────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────────── */

/** @brief 初始化配置模块，初始化 NVS 并加载存储的配置，无存储则使用默认值 */
esp_err_t config_init(void)
{
    static bool s_config_initialized = false;
    if (s_config_initialized) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();

    s_config_mutex = xSemaphoreCreateMutex();
    if (!s_config_mutex) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_FAIL;
    }

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* cam_config → mibee_cfg 一次性迁移（幂等） */
    migrate_legacy_namespace();

    apply_defaults(&s_config);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        load_keys_from_nvs(h, &s_config);
        nvs_close(h);
    } else {
        /* 首次出厂：落一份默认值（含 schema_ver） */
        ESP_LOGW(TAG, "No per-key config yet, saving defaults");
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            write_keys_to_nvs(h, &s_config);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    /* 板级边界钳制（契约 §4）：旧值 q<10 超JPEG fb 预算（PIT-021）、
     * 分辨率不在家族刻度 10-15 / 超本板热限（PIT-016）→ 加载时钳回 */
    if (s_config.cam_quality < CAMERA_QUALITY_MIN) {
        ESP_LOGW(TAG, "cam_quality=%u clamped to %d on load",
                 s_config.cam_quality, CAMERA_QUALITY_MIN);
        s_config.cam_quality = CAMERA_QUALITY_MIN;
    } else if (s_config.cam_quality > CAMERA_QUALITY_MAX) {
        s_config.cam_quality = CAMERA_QUALITY_MAX;
    }
    if (s_config.cam_framesize < CAMERA_RES_VGA || s_config.cam_framesize > CAMERA_RES_BOARD_MAX) {
        ESP_LOGW(TAG, "cam_framesize=%u out of family scale, defaulting to SVGA",
                 s_config.cam_framesize);
        s_config.cam_framesize = CAMERA_RES_SVGA;
    }
    if (s_config.cam_fps == 0 || s_config.cam_fps > 30) {
        s_config.cam_fps = 12;
    }
    if (s_config.day_night_mode > 2) {
        ESP_LOGW(TAG, "day_night_mode=%d out of range, resetting to 0", s_config.day_night_mode);
        s_config.day_night_mode = 0;
    }
    if (s_config.timelapse_enabled && s_config.timelapse_interval_s == 0) {
        s_config.timelapse_interval_s = 30;
    }
    if (s_config.onvif_enable > 1) {
        s_config.onvif_enable = 1;
    }
    /* 契约 v1.1：空密码迁移到家族统一默认 */
    if (s_config.web_password[0] == '\0') {
        strlcpy(s_config.web_password, CONFIG_DEFAULT_WEB_PASSWORD,
                sizeof(s_config.web_password));
    }
    if (s_config.rtsp_user[0] == '\0') {
        strlcpy(s_config.rtsp_user, "admin", sizeof(s_config.rtsp_user));
    }

    /* 延时模式最优参数（segment/fps 随 interval/mode 联动） */
    config_apply_optimal(&s_config);

    s_config_initialized = true;
    ESP_LOGI(TAG, "Config loaded from NVS ns=%s (schema v%d, framesize=%u)",
             NVS_NS, CONFIG_SCHEMA_VERSION, s_config.cam_framesize);
    return ESP_OK;
}

/** @brief 获取当前配置指针（指向全局静态配置结构体） */
cam_config_t *config_get(void)
{
    return &s_config;
}

/** @brief 线程安全复制当前配置（在互斥锁保护下拷贝完整配置） */
esp_err_t config_get_copy(cam_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    config_lock();
    memcpy(out, &s_config, sizeof(cam_config_t));
    config_unlock();
    return ESP_OK;
}

/** @brief 验证配置结构体关键字段是否在合法范围内
 *
 * 契约 §4 校验矩阵（与 POST /api/config 同表）；只在硬约束上拒绝，
 * 模式相关字段在对应模式启用时才检查。
 */
bool config_validate(const cam_config_t *cfg)
{
    if (!cfg) return false;

    /* ---- core numeric ranges ---- */
    if (cfg->cam_framesize < CAMERA_RES_VGA || cfg->cam_framesize > CAMERA_RES_BOARD_MAX) {
        ESP_LOGW(TAG, "validate: cam_framesize=%d out of %d-%d",
                 cfg->cam_framesize, (int)CAMERA_RES_VGA, (int)CAMERA_RES_BOARD_MAX);
        return false;
    }
    if (cfg->cam_fps < 1 || cfg->cam_fps > 30) {
        ESP_LOGW(TAG, "validate: cam_fps=%d out of 1-30", cfg->cam_fps);
        return false;
    }
    if (cfg->cam_quality < CAMERA_QUALITY_MIN || cfg->cam_quality > CAMERA_QUALITY_MAX) {
        ESP_LOGW(TAG, "validate: cam_quality=%d out of %d-%d",
                 cfg->cam_quality, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
        return false;
    }
    if (cfg->segment_sec < 5 || cfg->segment_sec > 3600) {
        ESP_LOGW(TAG, "validate: segment_sec=%d out of 5-3600", cfg->segment_sec);
        return false;
    }
    if (cfg->xclk_freq_mhz != 10 && cfg->xclk_freq_mhz != 16 && cfg->xclk_freq_mhz != 20) {
        ESP_LOGW(TAG, "validate: xclk_freq_mhz=%d not in {10,16,20}", cfg->xclk_freq_mhz);
        return false;
    }
    if (cfg->wifi_roam_rssi != 0 && (cfg->wifi_roam_rssi < -90 || cfg->wifi_roam_rssi > -50)) {
        ESP_LOGW(TAG, "validate: wifi_roam_rssi=%d not 0 or -90..-50", cfg->wifi_roam_rssi);
        return false;
    }
    if (cfg->wifi_roam_gap_s < 5 || cfg->wifi_roam_gap_s > 15) {
        ESP_LOGW(TAG, "validate: wifi_roam_gap_s=%d out of 5-15", cfg->wifi_roam_gap_s);
        return false;
    }
    if (cfg->day_night_mode > 2) {
        ESP_LOGW(TAG, "validate: day_night_mode=%d out of 0-2", cfg->day_night_mode);
        return false;
    }
    if (cfg->onvif_enable > 1) {
        ESP_LOGW(TAG, "validate: onvif_enable=%d out of 0-1", cfg->onvif_enable);
        return false;
    }

    /* ---- motion 家族超集（契约 §3.2/§4） ---- */
    if (cfg->motion_sensitivity > 100) {
        ESP_LOGW(TAG, "validate: motion_sensitivity=%d > 100", cfg->motion_sensitivity);
        return false;
    }
    if (cfg->motion_cooldown_s < 1 || cfg->motion_cooldown_s > 300) {
        ESP_LOGW(TAG, "validate: motion_cooldown_s=%d out of 1-300", cfg->motion_cooldown_s);
        return false;
    }
    if (cfg->motion_active_interval_s < 1 || cfg->motion_active_interval_s > 30) {
        ESP_LOGW(TAG, "validate: motion_active_interval_s=%d out of 1-30",
                 cfg->motion_active_interval_s);
        return false;
    }

    /* ---- timelapse 家族动态模型（契约 §3.2/§4） ---- */
    if (cfg->timelapse_mode > 1) {
        ESP_LOGW(TAG, "validate: timelapse_mode=%d out of 0-1", cfg->timelapse_mode);
        return false;
    }
    if (cfg->timelapse_enabled) {
        if (cfg->timelapse_interval_s < 1 || cfg->timelapse_interval_s > 255) {
            ESP_LOGW(TAG, "validate: timelapse_interval_s=%d out of 1-255 (enabled)",
                     cfg->timelapse_interval_s);
            return false;
        }
    }
    if (cfg->timelapse_min_interval_s < 1 || cfg->timelapse_min_interval_s > 3600) {
        ESP_LOGW(TAG, "validate: timelapse_min_interval_s=%d out of 1-3600",
                 cfg->timelapse_min_interval_s);
        return false;
    }
    if (cfg->timelapse_max_interval_s < 1 || cfg->timelapse_max_interval_s > 3600) {
        ESP_LOGW(TAG, "validate: timelapse_max_interval_s=%d out of 1-3600",
                 cfg->timelapse_max_interval_s);
        return false;
    }
    if (cfg->timelapse_enabled && cfg->timelapse_mode == 1 &&
        cfg->timelapse_max_interval_s < cfg->timelapse_min_interval_s) {
        ESP_LOGW(TAG, "validate: tl max=%u < min=%u",
                 cfg->timelapse_max_interval_s, cfg->timelapse_min_interval_s);
        return false;
    }
    if (cfg->timelapse_decay_factor < 1 || cfg->timelapse_decay_factor > 10) {
        ESP_LOGW(TAG, "validate: timelapse_decay_factor=%d out of 1-10",
                 cfg->timelapse_decay_factor);
        return false;
    }
    if (cfg->timelapse_decay_period_s < 1 || cfg->timelapse_decay_period_s > 3600) {
        ESP_LOGW(TAG, "validate: timelapse_decay_period_s=%d out of 1-3600",
                 cfg->timelapse_decay_period_s);
        return false;
    }
    if (cfg->timelapse_burst_count < 1 || cfg->timelapse_burst_count > 10) {
        ESP_LOGW(TAG, "validate: timelapse_burst_count=%d out of 1-10",
                 cfg->timelapse_burst_count);
        return false;
    }

    /* ---- storage cleanup（契约 §4） ---- */
    if (cfg->cleanup_low_pct < 1 || cfg->cleanup_low_pct > 99) {
        ESP_LOGW(TAG, "validate: cleanup_low_pct=%d out of 1-99", cfg->cleanup_low_pct);
        return false;
    }
    if (cfg->cleanup_high_pct > 80 || cfg->cleanup_high_pct < cfg->cleanup_low_pct + 5) {
        ESP_LOGW(TAG, "validate: cleanup_high_pct=%d invalid vs low=%d",
                 cfg->cleanup_high_pct, cfg->cleanup_low_pct);
        return false;
    }

    /* ---- string fields — only check buffer overflow ---- */
    if (strnlen(cfg->timezone, sizeof(cfg->timezone)) >= sizeof(cfg->timezone))  { ESP_LOGW(TAG, "validate: timezone overflow"); return false; }
    if (strnlen(cfg->device_name, sizeof(cfg->device_name)) >= sizeof(cfg->device_name)) { ESP_LOGW(TAG, "validate: device_name overflow"); return false; }
    if (strnlen(cfg->wifi_ssid, sizeof(cfg->wifi_ssid)) >= sizeof(cfg->wifi_ssid)) { ESP_LOGW(TAG, "validate: wifi_ssid overflow"); return false; }
    if (strnlen(cfg->wifi_pass, sizeof(cfg->wifi_pass)) >= sizeof(cfg->wifi_pass)) { ESP_LOGW(TAG, "validate: wifi_pass overflow"); return false; }
    if (strnlen(cfg->webdav_url, sizeof(cfg->webdav_url)) >= sizeof(cfg->webdav_url)) { ESP_LOGW(TAG, "validate: webdav_url overflow"); return false; }
    if (strnlen(cfg->webdav_user, sizeof(cfg->webdav_user)) >= sizeof(cfg->webdav_user)) { ESP_LOGW(TAG, "validate: webdav_user overflow"); return false; }
    if (strnlen(cfg->webdav_pass, sizeof(cfg->webdav_pass)) >= sizeof(cfg->webdav_pass)) { ESP_LOGW(TAG, "validate: webdav_pass overflow"); return false; }
    if (strnlen(cfg->webdav_base_path, sizeof(cfg->webdav_base_path)) >= sizeof(cfg->webdav_base_path)) { ESP_LOGW(TAG, "validate: webdav_base_path overflow"); return false; }
    if (strnlen(cfg->rtsp_user, sizeof(cfg->rtsp_user)) >= sizeof(cfg->rtsp_user)) { ESP_LOGW(TAG, "validate: rtsp_user overflow"); return false; }
    if (strnlen(cfg->rtsp_pass, sizeof(cfg->rtsp_pass)) >= sizeof(cfg->rtsp_pass)) { ESP_LOGW(TAG, "validate: rtsp_pass overflow"); return false; }

    return true;
}

/** @brief 将当前配置保存到 NVS 闪存持久化 */
esp_err_t config_save(void)
{
    /* Apply optimal timelapse parameters before persisting */
    config_apply_optimal(&s_config);

    if (!config_validate(&s_config)) {
        ESP_LOGW(TAG, "Config validation failed, not saving");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS ns=%s: %s", NVS_NS, esp_err_to_name(err));
        return err;
    }

    write_keys_to_nvs(h, &s_config);

    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS (ns=%s)", NVS_NS);
    }
    return err;
}

/** @brief 恢复出厂默认配置并保存到 NVS */
esp_err_t config_reset(void)
{
    config_lock();
    apply_defaults(&s_config);
    config_unlock();
    ESP_LOGI(TAG, "Config reset to factory defaults");
    return config_save();
}

/** @brief 从 SD 卡加载 wifi.txt、config.txt 和 nas.txt 配置文件，覆盖当前配置并保存到 NVS */
esp_err_t config_load_from_sd(void)
{
    parse_wifi_txt();
    parse_config_txt();
    parse_nas_txt();

    /* If we parsed anything, persist back to NVS
     * (Always save so NVS stays in sync with SD overrides) */
    esp_err_t ret = config_save();
    return ret;
}

void config_lock(void)
{
    if (s_config_mutex) {
        xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    }
}

void config_unlock(void)
{
    if (s_config_mutex) {
        xSemaphoreGive(s_config_mutex);
    }
}

/* ---- new JSON and web_password accessors ---- */

/** @brief Get current web_password value */
const char *config_get_web_password(void)
{
    return s_config.web_password;
}

/** @brief 生成当前配置的 JSON 对象（契约字段名；密码类掩码，调用方负责释放） */
cJSON *config_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    const cam_config_t *cfg = &s_config;

    /* WiFi + 身份（契约 §3.1） */
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", cfg->wifi_pass[0] ? "****" : "");
    cJSON_AddStringToObject(root, "wifi_ssid_2", cfg->wifi_ssid_2);
    cJSON_AddStringToObject(root, "wifi_pass_2", cfg->wifi_pass_2[0] ? "****" : "");
    cJSON_AddBoolToObject(root, "allow_ap_fallback", cfg->allow_ap_fallback);
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddStringToObject(root, "timezone", cfg->timezone);
    cJSON_AddStringToObject(root, "web_password", cfg->web_password[0] ? "****" : "");

    /* RTSP（契约 §3.2 rtsp 组） */
    cJSON_AddStringToObject(root, "rtsp_user", cfg->rtsp_user);
    cJSON_AddStringToObject(root, "rtsp_pass", cfg->rtsp_pass[0] ? "****" : "");

    /* 相机（含微调组） */
    cJSON_AddNumberToObject(root, "cam_framesize", (double)cfg->cam_framesize);
    cJSON_AddNumberToObject(root, "cam_fps", (double)cfg->cam_fps);
    cJSON_AddNumberToObject(root, "cam_quality", (double)cfg->cam_quality);
    cJSON_AddBoolToObject(root, "cam_vflip", cfg->cam_vflip);
    cJSON_AddBoolToObject(root, "cam_hmirror", cfg->cam_hmirror);
    cJSON_AddNumberToObject(root, "cam_brightness", (double)cfg->cam_brightness);
    cJSON_AddNumberToObject(root, "cam_contrast", (double)cfg->cam_contrast);
    cJSON_AddNumberToObject(root, "cam_saturation", (double)cfg->cam_saturation);
    cJSON_AddNumberToObject(root, "cam_sharpness", (double)cfg->cam_sharpness);
    cJSON_AddNumberToObject(root, "day_night_mode", (double)cfg->day_night_mode);
    cJSON_AddNumberToObject(root, "onvif_enable", (double)cfg->onvif_enable);
    cJSON_AddNumberToObject(root, "xclk_freq_mhz", (double)cfg->xclk_freq_mhz);
    cJSON_AddNumberToObject(root, "wifi_roam_rssi", (double)cfg->wifi_roam_rssi);
    cJSON_AddNumberToObject(root, "wifi_roam_gap_s", (double)cfg->wifi_roam_gap_s);

    /* 录像 / 存储（契约 §3.2 录像+SD 运维组） */
    cJSON_AddNumberToObject(root, "segment_sec", (double)cfg->segment_sec);
    cJSON_AddBoolToObject(root, "frame_drop_enabled", cfg->frame_drop_enabled);
    cJSON_AddBoolToObject(root, "video_record_to_sd", cfg->video_record_to_sd);
    cJSON_AddBoolToObject(root, "record_on_boot", cfg->record_on_boot);
    cJSON_AddBoolToObject(root, "audio_record_to_sd", cfg->audio_record_to_sd);
    cJSON_AddBoolToObject(root, "sd_log_enabled", cfg->sd_log_enabled);
    cJSON_AddNumberToObject(root, "cleanup_low_pct", (double)cfg->cleanup_low_pct);
    cJSON_AddNumberToObject(root, "cleanup_high_pct", (double)cfg->cleanup_high_pct);

    /* Motion 家族超集（契约 §3.2） */
    cJSON_AddBoolToObject(root, "motion_enabled", cfg->motion_enabled);
    cJSON_AddNumberToObject(root, "motion_sensitivity", (double)cfg->motion_sensitivity);
    cJSON_AddNumberToObject(root, "motion_cooldown_s", (double)cfg->motion_cooldown_s);
    cJSON_AddNumberToObject(root, "motion_active_interval_s", (double)cfg->motion_active_interval_s);

    /* Timelapse 家族动态模型 8 字段（契约 §3.2） */
    cJSON_AddBoolToObject(root, "timelapse_enabled", cfg->timelapse_enabled);
    cJSON_AddNumberToObject(root, "timelapse_interval_s", (double)cfg->timelapse_interval_s);
    cJSON_AddNumberToObject(root, "timelapse_burst_count", (double)cfg->timelapse_burst_count);
    cJSON_AddNumberToObject(root, "timelapse_mode", (double)cfg->timelapse_mode);
    cJSON_AddNumberToObject(root, "timelapse_min_interval_s", (double)cfg->timelapse_min_interval_s);
    cJSON_AddNumberToObject(root, "timelapse_max_interval_s", (double)cfg->timelapse_max_interval_s);
    cJSON_AddNumberToObject(root, "timelapse_decay_factor", (double)cfg->timelapse_decay_factor);
    cJSON_AddNumberToObject(root, "timelapse_decay_period_s", (double)cfg->timelapse_decay_period_s);

    /* NAS/WebDAV + Webhook（契约 §3.2） */
    cJSON_AddBoolToObject(root, "webdav_enabled", cfg->webdav_enabled);
    cJSON_AddStringToObject(root, "webdav_url", cfg->webdav_url);
    cJSON_AddStringToObject(root, "webdav_user", cfg->webdav_user);
    cJSON_AddStringToObject(root, "webdav_pass", cfg->webdav_pass[0] ? "****" : "");
    cJSON_AddStringToObject(root, "webdav_base_path", cfg->webdav_base_path);
    cJSON_AddBoolToObject(root, "alert_webhook_enabled", cfg->alert_webhook_enabled);
    cJSON_AddStringToObject(root, "alert_webhook_url", cfg->alert_webhook_url);

    /* 家族 schema 版本（契约 §1） */
    cJSON_AddNumberToObject(root, "schema_version", (double)CONFIG_SCHEMA_VERSION);

    return root;
}
