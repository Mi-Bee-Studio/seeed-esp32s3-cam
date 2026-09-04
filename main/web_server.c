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

#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "device_id.h"
#include "storage_manager.h"
#include "time_sync.h"
#include "camera_driver.h"
#include "video_recorder.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "nas_uploader.h"
#include "ws_server.h"
#include "motion_detector.h"

#include "ota_updater.h"
#include "audio_broadcaster.h"
#include "mjpeg_streamer.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "sha256.h"
#include "onvif_service.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "frame_broadcaster.h"

static const char *TAG = "web";

extern float get_chip_temp(void);

static httpd_handle_t s_server = NULL;
static uint16_t s_port = 0;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */


/** @brief 验证请求中的密码，仅检查X-Password头部（移除了?password=查询参数回退） */
static bool check_password(httpd_req_t *req)
{
    /* Only check X-Password header */
    char password[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Password", password, sizeof(password)) == ESP_OK) {
        cam_config_t *cfg = config_get();
        if (strcmp(password, cfg->web_password) == 0) return true;
    }
    return false;
}

/* 公开 wrapper，供 ota_updater.c 复用 - 返回 esp_err_t 以处理 SET_PASSWORD_FIRST 状态 */
esp_err_t web_server_check_auth(httpd_req_t *req) {
    cam_config_t *cfg = config_get();
    
    /* State A: web_password is empty - return SET_PASSWORD_FIRST for all write ops */
    if (cfg->web_password[0] == '\0') {
        return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
    }
    
    /* State B: web_password is set - require X-Password header */
    if (!check_password(req)) {
        return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
    }
    
    return ESP_OK;
}
/** @brief 设置跨域资源共享(CORS)响应头，允许所有来源访问 */
static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Password");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

/** @brief 检查认证状态，返回错误如果未授权或需要先设置密码 */
static esp_err_t require_auth(httpd_req_t *req, bool is_config_endpoint)
{
    cam_config_t *cfg = config_get();
    
    /* State A: web_password is empty - only allow POST /api/config with web_password field */
    if (cfg->web_password[0] == '\0') {
        if (is_config_endpoint) {
            /* This is a first-time password setup - allow it */
            return ESP_OK;
        }
        /* All other write operations blocked until password is set */
        return json_error(req, "SET_PASSWORD_FIRST", HTTPD_401_UNAUTHORIZED);
    }
    
    /* State B: web_password is set - require X-Password header */
    if (!check_password(req)) {
        return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
    }
    
    return ESP_OK;
}

/** @brief 发送JSON成功响应，格式为 {"ok":true,"data":...} */
esp_err_t json_ok(httpd_req_t *req, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (data) cJSON_AddItemToObject(root, "data", data);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/** @brief 发送JSON错误响应，格式为 {"ok":false,"error":...} */
esp_err_t json_error(httpd_req_t *req, const char *msg, int status)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    httpd_resp_send_err(req, status, json);
    free(json);
    return ESP_FAIL;
}

/** @brief Send JSON error with string status line for codes not in httpd_err_code_t (e.g., "409 Conflict") */
esp_err_t json_error_status(httpd_req_t *req, const char *msg, const char *status_line)
{
    char json[256];
    int len = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", msg);
    if (len >= (int)sizeof(json)) len = (int)sizeof(json) - 1;
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    return httpd_resp_send(req, json, len);
}

