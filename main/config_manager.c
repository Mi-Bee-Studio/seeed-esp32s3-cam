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

#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "config";

static cam_config_t s_config;

_Static_assert(sizeof(cam_config_t) < 2048, "cam_config_t too large for NVS blob");

static SemaphoreHandle_t s_config_mutex = NULL;
static const cam_config_t s_defaults = {
    .wifi_ssid      = "",
    .wifi_pass      = "",
    .wifi_ssid_2    = "",
    .wifi_pass_2    = "",
    .allow_ap_fallback = false,
    .upload_base_path = "/MiBee Cam",
    .webdav_url     = "",
    .webdav_user    = "",
    .webdav_pass    = "",
    .webdav_enabled = false,
    .resolution     = 1,
    /* tuned defaults: fps=12 smoother, quality=18 halves JPEG size */
    .fps            = 12,
    .segment_sec    = 300,
    .jpeg_quality   = 18,
    .vflip          = false,
    .hmirror         = false,
    .device_name    = "MiBee Cam",
    .timezone       = "CST-8",
    .cleanup_low_pct  = 20,
    .cleanup_high_pct = 30,
    .frame_drop_enabled = true,
    .alert_webhook_url = "",
    .alert_webhook_enabled = false,
    .timelapse_interval_sec = 0,
    .timelapse_mode = 0,
    .motion_sensitivity = 50,
    .motion_active_interval_sec = 1,
    .motion_idle_interval_sec = 30,
    .video_record_to_sd = true,
    .audio_record_to_sd = false,
    .sd_log_enabled = false,
    .day_night_mode = 0,
    .web_password   = "admin",
    .schema_version = CONFIG_CURRENT_VERSION,
    .xclk_freq_mhz = 16,
    .wifi_roam_rssi_threshold = -75,
    .wifi_roam_rssi_gap = 10,
};

/* ---- internal helpers ---- */

/** @brief 从 NVS 读取字符串值，不存在则保留默认值 */
static esp_err_t read_str(nvs_handle_t h, const char *key, char *out, size_t max_len)
{
    // First query the required length without touching the output buffer
    size_t required_len = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &required_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   // key not found, keep default value in out
    }
    if (err != ESP_OK) {
        return err;      // other error
    }
    // Key exists — read it (nvs_get_str needs len >= required_len)
    if (required_len > max_len) {
        required_len = max_len;
    }
    return nvs_get_str(h, key, out, &required_len);
}

/** @brief 向 NVS 写入字符串值 */
static esp_err_t write_str(nvs_handle_t h, const char *key, const char *val)
{
    return nvs_set_str(h, key, val);
}

/* ---- config TXT parser ---- */

/** @brief 解析 KEY=VALUE 格式的文本行，提取字符串值 */
static void parse_line(char *line, const char *key, char *dest, size_t dest_len)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return;
    if (line[klen] != '=') return;
    
    const char *val = line + klen + 1;
    // trim trailing \r\n
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n')) {
        vlen--;
    }
    if (vlen >= dest_len) vlen = dest_len - 1;
    memcpy(dest, val, vlen);
    dest[vlen] = '\0';
}

/** @brief 解析 KEY=VALUE 格式的文本行，提取 uint8 数值 */

/** @brief 解析 KEY=VALUE 格式的文本行，提取布尔值 */
static void parse_bool(char *line, const char *key, bool *dest)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return;
    if (line[klen] != '=') return;
    char val[32];
    strncpy(val, line + klen + 1, sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    // trim trailing \r\n
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == '\r' || val[vlen - 1] == '\n')) {
        val[--vlen] = '\0';
    }
    *dest = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
}

/** @brief 从 SD 卡读取 wifi.txt 配置文件并覆盖 WiFi 配置 */
static void parse_wifi_txt(void)
{
    bool one_time = false;
    FILE *f = fopen("/sdcard/config/wifi.txt", "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found wifi.txt on SD card");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_line(line, "SSID", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
        parse_line(line, "PASS", s_config.wifi_pass, sizeof(s_config.wifi_pass));
        parse_line(line, "SSID2", s_config.wifi_ssid_2, sizeof(s_config.wifi_ssid_2));
        parse_line(line, "PASS2", s_config.wifi_pass_2, sizeof(s_config.wifi_pass_2));
        parse_bool(line, "ALLOW_AP_FALLBACK", &s_config.allow_ap_fallback);
        parse_line(line, "TIMEZONE", s_config.timezone, sizeof(s_config.timezone));
        parse_bool(line, "one_time", &one_time);
    }
    fclose(f);
    if (one_time) {
        remove("/sdcard/config/wifi.txt");
        ESP_LOGW(TAG, "Deleted one-time config: /sdcard/config/wifi.txt");
    }
}

