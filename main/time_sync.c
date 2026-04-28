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

#include "time_sync.h"
#include "config_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "time_sync";
static bool s_synced = false;

/**
 * @brief 启动 SNTP 时间同步
 * 配置并启动 SNTP 客户端，使用 cn.pool.ntp.org 和 ntp.aliyun.com 作为服务器
 * 阻塞等待最多 5 秒（50×100ms）获取首次同步，超时后异步继续同步
 * @return ESP_OK 成功
 */
esp_err_t time_sync_init(void)
{
    if (s_synced) {
        ESP_LOGI(TAG, "Time already synced");
        return ESP_OK;
    }

    /* Apply configured timezone before SNTP sync */
    cam_config_t *cfg = config_get();
    if (cfg && cfg->timezone[0]) {
        setenv("TZ", cfg->timezone, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone applied: %s", cfg->timezone);
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

/**
 * @brief 检查系统时间是否已同步
 * 先检查同步标志位，再检查系统时间年份是否大于 2020
 * 若 SNTP 已异步设置时间也会返回 true
 * @return true 已同步，false 未同步
 */
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

/**
 * @brief 获取当前时间的格式化字符串
 * 将当前系统时间格式化为 "YYYY-MM-DD HH:MM:SS" 格式写入缓冲区
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 */
void time_get_str(char *buf, size_t len)
{
    time_t now = 0;
    time(&now);
    struct tm timeinfo = {0};
    localtime_r(&now, &timeinfo);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

/**
 * @brief 手动设置系统时间
 * 通过传入的年月日时分秒构造 struct tm 并设置为系统时间
 * 设置后标记为已同步状态
 * @param year 年（如 2024）
 * @param month 月（1-12）
 * @param day 日（1-31）
 * @param hour 时（0-23）
 * @param min 分（0-59）
 * @param sec 秒（0-59）
 * @return ESP_OK 成功
 */
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

/**
 * @brief 应用时区设置
 * 通过 setenv("TZ", tz) + tzset() 设置系统时区
 * 使用 POSIX 时区格式，如 "CST-8" (中国 UTC+8)、"UTC0"、"EST5EDT" 等
 * @param tz 时区字符串（POSIX格式）
 */
void time_sync_apply_timezone(const char *tz)
{
    if (!tz || tz[0] == '\0') {
        tz = "CST-8";  /* 默认中国标准时间 */
    }
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz);
}