/* Read the full request body into a heap-allocated buffer (caller frees). */
/** @brief 读取HTTP请求体到堆分配的缓冲区（调用者负责释放内存） */
char *read_body(httpd_req_t *req, size_t max_len)
{
    size_t len = req->content_len;
    if (len == 0 || len > max_len) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) { free(buf); return NULL; }
    buf[ret] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/*  GET /api/status                                                    */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/status请求，返回设备完整状态（录像、WiFi、存储、摄像头、运行时间等） */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();

    /* Recording state */
    recorder_state_t rs = recorder_get_state();
    cJSON_AddStringToObject(data, "recording",
        rs == RECORDER_RECORDING ? "recording" :
        rs == RECORDER_PAUSED   ? "paused" :
        rs == RECORDER_ERROR    ? "error" : "idle");
    cJSON_AddStringToObject(data, "current_file",
        rs == RECORDER_RECORDING || rs == RECORDER_PAUSED ? recorder_get_current_file() : "");

    /* SD card */
    storage_info_t sd_info;
    if (storage_get_info(&sd_info) == ESP_OK) {
        cJSON_AddNumberToObject(data, "sd_total_bytes", (double)sd_info.total_bytes);
        cJSON_AddNumberToObject(data, "sd_free_bytes", (double)sd_info.free_bytes);
    }
    cJSON_AddNumberToObject(data, "sd_free_percent", storage_get_free_percent());
    cJSON_AddBoolToObject(data, "sd_present", storage_is_available());

    /* Device + WiFi (统一契约: wifi_state 为小写枚举 ap|connecting|connected|disconnected) */
    cam_config_t *cfg = config_get();
    cJSON_AddStringToObject(data, "device_name",
        cfg->device_name[0] ? cfg->device_name : "MiBeeCam");
    cJSON_AddStringToObject(data, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(data, "current_ssid", wifi_get_current_ssid());
    /* 当前使用的配置槽位（契约字段，SPA WiFi 页"主网络/备用网络"标识用） */
    cJSON_AddStringToObject(data, "wifi_net", wifi_using_backup() ? "secondary" : "primary");
    wifi_state_t ws = wifi_get_state();
    cJSON_AddStringToObject(data, "wifi_state",
        ws == WIFI_STATE_AP ? "ap" :
        ws == WIFI_STATE_STA_CONNECTING ? "connecting" :
        ws == WIFI_STATE_STA_CONNECTED ? "connected" : "disconnected");

    cJSON_AddStringToObject(data, "ip", wifi_get_ip_str());

    /* WiFi RSSI, channel, and gateway (only in STA mode) */
    if (ws == WIFI_STATE_STA_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddNumberToObject(data, "wifi_rssi", ap_info.rssi);
            cJSON_AddNumberToObject(data, "wifi_channel", (double)ap_info.primary);
        }
        esp_netif_t *netif = wifi_get_sta_netif();
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char gateway_str[16];
                snprintf(gateway_str, sizeof(gateway_str), IPSTR, IP2STR(&ip_info.gw));
                cJSON_AddStringToObject(data, "wifi_gateway", gateway_str);
            }
        }
    }

    cJSON_AddStringToObject(data, "camera",
        camera_get_sensor() == CAMERA_SENSOR_OV2640 ? "OV2640" :
        camera_get_sensor() == CAMERA_SENSOR_OV3660 ? "OV3660" :
        camera_get_sensor() == CAMERA_SENSOR_OV5640 ? "OV5640" : "unknown");
    cJSON_AddStringToObject(data, "resolution", camera_res_to_str(camera_get_resolution()));

    /* Supported resolutions for the detected sensor (for dynamic UI filtering) */
    {
        camera_res_t max_res = camera_get_effective_max_res();
        cJSON *res_arr = cJSON_CreateArray();
        for (int r = CAMERA_RES_VGA; r <= (int)max_res; r++) {
            cJSON *item = cJSON_CreateObject();
            switch (r) {
                case CAMERA_RES_VGA:  cJSON_AddStringToObject(item, "label", "VGA (640x480)");   break;
                case CAMERA_RES_SVGA: cJSON_AddStringToObject(item, "label", "SVGA (800x600)");  break;
                case CAMERA_RES_XGA:  cJSON_AddStringToObject(item, "label", "XGA (1024x768)");  break;
                case CAMERA_RES_HD:   cJSON_AddStringToObject(item, "label", "HD (1280x720)");   break;
                case CAMERA_RES_SXGA: cJSON_AddStringToObject(item, "label", "SXGA (1280x1024)");break;
                case CAMERA_RES_UXGA: cJSON_AddStringToObject(item, "label", "UXGA (1600x1200)");break;
                case CAMERA_RES_FHD:  cJSON_AddStringToObject(item, "label", "FHD (1920x1080)"); break;
                case CAMERA_RES_QXGA: cJSON_AddStringToObject(item, "label", "QXGA (2048x1536)");break;
                default: continue;
            }
            cJSON_AddNumberToObject(item, "value", r);
            cJSON_AddItemToArray(res_arr, item);
        }
        cJSON_AddItemToObject(data, "supported_resolutions", res_arr);
    }

    /* Time */
    cJSON_AddBoolToObject(data, "time_synced", time_is_synced());
    char current_time[32] = "";
    time_get_str(current_time, sizeof(current_time));
    cJSON_AddStringToObject(data, "current_time", current_time);

    /* Uptime (seconds since boot) */
    int64_t uptime = esp_timer_get_time() / 1000000;
    cJSON_AddNumberToObject(data, "uptime", (double)uptime);
    cJSON_AddNumberToObject(data, "reset_reason", (double)esp_reset_reason());
    cJSON_AddNumberToObject(data, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(data, "min_heap", (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(data, "free_psram", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* MJPEG 流客户端（独立 :81 服务器） */
    cJSON_AddNumberToObject(data, "stream_clients", (double)mjpeg_streamer_client_count());
    cJSON_AddNumberToObject(data, "stream_clients_max", 3);

    /* NAS upload status */
    char last_upload[32] = "";
    int upload_queue = 0;
    bool upload_paused = false;
    nas_uploader_get_status(last_upload, sizeof(last_upload), &upload_queue, &upload_paused);
    cJSON_AddStringToObject(data, "last_upload", last_upload);
    cJSON_AddNumberToObject(data, "upload_queue", upload_queue);
    cJSON_AddNumberToObject(data, "chip_temp", (double)get_chip_temp());
    cJSON_AddNumberToObject(data, "frames_dropped", (double)recorder_get_frames_dropped());
    cJSON_AddStringToObject(data, "firmware_version", FW_VERSION);
    cJSON_AddNumberToObject(data, "timelapse_interval_sec", cfg->timelapse_interval_sec);
    cJSON_AddNumberToObject(data, "timelapse_mode", cfg->timelapse_mode);
    cJSON_AddNumberToObject(data, "motion_sensitivity", cfg->motion_sensitivity);
    cJSON_AddNumberToObject(data, "motion_active_interval_sec", cfg->motion_active_interval_sec);
    cJSON_AddNumberToObject(data, "motion_idle_interval_sec", cfg->motion_idle_interval_sec);
    cJSON_AddNumberToObject(data, "motion_score", motion_detector_get_score());

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/capabilities                                             */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/capabilities请求，返回设备功能能力标志 */
static esp_err_t api_capabilities_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();

    /* 契约 v1.1：12 个布尔能力位 + api_version/wifi_scan（见 docs/api-contract.md） */
    cJSON_AddStringToObject(data, "api_version", "1.2");
    cJSON_AddBoolToObject(data, "wifi_scan", true);
    cJSON_AddBoolToObject(data, "ai", false);           /* On-device AI detection */
    cJSON_AddBoolToObject(data, "sd", storage_is_available());  /* SD card storage */
    cJSON_AddBoolToObject(data, "audio", true);         /* Audio streaming/recording */
    cJSON_AddBoolToObject(data, "ota", true);           /* Over-the-air firmware update */
    cJSON_AddBoolToObject(data, "mic", true);           /* Microphone present */
    cJSON_AddBoolToObject(data, "flash_led", false);   /* Flash LED controllable */
    cJSON_AddBoolToObject(data, "recording", true);    /* Video recording to storage */
    cJSON_AddBoolToObject(data, "timelapse", true);    /* Timelapse recording modes (config: timelapse_mode) */
    cJSON_AddBoolToObject(data, "onvif", true);        /* ONVIF SOAP service */
    cJSON_AddBoolToObject(data, "rtsp", true);         /* RTSP server */
    cJSON_AddBoolToObject(data, "websocket", true);    /* WebSocket push */
    cJSON_AddBoolToObject(data, "mdns", true);         /* mDNS hostname advertisement */

    return json_ok(req, data);
}


/* ------------------------------------------------------------------ */
/*  GET /api/config                                                    */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/config请求，返回当前配置（密码字段已脱敏显示） */
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    cam_config_t *cfg = config_get();
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "wifi_ssid", cfg->wifi_ssid);
    /* Mask password */
    cJSON_AddStringToObject(data, "wifi_pass", cfg->wifi_pass[0] ? "****" : "");
    cJSON_AddStringToObject(data, "wifi_ssid_2", cfg->wifi_ssid_2);
    cJSON_AddStringToObject(data, "wifi_pass_2", cfg->wifi_pass_2[0] ? "****" : "");
    cJSON_AddBoolToObject(data, "allow_ap_fallback", cfg->allow_ap_fallback);
    cJSON_AddStringToObject(data, "device_name", cfg->device_name);
    cJSON_AddStringToObject(data, "upload_base_path", cfg->upload_base_path);
    cJSON_AddBoolToObject(data, "webdav_enabled", cfg->webdav_enabled);
    cJSON_AddStringToObject(data, "webdav_url", cfg->webdav_url);
    cJSON_AddStringToObject(data, "webdav_user", cfg->webdav_user);

    cJSON_AddNumberToObject(data, "cam_framesize", cfg->cam_framesize);
    cJSON_AddNumberToObject(data, "fps", cfg->fps);
    cJSON_AddNumberToObject(data, "segment_sec", cfg->segment_sec);
    cJSON_AddNumberToObject(data, "cam_quality", cfg->cam_quality);
    cJSON_AddBoolToObject(data, "cam_vflip", cfg->cam_vflip);
    cJSON_AddBoolToObject(data, "cam_hmirror", cfg->cam_hmirror);
    cJSON_AddNumberToObject(data, "day_night_mode", cfg->day_night_mode);

    /* Mask web password */
    cJSON_AddStringToObject(data, "web_password", cfg->web_password[0] ? "****" : "");
    cJSON_AddStringToObject(data, "timezone", cfg->timezone);
    cJSON_AddNumberToObject(data, "cleanup_low_pct", cfg->cleanup_low_pct);
    cJSON_AddNumberToObject(data, "cleanup_high_pct", cfg->cleanup_high_pct);
    cJSON_AddBoolToObject(data, "frame_drop_enabled", cfg->frame_drop_enabled);
    cJSON_AddNumberToObject(data, "timelapse_interval_sec", cfg->timelapse_interval_sec);
    cJSON_AddNumberToObject(data, "timelapse_mode", cfg->timelapse_mode);
    cJSON_AddNumberToObject(data, "motion_sensitivity", cfg->motion_sensitivity);
    cJSON_AddNumberToObject(data, "motion_active_interval_sec", cfg->motion_active_interval_sec);
    cJSON_AddNumberToObject(data, "motion_idle_interval_sec", cfg->motion_idle_interval_sec);
    cJSON_AddStringToObject(data, "alert_webhook_url", cfg->alert_webhook_url);
    cJSON_AddBoolToObject(data, "alert_webhook_enabled", cfg->alert_webhook_enabled);
    cJSON_AddBoolToObject(data, "video_record_to_sd", cfg->video_record_to_sd);
    cJSON_AddBoolToObject(data, "record_on_boot", cfg->record_on_boot);
    cJSON_AddBoolToObject(data, "audio_record_to_sd", cfg->audio_record_to_sd);
    cJSON_AddBoolToObject(data, "sd_log_enabled", cfg->sd_log_enabled);
    cJSON_AddNumberToObject(data, "xclk_freq_mhz", cfg->xclk_freq_mhz);
    cJSON_AddNumberToObject(data, "wifi_roam_rssi_threshold", cfg->wifi_roam_rssi_threshold);
    cJSON_AddNumberToObject(data, "wifi_roam_rssi_gap", cfg->wifi_roam_rssi_gap);
    cJSON_AddBoolToObject(data, "sd_present", storage_is_available());

    /* Add mDNS hostname for display in web UI */
    cJSON_AddStringToObject(data, "mdns_hostname", wifi_get_mdns_hostname());


    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/config                                                   */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/config请求，更新设备配置（需密码认证，支持部分更新） */
static esp_err_t api_config_post_handler(httpd_req_t *req)
{

    bool validate_uint_range(int val, int min, int max)
    {
        return (val >= min && val <= max);
    }
    esp_err_t auth_err = require_auth(req, true);  /* true = is_config_endpoint for SET_PASSWORD_FIRST handling */
    if (auth_err != ESP_OK) return auth_err;

    char *body = read_body(req, 2048);
    if (!body) return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);

    cam_config_t *cfg = config_get();
    cJSON *item;

    config_lock();

    if ((item = cJSON_GetObjectItem(json, "wifi_ssid")) && cJSON_IsString(item)) {
        strncpy(cfg->wifi_ssid, item->valuestring, sizeof(cfg->wifi_ssid) - 1);
        cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_pass")) && cJSON_IsString(item) && strcmp(item->valuestring, "****") != 0) {
        strncpy(cfg->wifi_pass, item->valuestring, sizeof(cfg->wifi_pass) - 1);
        cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_ssid_2")) && cJSON_IsString(item)) {
        strncpy(cfg->wifi_ssid_2, item->valuestring, sizeof(cfg->wifi_ssid_2) - 1);
        cfg->wifi_ssid_2[sizeof(cfg->wifi_ssid_2) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_pass_2")) && cJSON_IsString(item) && strcmp(item->valuestring, "****") != 0) {
        strncpy(cfg->wifi_pass_2, item->valuestring, sizeof(cfg->wifi_pass_2) - 1);
        cfg->wifi_pass_2[sizeof(cfg->wifi_pass_2) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "allow_ap_fallback"))) {
        cfg->allow_ap_fallback = item->valueint != 0;
    }
    if ((item = cJSON_GetObjectItem(json, "device_name")) && cJSON_IsString(item)) {
        strncpy(cfg->device_name, item->valuestring, sizeof(cfg->device_name) - 1);
        cfg->device_name[sizeof(cfg->device_name) - 1] = '\0';
    }

    if ((item = cJSON_GetObjectItem(json, "upload_base_path")) && cJSON_IsString(item)) {
        if (strstr(item->valuestring, "..") != NULL) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid upload_base_path (must not contain '..')", HTTPD_400_BAD_REQUEST);
        }
        strncpy(cfg->upload_base_path, item->valuestring, sizeof(cfg->upload_base_path) - 1);
        cfg->upload_base_path[sizeof(cfg->upload_base_path) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "webdav_enabled")))
        cfg->webdav_enabled = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "webdav_url")) && cJSON_IsString(item)) {
        strncpy(cfg->webdav_url, item->valuestring, sizeof(cfg->webdav_url) - 1);
        cfg->webdav_url[sizeof(cfg->webdav_url) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "webdav_user")) && cJSON_IsString(item)) {
        strncpy(cfg->webdav_user, item->valuestring, sizeof(cfg->webdav_user) - 1);
        cfg->webdav_user[sizeof(cfg->webdav_user) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "webdav_pass")) && cJSON_IsString(item) && strcmp(item->valuestring, "****") != 0) {
        strncpy(cfg->webdav_pass, item->valuestring, sizeof(cfg->webdav_pass) - 1);
        cfg->webdav_pass[sizeof(cfg->webdav_pass) - 1] = '\0';
    }

    uint8_t prev_resolution = cfg->cam_framesize;
    if ((item = cJSON_GetObjectItem(json, "cam_framesize"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, 0, (int)camera_get_effective_max_res())) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Invalid resolution (board-tested max: %s)",
                     camera_res_to_str(camera_get_effective_max_res()));
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        cfg->cam_framesize = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "fps"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, 1, 30)) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid fps (must be 1-30)", HTTPD_400_BAD_REQUEST);
        }
        cfg->fps = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "segment_sec"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, 5, 3600)) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid segment_sec (must be 5-3600)", HTTPD_400_BAD_REQUEST);
        }
        cfg->segment_sec = (uint16_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "cam_quality"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX)) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Invalid jpeg_quality (must be %d-%d)",
                     CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        cfg->cam_quality = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "cam_vflip")))
        cfg->cam_vflip = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "cam_hmirror")))
        cfg->cam_hmirror = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "day_night_mode"))) {
        int val = item->valueint;
        if (val >= 0 && val <= 2) {
            cfg->day_night_mode = (uint8_t)val;
        }
    }
    if ((item = cJSON_GetObjectItem(json, "xclk_freq_mhz"))) {
        int val = item->valueint;
        if (val == 10 || val == 16 || val == 20) {
            cfg->xclk_freq_mhz = (uint8_t)val;
        } else {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid xclk_freq_mhz (must be 10/16/20)", HTTPD_400_BAD_REQUEST);
        }
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_roam_rssi_threshold"))) {
        int val = item->valueint;
        /* Allow 0 (disabled) or -90 to -50 */
        if (val != 0 && (val < -90 || val > -50)) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid wifi_roam_rssi_threshold (must be 0 or -90 to -50)", HTTPD_400_BAD_REQUEST);
        }
        cfg->wifi_roam_rssi_threshold = (int8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "wifi_roam_rssi_gap"))) {
        int val = item->valueint;
        if (val < 5 || val > 15) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid wifi_roam_rssi_gap (must be 5-15)", HTTPD_400_BAD_REQUEST);
        }
        cfg->wifi_roam_rssi_gap = (uint8_t)val;
    }

    
    /* Apply camera flip/mirror immediately */
    camera_set_flip(cfg->cam_vflip, cfg->cam_hmirror);
    camera_set_day_night(cfg->day_night_mode);
    if ((item = cJSON_GetObjectItem(json, "web_password")) && cJSON_IsString(item) && strcmp(item->valuestring, "****") != 0) {
        /* 契约 v1.1：拒绝空/过短密码 — 空密码会让设备退回 SET_PASSWORD_FIRST 状态 */
        if (strlen(item->valuestring) < 6) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "web_password must be at least 6 characters", HTTPD_400_BAD_REQUEST);
        }
        strncpy(cfg->web_password, item->valuestring, sizeof(cfg->web_password) - 1);
        cfg->web_password[sizeof(cfg->web_password) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(json, "timezone")) && cJSON_IsString(item)) {
        size_t len = strlen(item->valuestring);
        if (len == 0 || len > 64) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid timezone (must be 1-64 characters)", HTTPD_400_BAD_REQUEST);
        }
        strncpy(cfg->timezone, item->valuestring, sizeof(cfg->timezone) - 1);
        cfg->timezone[sizeof(cfg->timezone) - 1] = '\0';
        /* Apply timezone change immediately */
        setenv("TZ", cfg->timezone, 1);
        tzset();
    }
    if ((item = cJSON_GetObjectItem(json, "cleanup_low_pct"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, 1, 99)) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid cleanup_low_pct (must be 1-99)", HTTPD_400_BAD_REQUEST);
        }
        cfg->cleanup_low_pct = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "cleanup_high_pct"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, 1, 100)) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid cleanup_high_pct (must be 1-100)", HTTPD_400_BAD_REQUEST);
        }
        cfg->cleanup_high_pct = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "frame_drop_enabled")))
        cfg->frame_drop_enabled = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "timelapse_interval_sec"))) {
        int val = item->valueint;
        if (!validate_uint_range(val, 0, 255)) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid timelapse_interval_sec (must be 0-255)", HTTPD_400_BAD_REQUEST);
        }
        cfg->timelapse_interval_sec = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "timelapse_mode"))) {
        int val = item->valueint;
        if (val < 0 || val > 2) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid timelapse_mode (must be 0-2)", HTTPD_400_BAD_REQUEST);
        }
        cfg->timelapse_mode = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "motion_sensitivity"))) {
        int val = item->valueint;
        if (val < 0 || val > 100) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid motion_sensitivity (must be 0-100)", HTTPD_400_BAD_REQUEST);
        }
        cfg->motion_sensitivity = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "motion_active_interval_sec"))) {
        int val = item->valueint;
        if (val < 1 || val > 30) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid motion_active_interval_sec (must be 1-30)", HTTPD_400_BAD_REQUEST);
        }
        cfg->motion_active_interval_sec = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "motion_idle_interval_sec"))) {
        int val = item->valueint;
        if (val < 5 || val > 300) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid motion_idle_interval_sec (must be 5-300)", HTTPD_400_BAD_REQUEST);
        }
        cfg->motion_idle_interval_sec = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "alert_webhook_url")) && cJSON_IsString(item)) {
        size_t len = strlen(item->valuestring);
        if (len < sizeof(cfg->alert_webhook_url)) {
            strncpy(cfg->alert_webhook_url, item->valuestring, sizeof(cfg->alert_webhook_url) - 1);
            cfg->alert_webhook_url[sizeof(cfg->alert_webhook_url) - 1] = '\0';
        }
    }
    if ((item = cJSON_GetObjectItem(json, "alert_webhook_enabled")))
        cfg->alert_webhook_enabled = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "video_record_to_sd"))) {
        /* SD absent: silently disable, don't block other config */
        cfg->video_record_to_sd = (item->valueint && !storage_is_available()) ? false : item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "record_on_boot")))
        cfg->record_on_boot = item->valueint;   /* 下次开机生效 */
    if ((item = cJSON_GetObjectItem(json, "audio_record_to_sd"))) {
        /* SD absent: silently disable, don't block other config */
        cfg->audio_record_to_sd = (item->valueint && !storage_is_available()) ? false : item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "sd_log_enabled"))) {
        /* SD absent: silently disable, don't block other config */
        cfg->sd_log_enabled = (item->valueint && !storage_is_available()) ? false : item->valueint;
    }


    cJSON_Delete(json);
    config_save();

    config_unlock();

    /* 分辨率变化走保存+重启应用（OV5640 运行时 set_framesize 无效，
     * 见 camera_driver.c 注释；2026-09-04）。 */
    if (cfg->cam_framesize != prev_resolution) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "status", "saved (rebooting to apply)");
        cJSON_AddBoolToObject(data, "rebooting", true);
        esp_err_t send_ret = json_ok(req, data);
        ESP_LOGW(TAG, "Camera resolution change %u->%u via /api/config — rebooting",
                 prev_resolution, cfg->cam_framesize);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return send_ret;
    }
    return json_ok(req, NULL);
}

