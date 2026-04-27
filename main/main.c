/*
 * MiBeeHomeCam v0.1 — Main application entry point
 *
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

/**
 * @brief 录像分段完成回调函数
 *
 * 当 video_recorder 完成一个 AVI 分段文件时调用此回调。
 * 将完成的文件路径加入 NAS 上传队列，并触发存储空间清理（循环删除旧文件）,
 * 确保存储空间不会耗尽。
 *
 * @param filepath 完成的分段文件路径
 * @param size     文件大小（字节）
 */
/* ------------------------------------------------------------------ */
/*  Recording segment callback — enqueue for NAS upload               */
/* ------------------------------------------------------------------ */

static void on_segment_complete(const char *filepath, size_t size)
{
    ESP_LOGI(TAG, "Segment complete: %s (%zu bytes)", filepath, size);

    storage_register_file(filepath, size);
    nas_uploader_enqueue(filepath);
    storage_cleanup();
}

/**
 * @brief 初始化 SPIFFS 文件系统
 *
 * 挂载 SPIFFS 分区用于存放 Web 管理界面的静态资源文件（HTML 页面）。
 * 配置最大打开文件数为 8，挂载失败时自动格式化分区。
 * 挂载点为 /spiffs，Web 服务器从此路径提供 UI 页面。
 */
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

/**
 * @brief BOOT 按键监控任务
 *
 * FreeRTOS 任务，持续监测 BOOT 按键（GPIO0）状态。
 * 当检测到按键持续按下超过 5 秒时，执行恢复出厂设置：
 * 重置配置 → LED 显示错误状态 → 延时 2 秒 → 重启设备。
 *
 * @param arg 未使用的任务参数
 */
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

/**
 * @brief SD 卡热插拔监控任务
 *
 * FreeRTOS 任务，每 10 秒轮询一次 SD 卡状态，实现热插拔检测：
 * - 拔出检测：发现 SD 卡移除后立即停止录像，记录之前录像状态用于自动恢复
 * - 插入检测：发现 SD 卡重新插入后自动重新挂载、清理文件，
 *   若之前正在录像则自动恢复录像
 *
 * @param arg 未使用的任务参数
 */
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

/**
 * @brief 系统健康状态监控任务
 *
 * FreeRTOS 任务，每 60 秒采集并记录一次系统资源使用情况：
 * - 可用堆内存（heap）
 * - 可用 PSRAM（外部 SPIRAM）
 * - 录像任务和 NAS 上传任务的栈高水位线（stack HWM）
 *
 * 当可用堆内存低于 20KB 时输出 CRITICAL 告警，
 * 当可用 PSRAM 低于 500KB 时输出 WARNING 告警。
 *
 * @param arg 未使用的任务参数
 */
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

/**
 * @brief 应用程序主入口函数
 *
 * ESP-IDF 固件入口点，执行 19 步有序初始化流程：
 *   1.  NVS 非易失性存储初始化
 *   2.  配置管理器初始化（从 NVS 加载配置）
 *   3.  状态 LED 初始化
 *   4.  SPIFFS 文件系统挂载（Web UI 资源）
 *   5.  摄像头初始化（GPIO10 作为 XCLK，必须在 SD 卡之前）
 *   6.  SD 卡存储初始化
 *   6a. 启动时清理不完整的 AVI 文件
 *   7.  从 SD 卡配置文件覆盖 NVS 配置
 *   8.  WiFi 初始化（AP 或 STA 模式）
 */
