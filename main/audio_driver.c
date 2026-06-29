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
 *   I2S PDM → 8 kHz / 16-bit / mono PCM (DSR_16S, 1.024 MHz PDM clock)
 *         → DC offset removal (IIR, α≈0.999)
 *         → Spectral noise subtraction (256-pt FFT, Wiener gain)
 *         → Software gain (4×) + hard clamp
 *         → Noise gate with hysteresis (open@300, close@150)
 *         → G.711 μ-law encoding
 *
 * No dynamic allocation in the capture path — all buffers are static.
 * I2S read uses a timeout to avoid blocking indefinitely.
 */

#include "audio_driver.h"
#include "audio_broadcaster.h"
#include "g711_codec.h"
#include "audio_ns.h"
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

/* ── Sample rate configuration ──
 * 8 kHz direct capture: ESP32-S3 PDM hardware applies proper
 * anti-aliasing filter, eliminating software decimation artifacts. */
#define PCM_SAMPLE_RATE     8000        /* Hardware PDM decimation to 8 kHz */
#define PCM_BIT_WIDTH       16          /* 16-bit signed samples */
#define PCM_MONO            1           /* Single channel */

/* ── Software gain + DC offset removal + noise gate ──
 * PDM MEMS mic outputs very low amplitude; apply moderate 4× gain.
 * DC offset tracked and removed to eliminate low-frequency noise.
 * Noise gate with hysteresis suppresses background hiss silently. */
#define AUDIO_GAIN_SHIFT      2     /* Shift left by 2 bits = 4× gain */
#define NOISE_GATE_OPEN       300   /* Avg |sample| to OPEN the gate */
#define NOISE_GATE_CLOSE      150   /* Avg |sample| to CLOSE the gate (hysteresis) */

/* ── Buffer sizes ──
 * One I2S read = 160 samples = 20 ms at 8 kHz = exactly one G.711 frame.
 * No accumulation needed — one read produces one publishable frame.
 */
#define DMA_BUF_SAMPLES     160     /* 160 int16_t samples per I2S read = 20 ms */
#define DMA_BUF_SIZE        (DMA_BUF_SAMPLES * sizeof(int16_t))  /* 320 bytes */
#define G711_FRAME_BYTES    G711_FRAME_SIZE  /* 160 from audio_common.h */

/* Non-blocking I2S read timeout (ms) — drop frame if exceeded */
#define I2S_READ_TIMEOUT_MS 50

/* ── Static state ── */
static i2s_chan_handle_t s_rx_handle = NULL;
static TaskHandle_t s_task_handle = NULL;
static volatile bool s_running = false;

/* Static DMA read buffer (no dynamic allocation in capture path) */
static int16_t s_dma_buf[DMA_BUF_SAMPLES];

/* Static G.711 frame buffer */
static uint8_t s_g711_frame[G711_FRAME_BYTES];

/* ── Forward declarations ── */
static void audio_capture_task(void *arg);

/* ===================================================================
 * FreeRTOS capture task (Core 0, priority 4)
 *
 * Reads PCM from I2S at 8 kHz, removes DC offset, applies spectral
 * noise subtraction, applies gain, encodes to μ-law, and publishes
 * 20 ms frames via the audio broadcaster.
 * =================================================================== */