/* ------------------------------------------------------------------ */
/*  GET /api/files                                                     */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/files请求，返回SD卡录像文件列表（含文件名、大小、日期） */
static esp_err_t api_files_get_handler(httpd_req_t *req)
{
    file_info_t *files = malloc(64 * sizeof(file_info_t));
    if (!files) return json_error(req, "No memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    int count = storage_list_files(files, 64);

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", files[i].name);
        cJSON_AddNumberToObject(f, "size", (double)files[i].size);
        cJSON_AddStringToObject(f, "date", files[i].time_str);
        cJSON_AddItemToArray(arr, f);
    }
    free(files);

    cJSON_AddItemToObject(data, "files", arr);

    /* Add SD card storage info */
    storage_info_t sd_info;
    if (storage_get_info(&sd_info) == ESP_OK) {
        cJSON_AddNumberToObject(data, "sd_total", (double)sd_info.total_bytes);
        cJSON_AddNumberToObject(data, "sd_free", (double)sd_info.free_bytes);
    }
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  DELETE /api/files?name=xxx                                         */
/* ------------------------------------------------------------------ */

/** @brief 处理DELETE /api/files请求，删除指定录像文件（含路径遍历攻击防护） */
static esp_err_t api_files_delete_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return json_error(req, "Missing query", HTTPD_400_BAD_REQUEST);

    char name[128] = {0};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
        return json_error(req, "Missing name parameter", HTTPD_400_BAD_REQUEST);

    /* Path traversal protection — allow subdirectory slashes but block .. */
    if (strstr(name, ".."))
        return json_error(req, "Invalid name", HTTPD_400_BAD_REQUEST);

    char path[256];
    snprintf(path, sizeof(path), "/sdcard/recordings/%s", name);

    if (remove(path) != 0)
        return json_error(req, "File not found or delete failed", HTTPD_404_NOT_FOUND);

    storage_unregister_file(name);
    return json_ok(req, NULL);
}

