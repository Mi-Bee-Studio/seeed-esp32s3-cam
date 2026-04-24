/*
 * ParrotCam v0.1 — Main application entry point
 *
 * Boot sequence:
 *   1. NVS flash
 *   2. Config manager (loads from NVS)
 *   3. Status LED
 *   4. SPIFFS (Web UI assets)
 *   5. SD card storage
 *   5a. Boot-time cleanup of incomplete AVI files
 *   6. Config SD override
 *   7. WiFi (AP or STA)
 *   8. Camera
 *   9. Time sync (NTP, if STA connected)
 *  10. NAS uploader
 *  11. Video recorder
 *  12. MJPEG streamer
 *  13. Web server + streamer registration
 *  14. LED update for WiFi state
 *  15. Start recording
 *  16. BOOT button monitor (factory reset on 5s hold)
 *  17. Watchdog (30s)
 *  18. SD monitor task (polling 10s)
 *  19. Health monitor task (60s)
 */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

/* Module headers */
#include "config_manager.h"
#include "status_led.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "time_sync.h"
#include "camera_driver.h"
#include "video_recorder.h"
#include "web_server.h"
#include "mjpeg_streamer.h"
#include "nas_uploader.h"

static const char *TAG = "main";

/* Factory-reset: hold BOOT button (GPIO0) for 5 seconds */
#define BOOT_BTN_GPIO    0
#define RESET_HOLD_MS    5000

/* ------------------------------------------------------------------ */
/*  Recording segment callback — enqueue for NAS upload               */
/* ------------------------------------------------------------------ */

static void on_segment_complete(const char *filepath, size_t size)
{
    ESP_LOGI(TAG, "Segment complete: %s (%zu bytes)", filepath, size);

    nas_uploader_enqueue(filepath);
    storage_cleanup();
}

/* ------------------------------------------------------------------ */
/*  SPIFFS mount for Web UI static assets                             */
/* ------------------------------------------------------------------ */

static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,     /* default SPIFFS partition */
        .max_files              = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted");
    }
}

/* ------------------------------------------------------------------ */
/*  BOOT button monitor task (factory reset on 5 s hold)              */
/* ------------------------------------------------------------------ */