/* ------------------------------------------------------------------ */
/*  app_main                                                           */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "MiBeeHomeCam v0.1 starting...");
    ESP_LOGI(TAG, "Free heap: %lu  Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- 1. NVS flash ------------------------------------------------ */
    /* 第1步：初始化NVS非易失性存储，损坏时自动擦除重建 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- 2. Config manager ------------------------------------------- */
    /* 第2步：初始化配置管理器，从NVS加载保存的配置参数 */
    config_init();
    cam_config_t *cfg = config_get();

    /* ---- 3. Status LED ----------------------------------------------- */
    /* 第3步：初始化状态LED，显示启动中状态 */
    led_init();
    led_set_status(LED_STARTING);

    /* ---- 4. SPIFFS for Web UI ---------------------------------------- */
    /* 第4步：挂载SPIFFS文件系统，用于存放Web管理界面页面 */
    init_spiffs();

    /* ---- 5. Camera (GPIO10 as XCLK) --------------------------------- */
    /* 第5步：初始化摄像头（必须在SD卡之前，避免GDMA通道冲突） */
    {
        camera_res_t res = CAMERA_RES_VGA; /* Force VGA for initial bring-up */
        uint8_t quality = cfg->jpeg_quality;
        if (quality < 10) quality = 10; // Floor to prevent oversized frames
        switch (cfg->resolution) {
            case 0:  res = CAMERA_RES_VGA;  break;
            case 1:  res = CAMERA_RES_SVGA; break;
            case 2:  res = CAMERA_RES_XGA;  break;
            default: res = CAMERA_RES_VGA; break;
        }
        camera_init(res, cfg->fps, quality);
    }

    /* ---- 6. SD card storage (SPI mode: CS=21, SCK=7, MOSI=9, MISO=8) --- */
    /* 第6步：初始化SD卡存储管理器（在摄像头之后，避免GDMA通道冲突） */
    storage_init();

    /* ---- 6a. Boot-time cleanup of incomplete AVI files --------------- */
    /* 第6a步：清理上次异常关机遗留的不完整AVI文件 */
    if (storage_is_available()) {
        recorder_cleanup_incomplete();
    }

    /* ---- 7. Config SD override --------------------------------------- */
    /* 第7步：从SD卡的wifi.txt/nas.txt文件读取配置，覆盖NVS中的值 */
    config_load_from_sd();
    cfg = config_get();

    /* ---- 8. WiFi ----------------------------------------------------- */
    /* 第8步：初始化WiFi，根据配置启动AP热点或STA客户端模式 */
    wifi_init();

    /* ---- 9. Time sync (only if STA connected) ----------------------- */

    /* ---- 9. Time sync (only if STA connected) ----------------------- */
    /* 第9步：仅在STA已连接时初始化SNTP时间同步 */
    if (wifi_get_state() == WIFI_STATE_STA_CONNECTED) {
        time_sync_init();
    }

    /* ---- 10. NAS uploader -------------------------------------------- */
    /* 第10步：初始化NAS上传调度器，准备FTP/WebDAV上传队列 */
    nas_uploader_init();

    /* ---- 11. Video recorder ------------------------------------------ */
    /* 第11步：初始化视频录像引擎，注册分段完成回调 */
    recorder_init();
    recorder_set_segment_cb(on_segment_complete);

    /* ---- 12. MJPEG streamer ------------------------------------------ */
    /* 第12步：初始化MJPEG实时视频流服务 */
    mjpeg_streamer_init();

    /* ---- 13. Web server + MJPEG registration ------------------------- */
    /* 第13步：启动Web服务器(端口80)并注册MJPEG流端点 */
    web_server_start(80);

    /* ---- 14. LED reflects WiFi state --------------------------------- */
    /* 第14步：根据当前WiFi状态更新LED显示模式 */
    if (wifi_get_state() == WIFI_STATE_AP) {
        led_set_status(LED_AP_MODE);
    } else {
        led_set_status(LED_WIFI_CONNECTING);
    }

    /* ---- 15. Wait for STA connection, then start recording ----------- */
    /* 第15步：STA模式下等待最多30秒连接WiFi，成功后同步时间并启动录像 */
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
    /* 第16步：创建BOOT按键监控任务，长按5秒恢复出厂设置 */
    xTaskCreate(boot_button_monitor, "boot_btn", 2048, NULL, 1, NULL);

    /* ---- 17. Watchdog (30s timeout) ---------------------------------- */
    /* 第17步：配置任务看门狗，30秒超时触发panic重启 */
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms     = 30000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic  = true,
    };
    /* Default TWDT is initialized by esp_system; reconfigure it. */
    esp_task_wdt_reconfigure(&wdt_config);

    /* ---- 18. SD monitor task (Core 1) -------------------------------- */
    /* 第18步：创建SD卡热插拔监控任务，绑定到Core 1，优先级2 */
    xTaskCreatePinnedToCore(sd_monitor_task, "sd_monitor", 4096, NULL, 2, NULL, 1);

    /* ---- 19. Health monitor task (Core 1) ----------------------------- */
    /* 第19步：创建健康状态监控任务，绑定到Core 1，优先级1 */
    xTaskCreatePinnedToCore(health_monitor_task, "health_mon", 4096, NULL, 1, NULL, 1);

    /* ---- Done -------------------------------------------------------- */
    /* 初始化完成，打印摄像头型号、分辨率和WiFi连接信息 */
    ESP_LOGI(TAG, "MiBeeHomeCam v0.1 initialized successfully");
    ESP_LOGI(TAG, "Camera: %s @ %s",
        camera_get_sensor() == CAMERA_SENSOR_OV2640 ? "OV2640" :
        camera_get_sensor() == CAMERA_SENSOR_OV3660 ? "OV3660" : "unknown",
        camera_res_to_str(camera_get_resolution()));
    ESP_LOGI(TAG, "WiFi: %s, IP: %s",
        wifi_get_state() == WIFI_STATE_AP            ? "AP" :
        wifi_get_state() == WIFI_STATE_STA_CONNECTED ? "STA" : "disconnected",
        wifi_get_ip_str());
}