/* ------------------------------------------------------------------ */
/*  POST /api/files/batch                                              */
/* ------------------------------------------------------------------ */

/** @brief 递归删除 /sdcard/recordings 下全部文件（契约 v1.2 scope 支持）。
 *  跳过当前正在写入的录像段；返回删除数。 */
static int delete_recordings_recursive(const char *dir, const char *current_rel)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    int deleted = 0;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            deleted += delete_recordings_recursive(path, current_rel);
            rmdir(path);  /* 空目录一并清理，非空则忽略 */
        } else {
            const char *rel = path + strlen("/sdcard/recordings/");
            if (current_rel && strcmp(rel, current_rel) == 0) continue;
            if (remove(path) == 0) {
                storage_unregister_file(rel);
                deleted++;
            }
        }
    }
    closedir(d);
    return deleted;
}

/** @brief Batch delete multiple recording files */
static esp_err_t api_files_batch_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 16384) {
        return json_error(req, "Invalid request body", HTTPD_400_BAD_REQUEST);
    }
    char *buf = malloc(content_len + 1);
    if (!buf) return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    int len = httpd_req_recv(req, buf, content_len);
    if (len <= 0) {
        free(buf);
        return json_error(req, "Empty request body", HTTPD_400_BAD_REQUEST);
    }
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        free(buf);
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    const char *current = recorder_get_current_file();
    char current_rel_buf[128] = {0};
    if (current) {
        const char *p = strstr(current, "/recordings/");
        strncpy(current_rel_buf, p ? p + 12 : current, sizeof(current_rel_buf) - 1);
    }
    const char *current_rel = current_rel_buf[0] ? current_rel_buf : NULL;

    int deleted = 0, failed = 0;

    cJSON *names_arr = cJSON_GetObjectItem(root, "names");
    cJSON *scope = cJSON_GetObjectItem(root, "scope");

    if (cJSON_IsArray(names_arr)) {
        int total = cJSON_GetArraySize(names_arr);
        for (int i = 0; i < total; i++) {
            /* NOTE: httpd worker task is NOT subscribed to TWDT (esp_task_wdt_add was
             * never called), so esp_task_wdt_reset() here was a no-op returning
             * ESP_ERR_NOT_FOUND. Removed. The idle-task WDT (30s) still protects
             * against true hangs, and remove() per file is well under that. */

            cJSON *item = cJSON_GetArrayItem(names_arr, i);
            const char *name = cJSON_GetStringValue(item);
            if (!name) { failed++; continue; }

            /* Block path traversal */
            if (strstr(name, "..")) { failed++; continue; }

            /* Do not delete currently recording file
             * NOTE: s_current_file is absolute (/sdcard/...), name is relative */
            if (current_rel && strcmp(name, current_rel) == 0) { failed++; continue; }

            char path[256];
            snprintf(path, sizeof(path), "/sdcard/recordings/%s", name);

            if (remove(path) == 0) {
                storage_unregister_file(name);
                deleted++;
            } else {
                failed++;
            }
        }
    } else if (cJSON_IsString(scope)) {
        /* 契约 v1.2：{"scope":"all|recordings|photos"} — seeed 只有录像，
         * photos 视为空集；递归删除不受 GET 列表 64 条上限约束。 */
        if (!strcmp(scope->valuestring, "recordings") || !strcmp(scope->valuestring, "all")) {
            deleted = delete_recordings_recursive("/sdcard/recordings", current_rel);
        }
    } else {
        cJSON_Delete(root);
        free(buf);
        return json_error(req, "Missing 'names' array or 'scope' string", HTTPD_400_BAD_REQUEST);
    }

    cJSON_Delete(root);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "deleted", deleted);
    cJSON_AddNumberToObject(data, "failed", failed);
    free(buf);
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/download?name=xxx                                         */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/download请求，以AVI格式流式下载指定录像文件（含路径遍历防护） */
static esp_err_t api_download_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "DL step 1: enter");
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return json_error(req, "Missing query", HTTPD_400_BAD_REQUEST);

    ESP_LOGI(TAG, "DL step 2: query=%s", query);
    char name[128] = {0};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
        return json_error(req, "Missing name parameter", HTTPD_400_BAD_REQUEST);

    /* Path traversal protection — allow subdirectory slashes but block .. */
    if (strstr(name, ".."))
        return json_error(req, "Invalid name", HTTPD_400_BAD_REQUEST);
    
    ESP_LOGI(TAG, "DL step 3: name=%s", name);
    
    /* Block download while recording is active */
    recorder_state_t state = recorder_get_state();
    if (state == RECORDER_RECORDING || state == RECORDER_PAUSED) {
        ESP_LOGW(TAG, "DL: recording active, must stop first");
        return json_error_status(req, "Please stop recording before downloading files", "409 Conflict");
    }
    /* Check if file is currently being recorded */
    const char *current_file = recorder_get_current_file();
    if (current_file && current_file[0] != '\0') {
        const char *relpath = current_file;
        const char *prefix = "/sdcard/recordings/";
        if (strncmp(current_file, prefix, strlen(prefix)) == 0) {
            relpath = current_file + strlen(prefix);
        }
        if (strcmp(name, relpath) == 0) {
            return json_error_status(req, "File is currently being recorded", "409 Conflict");
        }
    }

    /* Check for verify=true query parameter */
    char verify_str[8] = {0};
    if (httpd_query_key_value(query, "verify", verify_str, sizeof(verify_str)) == ESP_OK
        && strcmp(verify_str, "true") == 0) {

        char sha_path[288];
        snprintf(sha_path, sizeof(sha_path), "/sdcard/recordings/%s.sha256", name);
        FILE *sf = fopen(sha_path, "r");
        if (sf) {
            char expected[65] = {0};
            if (fread(expected, 1, 64, sf) > 0) {
                expected[64] = '\0';

                char avi_path[288];
                snprintf(avi_path, sizeof(avi_path), "/sdcard/recordings/%s", name);
                FILE *af = fopen(avi_path, "rb");
                if (af) {
                    sha256_ctx_t ctx;
                    sha256_init(&ctx);
                    uint8_t buf[4096];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), af)) > 0) {
                        sha256_update(&ctx, buf, n);
                    }
                    unsigned char hash[32];
                    sha256_finish(&ctx, hash);
                    fclose(af);

                    char computed[65];
                    for (int i = 0; i < 32; i++) {
                        sprintf(computed + i * 2, "%02x", hash[i]);
                    }
                    computed[64] = '\0';

                    if (strcmp(expected, computed) != 0) {
                        fclose(sf);
                        char err[256];
                        snprintf(err, sizeof(err),
                                 "SHA256 mismatch: expected=%s computed=%s",
                                 expected, computed);
                        return json_error(req, err, HTTPD_400_BAD_REQUEST);
                    }
                }
            }
            fclose(sf);
        }
        /* If no .sha256 file found or hash matches, proceed with normal download */
    }

    

    char path[256];
    snprintf(path, sizeof(path), "/sdcard/recordings/%s", name);
    ESP_LOGI(TAG, "DL step 4: open %s, free_heap=%u", path, (unsigned)esp_get_free_heap_size());

    /* Read entire file into PSRAM to minimize SD card interactions
     * then serve from memory. This avoids repeated small reads that
     * trigger FatFS internal allocations on corrupted heap. */
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        ESP_LOGE(TAG, "DL step 4a: stat failed");
        return json_error(req, "File not found", HTTPD_404_NOT_FOUND);
    }
    size_t file_size = (size_t)st.st_size;
    ESP_LOGI(TAG, "DL step 5: file_size=%zu", file_size);

    /* Cap at 4MB: protects against runaway downloads over slow WiFi. The
     * cap no longer needs a full-file PSRAM allocation (we stream), so it's
     * purely a request-size guard. */
    if (file_size > 4 * 1024 * 1024) {
        return json_error(req, "File too large for download", HTTPD_400_BAD_REQUEST);
    }

    /* Stream the file: read an 8KB chunk from SD, send it as an HTTP chunk,
     * repeat. Avoids the previous ~4MB PSRAM allocation that competed with
     * the frame broadcaster / RTSP packetizer for memory during downloads. */
    static const size_t CHUNK = 8192;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "DL: chunk buf alloc failed");
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "DL: open failed errno=%d", errno);
        free(buf);
        return json_error(req, "File not found", HTTPD_404_NOT_FOUND);
    }

    /* Headers: set Content-Length so clients show progress; chunked transfer
     * is used internally by httpd_resp_send_chunk. */
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "%zu", file_size);
    httpd_resp_set_hdr(req, "Content-Length", hdr);
    httpd_resp_set_type(req, "video/avi");
    const char *basename = strrchr(name, '/');
    basename = basename ? basename + 1 : name;
    char disp[192];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", basename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    set_cors_headers(req);

    size_t sent = 0;
    int chunks = 0;
    while (sent < file_size) {
        size_t want = file_size - sent;
        if (want > CHUNK) want = CHUNK;
        ssize_t n = read(fd, buf, want);
        if (n <= 0) {
            ESP_LOGE(TAG, "DL: short read at offset %zu (errno=%d)", sent, errno);
            close(fd);
            free(buf);
            return json_error(req, "Failed to read file", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, (size_t)n) != ESP_OK) {
            ESP_LOGE(TAG, "DL: send_chunk failed at offset %zu", sent);
            close(fd);
            free(buf);
            return ESP_FAIL;
        }
        sent += (size_t)n;
        chunks++;
    }
    close(fd);
    free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "DL done: %zu bytes, %d chunks", sent, chunks);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/scan                                                      */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/scan请求，扫描周围WiFi热点并返回SSID、信号强度和加密方式列表 */
