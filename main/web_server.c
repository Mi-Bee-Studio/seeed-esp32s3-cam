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

#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "storage_manager.h"
#include "time_sync.h"
#include "camera_driver.h"
#include "video_recorder.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nas_uploader.h"
#include "mjpeg_streamer.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "web";

extern float get_chip_temp(void);

static httpd_handle_t s_server = NULL;
static uint16_t s_port = 0;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** @brief 验证请求中的密码，先检查X-Password头部，再检查password查询参数 */
static bool check_password(httpd_req_t *req)
{
    /* Try X-Password header first */
    char password[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Password", password, sizeof(password)) == ESP_OK) {
        cam_config_t *cfg = config_get();
        if (strcmp(password, cfg->web_password) == 0) return true;
    }

    /* Try query parameter */
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[64] = {0};
        if (httpd_query_key_value(query, "password", val, sizeof(val)) == ESP_OK) {
            cam_config_t *cfg = config_get();
            if (strcmp(val, cfg->web_password) == 0) return true;
        }
    }

    return false;
}

/** @brief 设置跨域资源共享(CORS)响应头，允许所有来源访问 */
static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Password");
}

/** @brief 发送JSON成功响应，格式为 {"ok":true,"data":...} */
static esp_err_t json_ok(httpd_req_t *req, cJSON *data)
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
static esp_err_t json_error(httpd_req_t *req, const char *msg, int status)
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

/* Read the full request body into a heap-allocated buffer (caller frees). */
/** @brief 读取HTTP请求体到堆分配的缓冲区（调用者负责释放内存） */
static char *read_body(httpd_req_t *req, size_t max_len)
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

    /* WiFi */
    cam_config_t *cfg = config_get();
    cJSON_AddStringToObject(data, "wifi_ssid", cfg->wifi_ssid);
    wifi_state_t ws = wifi_get_state();
    cJSON_AddStringToObject(data, "wifi_state",
        ws == WIFI_STATE_AP ? "AP" :
        ws == WIFI_STATE_STA_CONNECTED ? "STA" : "disconnected");

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
        camera_get_sensor() == CAMERA_SENSOR_OV3660 ? "OV3660" : "unknown");
    cJSON_AddStringToObject(data, "resolution", camera_res_to_str(camera_get_resolution()));

    /* Time */
    cJSON_AddBoolToObject(data, "time_synced", time_is_synced());
    char current_time[32] = "";
    time_get_str(current_time, sizeof(current_time));
    cJSON_AddStringToObject(data, "current_time", current_time);

    /* Uptime (seconds since boot) */
    int64_t uptime = esp_timer_get_time() / 1000000;
    cJSON_AddNumberToObject(data, "uptime", (double)uptime);

    /* NAS upload status */
    char last_upload[32] = "";
    int upload_queue = 0;
    bool upload_paused = false;
    nas_uploader_get_status(last_upload, sizeof(last_upload), &upload_queue, &upload_paused);
    cJSON_AddStringToObject(data, "last_upload", last_upload);
    cJSON_AddNumberToObject(data, "upload_queue", upload_queue);
    cJSON_AddNumberToObject(data, "chip_temp", (double)get_chip_temp());

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
    cJSON_AddStringToObject(data, "device_name", cfg->device_name);

    cJSON_AddStringToObject(data, "ftp_host", cfg->ftp_host);
    cJSON_AddNumberToObject(data, "ftp_port", cfg->ftp_port);
    cJSON_AddStringToObject(data, "ftp_user", cfg->ftp_user);
    cJSON_AddStringToObject(data, "ftp_path", cfg->ftp_path);
    cJSON_AddBoolToObject(data, "ftp_enabled", cfg->ftp_enabled);

    cJSON_AddStringToObject(data, "webdav_url", cfg->webdav_url);
    cJSON_AddStringToObject(data, "webdav_user", cfg->webdav_user);
    cJSON_AddBoolToObject(data, "webdav_enabled", cfg->webdav_enabled);

    cJSON_AddNumberToObject(data, "resolution", cfg->resolution);
    cJSON_AddNumberToObject(data, "fps", cfg->fps);
    cJSON_AddNumberToObject(data, "segment_sec", cfg->segment_sec);
    cJSON_AddNumberToObject(data, "jpeg_quality", cfg->jpeg_quality);

    /* Mask web password */
    cJSON_AddStringToObject(data, "web_password", cfg->web_password[0] ? "****" : "");
    cJSON_AddStringToObject(data, "timezone", cfg->timezone);

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/config                                                   */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/config请求，更新设备配置（需密码认证，支持部分更新） */
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

    char *body = read_body(req, 2048);
    if (!body) return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);

    cam_config_t *cfg = config_get();
    cJSON *item;

    if ((item = cJSON_GetObjectItem(json, "wifi_ssid")))
        strncpy(cfg->wifi_ssid, item->valuestring, sizeof(cfg->wifi_ssid) - 1);
    if ((item = cJSON_GetObjectItem(json, "wifi_pass")) && strcmp(item->valuestring, "****") != 0)
        strncpy(cfg->wifi_pass, item->valuestring, sizeof(cfg->wifi_pass) - 1);
    if ((item = cJSON_GetObjectItem(json, "device_name")))
        strncpy(cfg->device_name, item->valuestring, sizeof(cfg->device_name) - 1);

    if ((item = cJSON_GetObjectItem(json, "ftp_host")))
        strncpy(cfg->ftp_host, item->valuestring, sizeof(cfg->ftp_host) - 1);
    if ((item = cJSON_GetObjectItem(json, "ftp_port")))
        cfg->ftp_port = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "ftp_user")))
        strncpy(cfg->ftp_user, item->valuestring, sizeof(cfg->ftp_user) - 1);
    if ((item = cJSON_GetObjectItem(json, "ftp_pass")) && strcmp(item->valuestring, "****") != 0)
        strncpy(cfg->ftp_pass, item->valuestring, sizeof(cfg->ftp_pass) - 1);
    if ((item = cJSON_GetObjectItem(json, "ftp_path")))
        strncpy(cfg->ftp_path, item->valuestring, sizeof(cfg->ftp_path) - 1);
    if ((item = cJSON_GetObjectItem(json, "ftp_enabled")))
        cfg->ftp_enabled = item->valueint;

    if ((item = cJSON_GetObjectItem(json, "webdav_url")))
        strncpy(cfg->webdav_url, item->valuestring, sizeof(cfg->webdav_url) - 1);
    if ((item = cJSON_GetObjectItem(json, "webdav_user")))
        strncpy(cfg->webdav_user, item->valuestring, sizeof(cfg->webdav_user) - 1);
    if ((item = cJSON_GetObjectItem(json, "webdav_pass")) && strcmp(item->valuestring, "****") != 0)
        strncpy(cfg->webdav_pass, item->valuestring, sizeof(cfg->webdav_pass) - 1);
    if ((item = cJSON_GetObjectItem(json, "webdav_enabled")))
        cfg->webdav_enabled = item->valueint;

    if ((item = cJSON_GetObjectItem(json, "resolution")))
        cfg->resolution = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "fps")))
        cfg->fps = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "segment_sec")))
        cfg->segment_sec = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "jpeg_quality")))
        cfg->jpeg_quality = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(json, "web_password")) && strcmp(item->valuestring, "****") != 0)
        strncpy(cfg->web_password, item->valuestring, sizeof(cfg->web_password) - 1);
    if ((item = cJSON_GetObjectItem(json, "timezone")) && strlen(item->valuestring) > 0) {
        strncpy(cfg->timezone, item->valuestring, sizeof(cfg->timezone) - 1);
        cfg->timezone[sizeof(cfg->timezone) - 1] = '\0';
        /* Apply timezone change immediately */
        setenv("TZ", cfg->timezone, 1);
        tzset();
    }

    cJSON_Delete(json);
    config_save();

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
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

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

