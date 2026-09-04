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
    /** OV5640 摄像头传感器 */
    CAMERA_SENSOR_OV5640,
} camera_sensor_t;

/** Resolution options */
typedef enum {
    /** VGA 分辨率 (640x480) */
    CAMERA_RES_VGA  = 0,   /* 640x480  */
    /** SVGA 分辨率 (800x600) */
    CAMERA_RES_SVGA = 1,   /* 800x600  */
    /** XGA 分辨率 (1024x768) */
    CAMERA_RES_XGA  = 2,   /* 1024x768  */
    /** HD 分辨率 (1280x720) */
    CAMERA_RES_HD   = 3,   /* 1280x720  */
    /** SXGA 分辨率 (1280x1024) */
    CAMERA_RES_SXGA = 4,   /* 1280x1024 */
    /** UXGA 分辨率 (1600x1200) */
    CAMERA_RES_UXGA = 5,   /* 1600x1200 */
    /** FHD 分辨率 (1920x1080) */
    CAMERA_RES_FHD  = 6,   /* 1920x1080 */
    /** QXGA 分辨率 (2048x1536) */
    CAMERA_RES_QXGA = 7,   /* 2048x1536 */
} camera_res_t;

/* ── 分辨率三层上限（2026-09-04 家族统一，PIT-021 附录）──────────────
 * effective = min(传感器上限, 板级实测上限, 运行时 fb 预算)，三层各司其职：
 *  1. sensor  — esp32-camera 自动检测（camera_sensor_info_t.max_size），
 *               换传感器（含与文档不符的实戴型号）候选表自适应收缩；
 *  2. board   — 本板实测常数（唯一手工数字，禁止沿用姐妹板数值）：
 *               2026-09-04 本机 OV5640 推流+录像负载 90s 实测，FHD 峰值
 *               97.5°C / QXGA 100.5°C 超 S3 规格 85°C（PIT-016）剔除，
 *               UXGA 峰值 93.5°C 且流稳定 → 定为板上限（热限）；
 *  3. memory  — 运行时 fb 预算校验（宽*高/5*fb_count + floor ≤ 可用堆），
 *               只能收紧（防御 PSRAM 退化态），不放宽超过实测常数。
 * /api/camera 下发 res_cap_source 报告被哪一层钳制（诊断用）。 */
#define CAMERA_RES_BOARD_MAX CAMERA_RES_UXGA

/* JPEG 画质边界（数值越小画质越高、帧越大）。esp32-camera 的 JPEG 帧缓冲
 * 按 宽*高/5 分配（假设最高 1:5 压缩）；q<10 在细节丰富的场景会超预算，
 * 产生截断帧。实测 UXGA q12 ≈ 193KB < 384KB fb 上限。 */
#define CAMERA_QUALITY_MIN 10
#define CAMERA_QUALITY_MAX 63

/** Captured frame descriptor (buffer owned by esp_camera driver, in PSRAM). */
typedef struct {
    /** 帧数据缓冲区指针 */
    uint8_t *buf;
    /** 帧数据长度（字节） */
    size_t   len;
    /** Internal driver handle — do not modify */
    void    *_fb;
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
/** @brief 动态切换摄像头分辨率
 * @param res 目标分辨率
 * @return ESP_OK 成功，ESP_FAIL 失败
 */
esp_err_t camera_set_resolution(camera_res_t res);

/** @brief 设置摄像头垂直翻转和水平镜像
 * @param vflip 垂直翻转（true=翻转）
 * @param hmirror 水平镜像（true=镜像）
 * @return ESP_OK 成功
 */
esp_err_t camera_set_flip(bool vflip, bool hmirror);

/** @brief 设置日夜模式（彩色/黑白）
 * @param mode 0=彩色, 1=黑白, 2=自动(预留,当前等同0)
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_FAIL 失败
 */
esp_err_t camera_set_day_night(uint8_t mode);

/** @brief 获取当前分辨率设置
 * @return 当前分辨率枚举值
 */
/** Get current resolution setting. */
camera_res_t camera_get_resolution(void);

/** @brief 获取指定传感器支持的最大分辨率
 * @param sensor 传感器类型
 * @return 该传感器支持的最大分辨率枚举值
 */
camera_res_t camera_get_max_resolution(camera_sensor_t sensor);

/** @brief 获取当前板上实际可用的最大分辨率
 * @return min(传感器上限, CAMERA_RES_BOARD_MAX, 运行时 fb 预算上限)
 */
camera_res_t camera_get_effective_max_res(void);

/** @brief 上限被哪一层钳制（sensor / board / memory）
 * @return 静态字符串，与 camera_get_effective_max_res() 最近一次计算对应
 */
const char *camera_res_cap_source(void);

/** @brief 运行时设置 JPEG 画质（OV5640 单寄存器写，无需重启）
 * @param quality 画质值（越界自动钳制到 [CAMERA_QUALITY_MIN, MAX]）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 相机未初始化
 */
esp_err_t camera_set_quality(uint8_t quality);

/** @brief 将分辨率枚举值转换为可读字符串
 * @param res 分辨率枚举值
 * @return 分辨率名称字符串（如 "VGA"、"SVGA"、"XGA"）
 */
/** Human-readable resolution name. */
const char *camera_res_to_str(camera_res_t res);

/** @brief 检查摄像头是否初始化成功
 * @return true 摄像头可用，false 初始化失败
 */
bool camera_is_available(void);