static esp_err_t api_scan_handler(httpd_req_t *req)
{
    wifi_ap_info_t aps[20];
    int count = wifi_scan(aps, 20);

    if (count < 0)
        return json_error(req, "Scan failed", HTTPD_500_INTERNAL_SERVER_ERROR);

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", aps[i].auth_mode);
        cJSON_AddItemToArray(arr, ap);
    }

    cJSON_AddItemToObject(data, "networks", arr);
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/time                                                     */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/time请求，手动设置系统时间（需密码认证） */
static esp_err_t api_time_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    char *body = read_body(req, 512);
    if (!body) return json_error(req, "Empty body", HTTPD_400_BAD_REQUEST);

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);

    cJSON *j_year  = cJSON_GetObjectItem(json, "year");
    cJSON *j_month = cJSON_GetObjectItem(json, "month");
    cJSON *j_day   = cJSON_GetObjectItem(json, "day");
    cJSON *j_hour  = cJSON_GetObjectItem(json, "hour");
    cJSON *j_min   = cJSON_GetObjectItem(json, "min");
    cJSON *j_sec   = cJSON_GetObjectItem(json, "sec");

    if (!j_year || !j_month || !j_day || !j_hour || !j_min || !j_sec) {
        cJSON_Delete(json);
        return json_error(req, "Missing time fields", HTTPD_400_BAD_REQUEST);
    }

    esp_err_t ret = time_set_manual(
        j_year->valueint, j_month->valueint, j_day->valueint,
        j_hour->valueint, j_min->valueint, j_sec->valueint
    );
    cJSON_Delete(json);

    if (ret != ESP_OK)
        return json_error(req, "Failed to set time", HTTPD_500_INTERNAL_SERVER_ERROR);

    return json_ok(req, NULL);
}

/* ------------------------------------------------------------------ */
/*  POST /api/record?action=start|stop                                 */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/record请求，通过action参数控制录像开始或停止（需密码认证） */
static esp_err_t api_record_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    char query[64] = {0};
    char action[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "action", action, sizeof(action));
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "action", action);

    if (strcmp(action, "start") == 0) {
        esp_err_t ret = recorder_start();
        if (ret == ESP_OK) {
            cJSON_AddStringToObject(data, "status", "recording");
        } else {
            cJSON_AddStringToObject(data, "status", "error");
        }
    } else if (strcmp(action, "stop") == 0) {
        esp_err_t ret = recorder_stop();
        if (ret == ESP_OK) {
            cJSON_AddStringToObject(data, "status", "stopped");
        } else {
            cJSON_AddStringToObject(data, "status", "error");
        }
    } else {
        cJSON_AddStringToObject(data, "status", "unknown_action");
    }

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/record — 录像状态（契约 v1.0 收敛项：POST 之外补 GET）      */
/* ------------------------------------------------------------------ */

