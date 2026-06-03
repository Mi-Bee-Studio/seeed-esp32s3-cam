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
#include "webhook.h"
#include "config_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "webhook";

/* ---- dedup tracking ---- */
#define MAX_EVENT_TYPES 8
#define DEDUP_INTERVAL_MS 60000

typedef struct {
    char event_type[64];
    int64_t last_send_ms;
} dedup_entry_t;

static dedup_entry_t s_dedup[MAX_EVENT_TYPES];
static int s_dedup_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* ------------------------------------------------------------------ */
/*  webhook_init                                                       */
/* ------------------------------------------------------------------ */

esp_err_t webhook_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    memset(s_dedup, 0, sizeof(s_dedup));
    s_dedup_count = 0;
    ESP_LOGI(TAG, "Webhook module initialized");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static bool is_duplicate(const char *event_type)
{
    for (int i = 0; i < s_dedup_count; i++) {
        if (strcmp(s_dedup[i].event_type, event_type) == 0) {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - s_dedup[i].last_send_ms < DEDUP_INTERVAL_MS) {
                return true;
            }
            return false;
        }
    }
    return false;
}

static void update_dedup(const char *event_type)
{
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < s_dedup_count; i++) {
        if (strcmp(s_dedup[i].event_type, event_type) == 0) {
            s_dedup[i].last_send_ms = now;
            return;
        }
    }
    if (s_dedup_count < MAX_EVENT_TYPES) {
        strncpy(s_dedup[s_dedup_count].event_type, event_type,
                sizeof(s_dedup[0].event_type) - 1);
        s_dedup[s_dedup_count].event_type[sizeof(s_dedup[0].event_type) - 1] = '\0';
        s_dedup[s_dedup_count].last_send_ms = now;
        s_dedup_count++;
    }
}

/* ------------------------------------------------------------------ */
/*  webhook_send_alert                                                 */
/* ------------------------------------------------------------------ */

esp_err_t webhook_send_alert(const char *event_type, const char *message)
{
    if (!event_type || !message) return ESP_ERR_INVALID_ARG;
    if (strlen(event_type) > 64 || strlen(message) > 256) return ESP_ERR_INVALID_ARG;

    cam_config_t *cfg = config_get();
    if (!cfg->alert_webhook_enabled || strlen(cfg->alert_webhook_url) == 0) {
        return ESP_OK; /* disabled or no URL configured */
    }

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (is_duplicate(event_type)) {
        ESP_LOGD(TAG, "Suppressed duplicate: %s (within 60s)", event_type);
        if (s_mutex) xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    /* Build ISO 8601 timestamp */
    time_t now_sec = time(NULL);
    struct tm tm_info;
    localtime_r(&now_sec, &tm_info);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_info);

    /* Build JSON payload */
    char body[512];
    snprintf(body, sizeof(body),
             "{\"event\":\"%s\",\"message\":\"%s\",\"ts\":\"%s\"}",
             event_type, message, ts);

    esp_err_t ret = ESP_FAIL;

    for (int attempt = 1; attempt <= 3; attempt++) {
        esp_http_client_config_t http_cfg = {
            .url = cfg->alert_webhook_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 5000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) {
            ESP_LOGW(TAG, "Attempt %d: failed to create client", attempt);
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS((1 << (attempt - 1)) * 1000));
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));

        ret = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (ret == ESP_OK && status >= 200 && status < 300) {
            ESP_LOGI(TAG, "Alert sent: %s (%s) -> %d", event_type, message, status);
            update_dedup(event_type);
            break;
        }

        ESP_LOGW(TAG, "Attempt %d failed: %s (HTTP %d)",
                 attempt, esp_err_to_name(ret), status);
        if (attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS((1 << (attempt - 1)) * 1000));
        }
    }

    if (s_mutex) xSemaphoreGive(s_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send alert after 3 retries");
    }
    return ret;
}
