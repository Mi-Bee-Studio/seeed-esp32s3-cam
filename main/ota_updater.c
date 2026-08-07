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

#include "ota_updater.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#include "video_recorder.h"
#include "web_server.h"

static const char *TAG = "ota";

static SemaphoreHandle_t s_ota_mutex = NULL;


#define OTA_BUF_SIZE 4096

/**
 * @brief Perform OTA update from a given firmware URL.
 *
 * Stops recording before starting the OTA process, restarts recording on
 * failure. On success the system restarts automatically.
 */
static esp_err_t ota_do_update(const char *url)
{
    bool was_recording = (recorder_get_state() == RECORDER_RECORDING ||
                          recorder_get_state() == RECORDER_PAUSED);
    if (was_recording) {
        recorder_stop();
        ESP_LOGI(TAG, "Recording stopped for OTA update");
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .cert_pem = NULL,  /* Skip cert verification (LAN only) */
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    /* If esp_https_ota returns OK, we should restart */
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));

    /* Resume recording on failure */
    if (was_recording) {
        recorder_start();
        ESP_LOGI(TAG, "Recording resumed after OTA failure");
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ota handler                                              */
/* ------------------------------------------------------------------ */

esp_err_t api_ota_handler(httpd_req_t *req)
{
    esp_err_t auth_err = web_server_check_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    
    if (!xSemaphoreTake(s_ota_mutex, 0)) {
        return json_error_status(req, "OTA already in progress", "429 Too Many Requests");
    }


    char *body = read_body(req, 1024);
    if (!body) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "Empty or too large body", 400);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "Invalid JSON", 400);
    }

    cJSON *j_url = cJSON_GetObjectItem(json, "url");
    if (!j_url || !cJSON_IsString(j_url) || strlen(j_url->valuestring) == 0) {
        cJSON_Delete(json);
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "Missing or empty 'url' field", 400);
    }

    const char *url = j_url->valuestring;

    /* Validate URL prefix — only http:// allowed (LAN only, no TLS) */
    if (strncmp(url, "http://", 7) != 0) {
        cJSON_Delete(json);
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "Only http:// URLs are supported (no TLS)", 400);
    }

    cJSON_Delete(json);

    /* Run the OTA update */
    esp_err_t ota_ret = ota_do_update(url);
    xSemaphoreGive(s_ota_mutex);

    if (ota_ret != ESP_OK) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "OTA failed: %s", esp_err_to_name(ota_ret));
        return json_error(req, err_msg, 500);
    }

    /* If we get here, ota_do_update called esp_restart() on success */
    return json_error(req, "OTA unexpected state", 500);
}

/* ------------------------------------------------------------------ */
/*  Init                                                               */
/* ------------------------------------------------------------------ */

esp_err_t ota_updater_init(void)
{
    s_ota_mutex = xSemaphoreCreateMutex();
    if (!s_ota_mutex) {
        ESP_LOGE(TAG, "Failed to create OTA mutex");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA updater ready — firmware version: %s", FW_VERSION);
    return ESP_OK;
}

/* ===== T9a: OTA Upload + SPIFFS OTA + Info Endpoints ===== */
#include "esp_partition.h"
#include "esp_app_desc.h"

/** @brief 停止系统外设，为 OTA 写入准备 */
static void ota_quiesce_system(void)
{
    if (recorder_get_state() != RECORDER_IDLE) {
        recorder_stop();
        ESP_LOGI(TAG, "Recorder stopped for OTA");
    }
    vTaskDelay(pdMS_TO_TICKS(500));  /* 等待录像完全停止 */
}

/** @brief GET /api/ota/info — 返回固件版本和分区信息 */
esp_err_t api_ota_info_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "fw_version", FW_VERSION);
    if (desc && desc->version[0]) {
        cJSON_AddStringToObject(data, "app_version", desc->version);
    }
    cJSON_AddStringToObject(data, "running_partition", running ? running->label : "unknown");
    cJSON_AddStringToObject(data, "next_partition", next ? next->label : "none");
    cJSON_AddNumberToObject(data, "max_image_size", (double)(next ? next->size : 0));
    cJSON_AddNumberToObject(data, "running_size", (double)(running ? running->size : 0));

    return json_ok(req, data);
}