static esp_err_t api_record_get_handler(httpd_req_t *req)
{
    recorder_state_t rs = recorder_get_state();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status",
        rs == RECORDER_RECORDING ? "recording" :
        rs == RECORDER_PAUSED   ? "paused" :
        rs == RECORDER_ERROR    ? "error" : "stopped");
    cJSON_AddStringToObject(data, "current_file",
        (rs == RECORDER_RECORDING || rs == RECORDER_PAUSED) ? recorder_get_current_file() : "");
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reset                                                    */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/reset请求，执行恢复出厂设置（需密码认证，会清空所有配置） */
static esp_err_t api_reset_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    ESP_LOGW(TAG, "Factory reset requested via web API");
    config_reset();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "Rebooting...");
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/format                                                   */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/format请求，格式化SD卡（需密码认证，会擦除所有数据） */
static esp_err_t api_format_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    /* Stop recording if active */
    bool was_recording = (recorder_get_state() == RECORDER_RECORDING ||
                          recorder_get_state() == RECORDER_PAUSED);
    if (was_recording) {
        recorder_stop();
        ESP_LOGI(TAG, "Stopped recording for SD format");
    }

    ESP_LOGW(TAG, "SD card format requested via web API");
    esp_err_t ret = storage_format();

    cJSON *data = cJSON_CreateObject();
    esp_err_t resp;
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(data, "message", "SD card formatted");
        resp = json_ok(req, data);
    } else {
        cJSON_Delete(data);
        resp = json_error(req, esp_err_to_name(ret), HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* Resume recording if it was active before format */
    if (was_recording && ret == ESP_OK) {
        recorder_start();
        ESP_LOGI(TAG, "Resumed recording after SD format");
    }

    return resp;
}

/* ------------------------------------------------------------------ */
/*  OPTIONS * — CORS preflight                                         */
/* ------------------------------------------------------------------ */

/** @brief 处理OPTIONS预检请求，返回CORS允许头信息 */
static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Static file serving from SPIFFS                                    */
/* ------------------------------------------------------------------ */

/** @brief 从SPIFFS提供静态文件服务，支持HTML/CSS/JS/图片，根路径映射到index.html */
static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) uri = "/index.html";

    /* 去掉 query string (?xxx=yyy) 用于 SPIFFS 文件查找 */
    char uri_path[256];
    strncpy(uri_path, uri, sizeof(uri_path) - 1);
    uri_path[sizeof(uri_path) - 1] = '\0';
    char *q = strchr(uri_path, '?');
    if (q) *q = '\0';
    uri = uri_path;

    char filepath[580];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);

    /* Content type */
    const char *type = "text/html";
    if (strstr(uri, ".css"))  type = "text/css";
    else if (strstr(uri, ".js"))   type = "application/javascript";
    else if (strstr(uri, ".png"))  type = "image/png";
    else if (strstr(uri, ".svg"))  type = "image/svg+xml";   /* favicon.svg（2026-09-04 家族 logo） */
    else if (strstr(uri, ".ico"))  type = "image/x-icon";
    else if (strstr(uri, ".json")) type = "application/json";

    /* Check if client accepts gzip -- serve .gz variant if available */
    char accept_enc[64] = {0};
    bool accepts_gzip = false;
    bool serving_gz = false;
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", accept_enc, sizeof(accept_enc)) == ESP_OK) {
        accepts_gzip = (strstr(accept_enc, "gzip") != NULL);
    }
    if (accepts_gzip) {
        char gz_path[600];
        snprintf(gz_path, sizeof(gz_path), "%s.gz", filepath);
        struct stat st;
        if (stat(gz_path, &st) == 0) {
            strcpy(filepath, gz_path);
            serving_gz = true;
        }
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, type);
    set_cors_headers(req);
    if (serving_gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    /* Cache-Control: 静态资源一律不缓存（UI 随固件更新，LAN 上 ~60KB 可忽略）。
     * 2026-09-03 事故：max-age=3600 让 crossOrigin 修复部署后浏览器仍跑旧 app.js */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/** @brief 标记首次配置已完成 (POST /api/setup/done) */
static esp_err_t api_setup_done_handler(httpd_req_t *req)
{
    config_save();
    return json_ok(req, NULL);
}


/* ------------------------------------------------------------------ */
/*  GET /metrics                                                     */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /metrics请求，返回Prometheus格式的系统指标 */
static esp_err_t metrics_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    set_cors_headers(req);

    /* Allocate from heap to avoid stack overflow on HTTPD task (default 4KB stack) */
    const int buf_size = 8192;
    char *buf = malloc(buf_size);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int len = 0;

    /* === Collect system metrics === */
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    float temp = get_chip_temp();
    recorder_state_t rs = recorder_get_state();
    storage_info_t sd;
    storage_get_info(&sd);
    wifi_state_t ws = wifi_get_state();
    int upload_queue = 0;
    bool upload_paused = false;
    char last_upload[32] = "";
    nas_uploader_get_status(last_upload, sizeof(last_upload), &upload_queue, &upload_paused);

    int64_t uptime_us = esp_timer_get_time();
    int64_t uptime_s = uptime_us / 1000000;
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();

    int wifi_rssi = -1;
    if (ws == WIFI_STATE_STA_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            wifi_rssi = ap_info.rssi;
        }
    }

    int upload_success = 0, upload_failure = 0;
    nas_uploader_get_stats(&upload_success, &upload_failure);


    uint32_t frames_dropped = recorder_get_frames_dropped();

    /* === Chip info for node_uname_info === */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    const char *machine_str = "ESP32";
    if (chip_info.model == CHIP_ESP32S3)      machine_str = "ESP32-S3";
    else if (chip_info.model == CHIP_ESP32S2)  machine_str = "ESP32-S2";
    else if (chip_info.model == CHIP_ESP32C3)  machine_str = "ESP32-C3";
    else if (chip_info.model == CHIP_ESP32C6)  machine_str = "ESP32-C6";

    cam_config_t *cfg = config_get();
    const char *nodename = cfg->device_name[0] ? cfg->device_name : "mibee_cam";

    /* === MAC address === (factory eFuse MAC, stable & WiFi-independent) */
    uint8_t mac[6] = {0};
    device_get_mac(mac);

    /* === Time === */
    time_t now = time(NULL);
    time_t boot_time = (now > (time_t)uptime_s) ? (now - (time_t)uptime_s) : 0;

    /* ======================================================== */
    /*  Section 1: Custom esp_* metrics (existing)              */
    /* ======================================================== */
    len = snprintf(buf, buf_size,
        "# HELP esp_free_heap_bytes Free heap memory\n"
        "# TYPE esp_free_heap_bytes gauge\n"
        "esp_free_heap_bytes %lu\n"
        "# HELP esp_free_psram_bytes Free PSRAM memory\n"
        "# TYPE esp_free_psram_bytes gauge\n"
        "esp_free_psram_bytes %lu\n"
        "# HELP esp_chip_temp_celsius ESP32-S3 chip temperature in Celsius\n"
        "# TYPE esp_chip_temp_celsius gauge\n"
        "esp_chip_temp_celsius %.1f\n"
        "# HELP esp_recording_state Recording state (0=idle, 1=recording)\n"
        "# TYPE esp_recording_state gauge\n"
        "esp_recording_state %d\n"
        "# HELP sd_free_bytes SD card free bytes\n"
        "# TYPE sd_free_bytes gauge\n"
        "sd_free_bytes %llu\n"
        "# HELP sd_total_bytes SD card total bytes\n"
        "# TYPE sd_total_bytes gauge\n"
        "sd_total_bytes %llu\n"
        "# HELP sd_free_percent SD card free space percentage\n"
        "# TYPE sd_free_percent gauge\n"
        "sd_free_percent %.1f\n"
        "# HELP wifi_state WiFi connection state (0=disconnected, 1=STA connected, 2=AP mode)\n"
        "# TYPE wifi_state gauge\n"
        "wifi_state %d\n"
        "# HELP upload_queue_size Number of files queued for upload\n"
        "# TYPE upload_queue_size gauge\n"
        "upload_queue_size %d\n"
        "# HELP esp_uptime_seconds System uptime in seconds\n"
        "# TYPE esp_uptime_seconds gauge\n"
        "esp_uptime_seconds %lld\n"
        "# HELP esp_min_free_heap_bytes Minimum free heap bytes (since boot)\n"
        "# TYPE esp_min_free_heap_bytes gauge\n"
        "esp_min_free_heap_bytes %lu\n"
        "# HELP esp_wifi_rssi_dbm WiFi signal strength in dBm (-1 when disconnected/AP mode)\n"
        "# TYPE esp_wifi_rssi_dbm gauge\n"
        "esp_wifi_rssi_dbm %d\n"
        "# HELP esp_upload_success_total Total successful uploads\n"
        "# TYPE esp_upload_success_total counter\n"
        "esp_upload_success_total %d\n"
        "# HELP esp_upload_failure_total Total failed uploads\n"
        "# TYPE esp_upload_failure_total counter\n"
        "esp_upload_failure_total %d\n"
        "# HELP esp_rtsp_clients Number of connected RTSP clients\n"
        "# TYPE esp_rtsp_clients gauge\n"
        "esp_rtsp_clients 0\n"
        "# HELP esp_frames_dropped_total Total frames dropped due to write backlog\n"
        "# TYPE esp_frames_dropped_total counter\n"
        "esp_frames_dropped_total %lu\n",
        (unsigned long)free_heap,
        (unsigned long)free_psram,
        temp,
        rs == RECORDER_RECORDING ? 1 : 0,
        (unsigned long long)sd.free_bytes,
        (unsigned long long)sd.total_bytes,
        storage_get_free_percent(),
        ws == WIFI_STATE_STA_CONNECTED ? 1 : (ws == WIFI_STATE_AP ? 2 : 0),
        upload_queue,
        (long long)uptime_s,
        (unsigned long)min_free_heap,
        wifi_rssi,
        upload_success,
        upload_failure,
        (unsigned long)frames_dropped);

    /* ======================================================== */
    /*  Section 2: node_exporter compatible metrics             */
    /* ======================================================== */

    /* --- node_exporter build info --- */
    len += snprintf(buf + len, buf_size - len,
        "\n# HELP node_exporter_build_info node_exporter build info\n"
        "# TYPE node_exporter_build_info gauge\n"
        "node_exporter_build_info{version=\"mibee-esp-simulated\",revision=\"%s\"} 1\n",
        FW_VERSION);

    /* --- node_uname_info --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_uname_info Labeled system information as provided by the uname system call\n"
        "# TYPE node_uname_info gauge\n"
        "node_uname_info{sysname=\"ESP-IDF\",release=\"%s\",version=\"%s\",machine=\"%s\",nodename=\"%s\"} 1\n",
        FW_VERSION, IDF_VER, machine_str, nodename);

    /* --- Time --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_boot_time_seconds Node boot time, in unixtime\n"
        "# TYPE node_boot_time_seconds gauge\n"
        "node_boot_time_seconds %lld\n"
        "# HELP node_time_seconds System time in seconds since epoch\n"
        "# TYPE node_time_seconds gauge\n"
        "node_time_seconds %lld\n",
        (long long)boot_time,
        (long long)now);

    /* --- Memory --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_memory_MemTotal_bytes Total physical memory (internal RAM)\n"
        "# TYPE node_memory_MemTotal_bytes gauge\n"
        "node_memory_MemTotal_bytes %lu\n"
        "# HELP node_memory_MemFree_bytes Free physical memory (internal RAM)\n"
        "# TYPE node_memory_MemFree_bytes gauge\n"
        "node_memory_MemFree_bytes %lu\n"
        "# HELP node_memory_MemAvailable_bytes Available physical memory (internal RAM)\n"
        "# TYPE node_memory_MemAvailable_bytes gauge\n"
        "node_memory_MemAvailable_bytes %lu\n"
        "# HELP node_memory_Buffers_bytes Total extended memory (PSRAM)\n"
        "# TYPE node_memory_Buffers_bytes gauge\n"
        "node_memory_Buffers_bytes %lu\n"
        "# HELP node_memory_Cached_bytes Used extended memory (PSRAM used)\n"
        "# TYPE node_memory_Cached_bytes gauge\n"
        "node_memory_Cached_bytes %lu\n"
        "# HELP node_memory_SwapTotal_bytes Total swap space (PSRAM total)\n"
        "# TYPE node_memory_SwapTotal_bytes gauge\n"
        "node_memory_SwapTotal_bytes %lu\n"
        "# HELP node_memory_SwapFree_bytes Free swap space (PSRAM free)\n"
        "# TYPE node_memory_SwapFree_bytes gauge\n"
        "node_memory_SwapFree_bytes %lu\n"
        "# HELP node_memory_HardwareCorrupted_bytes Amount of corrupted memory detected\n"
        "# TYPE node_memory_HardwareCorrupted_bytes gauge\n"
        "node_memory_HardwareCorrupted_bytes 0\n",
        (unsigned long)total_heap,
        (unsigned long)free_internal,
        (unsigned long)free_internal,
        (unsigned long)total_psram,
        (unsigned long)(total_psram - free_psram),
        (unsigned long)total_psram,
        (unsigned long)free_psram);

    /* --- Filesystem --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_filesystem_size_bytes Filesystem size in bytes\n"
        "# TYPE node_filesystem_size_bytes gauge\n"
        "node_filesystem_size_bytes{device=\"/dev/sdcard\",fstype=\"vfat\",mountpoint=\"/sdcard\"} %llu\n"
        "# HELP node_filesystem_avail_bytes Filesystem space available to non-root users in bytes\n"
        "# TYPE node_filesystem_avail_bytes gauge\n"
        "node_filesystem_avail_bytes{device=\"/dev/sdcard\",fstype=\"vfat\",mountpoint=\"/sdcard\"} %llu\n"
        "# HELP node_filesystem_free_bytes Filesystem free space in bytes\n"
        "# TYPE node_filesystem_free_bytes gauge\n"
        "node_filesystem_free_bytes{device=\"/dev/sdcard\",fstype=\"vfat\",mountpoint=\"/sdcard\"} %llu\n"
        "# HELP node_filesystem_readonly Filesystem read-only status\n"
        "# TYPE node_filesystem_readonly gauge\n"
        "node_filesystem_readonly{device=\"/dev/sdcard\",fstype=\"vfat\",mountpoint=\"/sdcard\"} 0\n",
        (unsigned long long)sd.total_bytes,
        (unsigned long long)sd.free_bytes,
        (unsigned long long)sd.free_bytes);

    /* --- SPIFFS filesystem (Web UI) --- */
    len += snprintf(buf + len, buf_size - len,
        "node_filesystem_size_bytes{device=\"/dev/spiffs\",fstype=\"spiffs\",mountpoint=\"/spiffs\"} 262144\n"
        "node_filesystem_avail_bytes{device=\"/dev/spiffs\",fstype=\"spiffs\",mountpoint=\"/spiffs\"} 131072\n"
        "node_filesystem_free_bytes{device=\"/dev/spiffs\",fstype=\"spiffs\",mountpoint=\"/spiffs\"} 131072\n"
        "node_filesystem_readonly{device=\"/dev/spiffs\",fstype=\"spiffs\",mountpoint=\"/spiffs\"} 0\n");

    /* --- Temperature --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_hwmon_temp_celsius Hardware monitor for temperature (Celsius)\n"
        "# TYPE node_hwmon_temp_celsius gauge\n"
        "node_hwmon_temp_celsius{chip=\"cpu\"} %.1f\n",
        temp);

    /* --- Network --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_network_up Value is 1 if interface is up (operational), 0 otherwise\n"
        "# TYPE node_network_up gauge\n"
        "node_network_up{device=\"wifi0\"} %d\n"
        "# HELP node_network_info node_network_info\n"
        "# TYPE node_network_info gauge\n"
        "node_network_info{device=\"wifi0\",address=\"%02x:%02x:%02x:%02x:%02x:%02x\"} 1\n"
        "# HELP node_network_mtu_bytes Network MTU in bytes\n"
        "# TYPE node_network_mtu_bytes gauge\n"
        "node_network_mtu_bytes{device=\"wifi0\"} 1500\n"
        "# HELP node_network_speed_bytes Network device speed in bytes\n"
        "# TYPE node_network_speed_bytes gauge\n"
        "node_network_speed_bytes{device=\"wifi0\"} 0\n"
        "# HELP node_network_receive_bytes_total Network device statistic receive_bytes\n"
        "# TYPE node_network_receive_bytes_total counter\n"
        "node_network_receive_bytes_total{device=\"wifi0\"} 0\n"
        "# HELP node_network_transmit_bytes_total Network device statistic transmit_bytes\n"
        "# TYPE node_network_transmit_bytes_total counter\n"
        "node_network_transmit_bytes_total{device=\"wifi0\"} 0\n"
        "# HELP node_network_receive_packets_total Network device statistic receive_packets\n"
        "# TYPE node_network_receive_packets_total counter\n"
        "node_network_receive_packets_total{device=\"wifi0\"} 0\n"
        "# HELP node_network_transmit_packets_total Network device statistic transmit_packets\n"
        "# TYPE node_network_transmit_packets_total counter\n"
        "node_network_transmit_packets_total{device=\"wifi0\"} 0\n"
        "# HELP node_network_receive_errs_total Network device statistic receive_errs\n"
        "# TYPE node_network_receive_errs_total counter\n"
        "node_network_receive_errs_total{device=\"wifi0\"} 0\n"
        "# HELP node_network_transmit_errs_total Network device statistic transmit_errs\n"
        "# TYPE node_network_transmit_errs_total counter\n"
        "node_network_transmit_errs_total{device=\"wifi0\"} 0\n",
        ws == WIFI_STATE_STA_CONNECTED ? 1 : 0,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* --- CPU --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_cpu_seconds_total Total seconds the cpu spent in each mode\n"
        "# TYPE node_cpu_seconds_total counter\n"
        "node_cpu_seconds_total{cpu=\"0\",mode=\"idle\"} %lld\n"
        "node_cpu_seconds_total{cpu=\"0\",mode=\"user\"} %lld\n"
        "node_cpu_seconds_total{cpu=\"0\",mode=\"system\"} %lld\n"
        "node_cpu_seconds_total{cpu=\"1\",mode=\"idle\"} %lld\n"
        "node_cpu_seconds_total{cpu=\"1\",mode=\"user\"} %lld\n"
        "node_cpu_seconds_total{cpu=\"1\",mode=\"system\"} %lld\n",
        (long long)(uptime_s * 80 / 100),
        (long long)(uptime_s * 18 / 100),
        (long long)(uptime_s * 2 / 100),
        (long long)(uptime_s * 85 / 100),
        (long long)(uptime_s * 13 / 100),
        (long long)(uptime_s * 2 / 100));

    /* --- Load averages (not meaningful on MCU, provided as 0) --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_load1 1m load average\n"
        "# TYPE node_load1 gauge\n"
        "node_load1 0.00\n"
        "# HELP node_load5 5m load average\n"
        "# TYPE node_load5 gauge\n"
        "node_load5 0.00\n"
        "# HELP node_load15 15m load average\n"
        "# TYPE node_load15 gauge\n"
        "node_load15 0.00\n");

    /* --- Disk I/O (counters not tracked, provided as 0) --- */
    len += snprintf(buf + len, buf_size - len,
        "# HELP node_disk_read_bytes_total Total bytes read from disk\n"
        "# TYPE node_disk_read_bytes_total counter\n"
        "node_disk_read_bytes_total{device=\"sdcard\"} 0\n"
        "# HELP node_disk_written_bytes_total Total bytes written to disk\n"
        "# TYPE node_disk_written_bytes_total counter\n"
        "node_disk_written_bytes_total{device=\"sdcard\"} 0\n"
        "# HELP node_disk_io_time_seconds_total Total seconds spent doing disk I/O\n"
        "# TYPE node_disk_io_time_seconds_total counter\n"
        "node_disk_io_time_seconds_total{device=\"sdcard\"} 0\n");

    /* === Send response === */
    httpd_resp_send(req, buf, len);
    free(buf);
    return ESP_OK;
}
/*  URI handler registration table                                     */
/* ------------------------------------------------------------------ */
/** @brief GET /api/audio — stream live G.711 μ-law audio for web preview */
static esp_err_t api_audio_stream_handler(httpd_req_t *req)
{
    audio_sub_t *sub = audio_bcast_subscribe(AUDIO_SUB_RTSP);
    if (!sub) {
        return json_error(req, "No audio subscriber slots", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    audio_frame_t frame;
    while (true) {
        if (!audio_bcast_receive(sub, &frame, pdMS_TO_TICKS(2000))) {
            continue;  /* timeout — keep alive */
        }

        /* Send frame as chunk — check for client disconnect */
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)frame.data, frame.len);
        audio_bcast_release(&frame);

        if (err != ESP_OK) {
            break;  /* client disconnected */
        }
    }

    /* Send empty chunk to close stream */
    httpd_resp_send_chunk(req, NULL, 0);
    audio_bcast_unsubscribe(sub);
    return ESP_OK;
}
/* ------------------------------------------------------------------ */
/*  GET /api/capture — single JPEG snapshot                             */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /api/capture请求，返回单帧JPEG快照 */
static esp_err_t api_capture_handler(httpd_req_t *req)
{
    /* 使用帧广播器的最新缓存帧（零拷贝），避免和录像任务竞争 camera */
    frame_msg_t msg = {0};
    if (!fbroadcast_get_latest(&msg) || !msg.fb) {
        ESP_LOGW(TAG, "No frame available for /api/capture");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");
    httpd_resp_send(req, (const char *)msg.fb->data, msg.fb->len);
    fbroadcast_release(&msg);
    return ESP_OK;
}


/* ------------------------------------------------------------------ */
/*  GET/POST /api/camera — 传感器设置（契约 v1.0 统一端点）             */
/* ------------------------------------------------------------------ */

/** 构造 supported_resolutions 数组（与 /api/status 保持一致的标签/取值） */
static cJSON *camera_supported_resolutions_json(void)
{
    camera_res_t max_res = camera_get_effective_max_res();
    cJSON *res_arr = cJSON_CreateArray();
    for (int r = CAMERA_RES_VGA; r <= (int)max_res; r++) {
        const char *label = NULL;
        switch (r) {
            case CAMERA_RES_VGA:  label = "VGA (640x480)";   break;
            case CAMERA_RES_SVGA: label = "SVGA (800x600)";  break;
            case CAMERA_RES_XGA:  label = "XGA (1024x768)";  break;
            case CAMERA_RES_HD:   label = "HD (1280x720)";   break;
            case CAMERA_RES_SXGA: label = "SXGA (1280x1024)";break;
            case CAMERA_RES_UXGA: label = "UXGA (1600x1200)";break;
            case CAMERA_RES_FHD:  label = "FHD (1920x1080)"; break;
            case CAMERA_RES_QXGA: label = "QXGA (2048x1536)";break;
            default: continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", label);
        cJSON_AddNumberToObject(item, "value", r);
        cJSON_AddItemToArray(res_arr, item);
    }
    return res_arr;
}

/** @brief GET /api/camera — 返回相机设置与传感器支持的分辨率列表 */
static esp_err_t api_camera_get_handler(httpd_req_t *req)
{
    cam_config_t *cfg = config_get();
    cJSON *data = cJSON_CreateObject();

    cJSON_AddStringToObject(data, "resolution", camera_res_to_str(camera_get_resolution()));
    cJSON_AddNumberToObject(data, "cam_framesize", cfg->cam_framesize);
    cJSON_AddNumberToObject(data, "cam_quality", cfg->cam_quality);
    /* 契约扩展（2026-09-04）：画质滑杆边界由板端声明，前端据此钳制输入 */
    cJSON_AddNumberToObject(data, "quality_min", CAMERA_QUALITY_MIN);
    cJSON_AddNumberToObject(data, "quality_max", CAMERA_QUALITY_MAX);
    cJSON_AddNumberToObject(data, "fps", cfg->fps);
    cJSON_AddBoolToObject(data, "cam_vflip", cfg->cam_vflip);
    cJSON_AddBoolToObject(data, "cam_hmirror", cfg->cam_hmirror);
    cJSON_AddNumberToObject(data, "day_night_mode", cfg->day_night_mode);
    cJSON_AddItemToObject(data, "supported_resolutions", camera_supported_resolutions_json());

    return json_ok(req, data);
}

/** @brief POST /api/camera — 更新相机设置（翻转/镜像/日夜模式立即生效，分辨率触发重配） */
static esp_err_t api_camera_post_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    char *body = read_body(req, 1024);
    if (!body) return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);

    cam_config_t *cfg = config_get();
    cJSON *item;

    config_lock();

    uint8_t prev_resolution = cfg->cam_framesize;
    if ((item = cJSON_GetObjectItem(json, "cam_framesize"))) {
        int val = item->valueint;
        if (val < 0 || val > (int)camera_get_effective_max_res()) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Invalid resolution (board-tested max: %s)",
                     camera_res_to_str(camera_get_effective_max_res()));
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        cfg->cam_framesize = (uint8_t)val;
    }
    bool quality_changed = false;
    if ((item = cJSON_GetObjectItem(json, "cam_quality"))) {
        int val = item->valueint;
        if (val < CAMERA_QUALITY_MIN || val > CAMERA_QUALITY_MAX) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Invalid cam_quality (must be %d-%d)",
                     CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        quality_changed = (val != cfg->cam_quality);
        cfg->cam_quality = (uint8_t)val;
    }
    if ((item = cJSON_GetObjectItem(json, "cam_vflip")))
        cfg->cam_vflip = item->valueint != 0;
    if ((item = cJSON_GetObjectItem(json, "cam_hmirror")))
        cfg->cam_hmirror = item->valueint != 0;
    if ((item = cJSON_GetObjectItem(json, "day_night_mode"))) {
        int val = item->valueint;
        if (val < 0 || val > 2) {
            cJSON_Delete(json);
            config_unlock();
            return json_error(req, "Invalid day_night_mode (must be 0-2)", HTTPD_400_BAD_REQUEST);
        }
        cfg->day_night_mode = (uint8_t)val;
    }

    cJSON_Delete(json);
    config_save();
    config_unlock();

    /* 立即生效：翻转/镜像/日夜模式/画质（OV5640 set_quality 单寄存器写，安全） */
    camera_set_flip(cfg->cam_vflip, cfg->cam_hmirror);
    camera_set_day_night(cfg->day_night_mode);
    if (quality_changed) {
        camera_set_quality(cfg->cam_quality);
    }
    /* 分辨率变化：OV5640 运行时 set_framesize 实测无效（只写寄存器不重启
     * DSP，2026-09-04），必须保存+重启让 esp_camera_init 以新 framesize
     * 初始化。应答后 1s 重启（同 luatos 模式）。 */
    if (cfg->cam_framesize != prev_resolution) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "status", "saved (rebooting to apply)");
        cJSON_AddBoolToObject(data, "rebooting", true);
        esp_err_t send_ret = json_ok(req, data);
        ESP_LOGW(TAG, "Camera resolution change %u->%u — rebooting to apply",
                 prev_resolution, cfg->cam_framesize);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return send_ret;
    }

    return api_camera_get_handler(req);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reboot · GET /api/auth — 核心系统端点（契约 v1.0）       */
