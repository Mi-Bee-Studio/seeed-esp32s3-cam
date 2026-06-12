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
        esp_ota_mark_app_valid_cancel_rollback();
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
    if (!xSemaphoreTake(s_ota_mutex, 0)) {
        return json_error(req, "OTA already in progress", 429);
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