static void audio_capture_task(void *arg)
{
    size_t bytes_read = 0;
    uint64_t frame_start_timestamp = 0;

    ESP_LOGI(TAG, "Audio capture task started");

    int32_t dc_offset = 0;   /* Running DC offset estimate (IIR) */
    bool gate_open = false;  /* Noise gate state (hysteresis) */

    while (s_running) {
        uint64_t frame_ts = esp_timer_get_time();

        /* Non-blocking I2S read — 160 samples = one complete G.711 frame */
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_handle, s_dma_buf, DMA_BUF_SIZE,
                                         &bytes_read, pdMS_TO_TICKS(I2S_READ_TIMEOUT_MS));
        if (ret != ESP_OK || bytes_read < sizeof(int16_t)) {
            ESP_LOGW(TAG, "I2S read timeout or short read (ret=0x%x, bytes=%zu)",
                     ret, bytes_read);
            continue;
        }
        /* Compute frame energy (sum of |sample|) for noise gate */
        size_t num_samples = bytes_read / sizeof(int16_t);
        if (num_samples > DMA_BUF_SAMPLES) num_samples = DMA_BUF_SAMPLES;
        for (size_t i = num_samples; i < DMA_BUF_SAMPLES; i++) {
            s_dma_buf[i] = 0;  /* pad short reads with silence */
        }

        int32_t abs_sum = 0;
        for (size_t i = 0; i < DMA_BUF_SAMPLES; i++) {
            int32_t s = s_dma_buf[i];
            abs_sum += (s < 0) ? -s : s;
        }
        int32_t avg_abs = abs_sum / DMA_BUF_SAMPLES;

        /* Noise gate with hysteresis: prevents rapid on/off cycling
         * Open when signal rises above OPEN threshold,
         * close only when it drops below CLOSE threshold. */
        if (!gate_open) {
            if (avg_abs < NOISE_GATE_OPEN) {
                memset(s_g711_frame, 0xFF, G711_FRAME_BYTES);
                audio_bcast_publish(s_g711_frame, G711_FRAME_BYTES, frame_ts);
                continue;
            }
            gate_open = true;
        } else {
            if (avg_abs < NOISE_GATE_CLOSE) {
                gate_open = false;
                memset(s_g711_frame, 0xFF, G711_FRAME_BYTES);
                audio_bcast_publish(s_g711_frame, G711_FRAME_BYTES, frame_ts);
                continue;
            }
        }

        /* Step 1: DC offset removal (in-place) */
        for (size_t i = 0; i < DMA_BUF_SAMPLES; i++) {
            int32_t sample = s_dma_buf[i];
            dc_offset += (sample - dc_offset) >> 10;
            s_dma_buf[i] = (int16_t)(sample - dc_offset);
        }

        /* Step 2: Spectral noise subtraction (256-pt FFT, in-place) */
        audio_ns_process(s_dma_buf, DMA_BUF_SAMPLES);

        /* Step 3: Gain + clamp + μ-law encode */
        for (size_t i = 0; i < DMA_BUF_SAMPLES; i++) {
            int32_t val = (int32_t)s_dma_buf[i] << AUDIO_GAIN_SHIFT;
            if (val > 32767)  val = 32767;
            if (val < -32768) val = -32768;
            s_g711_frame[i] = (uint8_t)(linear16_to_ulaw((int16_t)val) & 0xFF);
        }
        /* Publish complete frame */
        audio_bcast_publish(s_g711_frame, G711_FRAME_BYTES, frame_ts);
    }

    ESP_LOGI(TAG, "Audio capture task stopped");
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

    /* 1. Allocate I2S RX channel — DMA matched to 20 ms frame */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = DMA_BUF_SAMPLES;  /* 160 frames = exact 20 ms */
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel: 0x%x", ret);
        return ret;
    }

    /* 2. Configure PDM RX — optimized for voice quality */
    i2s_pdm_rx_clk_config_t clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(PCM_SAMPLE_RATE);
    clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;  /* 1.024 MHz PDM clock (+6 dB SNR) */

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = clk_cfg,
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

    /* NOTE: channel_enable() deferred to audio_driver_start() to avoid
     * DMA buffer overflow when the capture task starts late. */

    ESP_LOGI(TAG, "Audio driver initialized (I2S PDM RX, %d Hz, %d-bit, mono)",
             PCM_SAMPLE_RATE, PCM_BIT_WIDTH);
    audio_ns_init();
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

    /* Enable I2S channel right before starting the reader task.
     * This prevents DMA buffers from overflowing during the gap
     * between init and start (can be 30+ seconds when WiFi is slow). */
    esp_err_t ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: 0x%x", ret);
        return ret;
    }

    s_running = true;

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
