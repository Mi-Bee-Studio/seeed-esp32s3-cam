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
 *
 * ── I2S PDM Microphone Capture Driver ──
 *
 * Captures audio from the XIAO ESP32-S3 Sense onboard MEMS microphone
 * via I2S PDM RX (GPIO41=DATA, GPIO42=CLK).  The pipeline is:
 *
 *   I2S PDM → 16 kHz / 16-bit / mono PCM
 *         → 2:1 decimation (every 2nd sample) → 8 kHz
 *         → G.711 mu-law encoding
 *         → 20 ms frame (160 bytes) → audio_bcast_publish()
 *
 * No dynamic allocation in the capture path — all buffers are static.
 * I2S read uses a timeout to avoid blocking indefinitely.
 */

#include "audio_driver.h"
#include "audio_broadcaster.h"
#include "g711_codec.h"
#include "audio_common.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_pdm.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "audio";

/* ── Hardware pin mapping (XIAO ESP32-S3 Sense onboard MEMS mic) ── */
#define I2S_PDM_CLK_PIN     GPIO_NUM_42
#define I2S_PDM_DATA_PIN    GPIO_NUM_41

/* ── Sample rate configuration ── */
#define PCM_SAMPLE_RATE     16000       /* I2S captures PDM decoded to 16 kHz PCM */
#define PCM_BIT_WIDTH       16          /* 16-bit signed samples */
#define PCM_MONO            1           /* Single channel */

/* ── Buffer sizes ──
 * I2S DMA read:  320 bytes = 160 samples = 10 ms at 16 kHz / 16-bit mono
 * Decimation (2:1):  160 samples → 80 samples → 80 bytes G.711
 * Frame accumulation:  2 reads × 80 bytes = 160 bytes = 20 ms frame
 */
#define DMA_BUF_SAMPLES     160     /* 160 int16_t samples per I2S read */
#define DMA_BUF_SIZE        (DMA_BUF_SAMPLES * sizeof(int16_t))  /* 320 bytes */
#define DECIMATED_SIZE      80      /* After 2:1 decimation: 80 samples */
#define G711_FRAME_BYTES    G711_FRAME_SIZE  /* 160 from audio_common.h */

/* Non-blocking I2S read timeout (ms) — drop frame if exceeded */
#define I2S_READ_TIMEOUT_MS 50

/* ── Static state ── */
static i2s_chan_handle_t s_rx_handle = NULL;
static TaskHandle_t s_task_handle = NULL;
static volatile bool s_running = false;

/* Static DMA read buffer (no dynamic allocation in capture path) */
static int16_t s_dma_buf[DMA_BUF_SAMPLES];

/* Static G.711 frame accumulation buffer */
static uint8_t s_g711_frame[G711_FRAME_BYTES];

/* Current write offset in the accumulation buffer */
static size_t s_g711_offset = 0;

/* ── Forward declarations ── */
static void audio_capture_task(void *arg);

/* ===================================================================
 * FreeRTOS capture task (Core 0, priority 4)
 *
 * Reads PCM from I2S in a loop, decimates 2:1, encodes to mu-law,
 * and publishes 20 ms frames via the audio broadcaster.
 * =================================================================== */
static void audio_capture_task(void *arg)
{
    size_t bytes_read = 0;
    uint64_t frame_start_timestamp = 0;

    ESP_LOGI(TAG, "Audio capture task started");

    while (s_running) {
        /* Beginning of a new frame — snapshot timestamp */
        if (s_g711_offset == 0) {
            frame_start_timestamp = esp_timer_get_time();
        }

        /* Non-blocking I2S read with timeout */
        esp_err_t ret = i2s_channel_read(s_rx_handle, s_dma_buf, DMA_BUF_SIZE,
                                         &bytes_read, pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS));
        if (ret != ESP_OK || bytes_read < sizeof(int16_t)) {
            ESP_LOGW(TAG, "I2S read timeout or error (ret=0x%x, bytes=%zu)",
                     ret, bytes_read);
            continue;
        }

        /* Number of valid 16-bit PCM samples */
        size_t num_samples = bytes_read / sizeof(int16_t);

        /* Decimate 2:1 (take every 2nd sample) and encode to G.711 μ-law */
        for (size_t i = 0; i < num_samples; i += 2) {
            int16_t encoded = linear16_to_ulaw(s_dma_buf[i]);
            s_g711_frame[s_g711_offset++] = (uint8_t)(encoded & 0xFF);

            /* Frame complete — publish to all subscribers */
            if (s_g711_offset >= G711_FRAME_BYTES) {
                audio_bcast_publish(s_g711_frame, G711_FRAME_BYTES,
                                    frame_start_timestamp);
                s_g711_offset = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Audio capture task stopped");
    s_g711_offset = 0;
    vTaskDelete(NULL);
}

/* ===================================================================
 * Public API
 * =================================================================== */

esp_err_t audio_driver_init(void)
{
    if (s_rx_handle != NULL) {
        ESP_LOGW(TAG, "Audio driver already initialized");
        return ESP_OK;
    }

    /* 1. Allocate I2S RX channel (I2S_NUM_0, master role) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel: 0x%x", ret);
        return ret;
    }

    /* 2. Configure PDM RX mode — hardware PDM-to-PCM conversion */
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PCM_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_PDM_CLK_PIN,
            .din = I2S_PDM_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ret = i2s_channel_init_pdm_rx_mode(s_rx_handle, &pdm_rx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init PDM RX mode: 0x%x", ret);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    /* 3. Enable channel (starts I2S hardware) */
    ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: 0x%x", ret);
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return ret;
    }

    s_g711_offset = 0;
    ESP_LOGI(TAG, "Audio driver initialized (I2S PDM RX, %d Hz, %d-bit, mono)",
             PCM_SAMPLE_RATE, PCM_BIT_WIDTH);
    return ESP_OK;
}

esp_err_t audio_driver_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Audio capture already running");
        return ESP_OK;
    }
    if (s_rx_handle == NULL) {
        ESP_LOGE(TAG, "Audio driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;
    s_g711_offset = 0;

    BaseType_t created = xTaskCreatePinnedToCore(
        audio_capture_task,
        "audio_capture",
        4096,       /* stack depth in words */
        NULL,       /* task parameters */
        4,          /* priority */
        &s_task_handle,
        0);         /* Core 0 */

    if (created != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create audio capture task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

esp_err_t audio_driver_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;

    /* Unblock the task from any blocking I2S read */
    if (s_task_handle != NULL) {
        xTaskAbortDelay(s_task_handle);
        s_task_handle = NULL;
    }

    ESP_LOGI(TAG, "Audio capture stopped");
    return ESP_OK;
}

esp_err_t audio_driver_deinit(void)
{
    if (s_rx_handle == NULL) {
        return ESP_OK;
    }

    /* Ensure capture is stopped */
    audio_driver_stop();

    /* Short delay to let the task exit if it hasn't already */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Disable and delete I2S channel */
    i2s_channel_disable(s_rx_handle);
    i2s_del_channel(s_rx_handle);
    s_rx_handle = NULL;

    ESP_LOGI(TAG, "Audio driver deinitialized");
    return ESP_OK;
}

bool audio_driver_is_running(void)
{
    return s_running;
}
