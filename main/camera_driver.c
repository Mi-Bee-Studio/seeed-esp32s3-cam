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

#include "camera_driver.h"
#include "esp_camera.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sensor.h"
#include "esp_heap_caps.h"
#include "config_manager.h"
#include "freertos/timers.h"


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
static const char     *s_cap_source  = "board";   /* 最近一次 effective 计算的钳制层 */

/* Timeout for camera capture — prevents indefinite block in esp_camera_fb_get()
 * that would starve the TWDT and trigger a panic reset.
 * Must be < TWDT_TIMEOUT_MS (30000) to give the caller a chance to feed the WDT. */
#define CAMERA_CAPTURE_TIMEOUT_MS  20000

static TimerHandle_t s_capture_timer = NULL;
static volatile bool  s_capture_timed_out = false;

static void capture_timeout_cb(TimerHandle_t xTimer)
{
    s_capture_timed_out = true;
    TaskHandle_t task = (TaskHandle_t)pvTimerGetTimerID(xTimer);
    if (task) {
        xTaskAbortDelay(task);
    }
}

/** @brief 将分辨率枚举转换为 esp_camera 驱动的帧大小枚举
 *
 * 家族统一刻度（契约 v1.3 §5）：camera_res_t 的值即 framesize_t 枚举，
 * 映射为恒等变换，仅做值域保护。
 */
static framesize_t res_to_framesize(camera_res_t res)
{
    if ((int)res >= FRAMESIZE_VGA && (int)res <= FRAMESIZE_UXGA) {
        return (framesize_t)res;
    }
    return FRAMESIZE_SVGA;
}

/* ── 三层上限之 memory 层：fb 预算运行时校验 ─────────────────────────
 * esp32-camera 的 JPEG fb 按 宽*高/5 分配（cam_hal cam_config 同式），
 * PSRAM 板预算 = fb_size * fb_count，要求分完后仍留 floor 给 lwIP 大缓冲
 * 等非相机消费者。此层只能收紧上限（防御 PSRAM 退化态/泄漏），不会把
 * effective 放宽超过 CAMERA_RES_BOARD_MAX 实测常数。 */
#define CAMERA_FB_COUNT       3     /* 与 camera_init 一致（triple buffer） */
#define CAMERA_RES_MEM_FLOOR  (512 * 1024)   /* PSRAM 保留水位 */

/* framesize → 尺寸表，下标 0 对应 FRAMESIZE_VGA（钉死 esp32-camera 2.1.x
 * 枚举序；组件枚举漂移时由下方 _Static_assert 在构建期暴露） */
static const struct { uint16_t w, h; } s_fs_dims[] = {
    { 640,  480},  { 800,  600},  {1024,  768},  {1280,  720},  {1280, 1024},
    {1600, 1200},  {1920, 1080},  { 720, 1280},  { 864, 1536},  {2048, 1536},
    {2560, 1440},  {2560, 1600},  {1080, 1920},  {2560, 1920},  {2592, 1944},
};
_Static_assert(FRAMESIZE_VGA == 10, "s_fs_dims pinned to esp32-camera 2.1.x enum");

/** @brief 该档位单个 JPEG fb 的字节数（宽*高/5），非法档位返回 0 */
static size_t fb_bytes_for_res(camera_res_t res)
{
    int idx = (int)res - (int)FRAMESIZE_VGA;   /* 家族刻度：下标 0 = VGA */
    if (idx < 0 || (size_t)idx >= sizeof(s_fs_dims) / sizeof(s_fs_dims[0])) {
        return 0;
    }
    return (size_t)s_fs_dims[idx].w * s_fs_dims[idx].h / 5;
}

