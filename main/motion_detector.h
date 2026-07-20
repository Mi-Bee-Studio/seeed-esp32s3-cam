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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/** Motion detection result */
typedef struct {
    bool motion_detected;         /* true if score above trigger threshold */
    uint8_t motion_score;         /* 0-100, percentage of changed pixels */
    uint16_t current_interval_sec; /* computed capture interval for this cycle */
} motion_result_t;

/** Initialize motion detector (allocates internal buffers) */
esp_err_t motion_detector_init(void);

/**
 * Process a JPEG frame and compute motion score.
 * @param jpeg_data  JPEG buffer from camera_capture()
 * @param jpeg_len   JPEG buffer length
 * @param sensitivity  0-100 (higher = more sensitive, lower threshold for change)
 * @param active_interval_sec  interval when motion detected (seconds)
 * @param idle_interval_sec    interval when no motion (seconds)
 * @param result    Output: motion score and computed interval
 * @return ESP_OK on success
 */
esp_err_t motion_detector_process(
    const uint8_t *jpeg_data, size_t jpeg_len,
    uint8_t sensitivity,
    uint8_t active_interval_sec, uint8_t idle_interval_sec,
    motion_result_t *result);

/** Get current motion score (0-100), thread-safe read */
uint8_t motion_detector_get_score(void);

/**
 * @brief Get the dynamic capture interval computed from the latest motion
 *        detection cycle (active_interval when motion seen, idle_interval
 *        otherwise). Returns 0 if motion detection hasn't run yet.
 *
 * The async motion task (motion_detector_start) updates this continuously;
 * the recorder reads it instead of running motion detection inline.
 */
uint16_t motion_detector_get_dynamic_interval(void);

/**
 * @brief Start the async motion-detection task.
 *
 * Subscribes to the frame broadcaster (FRAMESUB_MOTION) on Core 1, decodes
 * each received JPEG at 1/4 scale, and computes the motion score off the
 * recorder's hot path. Results are read via motion_detector_get_score() /
 * motion_detector_get_dynamic_interval(). Safe to call when not in dynamic
 * timelapse mode — the task just consumes frames and discards results.
 *
 * Reads sensitivity/interval config live from config_get() each cycle, so
 * runtime config changes take effect without restart.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already running or
 *         motion_detector_init() not called.
 */
esp_err_t motion_detector_start(void);

/**
 * @brief Stop the async motion-detection task and release the subscriber.
 *        Safe to call multiple times; no-op if not running.
 */
void motion_detector_stop(void);

/** Free motion detector resources */
void motion_detector_deinit(void);
