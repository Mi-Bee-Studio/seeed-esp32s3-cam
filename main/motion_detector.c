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

#include "motion_detector.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "motion";

#define DETECT_W  80
#define DETECT_H  60
#define DETECT_PIXELS (DETECT_W * DETECT_H)

static uint8_t *s_prev_gray = NULL;
static bool s_motion_active = false;
static uint8_t s_last_score = 0;
static bool s_initialized = false;
static SemaphoreHandle_t s_mutex = NULL;

/* Throttle motion state change logs to once per 5 seconds */
static TickType_t s_last_log_tick = 0;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static uint8_t compute_pixel_threshold(uint8_t sensitivity)
{
    uint8_t pixel_thresh = (uint8_t)(255 * (100 - sensitivity) / 100);
    if (pixel_thresh < 8) {
        pixel_thresh = 8;
    }
    return pixel_thresh;
}

static void downsample_to_gray(const uint8_t *src_rgb, uint16_t src_w, uint16_t src_h,
                                uint8_t *dst_gray, uint16_t dst_w, uint16_t dst_h)
{
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return;
    }

    uint16_t block_w = src_w / dst_w;
    uint16_t block_h = src_h / dst_h;
    if (block_w == 0) block_w = 1;
    if (block_h == 0) block_h = 1;

    for (uint16_t y = 0; y < dst_h; y++) {
        for (uint16_t x = 0; x < dst_w; x++) {
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
            uint16_t count = 0;

            uint16_t start_y = y * block_h;
            uint16_t end_y = start_y + block_h;
            uint16_t start_x = x * block_w;
            uint16_t end_x = start_x + block_w;

            if (end_y > src_h) end_y = src_h;
            if (end_x > src_w) end_x = src_w;

            for (uint16_t sy = start_y; sy < end_y; sy++) {
                for (uint16_t sx = start_x; sx < end_x; sx++) {
                    size_t idx = (sy * src_w + sx) * 3;
                    sum_r += src_rgb[idx];
                    sum_g += src_rgb[idx + 1];
                    sum_b += src_rgb[idx + 2];
                    count++;
                }
            }

            if (count > 0) {
                /* Fast grayscale: (R*77 + G*150 + B*29) >> 8 */
                uint32_t gray = ((sum_r / count) * 77 +
                                 (sum_g / count) * 150 +
                                 (sum_b / count) * 29) >> 8;
                if (gray > 255) gray = 255;
                dst_gray[y * dst_w + x] = (uint8_t)gray;
            }
        }
    }
}

static uint8_t compute_motion_score(const uint8_t *prev, const uint8_t *curr,
                                     uint8_t pixel_threshold)
{
    uint32_t changed = 0;

    for (int i = 0; i < DETECT_PIXELS; i++) {
        int diff = (int)curr[i] - (int)prev[i];
        if (diff < 0) diff = -diff;
        if ((uint8_t)diff > pixel_threshold) {
            changed++;
        }
    }

    return (uint8_t)((changed * 100) / DETECT_PIXELS);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t motion_detector_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_prev_gray = (uint8_t *)calloc(DETECT_PIXELS, 1);
    if (s_prev_gray == NULL) {
        ESP_LOGE(TAG, "Failed to allocate grayscale buffer");
        free(s_prev_gray);
        s_prev_gray = NULL;
        return ESP_FAIL;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        free(s_prev_gray);
        s_prev_gray = NULL;
        return ESP_FAIL;
    }

    s_motion_active = false;
    s_last_score = 0;
    s_initialized = true;

    ESP_LOGI(TAG, "Motion detector initialized (%dx%d)", DETECT_W, DETECT_H);
    return ESP_OK;
}

