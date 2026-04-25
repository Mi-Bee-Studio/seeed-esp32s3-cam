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

#include "camera_driver.h"
#include "esp_camera.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sensor.h"

static const char *TAG = "camera";

/* XIAO ESP32S3 Sense DVP pin mapping（与 Arduino CameraWebServer 官方定义一致） */
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  10
#define CAM_PIN_SIOD  40
#define CAM_PIN_SIOC  39
#define CAM_PIN_D7    48
#define CAM_PIN_D6    11
#define CAM_PIN_D5    12
#define CAM_PIN_D4    14
#define CAM_PIN_D3    16
#define CAM_PIN_D2    18
#define CAM_PIN_D1    17
#define CAM_PIN_D0    15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  47
#define CAM_PIN_PCLK  13

static camera_sensor_t s_sensor      = CAMERA_SENSOR_UNKNOWN;
static camera_res_t    s_current_res = CAMERA_RES_SVGA;
static bool            s_initialized = false;
static camera_fb_t    *s_pending_fb  = NULL;

/** @brief 将分辨率枚举转换为 esp_camera 驱动的帧大小枚举
 *
 * 内部辅助函数，将 camera_res_t 映射为 framesize_t。
 * @param res 分辨率枚举值
 * @return 对应的 framesize_t 值，未知时默认返回 FRAMESIZE_SVGA
 */
static framesize_t res_to_framesize(camera_res_t res)
{
    switch (res) {
        case CAMERA_RES_VGA:  return FRAMESIZE_VGA;
        case CAMERA_RES_SVGA: return FRAMESIZE_SVGA;
        case CAMERA_RES_XGA:  return FRAMESIZE_XGA;
        default:              return FRAMESIZE_SVGA;
    }
}

/** @brief 初始化摄像头
 *
 * 配置 XIAO ESP32-S3 Sense 的 DVP 引脚映射，设定 JPEG 像素格式、
 * 双缓冲（PSRAM）、最新帧抓取模式。自动检测 OV2640/OV3660 传感器，
 * 并执行一次测试采集验证管线是否正常。
 * @param res 分辨率（VGA/SVGA/XGA）
 * @param fps 帧率（未直接使用，保留参数）
 * @param quality JPEG 质量（0-63，越小质量越好）
 * @return ESP_OK 成功，其他值表示初始化失败
 */
esp_err_t camera_init(camera_res_t res, uint8_t fps, uint8_t quality)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Camera already initialized");
        return ESP_OK;
    }

    /* Release GPIO10 from SDMMC (SD card CMD) so camera can use it as XCLK */
    gpio_reset_pin(CAM_PIN_XCLK);

    camera_config_t config = {
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format  = PIXFORMAT_JPEG,
        .frame_size    = res_to_framesize(res),
        .jpeg_quality  = quality,
        .fb_count      = 2,                /* double buffer in PSRAM */
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .grab_mode     = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return err;
    }

    s_current_res = res;

    /* Auto-detect sensor */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        switch (sensor->id.PID) {
            case 0x2642:
                s_sensor = CAMERA_SENSOR_OV2640;
                break;
            case 0x3660:
                s_sensor = CAMERA_SENSOR_OV3660;
                break;
            default:
                s_sensor = CAMERA_SENSOR_UNKNOWN;
                break;
        }
    }

    const char *sensor_name = "UNKNOWN";
    switch (s_sensor) {
        case CAMERA_SENSOR_OV2640: sensor_name = "OV2640"; break;
        case CAMERA_SENSOR_OV3660: sensor_name = "OV3660"; break;
        default: break;
    }
    ESP_LOGI(TAG, "Sensor: %s, Resolution: %s, Quality: %d",
             sensor_name, camera_res_to_str(res), quality);

    /* Warmup: allow sensor to stabilize auto-exposure */
    ESP_LOGI(TAG, "Warming up sensor (discarding initial frames)...");
    for (int i = 0; i < 10; i++) {
        camera_fb_t *warmup_fb = esp_camera_fb_get();
        if (warmup_fb) {
            ESP_LOGD(TAG, "Warmup frame %d: %zu bytes", i, warmup_fb->len);
            esp_camera_fb_return(warmup_fb);
        } else {
            ESP_LOGD(TAG, "Warmup frame %d: NULL", i);
        }
    }

    /* Test capture to verify pipeline */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        ESP_LOGI(TAG, "Test capture OK, frame size: %zu bytes", fb->len);
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGW(TAG, "Test capture returned NULL (may succeed later)");
    }

    s_initialized = true;
    return ESP_OK;
}

/** @brief 获取当前检测到的摄像头传感器类型
 * @return 传感器类型枚举值
 */
camera_sensor_t camera_get_sensor(void)
{
    return s_sensor;
}

/** @brief 捕获一帧 JPEG 图像
 *
 * 从摄像头驱动获取一帧 JPEG 数据。帧缓冲区位于 PSRAM，
 * 调用者必须在处理完毕后调用 camera_return_fb() 释放。
 * @param frame 输出帧描述符（buf 和 len 被填充）
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，ESP_FAIL 捕获失败
 */
esp_err_t camera_capture(camera_frame_t *frame)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Camera not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Capture failed");
        return ESP_FAIL;
    }

    frame->buf = fb->buf;
    frame->len = fb->len;
    s_pending_fb = fb;

    return ESP_OK;
}

/** @brief 归还帧缓冲区给摄像头驱动
 *
 * 在 camera_capture() 使用完帧数据后必须调用，以释放 PSRAM 帧缓冲区。
 * @param frame 之前捕获的帧描述符（指针内容未使用）
 */
void camera_return_fb(camera_frame_t *frame)
{
    (void)frame;
    if (s_pending_fb) {
        esp_camera_fb_return(s_pending_fb);
        s_pending_fb = NULL;
    }
}

/** @brief 动态切换摄像头分辨率
 *
 * 运行时修改传感器输出分辨率，无需重新初始化摄像头。
 * @param res 目标分辨率
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，ESP_FAIL 设置失败
 */
esp_err_t camera_set_resolution(camera_res_t res)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return ESP_FAIL;
    }

    if (sensor->set_framesize(sensor, res_to_framesize(res)) != 0) {
        ESP_LOGE(TAG, "Failed to set resolution to %s", camera_res_to_str(res));
        return ESP_FAIL;
    }

    s_current_res = res;
    ESP_LOGI(TAG, "Resolution set to %s", camera_res_to_str(res));
    return ESP_OK;
}

/** @brief 获取当前分辨率设置
 * @return 当前分辨率枚举值
 */
camera_res_t camera_get_resolution(void)
{
    return s_current_res;
}

/** @brief 将分辨率枚举值转换为可读字符串
 * @param res 分辨率枚举值
 * @return 分辨率名称字符串（"VGA"、"SVGA"、"XGA" 或 "UNKNOWN"）
 */
const char *camera_res_to_str(camera_res_t res)
{
    switch (res) {
        case CAMERA_RES_VGA:  return "VGA";
        case CAMERA_RES_SVGA: return "SVGA";
        case CAMERA_RES_XGA:  return "XGA";
        default:              return "UNKNOWN";
    }
}