/** @brief fb 预算校验：换到该档位后 PSRAM 是否还付得起（含释放现有 fb） */
static bool fb_budget_ok(camera_res_t res)
{
    size_t need = fb_bytes_for_res(res) * CAMERA_FB_COUNT;
    size_t cur_fb = s_initialized ? fb_bytes_for_res(s_current_res) * CAMERA_FB_COUNT : 0;
    size_t avail = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) + cur_fb;
    return need != 0 && avail >= need + CAMERA_RES_MEM_FLOOR;
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
    /* 参数校验（家族刻度 10-15；越界回退 SVGA=本板默认档） */
    if ((int)res < (int)CAMERA_RES_VGA || (int)res > (int)CAMERA_RES_UXGA) {
        ESP_LOGW(TAG, "Invalid resolution %d, defaulting to SVGA", res);
        res = CAMERA_RES_SVGA;
    }
    if (quality < CAMERA_QUALITY_MIN || quality > CAMERA_QUALITY_MAX) {
        ESP_LOGW(TAG, "Quality %d out of range [%d-%d], clamping",
                 quality, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
        quality = (quality < CAMERA_QUALITY_MIN) ? CAMERA_QUALITY_MIN : CAMERA_QUALITY_MAX;
    }

    /* Release GPIO10 so camera can use it as XCLK (no longer shared with SD card) */
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

        .xclk_freq_hz = (uint32_t)(config_get()->xclk_freq_mhz ? config_get()->xclk_freq_mhz : 16) * 1000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format  = PIXFORMAT_JPEG,
        .frame_size    = res_to_framesize(res),
        .jpeg_quality  = quality,
        .fb_count      = 3,                /* triple buffer — broadcaster eliminates contention */
        .fb_location   = CAMERA_FB_IN_PSRAM,
        .grab_mode     = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Camera init with res=%d failed: 0x%x, retrying with VGA", res, err);
        config.frame_size = FRAMESIZE_VGA;
        err = esp_camera_init(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed (VGA fallback): 0x%x", err);
            return err;
        }
        res = CAMERA_RES_VGA;
    }    /* Auto-detect sensor */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        switch (sensor->id.PID) {
            case OV2640_PID:
                s_sensor = CAMERA_SENSOR_OV2640;
                break;
            case OV3660_PID:
                s_sensor = CAMERA_SENSOR_OV3660;
                break;
            case OV5640_PID:
                s_sensor = CAMERA_SENSOR_OV5640;
                break;
            default:
                s_sensor = CAMERA_SENSOR_UNKNOWN;
                break;
        }
    }

    /* Clamp resolution to sensor capability AND board-tested maximum
     * (thermal: FHD/QXGA exceed safe chip temp on this board — see
     * CAMERA_RES_BOARD_MAX note in camera_driver.h) */
    /* 三层上限统一钳制（sensor ∩ board ∩ memory；热限说明见
     * camera_driver.h CAMERA_RES_BOARD_MAX 注）。注意 PIT-019：OV5640
     * 运行时 set_framesize 不生效，此处钳到超限档时实际帧尺寸要等
     * 重启（保存路径 POST 已拒绝超限值，这里兜底 NVS 残留）。 */
    if (res > camera_get_effective_max_res()) {
        ESP_LOGW(TAG, "Requested res %d exceeds effective max %d (source: %s), clamping",
                 res, camera_get_effective_max_res(), camera_res_cap_source());
        res = camera_get_effective_max_res();
        sensor->set_framesize(sensor, res_to_framesize(res));
    }

    s_current_res = res;

    const char *sensor_name = "UNKNOWN";
    switch (s_sensor) {
        case CAMERA_SENSOR_OV2640: sensor_name = "OV2640"; break;
        case CAMERA_SENSOR_OV3660: sensor_name = "OV3660"; break;
        case CAMERA_SENSOR_OV5640: sensor_name = "OV5640"; break;
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

    /* Apply initial flip/mirror settings */
    camera_set_flip(false, false);
    
    /* Create capture timeout timer (one-shot, reused across calls) */
    s_capture_timer = xTimerCreate("cam_cap_to",
        pdMS_TO_TICKS(CAMERA_CAPTURE_TIMEOUT_MS),
        pdFALSE,  /* one-shot */
        NULL,     /* timer ID set dynamically before each capture */
        capture_timeout_cb);
    if (!s_capture_timer) {
        ESP_LOGW(TAG, "Cannot create capture timeout timer — no timeout protection");
    }
    return ESP_OK;
}

/** @brief 获取当前检测到的摄像头传感器类型
 * @return 传感器类型枚举值
 */
camera_sensor_t camera_get_sensor(void)
{
    return s_sensor;
}

camera_res_t camera_get_max_resolution(camera_sensor_t sensor)
{
    /* sensor 层：直接查 esp32-camera 组件自带的传感器能力表
     * （camera_sensor[].max_size，单一事实源——不在此维护第二份 PID 表）。
     * 家族统一刻度下组件表值即本枚举值，只需钳到本板枚举天花板 UXGA。
     *
     * 判活用 esp_camera_sensor_get() 句柄而非 s_initialized 旗标：
     * camera_init 中途（esp_camera_init 已成、旗标未置位）会调用本函数
     * 做三层钳制，按旗标判定会错误走 XGA 回退，把 HD/SXGA/UXGA 全部
     * 钳到 XGA（v0.4.0 起的实机行为，2026-09-05 实拍 SOF 证实）。 */
    {
        sensor_t *s = esp_camera_sensor_get();
        camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
        if (info && info->max_size >= FRAMESIZE_VGA) {
            int res = (int)info->max_size;
            if (res > (int)CAMERA_RES_UXGA) {
                res = (int)CAMERA_RES_UXGA;
            }
            return (camera_res_t)res;
        }
        if (s) {
            ESP_LOGW(TAG, "Unknown sensor PID — sensor layer falls back to XGA");
        }
    }
    /* 相机未初始化（无传感器句柄）：保守回退（仅在初始化前的调用窗口生效） */
    (void)sensor;
    return CAMERA_RES_XGA;
}

camera_res_t camera_get_effective_max_res(void)
{
    /* sensor 层（未初始化时回退 XGA，与本函数历史行为一致） */
    camera_res_t sensor_cap = camera_get_max_resolution(s_sensor);
    /* board 层：本板实测常数 */
    camera_res_t board_cap = CAMERA_RES_BOARD_MAX;
    camera_res_t cap = (sensor_cap < board_cap) ? sensor_cap : board_cap;
    /* memory 层：运行时 fb 预算，只能收紧 */
    while (cap > CAMERA_RES_VGA && !fb_budget_ok(cap)) {
        cap = (camera_res_t)((int)cap - 1);
    }
    /* 钳制层归属（并列时 sensor > board > memory） */
    if (cap == sensor_cap) {
        s_cap_source = "sensor";
    } else if (cap == board_cap) {
        s_cap_source = "board";
    } else {
        s_cap_source = "memory";
    }
    return cap;
}

