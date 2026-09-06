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
} motion_result_t;

/** Initialize motion detector (allocates internal buffers) */
esp_err_t motion_detector_init(void);

/**
 * Process a JPEG frame and compute motion score.
 * @param jpeg_data  JPEG buffer from camera_capture()
 * @param jpeg_len   JPEG buffer length
 * @param sensitivity  0-100 (higher = more sensitive, lower threshold for change)
 * @param result    Output: motion score and active state
 * @return ESP_OK on success
 *
 * 契约 §3.2：interval 计算已移出（延时摄影的动态间隔由 video_recorder
 * 基于 min/max/decay 模型自行衰减，本模块只提供运动状态信号）。
 */
esp_err_t motion_detector_process(
    const uint8_t *jpeg_data, size_t jpeg_len,
    uint8_t sensitivity,
    motion_result_t *result);

/** Get current motion score (0-100), thread-safe read */
uint8_t motion_detector_get_score(void);

/**
 * @brief 运动状态信号（家族延时动态模型的运动钩子）。
 *
 * 返回最近一次分析周期是否处于运动状态（滞回判定）。video_recorder 的
 * 动态延时分支以此驱动 min/max/decay 间隔衰减——选择该 getter 而非事件
 * 回调是最小侵入钩子：无需新任务间耦合，读侧最多滞后一个分析周期。
 */
bool motion_detector_is_active(void);

/**
 * @brief Start the async motion-detection task.
 *
 * Subscribes to the frame broadcaster (FRAMESUB_MOTION) on Core 1, decodes
 * each received JPEG at 1/4 scale, and computes the motion score off the
 * recorder's hot path. Results are read via motion_detector_get_score() /
 * motion_detector_is_active(). Safe to call when not in dynamic timelapse
 * mode — the task just consumes frames and discards results.
 *
 * Reads sensitivity config live from config_get() each cycle, so runtime
 * config changes take effect without restart. motion_enabled=0 时检测照跑
 * （供动态延时取信号/诊断），但 WS 触发事件被抑制（契约 §3.2 触发路径门控）。
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