/* ------------------------------------------------------------------ */

/** @brief POST /api/reboot — 重启设备（需密码认证，响应发出后延迟重启） */
static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    esp_err_t auth_err = require_auth(req, false);
    if (auth_err != ESP_OK) return auth_err;

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "Rebooting...");
    esp_err_t resp = json_ok(req, data);

    ESP_LOGW(TAG, "Reboot requested via web API");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return resp;  /* not reached */
}

/** @brief GET /api/auth — 校验 X-Password；未设密码时返回 password_set:false */
static esp_err_t api_auth_handler(httpd_req_t *req)
{
    cam_config_t *cfg = config_get();

    if (cfg->web_password[0] == '\0') {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "auth", true);
        cJSON_AddBoolToObject(data, "password_set", false);
        return json_ok(req, data);
    }
    if (check_password(req)) {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddBoolToObject(data, "auth", true);
        cJSON_AddBoolToObject(data, "password_set", true);
        return json_ok(req, data);
    }
    return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);
}


typedef struct {
    const char  *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} uri_entry_t;

static const uri_entry_t s_uris[] = {
    { "/api/status",   HTTP_GET,    api_status_handler        },
    { "/api/capabilities", HTTP_GET,    api_capabilities_handler },
    { "/api/config",   HTTP_GET,    api_config_get_handler    },
    { "/api/config",   HTTP_POST,   api_config_post_handler   },
    { "/api/camera",   HTTP_GET,    api_camera_get_handler    },
    { "/api/camera",   HTTP_POST,   api_camera_post_handler   },
    { "/api/auth",     HTTP_GET,    api_auth_handler          },
    { "/api/reboot",   HTTP_POST,   api_reboot_handler        },
    { "/api/setup/done", HTTP_POST,   api_setup_done_handler   },
    { "/api/files",    HTTP_GET,    api_files_get_handler     },
    { "/api/files",    HTTP_DELETE, api_files_delete_handler  },
    { "/api/files/batch", HTTP_POST,  api_files_batch_handler  },
    { "/api/download", HTTP_GET,    api_download_handler      },
    { "/api/scan",     HTTP_GET,    api_scan_handler          },
    { "/api/time",     HTTP_POST,   api_time_handler          },
    { "/api/record",   HTTP_POST,   api_record_handler        },
    { "/api/record",   HTTP_GET,    api_record_get_handler    },
    { "/api/reset",    HTTP_POST,   api_reset_handler         },
    { "/api/format",   HTTP_POST,   api_format_handler        },
    { "/api/ota",     HTTP_POST,   api_ota_handler           },
    { "/api/ota/info",   HTTP_GET,   api_ota_info_handler    },
    { "/api/ota/upload", HTTP_POST,  api_ota_upload_handler  },
    { "/api/ota/spiffs", HTTP_POST,  api_ota_spiffs_handler  },
    { "/metrics",      HTTP_GET,    metrics_handler           },
    { "/api/audio",   HTTP_GET,    api_audio_stream_handler  },
    { "/api/capture", HTTP_GET,    api_capture_handler       },
    /* CORS preflight — wildcard */
    { "/*",            HTTP_OPTIONS, options_handler           },
    /* Static files — catch-all (lowest priority) */
    { "/*",            HTTP_GET,    static_file_handler       },
};

