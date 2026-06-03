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

#include "ota_updater.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#include "video_recorder.h"
#include "web_server.h"

static const char *TAG = "ota";

static SemaphoreHandle_t s_ota_mutex = NULL;

/* ---- Forward declarations from web_server.c ---- */
extern esp_err_t json_ok(httpd_req_t *req, cJSON *data);
extern esp_err_t json_error(httpd_req_t *req, const char *msg, int status);
extern char *read_body(httpd_req_t *req, size_t max_len);

#define OTA_BUF_SIZE 4096

/**
 * @brief Perform OTA update from a given firmware URL.
 *
 * Stops recording before starting the OTA process, restarts recording on
 * failure. On success the system restarts automatically.
 */
static esp_err_t ota_do_update(const char *url)
{
    /* ---- Stop recording before OTA ---- */
    bool was_recording = (recorder_get_state() == RECORDER_RECORDING ||
                          recorder_get_state() == RECORDER_PAUSED);
    if (was_recording) {
        recorder_stop();
        ESP_LOGI(TAG, "Recording stopped for OTA update");
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);

    esp_https_ota_config_t ota_config = {
        .url = url,
        .cert_pem = NULL, /* Skip cert verification (LAN only) */
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        goto resume_recording;
    }

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    ESP_LOGI(TAG, "OTA image size: %d bytes", image_size);

    int remaining = image_size;
    while (remaining > 0) {
        char ota_buf[OTA_BUF_SIZE];
        int read = esp_https_ota_read(https_ota_handle, ota_buf, OTA_BUF_SIZE);
        if (read < 0) {
            ESP_LOGE(TAG, "OTA read failed");
            err = ESP_FAIL;
            break;
        }
        err = esp_https_ota_write(https_ota_handle, ota_buf, read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            break;
        }
        remaining -= read;
    }

    if (err == ESP_OK) {
        err = esp_https_ota_finish(https_ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA update successful, marking app valid");
            esp_ota_mark_app_valid_cancel_rollback();
            /* Small delay for logs to flush, then restart */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        }
    } else {
        esp_https_ota_abort(https_ota_handle);
    }

resume_recording:
    if (was_recording) {
        recorder_start();
        ESP_LOGI(TAG, "Recording resumed after OTA failure");
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ota handler                                              */
/* ------------------------------------------------------------------ */

esp_err_t api_ota_handler(httpd_req_t *req)
{
    if (!xSemaphoreTake(s_ota_mutex, 0)) {
        return json_error(req, "OTA already in progress", 429);
    }

    esp_err_t ret;

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