/** @brief Batch delete multiple recording files */
static esp_err_t api_files_batch_handler(httpd_req_t *req)
{
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) {
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

    cJSON *names_arr = cJSON_GetObjectItem(root, "names");
    if (!cJSON_IsArray(names_arr)) {
        cJSON_Delete(root);
        free(buf);
        return json_error(req, "Missing 'names' array", HTTPD_400_BAD_REQUEST);
    }

    const char *current = recorder_get_current_file();
    int deleted = 0, failed = 0;

    for (int i = 0; i < cJSON_GetArraySize(names_arr); i++) {
        cJSON *item = cJSON_GetArrayItem(names_arr, i);
        const char *name = cJSON_GetStringValue(item);
        if (!name) { failed++; continue; }

        /* Block path traversal */
        if (strstr(name, "..")) { failed++; continue; }

        /* Do not delete currently recording file */
        if (current && strcmp(name, current) == 0) { failed++; continue; }

        char path[256];
        snprintf(path, sizeof(path), "/sdcard/recordings/%s", name);

        if (remove(path) == 0) {
            storage_unregister_file(name);
            deleted++;
        } else {
            failed++;
        }
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
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return json_error(req, "Missing query", HTTPD_400_BAD_REQUEST);

    char name[128] = {0};
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
        return json_error(req, "Missing name parameter", HTTPD_400_BAD_REQUEST);

    /* Path traversal protection — allow subdirectory slashes but block .. */
    if (strstr(name, ".."))
        return json_error(req, "Invalid name", HTTPD_400_BAD_REQUEST);
    
    /* Check if file is currently being recorded */
    const char *current_file = recorder_get_current_file();
    if (current_file && current_file[0] != '\0') {
        /* Get relative path from current_file */
        const char *relpath = current_file;
        const char *prefix = "/sdcard/recordings/";
        if (strncmp(current_file, prefix, strlen(prefix)) == 0) {
            relpath = current_file + strlen(prefix);
        }
        if (strcmp(name, relpath) == 0) {
            httpd_resp_set_status(req, "409 Conflict");
            return json_error(req, "File is currently being recorded", 0);
        }
    }

    
    char path[256];
    snprintf(path, sizeof(path), "/sdcard/recordings/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return json_error(req, "File not found", HTTPD_404_NOT_FOUND);

    /* Get file size for Content-Length (skip if 0 - file is being written) */
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", hdr);
    }

    httpd_resp_set_type(req, "video/avi");
    const char *basename = strrchr(name, '/');
    basename = basename ? basename + 1 : name;
    char disp[192];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", basename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    set_cors_headers(req);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
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
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

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
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

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
/*  POST /api/reset                                                    */
/* ------------------------------------------------------------------ */

/** @brief 处理POST /api/reset请求，执行恢复出厂设置（需密码认证，会清空所有配置） */
static esp_err_t api_reset_handler(httpd_req_t *req)
{
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

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
    if (!check_password(req)) return json_error(req, "Unauthorized", HTTPD_401_UNAUTHORIZED);

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
    if (ret == ESP_OK) {
        cJSON_AddBoolToObject(data, "ok", true);
        cJSON_AddStringToObject(data, "message", "SD card formatted");
    } else {
        cJSON_AddBoolToObject(data, "ok", false);
        cJSON_AddStringToObject(data, "error", esp_err_to_name(ret));
    }

    esp_err_t resp = json_ok(req, data);

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

    char filepath[580];
    snprintf(filepath, sizeof(filepath), "/spiffs%s", uri);

    /* Content type */
    const char *type = "text/html";
    if (strstr(uri, ".css"))  type = "text/css";
    else if (strstr(uri, ".js"))   type = "application/javascript";
    else if (strstr(uri, ".png"))  type = "image/png";
    else if (strstr(uri, ".ico"))  type = "image/x-icon";

    FILE *f = fopen(filepath, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, type);
    set_cors_headers(req);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


/* ------------------------------------------------------------------ */
/*  GET /metrics                                                     */
/* ------------------------------------------------------------------ */

/** @brief 处理GET /metrics请求，返回Prometheus格式的系统指标 */
static esp_err_t metrics_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    set_cors_headers(req);

    char buf[2048];
    int len = 0;

    /* 收集系统指标 */
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    float temp = get_chip_temp();
    recorder_state_t rs = recorder_get_state();
    storage_info_t sd;
    storage_get_info(&sd);
    wifi_state_t ws = wifi_get_state();
    int upload_queue = 0;
    bool upload_paused = false;
    char last_upload[32] = "";
    nas_uploader_get_status(last_upload, sizeof(last_upload), &upload_queue, &upload_paused);

    len = snprintf(buf, sizeof(buf),
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
        "upload_queue_size %d\n",
        (unsigned long)free_heap,
        (unsigned long)free_psram,
        temp,
        rs == RECORDER_RECORDING ? 1 : 0,
        (unsigned long long)sd.free_bytes,
        (unsigned long long)sd.total_bytes,
        storage_get_free_percent(),
        ws == WIFI_STATE_STA_CONNECTED ? 1 : (ws == WIFI_STATE_AP ? 2 : 0),
        upload_queue);

    httpd_resp_send(req, buf, len);
    return ESP_OK;
}
/*  URI handler registration table                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char  *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} uri_entry_t;

static const uri_entry_t s_uris[] = {
    { "/api/status",   HTTP_GET,    api_status_handler        },
    { "/api/config",   HTTP_GET,    api_config_get_handler    },
    { "/api/config",   HTTP_POST,   api_config_post_handler   },
    { "/api/files",    HTTP_GET,    api_files_get_handler     },
    { "/api/files",    HTTP_DELETE, api_files_delete_handler  },
    { "/api/files/batch", HTTP_POST,  api_files_batch_handler  },
    { "/api/download", HTTP_GET,    api_download_handler      },
    { "/api/scan",     HTTP_GET,    api_scan_handler          },
    { "/api/time",     HTTP_POST,   api_time_handler          },
    { "/api/record",   HTTP_POST,   api_record_handler        },
    { "/api/reset",    HTTP_POST,   api_reset_handler         },
    { "/api/format",   HTTP_POST,   api_format_handler        },
    { "/metrics",      HTTP_GET,    metrics_handler           },
/* MJPEG stream — before wildcard to avoid conflict */
    { "/stream",       HTTP_GET,    mjpeg_stream_handler      },
    /* CORS preflight — wildcard */
    { "/*",            HTTP_OPTIONS, options_handler           },
    /* Static files — catch-all (lowest priority) */
    { "/*",            HTTP_GET,    static_file_handler       },
};

#define NUM_URIS (sizeof(s_uris) / sizeof(s_uris[0]))

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/** @brief 启动HTTP Web服务器，注册所有URI处理程序，配置通配符路由匹配 */
esp_err_t web_server_start(uint16_t port)
{
    if (s_server) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < NUM_URIS; i++) {
        httpd_uri_t uri = {
            .uri      = s_uris[i].uri,
            .method   = s_uris[i].method,
            .handler  = s_uris[i].handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_server, &uri);
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