const char *camera_res_cap_source(void)
{
    return s_cap_source;
}

esp_err_t camera_set_quality(uint8_t quality)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (quality < CAMERA_QUALITY_MIN) quality = CAMERA_QUALITY_MIN;
    if (quality > CAMERA_QUALITY_MAX) quality = CAMERA_QUALITY_MAX;

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || !sensor->set_quality) {
        return ESP_FAIL;
    }
    if (sensor->set_quality(sensor, quality) != 0) {
        ESP_LOGE(TAG, "Failed to set quality to %u", quality);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Quality set to %u", quality);
    return ESP_OK;
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

    s_capture_timed_out = false;

    /* Arm timeout timer before the blocking esp_camera_fb_get() call.
     * If the sensor stops producing frames, the timer fires -> xTaskAbortDelay()
     * unblocks the task -> esp_camera_fb_get() returns NULL -> we report timeout. */
    if (s_capture_timer) {
        vTimerSetTimerID(s_capture_timer, (void*)xTaskGetCurrentTaskHandle());
        xTimerStart(s_capture_timer, 0);
    }

    camera_fb_t *fb = esp_camera_fb_get();

    /* Disarm timer (harmless if it already fired) */
    if (s_capture_timer) {
        xTimerStop(s_capture_timer, 0);
    }

    if (!fb) {
        if (s_capture_timed_out) {
            ESP_LOGW(TAG, "Capture timed out after %dms", CAMERA_CAPTURE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "Capture failed");
        return ESP_FAIL;
    }

    frame->buf = fb->buf;
    frame->len = fb->len;
    frame->_fb = fb;

    return ESP_OK;
}

/** @brief 归还帧缓冲区给摄像头驱动
 *
 * 在 camera_capture() 使用完帧数据后必须调用，以释放 PSRAM 帧缓冲区。
 * @param frame 之前捕获的帧描述符（指针内容未使用）
 */
void camera_return_fb(camera_frame_t *frame)
{
    if (frame && frame->_fb) {
        esp_camera_fb_return((camera_fb_t *)frame->_fb);
        frame->_fb = NULL;
    }
}

/** @brief 动态切换摄像头分辨率
 *
 * 运行时修改传感器输出分辨率，无需重新初始化摄像头。
 * @param res 目标分辨率
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，ESP_FAIL 设置失败
 */
/* ⚠️ 2026-09-04 实测证伪：OV5640 上运行时 sensor->set_framesize() 只写
 * 寄存器不重启 DSP——返回 0 "成功"但输出帧仍是旧分辨率（HD 改 SXGA 后
 * /api/capture 仍出 1280x720）。分辨率变更必须走保存+重启（esp_camera_init
 * 在启动时以新 framesize 初始化才真正生效）。本函数仅供启动期使用。 */
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

/** @brief 设置摄像头垂直翻转和水平镜像
 *
 * 运行时修改传感器翻转设置，无需重新初始化摄像头。
 * @param vflip 垂直翻转（true=翻转）
 * @param hmirror 水平镜像（true=镜像）
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，ESP_FAIL 设置失败
 */
esp_err_t camera_set_flip(bool vflip, bool hmirror)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return ESP_FAIL;
    }

    if (sensor->set_vflip) {
        sensor->set_vflip(sensor, vflip ? 1 : 0);
    }
    if (sensor->set_hmirror) {
        sensor->set_hmirror(sensor, hmirror ? 1 : 0);
    }

    ESP_LOGI(TAG, "Flip/Mirror set: vflip=%d, hmirror=%d", vflip, hmirror);
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
        case CAMERA_RES_HD:   return "HD";
        case CAMERA_RES_SXGA: return "SXGA";
        case CAMERA_RES_UXGA: return "UXGA";
        default:              return "UNKNOWN";
    }
}

/** @brief 设置日夜模式（彩色/黑白）
 * @param mode 0=彩色, 1=黑白, 2=自动(预留,当前等同0)
 * @return ESP_OK 成功, ESP_ERR_INVALID_STATE 未初始化, ESP_FAIL 失败
 */
esp_err_t camera_set_day_night(uint8_t mode)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return ESP_FAIL;
    }
    /* mode 0=color, 1=B/W(grayscale), 2=auto(预留,暂同color)
     * effect=2 is grayscale for OV2640/OV3660/OV5640 in esp32-camera */
    int effect = 0;
    if (mode == 1) {
        effect = 2; /* grayscale — 待硬件验证: set_special_effect 值需确认 */
    }
    if (sensor->set_special_effect) {
        sensor->set_special_effect(sensor, effect);
    }
    ESP_LOGI(TAG, "Day/night mode set: %d (effect=%d)", mode, effect);
    return ESP_OK;
}
