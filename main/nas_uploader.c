#include "nas_uploader.h"
#include "ftp_client.h"
#include "webdav_client.h"
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
                     cfg->ftp_path, rec_part + strlen("/recordings"));
        } else {
            snprintf(remote_path, sizeof(remote_path), "%s/%s",
                     cfg->ftp_path, filename);
        }

        /* Extract parent directory for mkdir */
        char remote_dir[256];
        strncpy(remote_dir, remote_path, sizeof(remote_dir) - 1);
        remote_dir[sizeof(remote_dir) - 1] = '\0';
        char *last_slash = strrchr(remote_dir, '/');
        if (last_slash) *last_slash = '\0';

        /* ---- FTP: try first ---- */
        if (cfg->ftp_enabled && strlen(cfg->ftp_host) > 0) {
            ftp_config_t ftp_cfg = {0};
            strncpy(ftp_cfg.host, cfg->ftp_host, sizeof(ftp_cfg.host) - 1);
            ftp_cfg.port = cfg->ftp_port;
            strncpy(ftp_cfg.user, cfg->ftp_user, sizeof(ftp_cfg.user) - 1);
            strncpy(ftp_cfg.pass, cfg->ftp_pass, sizeof(ftp_cfg.pass) - 1);

            for (int retry = 0; retry < MAX_RETRIES; retry++) {
                esp_task_wdt_reset();
                if (retry > 0) {
                    int delay_ms = 1000 * (1 << retry);  /* 2 s, 4 s */
                    ESP_LOGI(TAG, "FTP retry %d/%d in %d ms", retry + 1, MAX_RETRIES, delay_ms);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                }

                if (ftp_connect(&ftp_cfg) != ESP_OK) continue;
                ftp_mkdir_recursive(remote_dir);
                esp_err_t ret = ftp_upload(remote_path, filepath);
                ftp_disconnect();

                if (ret == ESP_OK) {
                    success = true;
                    break;
                }
            }
        }

        /* ---- WebDAV: fallback ---- */
        if (!success && cfg->webdav_enabled && strlen(cfg->webdav_url) > 0) {
            esp_task_wdt_reset();
            webdav_config_t dav_cfg = {0};
            strncpy(dav_cfg.url, cfg->webdav_url, sizeof(dav_cfg.url) - 1);
            strncpy(dav_cfg.user, cfg->webdav_user, sizeof(dav_cfg.user) - 1);
            strncpy(dav_cfg.pass, cfg->webdav_pass, sizeof(dav_cfg.pass) - 1);

            webdav_mkdir_recursive(&dav_cfg, remote_dir);
            if (webdav_upload(&dav_cfg, remote_path, filepath) == ESP_OK) {
                success = true;
            }
        }

        /* ---- Handle result ---- */
        if (success) {
            s_consec_fails = 0;
            time_get_str(s_last_upload_str, sizeof(s_last_upload_str));
            ESP_LOGI(TAG, "Upload success: %s", filepath);
        } else {
            s_consec_fails++;
            ESP_LOGW(TAG, "Upload failed (%d consecutive): %s",
                     s_consec_fails, filepath);

            if (s_consec_fails >= MAX_CONSEC_FAILS) {
                s_paused_until_ms = esp_timer_get_time() / 1000 + PAUSE_DURATION_MS;
                ESP_LOGW(TAG, "Too many failures, pausing for 5 minutes");
            }
        }
    }
}

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

uint32_t nas_uploader_get_stack_hwm(void)
{
    return s_stack_hwm;
}
