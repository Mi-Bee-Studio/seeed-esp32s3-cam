/*
 * Copyright (C) 2024 ParrotCam Authors
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

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/** Camera sensor types */
typedef enum {
    /** 未知传感器 */
    CAMERA_SENSOR_UNKNOWN = 0,
    /** OV2640 摄像头传感器 */
    CAMERA_SENSOR_OV2640,
    /** OV3660 摄像头传感器 */
    CAMERA_SENSOR_OV3660,
} camera_sensor_t;

/** Resolution options */
typedef enum {
    /** VGA 分辨率 (640x480) */
    CAMERA_RES_VGA  = 0,   /* 640x480  */
    /** SVGA 分辨率 (800x600) */
    CAMERA_RES_SVGA = 1,   /* 800x600  */
    /** XGA 分辨率 (1024x768) */
    CAMERA_RES_XGA  = 2,   /* 1024x768 */
} camera_res_t;

/** Captured frame descriptor (buffer owned by esp_camera driver, in PSRAM). */
typedef struct {
    /** 帧数据缓冲区指针 */
    uint8_t *buf;
    /** 帧数据长度（字节） */
    size_t   len;
} camera_frame_t;

/** @brief 初始化摄像头
 *
 * 设定分辨率、帧率和 JPEG 质量，自动检测传感器型号。
 * @param res 分辨率
 * @param fps 帧率
 * @param quality JPEG 质量（0-63，越小质量越好）
 * @return ESP_OK 成功，其他值失败
 */
/**
 * Initialize the camera with given resolution, JPEG quality (0-63, lower=better).
 * Auto-detects sensor (OV2640 / OV3660).
 */
esp_err_t camera_init(camera_res_t res, uint8_t fps, uint8_t quality);

/** @brief 获取当前检测到的摄像头传感器类型
 * @return 传感器类型枚举值
 */
/** Return the detected sensor type. */
camera_sensor_t camera_get_sensor(void);

/** @brief 捕获一帧 JPEG 图像
 *
 * 调用后必须调用 camera_return_fb() 释放帧缓冲区。
 * @param frame 输出帧描述符
 * @return ESP_OK 成功，ESP_FAIL 捕获失败
 */
/** Capture a single JPEG frame. Caller must call camera_return_fb() when done. */
esp_err_t camera_capture(camera_frame_t *frame);

/** @brief 归还帧缓冲区给摄像头驱动
 *
 * 在 camera_capture() 之后必须调用此函数释放资源。
 * @param frame 之前捕获的帧描述符
 */
/** Return frame buffer to the driver (required after camera_capture). */
void camera_return_fb(camera_frame_t *frame);

/** @brief 动态切换摄像头分辨率
 * @param res 目标分辨率
 * @return ESP_OK 成功，ESP_FAIL 失败
 */
/** Change resolution on the fly. */
esp_err_t camera_set_resolution(camera_res_t res);

/** @brief 获取当前分辨率设置
 * @return 当前分辨率枚举值
 */
/** Get current resolution setting. */
camera_res_t camera_get_resolution(void);

/** @brief 将分辨率枚举值转换为可读字符串
 * @param res 分辨率枚举值
 * @return 分辨率名称字符串（如 "VGA"、"SVGA"、"XGA"）
 */
/** Human-readable resolution name. */
const char *camera_res_to_str(camera_res_t res);
