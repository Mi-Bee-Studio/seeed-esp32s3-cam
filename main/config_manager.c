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

#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config";

static cam_config_t s_config;

static const cam_config_t s_defaults = {
    .wifi_ssid      = "",
    .wifi_pass      = "",
    .ftp_host       = "",
    .ftp_port       = 21,
    .ftp_user       = "",
    .ftp_pass       = "",
    .ftp_path       = "/ParrotCam",
    .ftp_enabled    = false,
    .webdav_url     = "",
    .webdav_user    = "",
    .webdav_pass    = "",
    .webdav_enabled = false,
    .resolution     = 1,     // SVGA
    .fps            = 10,
    .segment_sec    = 300,
    .jpeg_quality   = 12,
    .web_password   = "admin",
    .device_name    = "ParrotCam",
};

/* ---- internal helpers ---- */

/** @brief 从 NVS 读取字符串值，不存在则保留默认值 */
static esp_err_t read_str(nvs_handle_t h, const char *key, char *out, size_t max_len)
{
    size_t len = max_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   // keep default
    }
    return err;
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
static void parse_uint8(char *line, const char *key, uint8_t *dest)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return;
    if (line[klen] != '=') return;
    *dest = (uint8_t)atoi(line + klen + 1);
}

/** @brief 解析 KEY=VALUE 格式的文本行，提取 uint16 数值 */
static void parse_uint16(char *line, const char *key, uint16_t *dest)
{
    size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return;
    if (line[klen] != '=') return;
    *dest = (uint16_t)atoi(line + klen + 1);
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
    FILE *f = fopen("/sdcard/config/wifi.txt", "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found wifi.txt on SD card");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_line(line, "SSID", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
        parse_line(line, "PASS", s_config.wifi_pass, sizeof(s_config.wifi_pass));
    }
    fclose(f);
}

/** @brief 从 SD 卡读取 nas.txt 配置文件并覆盖 NAS 上传配置 */
static void parse_nas_txt(void)
{
    FILE *f = fopen("/sdcard/config/nas.txt", "r");
    if (!f) return;

    ESP_LOGI(TAG, "Found nas.txt on SD card");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_line(line, "FTP_HOST", s_config.ftp_host, sizeof(s_config.ftp_host));
        parse_uint16(line, "FTP_PORT", &s_config.ftp_port);
        parse_line(line, "FTP_USER", s_config.ftp_user, sizeof(s_config.ftp_user));
        parse_line(line, "FTP_PASS", s_config.ftp_pass, sizeof(s_config.ftp_pass));
        parse_line(line, "FTP_PATH", s_config.ftp_path, sizeof(s_config.ftp_path));
        parse_bool(line, "FTP_ENABLED", &s_config.ftp_enabled);
        parse_line(line, "WEBDAV_URL", s_config.webdav_url, sizeof(s_config.webdav_url));
        parse_line(line, "WEBDAV_USER", s_config.webdav_user, sizeof(s_config.webdav_user));
        parse_line(line, "WEBDAV_PASS", s_config.webdav_pass, sizeof(s_config.webdav_pass));
        parse_bool(line, "WEBDAV_ENABLED", &s_config.webdav_enabled);
    }
    fclose(f);
}

/* ---- public API ---- */

/** @brief 初始化配置模块，初始化 NVS 并加载存储的配置，无存储则使用默认值 */
esp_err_t config_init(void)
{
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
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
    read_str(h, "ftp_host", s_config.ftp_host, sizeof(s_config.ftp_host));
    nvs_get_u16(h, "ftp_port", &s_config.ftp_port);
    read_str(h, "ftp_user", s_config.ftp_user, sizeof(s_config.ftp_user));
    read_str(h, "ftp_pass", s_config.ftp_pass, sizeof(s_config.ftp_pass));
    read_str(h, "ftp_path", s_config.ftp_path, sizeof(s_config.ftp_path));
    nvs_get_u8(h, "ftp_enabled", (uint8_t *)&s_config.ftp_enabled);
    read_str(h, "webdav_url", s_config.webdav_url, sizeof(s_config.webdav_url));
    read_str(h, "webdav_user", s_config.webdav_user, sizeof(s_config.webdav_user));
    read_str(h, "webdav_pass", s_config.webdav_pass, sizeof(s_config.webdav_pass));
    nvs_get_u8(h, "webdav_enabled", (uint8_t *)&s_config.webdav_enabled);
    nvs_get_u8(h, "resolution", &s_config.resolution);
    nvs_get_u8(h, "fps", &s_config.fps);
    nvs_get_u16(h, "segment_sec", &s_config.segment_sec);
    nvs_get_u8(h, "jpeg_quality", &s_config.jpeg_quality);
    read_str(h, "web_password", s_config.web_password, sizeof(s_config.web_password));
    read_str(h, "device_name", s_config.device_name, sizeof(s_config.device_name));

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded from NVS");
    return ESP_OK;
}

/** @brief 获取当前配置指针（指向全局静态配置结构体） */
cam_config_t* config_get(void)
{
    return &s_config;
}

/** @brief 将当前配置保存到 NVS 闪存持久化 */
esp_err_t config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cam_config", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    write_str(h, "wifi_ssid", s_config.wifi_ssid);
    write_str(h, "wifi_pass", s_config.wifi_pass);
    write_str(h, "ftp_host", s_config.ftp_host);
    nvs_set_u16(h, "ftp_port", s_config.ftp_port);
    write_str(h, "ftp_user", s_config.ftp_user);
    write_str(h, "ftp_pass", s_config.ftp_pass);
    write_str(h, "ftp_path", s_config.ftp_path);
    nvs_set_u8(h, "ftp_enabled", s_config.ftp_enabled ? 1 : 0);
    write_str(h, "webdav_url", s_config.webdav_url);
    write_str(h, "webdav_user", s_config.webdav_user);
    write_str(h, "webdav_pass", s_config.webdav_pass);
    nvs_set_u8(h, "webdav_enabled", s_config.webdav_enabled ? 1 : 0);
    nvs_set_u8(h, "resolution", s_config.resolution);
    nvs_set_u8(h, "fps", s_config.fps);
    nvs_set_u16(h, "segment_sec", s_config.segment_sec);
    nvs_set_u8(h, "jpeg_quality", s_config.jpeg_quality);
    write_str(h, "web_password", s_config.web_password);
    write_str(h, "device_name", s_config.device_name);

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
    parse_nas_txt();

    // If we parsed anything, persist back to NVS
    // (Always save so NVS stays in sync with SD overrides)
    return config_save();
}