esp_err_t motion_detector_process(
    const uint8_t *jpeg_data, size_t jpeg_len,
    uint8_t sensitivity,
    uint8_t active_interval_sec, uint8_t idle_interval_sec,
    motion_result_t *result)
{
    if (jpeg_data == NULL || jpeg_len == 0 || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Decode JPEG to RGB888 using esp_jpeg */
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = (uint8_t *)jpeg_data,
        .indata_size = (uint32_t)jpeg_len,
        .outbuf = NULL,
        .outbuf_size = 0,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        /* 1/4 downscale during decode: motion detection only needs 80x60 grayscale,
         * so decoding at 1/16 resolution (e.g. SVGA 800x600 -> 200x150) is sufficient.
         * Cuts decode time from ~500ms to ~30ms and decode buffer from ~1.4MB to ~90KB.
         * downsample_to_gray adapts: 200x150 -> 80x60 uses 2x2 block averaging. */
        .out_scale = JPEG_IMAGE_SCALE_1_4,
    };
    esp_jpeg_image_output_t outimg;

    /* First, get image info to allocate decode buffer */
    esp_err_t err = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG get info failed, using idle interval");
        result->motion_detected = false;
        result->motion_score = 0;
        result->current_interval_sec = idle_interval_sec;
        return ESP_FAIL;
    }

    /* Allocate decode buffer in PSRAM (large) */
    size_t decode_buf_size = outimg.width * outimg.height * 3;
    uint8_t *decode_buf = (uint8_t *)heap_caps_malloc(decode_buf_size, MALLOC_CAP_SPIRAM);
    if (decode_buf == NULL) {
        ESP_LOGW(TAG, "Failed to allocate decode buffer (%zu bytes), using idle interval", decode_buf_size);
        result->motion_detected = false;
        result->motion_score = 0;
        result->current_interval_sec = idle_interval_sec;
        return ESP_FAIL;
    }

    jpeg_cfg.outbuf = decode_buf;
    jpeg_cfg.outbuf_size = decode_buf_size;

    err = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG decode failed, using idle interval");
        heap_caps_free(decode_buf);
        result->motion_detected = false;
        result->motion_score = 0;
        result->current_interval_sec = idle_interval_sec;
        return ESP_FAIL;
    }

    /* Downsample to 80x60 grayscale */
    uint8_t temp_gray[DETECT_PIXELS];
    downsample_to_gray(decode_buf, outimg.width, outimg.height,
                       temp_gray, DETECT_W, DETECT_H);

    heap_caps_free(decode_buf);

    /* Compute motion score against previous frame */
    uint8_t pixel_thresh = compute_pixel_threshold(sensitivity);
    uint8_t score = compute_motion_score(s_prev_gray, temp_gray, pixel_thresh);

    /* Hysteresis */
    uint8_t trigger_threshold = sensitivity / 2;
    uint8_t release_threshold = sensitivity / 4;

    if (trigger_threshold == 0) trigger_threshold = 1;
    if (release_threshold == 0) release_threshold = 1;

    bool motion_now = s_motion_active;
    if (score >= trigger_threshold) {
        motion_now = true;
    } else if (score < release_threshold) {
        motion_now = false;
    }

    /* Throttled logging for state changes */
    if (motion_now != s_motion_active) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_log_tick) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "Motion %s (score=%d)", motion_now ? "detected" : "cleared", score);
            s_last_log_tick = now;
        }
    }

    s_motion_active = motion_now;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_score = score;
    xSemaphoreGive(s_mutex);

    /* Swap buffers: current becomes previous */
    memcpy(s_prev_gray, temp_gray, DETECT_PIXELS);

    result->motion_detected = s_motion_active;
    result->motion_score = score;
    result->current_interval_sec = s_motion_active ? active_interval_sec : idle_interval_sec;

    return ESP_OK;
}

uint8_t motion_detector_get_score(void)
{
    uint8_t score = 0;
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        score = s_last_score;
        xSemaphoreGive(s_mutex);
    }
    return score;
}

void motion_detector_deinit(void)
{
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    if (s_prev_gray != NULL) {
        free(s_prev_gray);
        s_prev_gray = NULL;
    }

    s_motion_active = false;
    s_last_score = 0;
    s_initialized = false;
    s_last_log_tick = 0;
}
