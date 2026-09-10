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

#include "day_night.h"

#if CONFIG_MIBEE_DAY_NIGHT_AUTO

#include "camera_driver.h"
#include "config_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "frame_broadcaster.h"
#include "jpeg_decoder.h"
#include <string.h>

static const char *TAG = "day_night";

/* 判定参数（0-255 luma 尺度）。迟滞死带 40/65 防黄昏抖动；
 * 连续 2 次同向采样才切换（10s 周期 → ~20s 确认）。 */
#define DN_SAMPLE_PERIOD_S   10
#define DN_DARK_LUMA         40
#define DN_BRIGHT_LUMA       65
#define DN_CONFIRM_SAMPLES   2

/* 1/8 解码输出缓冲上限：UXGA(1600x1200)/8 = 200x150x3 = 90KB */
#define DN_RGB_MAX_BYTES     (96u * 1024u)

static struct {
    bool          started;
    bool          bw_supported;    /* 支持度探测结果（未知=true，失败后置否） */
    bool          probed;
    dn_effective_t effective;
    bool          applied_by_auto; /* 自动模式曾施加特效（退出时需恢复彩色） */
    int           luma;
    uint32_t      samples;
    uint32_t      switches;
    int           confirm_cnt;     /* 同向确认计数（负=暗向，正=亮向） */
    /* 解码工作区（PSRAM，惰性分配） */
    uint8_t      *rgb;
    size_t        rgb_cap;
    uint8_t      *tjpgd_ws;
} s_dn;

/* 从广播器缓存帧取 1/8 缩放平均 luma；无帧/解码失败返回 -1 */
static int sample_luma(void)
{
    frame_msg_t msg = {0};
    if (!fbroadcast_get_latest(&msg) || !msg.fb) {
        return -1;
    }

    esp_jpeg_image_output_t info = {};
    esp_jpeg_image_cfg_t dec = {};
    dec.indata = msg.fb->data;
    dec.indata_size = (uint32_t)msg.fb->len;
    dec.out_format = JPEG_IMAGE_FORMAT_RGB888;
    dec.out_scale = JPEG_IMAGE_SCALE_1_8;
    if (esp_jpeg_get_image_info(&dec, &info) != ESP_OK) {
        fbroadcast_release(&msg);
        return -1;
    }
    size_t need = (size_t)info.width * info.height * 3;
    if (need > DN_RGB_MAX_BYTES) {
        fbroadcast_release(&msg);
        return -1;
    }
    if (s_dn.rgb_cap < need) {
        free(s_dn.rgb);
        s_dn.rgb = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        s_dn.rgb_cap = s_dn.rgb ? need : 0;
        if (!s_dn.rgb) {
            fbroadcast_release(&msg);
            return -1;
        }
    }
    if (!s_dn.tjpgd_ws) {
        s_dn.tjpgd_ws = (uint8_t *)heap_caps_malloc(3100 + 512, MALLOC_CAP_SPIRAM);
        if (!s_dn.tjpgd_ws) {
            fbroadcast_release(&msg);
            return -1;
        }
    }
    dec.outbuf = s_dn.rgb;
    dec.outbuf_size = (uint32_t)s_dn.rgb_cap;
    dec.advanced.working_buffer = s_dn.tjpgd_ws;
    dec.advanced.working_buffer_size = 3100 + 512;

    esp_err_t err = esp_jpeg_decode(&dec, &info);
    fbroadcast_release(&msg);      /* 像素已在我们缓冲里，立刻归还帧 */
    if (err != ESP_OK) return -1;

    /* 平均 luma：Y ≈ (77R + 150G + 29B) >> 8（整数近似 BT.601） */
    const uint8_t *p = s_dn.rgb;
    uint32_t acc = 0;
    size_t n = (size_t)info.width * info.height;
    for (size_t i = 0; i < n; i++, p += 3) {
        acc += (uint32_t)p[0] * 77 + (uint32_t)p[1] * 150 + (uint32_t)p[2] * 29;
    }
    return (int)(acc >> 8) / (n > 0 ? n : 1);
}