/** @brief POST /api/ota/upload — 流式上传固件二进制 */
esp_err_t api_ota_upload_handler(httpd_req_t *req)
{
    esp_err_t auth_err = web_server_check_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (!xSemaphoreTake(s_ota_mutex, 0)) {
        return json_error_status(req, "OTA already in progress", "429 Too Many Requests");
    }

    int content_len = req->content_len;
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "No OTA partition available", 500);
    }

    if (content_len <= 0 || (size_t)content_len > update_part->size) {
        xSemaphoreGive(s_ota_mutex);
        char msg[128];
        snprintf(msg, sizeof(msg), "Invalid size: %d (max %u)", content_len, (unsigned)update_part->size);
        return json_error(req, msg, 400);
    }

    ESP_LOGI(TAG, "OTA upload: %d bytes to partition '%s'", content_len, update_part->label);
    ota_quiesce_system();

    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "esp_ota_begin failed", 500);
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(update_handle);
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "Out of memory", 500);
    }

    int received = 0;
    while (received < content_len) {
        int to_recv = (content_len - received < OTA_BUF_SIZE) ? (content_len - received) : OTA_BUF_SIZE;
        int ret = httpd_req_recv(req, buf, to_recv);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            esp_ota_abort(update_handle);
            xSemaphoreGive(s_ota_mutex);
            return json_error(req, "Connection lost during upload", 500);
        }
        err = esp_ota_write(update_handle, buf, ret);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(update_handle);
            xSemaphoreGive(s_ota_mutex);
            char msg[128];
            snprintf(msg, sizeof(msg), "esp_ota_write failed: %s", esp_err_to_name(err));
            return json_error(req, msg, 500);
        }
        received += ret;
    }
    free(buf);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mutex);
        char msg[128];
        snprintf(msg, sizeof(msg), "esp_ota_end failed: %s", esp_err_to_name(err));
        return json_error(req, msg, 500);
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "esp_ota_set_boot_partition failed", 500);
    }

    ESP_LOGI(TAG, "OTA upload complete (%d bytes), restarting...", received);
    /* 发送成功响应后重启 */
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"status\":\"ok\",\"msg\":\"Upload complete, restarting\"}";
    httpd_resp_send(req, resp, strlen(resp));
    xSemaphoreGive(s_ota_mutex);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/** @brief POST /api/ota/spiffs — 流式上传 SPIFFS 镜像 */
esp_err_t api_ota_spiffs_handler(httpd_req_t *req)
{
    esp_err_t auth_err = web_server_check_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }

    if (!xSemaphoreTake(s_ota_mutex, 0)) {
        return json_error_status(req, "OTA already in progress", "429 Too Many Requests");
    }

    int content_len = req->content_len;
    const esp_partition_t *spiffs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (!spiffs_part) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "No SPIFFS partition found", 500);
    }

    if (content_len <= 0 || (size_t)content_len > spiffs_part->size) {
        xSemaphoreGive(s_ota_mutex);
        char msg[128];
        snprintf(msg, sizeof(msg), "Invalid size: %d (max %u)", content_len, (unsigned)spiffs_part->size);
        return json_error(req, msg, 400);
    }

    ESP_LOGI(TAG, "SPIFFS OTA: %d bytes to partition '%s'", content_len, spiffs_part->label);
    ota_quiesce_system();

    /* 全擦除 SPIFFS 分区 */
    esp_err_t err = esp_partition_erase_range(spiffs_part, 0, spiffs_part->size);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "SPIFFS erase failed", 500);
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        xSemaphoreGive(s_ota_mutex);
        return json_error(req, "Out of memory", 500);
    }

    int received = 0;
    uint32_t offset = 0;
    while (received < content_len) {
        int to_recv = (content_len - received < OTA_BUF_SIZE) ? (content_len - received) : OTA_BUF_SIZE;
        int ret = httpd_req_recv(req, buf, to_recv);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            xSemaphoreGive(s_ota_mutex);
            return json_error(req, "Connection lost during upload", 500);
        }
        err = esp_partition_write(spiffs_part, offset, buf, ret);
        if (err != ESP_OK) {
            free(buf);
            xSemaphoreGive(s_ota_mutex);
            char msg[128];
            snprintf(msg, sizeof(msg), "esp_partition_write failed: %s", esp_err_to_name(err));
            return json_error(req, msg, 500);
        }
        offset += ret;
        received += ret;
    }
    free(buf);

    ESP_LOGI(TAG, "SPIFFS OTA complete (%d bytes), restarting...", received);
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"status\":\"ok\",\"msg\":\"SPIFFS update complete, restarting\"}";
    httpd_resp_send(req, resp, strlen(resp));
    xSemaphoreGive(s_ota_mutex);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