/** @brief 从 SD 卡读取 nas.txt 配置文件并覆盖 NAS 上传配置 */
static void parse_nas_txt(void)
{
    bool one_time = false;
    FILE *f = fopen("/sdcard/config/nas.txt", "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found nas.txt on SD card");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        parse_line(line, "UPLOAD_BASE_PATH", s_config.upload_base_path, sizeof(s_config.upload_base_path));
        parse_line(line, "WEBDAV_URL", s_config.webdav_url, sizeof(s_config.webdav_url));
        parse_line(line, "WEBDAV_USER", s_config.webdav_user, sizeof(s_config.webdav_user));
        parse_line(line, "WEBDAV_PASS", s_config.webdav_pass, sizeof(s_config.webdav_pass));
        parse_bool(line, "one_time", &one_time);
    }
    fclose(f);
    if (one_time) {
        remove("/sdcard/config/nas.txt");
        ESP_LOGW(TAG, "Deleted one-time config: /sdcard/config/nas.txt");
    }
}

/** @brief 从 SD 卡读取 config.txt 配置文件并覆盖 WiFi 配置 */
static void parse_config_txt(void)
{
    bool one_time = false;
    FILE *f = fopen("/sdcard/config.txt", "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found config.txt on SD card");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_line(line, "WIFI.SSID", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
        parse_line(line, "WIFI.PASS", s_config.wifi_pass, sizeof(s_config.wifi_pass));
        parse_line(line, "WIFI.SSID2", s_config.wifi_ssid_2, sizeof(s_config.wifi_ssid_2));
        parse_line(line, "WIFI.PASS2", s_config.wifi_pass_2, sizeof(s_config.wifi_pass_2));
        parse_bool(line, "ALLOW_AP_FALLBACK", &s_config.allow_ap_fallback);
        parse_bool(line, "VIDEO_RECORD_TO_SD", &s_config.video_record_to_sd);
        parse_bool(line, "AUDIO_RECORD_TO_SD", &s_config.audio_record_to_sd);
        parse_bool(line, "SD_LOG_ENABLED", &s_config.sd_log_enabled);
        parse_bool(line, "one_time", &one_time);
    }
    fclose(f);
    if (one_time) {
        remove("/sdcard/config.txt");
        ESP_LOGW(TAG, "Deleted one-time config: /sdcard/config.txt");
    }
}

/* ---- optimal timelapse parameter mapping ---- */

typedef struct {
    uint8_t  interval_sec;
    uint16_t segment_sec_optimal;
    uint8_t  fps_optimal;
} optimal_timelapse_entry_t;

/**
 * @brief 最优配置映射表：根据 timelapse_interval_sec 查表得到最优 segment_sec 和 fps
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
    /* 注：timelapse_interval_sec 为 uint8_t（最大 255），300 通过最近邻匹配到本表 */
};

#define OPTIMAL_TIMELAPSE_ENTRIES (sizeof(s_optimal_timelapse) / sizeof(s_optimal_timelapse[0]))