static void apply_effect(dn_effective_t eff)
{
    esp_err_t ret = camera_set_day_night(eff == DN_EFFECT_BW ? 1 : 0);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        if (s_dn.bw_supported) {
            ESP_LOGW(TAG, "sensor lacks grayscale effect — auto mode degrades to color");
        }
        s_dn.bw_supported = false;
        s_dn.effective = DN_EFFECT_COLOR;
        s_dn.applied_by_auto = false;
        return;
    }
    s_dn.probed = true;
    s_dn.bw_supported = true;
    s_dn.effective = eff;
    s_dn.applied_by_auto = true;
    s_dn.switches++;
    ESP_LOGI(TAG, "auto switch -> %s (luma=%d)",
             eff == DN_EFFECT_BW ? "BW" : "color", s_dn.luma);
}

static void dn_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sampler started (period=%us dark<%d bright>%d confirm=%d)",
             DN_SAMPLE_PERIOD_S, DN_DARK_LUMA, DN_BRIGHT_LUMA, DN_CONFIRM_SAMPLES);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(DN_SAMPLE_PERIOD_S * 1000));
        const cam_config_t *cfg = config_get();
        if (cfg->day_night_mode != 2) {
            /* 手动模式：退出自动时恢复彩色一次（仅当自动曾动过特效） */
            if (s_dn.applied_by_auto) {
                camera_set_day_night(0);
                s_dn.applied_by_auto = false;
                s_dn.effective = DN_EFFECT_COLOR;
                s_dn.confirm_cnt = 0;
                ESP_LOGI(TAG, "manual mode — auto effect restored to color");
            }
            continue;
        }
        int luma = sample_luma();
        if (luma < 0) {
            ESP_LOGD(TAG, "no frame / decode failed, skip");
            continue;
        }
        s_dn.luma = luma;
        s_dn.samples++;

        /* 迟滞 + 确认计数：暗向累计 -N、亮向累计 +N 才切换 */
        if (luma < DN_DARK_LUMA) {
            s_dn.confirm_cnt = (s_dn.confirm_cnt > 0) ? -1 : s_dn.confirm_cnt - 1;
        } else if (luma > DN_BRIGHT_LUMA) {
            s_dn.confirm_cnt = (s_dn.confirm_cnt < 0) ? 1 : s_dn.confirm_cnt + 1;
        } else {
            s_dn.confirm_cnt = 0;      /* 死带内保持 */
        }

        dn_effective_t want = s_dn.effective;
        if (s_dn.confirm_cnt <= -DN_CONFIRM_SAMPLES) {
            want = DN_EFFECT_BW;
            s_dn.confirm_cnt = 0;
        } else if (s_dn.confirm_cnt >= DN_CONFIRM_SAMPLES) {
            want = DN_EFFECT_COLOR;
            s_dn.confirm_cnt = 0;
        }
        if (want != s_dn.effective || !s_dn.probed) {
            apply_effect(want);
        } else {
            ESP_LOGI(TAG, "luma=%d hold %s", luma,
                     s_dn.effective == DN_EFFECT_BW ? "BW" : "color");
        }
    }
}

esp_err_t day_night_start(void)
{
    if (s_dn.started) return ESP_OK;
    s_dn.bw_supported = true;      /* 乐观初值，首 apply 探测校正 */
    if (xTaskCreate(dn_task, "day_night", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    s_dn.started = true;
    return ESP_OK;
}

void day_night_get_state(day_night_state_t *out)
{
    if (!out) return;
    out->configured = config_get()->day_night_mode;
    out->effective = s_dn.effective;
    out->bw_supported = s_dn.bw_supported;
    out->luma = s_dn.samples ? s_dn.luma : -1;
    out->samples = s_dn.samples;
    out->switches = s_dn.switches;
}

#else /* !CONFIG_MIBEE_DAY_NIGHT_AUTO */

esp_err_t day_night_start(void) { return ESP_OK; }
void day_night_get_state(day_night_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->configured = config_get()->day_night_mode;
    out->effective = DN_EFFECT_COLOR;
    out->bw_supported = false;
    out->luma = -1;
}

#endif /* CONFIG_MIBEE_DAY_NIGHT_AUTO */
