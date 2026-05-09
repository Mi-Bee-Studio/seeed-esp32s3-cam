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

#include "nas_uploader.h"
#include "webdav_client.h"
#include "http_upload_client.h"
#include "config_manager.h"
#include "time_sync.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>
static const char *TAG = "uploader";

#define UPLOAD_QUEUE_SIZE    16
#define MAX_RETRIES          3
#define MAX_CONSEC_FAILS     10
#define PAUSE_DURATION_MS    (5 * 60 * 1000)  /* 5 minutes */
#define PATH_BUF_SIZE        128

static QueueHandle_t s_queue        = NULL;
static TaskHandle_t  s_task_handle  = NULL;
static bool          s_initialized  = false;
static int           s_consec_fails = 0;
static int64_t       s_paused_until_ms = 0;
static char          s_last_upload_str[32] = "";
static int           s_queue_count  = 0;
static uint32_t      s_stack_hwm    = 0;
static volatile int   s_upload_success = 0;
static volatile int   s_upload_failure = 0;

/**
 * @brief 上传任务主循环（FreeRTOS任务函数）
 * 从队列中取出文件路径，根据 upload_method 分发上传
 * 支持指数退避重试和连续失败自动暂停
 * @param arg 未使用
 */
static void upload_task(void *arg)
{
    (void)arg;

    /* Register with task watchdog */
    esp_task_wdt_add(NULL);

    while (1) {
        /* Feed task watchdog each iteration */
        esp_task_wdt_reset();

        /* Track stack high-water mark */
        s_stack_hwm = uxTaskGetStackHighWaterMark(NULL);

        /* Check global pause */
        if (esp_timer_get_time() / 1000 < s_paused_until_ms) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* Wait for a queue item */
        char filepath[PATH_BUF_SIZE];
        if (xQueueReceive(s_queue, filepath, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        s_queue_count = (int)uxQueueMessagesWaiting(s_queue);

        cam_config_t *cfg = config_get();
        bool success = false;

        /* Extract filename for fallback remote path */
        const char *filename = strrchr(filepath, '/');
        filename = filename ? filename + 1 : filepath;

        /* Build remote path from /recordings/... portion */
        char remote_path[256];
        const char *rec_part = strstr(filepath, "/recordings/");
        if (rec_part) {
            snprintf(remote_path, sizeof(remote_path), "%s%s",
                     cfg->upload_base_path, rec_part + strlen("/recordings"));
        } else {
            snprintf(remote_path, sizeof(remote_path), "%s/%s",
                     cfg->upload_base_path, filename);
        }

        /* Extract parent directory for mkdir */
        char remote_dir[256];
        strncpy(remote_dir, remote_path, sizeof(remote_dir) - 1);
        remote_dir[sizeof(remote_dir) - 1] = '\0';
        char *last_slash = strrchr(remote_dir, '/');
        if (last_slash) *last_slash = '\0';

        /* ---- Upload dispatch ---- */
        esp_task_wdt_reset();

        switch (cfg->upload_method) {
        case 0: /* 禁用 */
            ESP_LOGD(TAG, "Upload disabled, skipping: %s", filepath);
            success = true;
            break;

        case 1: /* WebDAV */
            if (strlen(cfg->webdav_url) > 0) {
                webdav_config_t dav_cfg = {0};
                strncpy(dav_cfg.url, cfg->webdav_url, sizeof(dav_cfg.url) - 1);
                strncpy(dav_cfg.user, cfg->webdav_user, sizeof(dav_cfg.user) - 1);
                strncpy(dav_cfg.pass, cfg->webdav_pass, sizeof(dav_cfg.pass) - 1);

                webdav_mkdir_recursive(&dav_cfg, remote_dir);
                if (webdav_upload(&dav_cfg, remote_path, filepath) == ESP_OK) {
                    success = true;
                }
            } else {
                ESP_LOGW(TAG, "WebDAV selected but URL not configured");
            }
            break;

        case 2: /* HTTP/HTTPS */ {
            if (strlen(cfg->http_upload_url) > 0) {
                http_upload_config_t http_cfg = {0};
                strncpy(http_cfg.url, cfg->http_upload_url, sizeof(http_cfg.url) - 1);
                strncpy(http_cfg.user, cfg->http_upload_user, sizeof(http_cfg.user) - 1);
                strncpy(http_cfg.pass, cfg->http_upload_pass, sizeof(http_cfg.pass) - 1);
                http_cfg.skip_cert_verify = cfg->http_upload_skip_cert;

                http_upload_mkdir(&http_cfg, remote_dir);
                if (http_upload_file(&http_cfg, remote_path, filepath) == ESP_OK) {
                    success = true;
                }
            } else {
                ESP_LOGW(TAG, "HTTP upload selected but URL not configured");
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown upload_method %d, skipping", cfg->upload_method);
            break;
        }

        /* ---- Handle result ---- */
        if (success) {
            s_consec_fails = 0;
            time_get_str(s_last_upload_str, sizeof(s_last_upload_str));
            s_upload_success++;
            ESP_LOGI(TAG, "Upload success: %s", filepath);
        } else {
            s_consec_fails++;
            s_upload_failure++;
            ESP_LOGW(TAG, "Upload failed (%d consecutive): %s",
                     s_consec_fails, filepath);

            if (s_consec_fails >= MAX_CONSEC_FAILS) {
                s_paused_until_ms = esp_timer_get_time() / 1000 + PAUSE_DURATION_MS;
                ESP_LOGW(TAG, "Too many failures, pausing for 5 minutes");
            }
        }
    }
}

/**
 * @brief 初始化NAS上传模块
 * 创建上传队列（容量16）和后台上传任务（栈6144，核心1，优先级3）
 * @return ESP_OK 成功，ESP_FAIL 创建队列或任务失败
 */
esp_err_t nas_uploader_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(UPLOAD_QUEUE_SIZE, PATH_BUF_SIZE);
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create upload queue");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(upload_task, "upload",
                                              6144, NULL, 3,
                                              &s_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NAS uploader initialized (queue %d, core 1)", UPLOAD_QUEUE_SIZE);
    return ESP_OK;
}

/**
 * @brief 将文件路径加入上传队列
 * @param filepath 待上传文件的完整路径
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，ESP_ERR_NO_MEM 队列已满
 */
esp_err_t nas_uploader_enqueue(const char *filepath)
{
    if (!s_initialized || !s_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    char buf[PATH_BUF_SIZE] = {0};
    strncpy(buf, filepath, sizeof(buf) - 1);

    if (xQueueSend(s_queue, buf, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Upload queue full, dropping: %s", filepath);
        return ESP_ERR_NO_MEM;
    }

    s_queue_count = (int)uxQueueMessagesWaiting(s_queue);
    return ESP_OK;
}

/**
 * @brief 获取上传模块当前状态
 * @param last_upload 输出上次成功上传时间字符串
 * @param len last_upload缓冲区长度
 * @param queue_count 输出当前队列中的文件数量
 * @param paused 输出是否因连续失败而暂停
 */
void nas_uploader_get_status(char *last_upload, size_t len,
                             int *queue_count, bool *paused)
{
    if (last_upload) {
        strncpy(last_upload, s_last_upload_str, len - 1);
        last_upload[len - 1] = '\0';
    }
    if (queue_count) {
        *queue_count = s_queue_count;
    }
    if (paused) {
        *paused = (esp_timer_get_time() / 1000 < s_paused_until_ms);
    }
}

/**
 * @brief 获取上传任务的栈高水位标记
 * @return 剩余栈空间（字节）
 */
uint32_t nas_uploader_get_stack_hwm(void)
{
    return s_stack_hwm;
}

/**
 * @brief 获取上传成功/失败累计计数
 * @param success 输出成功次数
 * @param failure 输出失败次数
 */
void nas_uploader_get_stats(int *success, int *failure)
{
    if (success) {
        *success = s_upload_success;
    }
    if (failure) {
        *failure = s_upload_failure;
    }
}