/** @brief 根据 timelapse_interval_sec 查最优映射，返回最接近的索引 */
static int optimal_lookup(uint8_t interval_sec)
{
    for (int i = 0; i < (int)OPTIMAL_TIMELAPSE_ENTRIES; i++) {
        if (s_optimal_timelapse[i].interval_sec == interval_sec) {
            return i;
        }
    }
    /* Fallback: 不在表中的间隔值，使用最近邻匹配 */
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
 * @brief 根据拍摄间隔和录制模式计算最优片段时长和帧率
 *
 * - timelapse_mode=0（连续录制）：不做任何修改，保留用户自定义值
 * - timelapse_mode=1（普通延时）：根据 timelapse_interval_sec 查表
 * - timelapse_mode=2（动态延时）：固定值 1800s，fps=15
 */
void config_apply_optimal(cam_config_t *cfg)
{
    if (!cfg) return;

    if (cfg->timelapse_mode == 1) {
        int idx = optimal_lookup(cfg->timelapse_interval_sec);
        cfg->segment_sec = s_optimal_timelapse[idx].segment_sec_optimal;
        cfg->fps         = s_optimal_timelapse[idx].fps_optimal;
        ESP_LOGD(TAG, "optimal: timelapse interval=%d → segment_sec=%u, fps=%u",
                 cfg->timelapse_interval_sec, cfg->segment_sec, cfg->fps);
    } else if (cfg->timelapse_mode == 2) {
        cfg->segment_sec = 1800;
        cfg->fps         = 15;
        ESP_LOGD(TAG, "optimal: dynamic timelapse → segment_sec=%u, fps=%u",
                 cfg->segment_sec, cfg->fps);
    }
    /* 连续模式 (mode=0)：不修改，保留用户配置 */
}

/* ---- public API ---- */

/** @brief 初始化配置模块，初始化 NVS 并加载存储的配置，无存储则使用默认值 */
esp_err_t config_init(void)
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();

    // Initialize config mutex
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

    // Apply defaults first
    s_config = s_defaults;

    // Open NVS and read stored values
    nvs_handle_t h;
    err = nvs_open("cam_config", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored config, using defaults");
        return ESP_OK;  // defaults are already applied
    }

    read_str(h, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    read_str(h, "wifi_pass", s_config.wifi_pass, sizeof(s_config.wifi_pass));
    read_str(h, "wifi_ssid_2", s_config.wifi_ssid_2, sizeof(s_config.wifi_ssid_2));
    read_str(h, "wifi_pass_2", s_config.wifi_pass_2, sizeof(s_config.wifi_pass_2));
    nvs_get_u8(h, "allow_ap_fallback", (uint8_t *)&s_config.allow_ap_fallback);
    read_str(h, "upload_base_path", s_config.upload_base_path, sizeof(s_config.upload_base_path));
    nvs_get_u8(h, "webdav_enabled", (uint8_t *)&s_config.webdav_enabled);
    read_str(h, "webdav_url", s_config.webdav_url, sizeof(s_config.webdav_url));
    read_str(h, "webdav_user", s_config.webdav_user, sizeof(s_config.webdav_user));
    read_str(h, "webdav_pass", s_config.webdav_pass, sizeof(s_config.webdav_pass));
    nvs_get_u8(h, "resolution", &s_config.resolution);
    nvs_get_u8(h, "fps", &s_config.fps);
    nvs_get_u16(h, "segment_sec", &s_config.segment_sec);
    nvs_get_u8(h, "jpeg_quality", &s_config.jpeg_quality);
    nvs_get_u8(h, "vflip", (uint8_t *)&s_config.vflip);
    nvs_get_u8(h, "hmirror", (uint8_t *)&s_config.hmirror);
    read_str(h, "web_password", s_config.web_password, sizeof(s_config.web_password));
    read_str(h, "device_name", s_config.device_name, sizeof(s_config.device_name));
    read_str(h, "timezone", s_config.timezone, sizeof(s_config.timezone));
    nvs_get_u8(h, "cleanup_low", &s_config.cleanup_low_pct);
    nvs_get_u8(h, "frame_drop", (uint8_t *)&s_config.frame_drop_enabled);
    nvs_get_u8(h, "cleanup_high", &s_config.cleanup_high_pct);
    read_str(h, "alert_webhook_url", s_config.alert_webhook_url, sizeof(s_config.alert_webhook_url));
    nvs_get_u8(h, "alert_webhook_enabled", (uint8_t *)&s_config.alert_webhook_enabled);
    nvs_get_u8(h, "timelapse_interval", &s_config.timelapse_interval_sec);
    nvs_get_u8(h, "timelapse_mode", &s_config.timelapse_mode);
    nvs_get_u8(h, "motion_sens", &s_config.motion_sensitivity);
    nvs_get_u8(h, "motion_active_int", &s_config.motion_active_interval_sec);
    nvs_get_u8(h, "motion_idle_int", &s_config.motion_idle_interval_sec);
    nvs_get_u8(h, "video_record_to_sd", (uint8_t *)&s_config.video_record_to_sd);
    nvs_get_u8(h, "audio_record_to_sd", (uint8_t *)&s_config.audio_record_to_sd);
    nvs_get_u8(h, "sd_log_enabled", (uint8_t *)&s_config.sd_log_enabled);
    nvs_get_u8(h, "day_night", &s_config.day_night_mode);
    if (s_config.day_night_mode > 2) {
        ESP_LOGW(TAG, "day_night_mode=%d out of range, resetting to 0", s_config.day_night_mode);
        s_config.day_night_mode = 0;
    }

    /* Schema v2 新增字段 */
    nvs_get_u8(h, "xclk_freq_mhz", &s_config.xclk_freq_mhz);
    nvs_get_i8(h, "wifi_roam_rssi", &s_config.wifi_roam_rssi_threshold);
    nvs_get_u8(h, "wifi_roam_gap", &s_config.wifi_roam_rssi_gap);
    uint8_t stored_schema_version = 0;
    nvs_get_u8(h, "schema_version", &stored_schema_version);

    /* Migration: web_password must not be empty (older firmware defaulted to "") */
    if (s_config.web_password[0] == '\0') {
        ESP_LOGI(TAG, "web_password empty, setting default 'admin'");
        strcpy(s_config.web_password, "admin");
    }

    /* ---- Schema v1→v2 迁移：旧固件没有这些字段，重置为默认值 ---- */
    if (stored_schema_version < CONFIG_CURRENT_VERSION) {
        ESP_LOGI(TAG, "Migrating config schema v%d→v%d", stored_schema_version, CONFIG_CURRENT_VERSION);
        if (s_config.xclk_freq_mhz == 0 || s_config.xclk_freq_mhz > 20) s_config.xclk_freq_mhz = 16;
        if (s_config.wifi_roam_rssi_threshold == 0) s_config.wifi_roam_rssi_threshold = -75;
        if (s_config.wifi_roam_rssi_gap == 0) s_config.wifi_roam_rssi_gap = 10;
        s_config.schema_version = CONFIG_CURRENT_VERSION;
        /* 持久化迁移结果 */
        nvs_handle_t h_m;
        if (nvs_open("cam_config", NVS_READWRITE, &h_m) == ESP_OK) {
            nvs_set_u8(h_m, "schema_version", CONFIG_CURRENT_VERSION);
            nvs_set_u8(h_m, "xclk_freq_mhz", s_config.xclk_freq_mhz);
            nvs_set_i8(h_m, "wifi_roam_rssi", s_config.wifi_roam_rssi_threshold);
            nvs_set_u8(h_m, "wifi_roam_gap", s_config.wifi_roam_rssi_gap);
            nvs_commit(h_m);
            nvs_close(h_m);
        }
        ESP_LOGI(TAG, "Config migrated to schema v%d", CONFIG_CURRENT_VERSION);
    }

    /* ---- Legacy FTP config migration check ---- */
    uint8_t old_ftp_enabled = 0;
    bool needs_migration = (nvs_get_u8(h, "ftp_enabled", &old_ftp_enabled) == ESP_OK);

    nvs_close(h);

    /* Backward compat: old configs had timelapse_interval_sec>0 but no timelapse_mode */
    if (s_config.timelapse_interval_sec > 0 && s_config.timelapse_mode == 0) {
        s_config.timelapse_mode = 1;
        ESP_LOGI(TAG, "Migrated: timelapse_mode=1 (normal timelapse)");
        /* Will be persisted on next config_save() */
    }

    if (needs_migration) {
        ESP_LOGI(TAG, "Migrating legacy FTP config to webdav_enabled");
        if (old_ftp_enabled || s_config.webdav_enabled) {
            s_config.webdav_enabled = true;
        }
        // Erase legacy FTP keys (need readwrite handle)
        nvs_handle_t h_m;
        if (nvs_open("cam_config", NVS_READWRITE, &h_m) == ESP_OK) {
            nvs_set_u8(h_m, "webdav_enabled", s_config.webdav_enabled ? 1 : 0);
            nvs_erase_key(h_m, "ftp_enabled");
            nvs_erase_key(h_m, "ftp_host");
            nvs_erase_key(h_m, "ftp_port");
            nvs_erase_key(h_m, "ftp_user");
            nvs_erase_key(h_m, "ftp_pass");
            nvs_erase_key(h_m, "ftp_path");
            nvs_commit(h_m);
            nvs_close(h_m);
        }
        ESP_LOGI(TAG, "Migration complete: webdav_enabled=%d", s_config.webdav_enabled);
        return ESP_OK;
    }

    /* Apply optimal timelapse parameters after loading */
    config_apply_optimal(&s_config);

    ESP_LOGI(TAG, "Config loaded from NVS");
    return ESP_OK;
}

/** @brief 获取当前配置指针（指向全局静态配置结构体） */
cam_config_t* config_get(void)
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
 * Only validates hard constraints — mode-specific fields are checked
 * when the corresponding mode is active, not at save time.
 */
bool config_validate(const cam_config_t *cfg)
{
    if (!cfg) return false;

    /* ---- core numeric ranges ---- */
    if (cfg->resolution > 7)         { ESP_LOGW(TAG, "validate: resolution=%d > 7", cfg->resolution); return false; }
    if (cfg->fps < 1 || cfg->fps > 30) { ESP_LOGW(TAG, "validate: fps=%d out of 1-30", cfg->fps); return false; }
    if (cfg->jpeg_quality < 1 || cfg->jpeg_quality > 63)
                                       { ESP_LOGW(TAG, "validate: jpeg_quality=%d out of 1-63", cfg->jpeg_quality); return false; }
    if (cfg->segment_sec < 5 || cfg->segment_sec > 3600)
                                       { ESP_LOGW(TAG, "validate: segment_sec=%d out of 5-3600", cfg->segment_sec); return false; }
    if (cfg->timelapse_mode > 2)       { ESP_LOGW(TAG, "validate: timelapse_mode=%d > 2", cfg->timelapse_mode); return false; }
    if (cfg->motion_sensitivity > 100) { ESP_LOGW(TAG, "validate: motion_sensitivity=%d > 100", cfg->motion_sensitivity); return false; }
    if (cfg->cleanup_low_pct < 5 || cfg->cleanup_low_pct > 50)
                                       { ESP_LOGW(TAG, "validate: cleanup_low_pct=%d out of 5-50", cfg->cleanup_low_pct); return false; }
    if (cfg->cleanup_high_pct > 80 ||
        cfg->cleanup_high_pct < cfg->cleanup_low_pct + 5)
                                       { ESP_LOGW(TAG, "validate: cleanup_high_pct=%d invalid vs low=%d", cfg->cleanup_high_pct, cfg->cleanup_low_pct); return false; }
    if (cfg->day_night_mode > 2)
    { ESP_LOGW(TAG, "validate: day_night_mode=%d out of 0-2, clamping to 0", cfg->day_night_mode); }

    /* timelapse_interval_sec: 0 means "use recorder default" — always valid */

    /* motion fields: validated at usage time, not at save time */

    /* ---- string fields — only check buffer overflow ---- */
    if (strnlen(cfg->timezone, sizeof(cfg->timezone)) >= sizeof(cfg->timezone))  { ESP_LOGW(TAG, "validate: timezone overflow"); return false; }
    if (strnlen(cfg->device_name, sizeof(cfg->device_name)) >= sizeof(cfg->device_name)) { ESP_LOGW(TAG, "validate: device_name overflow"); return false; }
    if (strnlen(cfg->wifi_ssid, sizeof(cfg->wifi_ssid)) >= sizeof(cfg->wifi_ssid)) { ESP_LOGW(TAG, "validate: wifi_ssid overflow"); return false; }
    if (strnlen(cfg->wifi_pass, sizeof(cfg->wifi_pass)) >= sizeof(cfg->wifi_pass)) { ESP_LOGW(TAG, "validate: wifi_pass overflow"); return false; }
    if (strnlen(cfg->webdav_url, sizeof(cfg->webdav_url)) >= sizeof(cfg->webdav_url)) { ESP_LOGW(TAG, "validate: webdav_url overflow"); return false; }
    if (strnlen(cfg->webdav_user, sizeof(cfg->webdav_user)) >= sizeof(cfg->webdav_user)) { ESP_LOGW(TAG, "validate: webdav_user overflow"); return false; }
    if (strnlen(cfg->webdav_pass, sizeof(cfg->webdav_pass)) >= sizeof(cfg->webdav_pass)) { ESP_LOGW(TAG, "validate: webdav_pass overflow"); return false; }

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
    esp_err_t err = nvs_open("cam_config", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    write_str(h, "wifi_ssid", s_config.wifi_ssid);
    write_str(h, "wifi_pass", s_config.wifi_pass);
    write_str(h, "wifi_ssid_2", s_config.wifi_ssid_2);
    write_str(h, "wifi_pass_2", s_config.wifi_pass_2);
    nvs_set_u8(h, "allow_ap_fallback", s_config.allow_ap_fallback ? 1 : 0);
    write_str(h, "upload_base_path", s_config.upload_base_path);
    write_str(h, "webdav_url", s_config.webdav_url);
    write_str(h, "webdav_user", s_config.webdav_user);
    write_str(h, "webdav_pass", s_config.webdav_pass);
    nvs_set_u8(h, "webdav_enabled", s_config.webdav_enabled ? 1 : 0);
    nvs_set_u8(h, "resolution", s_config.resolution);
    nvs_set_u8(h, "fps", s_config.fps);
    nvs_set_u16(h, "segment_sec", s_config.segment_sec);
    nvs_set_u8(h, "jpeg_quality", s_config.jpeg_quality);
    nvs_set_u8(h, "vflip", s_config.vflip ? 1 : 0);
    nvs_set_u8(h, "hmirror", s_config.hmirror ? 1 : 0);
    write_str(h, "web_password", s_config.web_password);
    write_str(h, "device_name", s_config.device_name);
    write_str(h, "timezone", s_config.timezone);
    nvs_set_u8(h, "cleanup_low", s_config.cleanup_low_pct);
    nvs_set_u8(h, "cleanup_high", s_config.cleanup_high_pct);
    nvs_set_u8(h, "frame_drop", s_config.frame_drop_enabled ? 1 : 0);
    write_str(h, "alert_webhook_url", s_config.alert_webhook_url);
    nvs_set_u8(h, "alert_webhook_enabled", s_config.alert_webhook_enabled ? 1 : 0);
    nvs_set_u8(h, "timelapse_interval", s_config.timelapse_interval_sec);
    nvs_set_u8(h, "timelapse_mode", s_config.timelapse_mode);
    nvs_set_u8(h, "motion_sens", s_config.motion_sensitivity);
    nvs_set_u8(h, "motion_active_int", s_config.motion_active_interval_sec);
    nvs_set_u8(h, "motion_idle_int", s_config.motion_idle_interval_sec);
    nvs_set_u8(h, "video_record_to_sd", s_config.video_record_to_sd ? 1 : 0);
    nvs_set_u8(h, "audio_record_to_sd", s_config.audio_record_to_sd ? 1 : 0);
    nvs_set_u8(h, "sd_log_enabled", s_config.sd_log_enabled ? 1 : 0);
    nvs_set_u8(h, "day_night", s_config.day_night_mode);
    nvs_set_u8(h, "schema_version", s_config.schema_version);
    nvs_set_u8(h, "xclk_freq_mhz", s_config.xclk_freq_mhz);
    nvs_set_i8(h, "wifi_roam_rssi", s_config.wifi_roam_rssi_threshold);
    nvs_set_u8(h, "wifi_roam_gap", s_config.wifi_roam_rssi_gap);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

/** @brief 恢复出厂默认配置并保存到 NVS */
esp_err_t config_reset(void)
{
    s_config = s_defaults;
    ESP_LOGI(TAG, "Config reset to factory defaults");
    return config_save();
}

/** @brief 从 SD 卡加载 wifi.txt 和 nas.txt 配置文件，覆盖当前配置并保存到 NVS */
esp_err_t config_load_from_sd(void)
{
    parse_wifi_txt();
    parse_config_txt();
    parse_nas_txt();

    // If we parsed anything, persist back to NVS
    // (Always save so NVS stays in sync with SD overrides)
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