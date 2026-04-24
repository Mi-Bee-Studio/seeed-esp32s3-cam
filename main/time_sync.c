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

#include "time_sync.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "time_sync";
static bool s_synced = false;

esp_err_t time_sync_init(void)
{
    if (s_synced) {
        ESP_LOGI(TAG, "Time already synced");
        return ESP_OK;
    }

    esp_sntp_stop();  /* idempotent */

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "cn.pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP started, waiting for sync...");

    /* Wait up to 5 seconds for time to be set */
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_count = 50;  /* 50 × 100ms = 5s */

    while (retry < retry_count) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2020 - 1900)) {
            s_synced = true;
            char buf[32];
            time_get_str(buf, sizeof(buf));
            ESP_LOGI(TAG, "Time synced: %s", buf);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }

    ESP_LOGW(TAG, "NTP sync not completed within 5s, continuing async");
    return ESP_OK;
}

bool time_is_synced(void)
{
    /* Check if time was recently synced by SNTP callback or manual set */
    if (s_synced) {
        return true;
    }
    /* Also check if system time looks valid (SNTP may have set it async) */
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2020 - 1900)) {
        s_synced = true;
        return true;
    }
    return false;
}

void time_get_str(char *buf, size_t len)
{
    time_t now = 0;
    time(&now);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec)
{
    struct tm timeinfo = {
        .tm_year = year - 1900,
        .tm_mon  = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min  = min,
        .tm_sec  = sec,
    };
    time_t t = mktime(&timeinfo);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    s_synced = true;

    char buf[32];
    time_get_str(buf, sizeof(buf));
    ESP_LOGI(TAG, "Time set manually: %s", buf);

    return ESP_OK;
}