static void boot_button_monitor(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << BOOT_BTN_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    while (1) {
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            int held_ms = 0;
            while (gpio_get_level(BOOT_BTN_GPIO) == 0 && held_ms < RESET_HOLD_MS) {
                vTaskDelay(pdMS_TO_TICKS(100));
                held_ms += 100;
            }
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT button held 5s — FACTORY RESET");
                config_reset();
                led_set_status(LED_ERROR);
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------------ */
/*  SD card monitor task — polls every 10s, handles removal/insertion  */
/* ------------------------------------------------------------------ */

static bool s_was_recording = false;

static void sd_monitor_task(void *arg)
{
    (void)arg;
    bool prev_available = storage_is_available();

    ESP_LOGI(TAG, "SD monitor started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        bool now_available = (storage_check() == ESP_OK);

        if (prev_available && !now_available) {
            /* SD card removed */
            ESP_LOGW(TAG, "SD card removed detected");
            recorder_state_t state = recorder_get_state();
            if (state == RECORDER_RECORDING || state == RECORDER_PAUSED) {
                s_was_recording = true;
                recorder_stop();
                ESP_LOGW(TAG, "Recording stopped due to SD removal");
            } else if (state == RECORDER_ERROR) {
                /* Recorder already detected SD failure — still mark for auto-resume */
                s_was_recording = true;
            } else {
                s_was_recording = false;
            }
            led_set_status(LED_ERROR);
        }

        if (!prev_available && now_available) {
            /* SD card reinserted */
            ESP_LOGI(TAG, "SD card reinserted — remounting...");
            if (storage_remount() == ESP_OK) {
                storage_cleanup();
                if (s_was_recording) {
                    ESP_LOGI(TAG, "Auto-resuming recording after SD reinsert");
                    recorder_start();
                    s_was_recording = false;
                    led_set_status(LED_RUNNING);
                }
            }
        }

        prev_available = now_available;
    }
}

/* ------------------------------------------------------------------ */
/*  Health monitor task — logs heap/PSRAM/stack every 60s              */
/* ------------------------------------------------------------------ */

static void health_monitor_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Health monitor started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));

        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t free_psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t rec_hwm = recorder_get_stack_hwm();
        uint32_t nas_hwm = nas_uploader_get_stack_hwm();

        ESP_LOGI(TAG, "HEALTH: heap=%lu PSRAM=%lu rec_hwm=%lu nas_hwm=%lu",
                 (unsigned long)free_heap, (unsigned long)free_psram,
                 (unsigned long)rec_hwm, (unsigned long)nas_hwm);

        if (free_heap < 20000) {
            ESP_LOGE(TAG, "CRITICAL: Free heap below 20KB (%lu bytes)", (unsigned long)free_heap);
        }
        if (free_psram < 500000) {
            ESP_LOGW(TAG, "WARNING: Free PSRAM below 500KB (%lu bytes)", (unsigned long)free_psram);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  app_main                                                           */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "ParrotCam v0.1 starting...");
    ESP_LOGI(TAG, "Free heap: %lu  Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- 1. NVS flash ------------------------------------------------ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- 2. Config manager ------------------------------------------- */
    config_init();
    cam_config_t *cfg = config_get();

    /* ---- 3. Status LED ----------------------------------------------- */
    led_init();
    led_set_status(LED_STARTING);

    /* ---- 4. SPIFFS for Web UI ---------------------------------------- */
    init_spiffs();

    /* ---- 5. SD card storage ------------------------------------------ */
    storage_init();

    /* ---- 5a. Boot-time cleanup of incomplete AVI files --------------- */
    if (storage_is_available()) {
        recorder_cleanup_incomplete();
    }

    /* ---- 6. Config SD override --------------------------------------- */
    config_load_from_sd();
    cfg = config_get();

    /* ---- 7. WiFi ----------------------------------------------------- */
    wifi_init();

    /* ---- 8. Camera --------------------------------------------------- */
    camera_res_t res = CAMERA_RES_SVGA;
    switch (cfg->resolution) {
        case 0:  res = CAMERA_RES_VGA;  break;
        case 1:  res = CAMERA_RES_SVGA; break;
        case 2:  res = CAMERA_RES_XGA;  break;
        default: res = CAMERA_RES_SVGA; break;
    }
    camera_init(res, cfg->fps, cfg->jpeg_quality);

    /* ---- 9. Time sync (only if STA connected) ----------------------- */
    if (wifi_get_state() == WIFI_STATE_STA_CONNECTED) {
        time_sync_init();
    }

    /* ---- 10. NAS uploader -------------------------------------------- */
    nas_uploader_init();

    /* ---- 11. Video recorder ------------------------------------------ */
    recorder_init();
    recorder_set_segment_cb(on_segment_complete);

    /* ---- 12. MJPEG streamer ------------------------------------------ */
    mjpeg_streamer_init();

    /* ---- 13. Web server + MJPEG registration ------------------------- */
    web_server_start(80);
    mjpeg_streamer_register(web_server_get_handle());

    /* ---- 14. LED reflects WiFi state --------------------------------- */
    if (wifi_get_state() == WIFI_STATE_AP) {
        led_set_status(LED_AP_MODE);
    } else {
        led_set_status(LED_WIFI_CONNECTING);
    }

    /* ---- 15. Wait for STA connection, then start recording ----------- */
    if (wifi_get_state() != WIFI_STATE_AP) {
        for (int i = 0; i < 30; i++) {
            if (wifi_get_state() == WIFI_STATE_STA_CONNECTED) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (wifi_get_state() == WIFI_STATE_STA_CONNECTED) {
            time_sync_init();   /* retry / ensure synced */
            led_set_status(LED_RUNNING);
        }
    }

    if (storage_is_available()) {
        recorder_start();
        ESP_LOGI(TAG, "Recording started");
    }

    /* ---- 16. BOOT button monitor task -------------------------------- */
    xTaskCreate(boot_button_monitor, "boot_btn", 2048, NULL, 1, NULL);

    /* ---- 17. Watchdog (30s timeout) ---------------------------------- */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms     = 30000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic  = true,
    };
    /* Default TWDT is initialized by esp_system; reconfigure it. */
    esp_task_wdt_reconfigure(&wdt_config);

    /* ---- 18. SD monitor task (Core 1) -------------------------------- */
    xTaskCreatePinnedToCore(sd_monitor_task, "sd_monitor", 3072, NULL, 2, NULL, 1);

    /* ---- 19. Health monitor task (Core 1) ----------------------------- */
    xTaskCreatePinnedToCore(health_monitor_task, "health_mon", 3072, NULL, 1, NULL, 1);

    /* ---- Done -------------------------------------------------------- */
    ESP_LOGI(TAG, "ParrotCam v0.1 initialized successfully");
    ESP_LOGI(TAG, "Camera: %s @ %s",
        camera_get_sensor() == CAMERA_SENSOR_OV2640 ? "OV2640" :
        camera_get_sensor() == CAMERA_SENSOR_OV3660 ? "OV3660" : "unknown",
        camera_res_to_str(camera_get_resolution()));
    ESP_LOGI(TAG, "WiFi: %s, IP: %s",
        wifi_get_state() == WIFI_STATE_AP            ? "AP" :
        wifi_get_state() == WIFI_STATE_STA_CONNECTED ? "STA" : "disconnected",
        wifi_get_ip_str());
}