#define NUM_URIS (sizeof(s_uris) / sizeof(s_uris[0]))

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* 每个新 HTTP 连接打开时设置 TCP 选项（弱链路优化） */
static esp_err_t on_http_session_open(httpd_handle_t hd, int sockfd)
{
    int enable = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    int keepalive = 1;
    int keepidle = 10;     /* 10s 空闲后开始 keepalive 探测 */
    int keepintvl = 3;     /* 3s 探测间隔 */
    int keepcnt = 3;       /* 3 次探测失败才断开 */
    setsockopt(sockfd, SOL_SOCKET,  SO_KEEPALIVE,  &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

/** @brief 启动HTTP Web服务器，注册所有URI处理程序，配置通配符路由匹配 */
esp_err_t web_server_start(uint16_t port)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;   /* 27 static + /ws + 2 ONVIF, margin for growth */
    config.stack_size = 16384;   /* 16KB: download handler has ~6KB locals + nested calls */
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* 2026-09-02 EMFILE 事故：httpd 独占 11 会挤占 :81/:554/上传器。
     * 收缩到 8 给系统其他 socket 使用者留余量（LWIP_MAX_SOCKETS=24）。 */
    config.max_open_sockets = 8;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 5;    /* 弱链路下更快超时（原 10s） */
    config.keep_alive_enable = false;   /* socket-level keepalive set in open_fn */
    config.lru_purge_enable = true;     /* auto-close LRU session when full */
    config.open_fn = on_http_session_open;   /* TCP_NODELAY + keepalive per socket */
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* /ws 必须先于 GET 通配符静态处理器注册 —— 通配符匹配按注册顺序生效，
     * 若 /ws 注册在后会被静态处理器吞掉导致 404（曾致 WS 推送失效） */
    ws_server_init(s_server);

    for (size_t i = 0; i < NUM_URIS; i++) {
        httpd_uri_t uri = {
            .uri      = s_uris[i].uri,
            .method   = s_uris[i].method,
            .handler  = s_uris[i].handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &uri);
    }

    /* Register ONVIF SOAP service handlers */
    esp_err_t onvif_ret = onvif_register_handlers(s_server);
    if (onvif_ret != ESP_OK) {
        ESP_LOGW(TAG, "ONVIF handler registration: %s", esp_err_to_name(onvif_ret));
    }

    s_port = port;
    ESP_LOGI(TAG, "Web server started on port %d", port);
    return ESP_OK;
}

/** @brief 停止HTTP Web服务器并释放所有资源 */
void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        s_port = 0;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

/** @brief 获取当前Web服务器的句柄，供其他模块使用 */
httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}
