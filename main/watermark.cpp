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

/* 水印实现（issue #11，仅 seeed）：
 * - 解码：espressif/esp_jpeg（tjpgd 内核）→ RGB888，输出缓冲全 PSRAM；
 * - 渲染：font8x8（公有领域）整数倍缩放（≥720p 时 2x），四角定位，
 *   黑描边 + 白填充，任意背景可读；
 * - 编码：vendored jpge（质量 wm_quality）；
 * - 缓冲惰性分配、按帧分辨率变化重配；4MB RGB 上限护栏（UXGA 5.6MB
 *   超 PSRAM 预算 → 放弃水印写原帧）；
 * - 失败语义：一律回退原帧（永不吞帧），速率限制告警。 */

#include "watermark.h"

#if CONFIG_MIBEE_WATERMARK

#include "config_manager.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"     /* espressif/esp_jpeg v1.3.1 解码器 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

#include "jpge.h"
#include "watermark_font.h"

static const char *TAG = "watermark";

/* RGB888 工作缓冲上限：SVGA≈2.7MB / HD≈2.8MB / SXGA≈3.9MB / UXGA≈5.6MB。
 * 4MB 上限保住推流/CSI/录像并存的 PSRAM 预算（当前 free ~6.3MB）。 */
#define WM_RGB_MAX_BYTES   (4u * 1024u * 1024u)
#define WM_JPEG_OUT_MARGIN (96u * 1024u)   /* 重编码输出 ≈ 原帧 + 余量 */

typedef struct {
    uint8_t *rgb;            /* RGB888 解码缓冲（PSRAM） */
    size_t   rgb_cap;
    uint8_t *out;            /* 重编码 JPEG 缓冲（PSRAM） */
    size_t   out_cap;
    uint8_t *tjpgd_ws;       /* tjpgd 工作缓冲 3.5KB（PSRAM） */
    SemaphoreHandle_t mtx;
    /* 统计（日志速率限制用） */
    uint32_t frames;
    uint32_t fallbacks;
    uint32_t last_log_frame;
    int64_t  decode_us_acc;
    int64_t  encode_us_acc;
} wm_state_t;

static wm_state_t s_wm;

/* ------------------------------------------------------------------ */
/*  渲染                                                              */
/* ------------------------------------------------------------------ */

static void blit_glyph_rgb888(uint8_t *rgb, int stride, int x, int y,
                              char c, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    const unsigned char uc = (unsigned char)c;
    if (uc < 32 || uc > 126) return;
    const char *glyph = font8x8_basic[uc];
    for (int row = 0; row < 8; row++) {
        unsigned bits = (unsigned char)glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!((bits >> col) & 1)) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    uint8_t *p = rgb + (py * stride + px) * 3;
                    p[0] = r; p[1] = g; p[2] = b;
                }
            }
        }
    }
}

static void blit_text_rgb888(uint8_t *rgb, int stride, int w, int h,
                             const char *text, int x, int y, int scale,
                             uint8_t r, uint8_t g, uint8_t b)
{
    for (const char *p = text; *p; p++, x += 8 * scale) {
        if (x + 8 * scale > w) break;   /* 越界截断（防御） */
        if (y + 8 * scale > h) break;
        blit_glyph_rgb888(rgb, stride, x, y, *p, scale, r, g, b);
    }
}

/* 组合两行文案：line1=自定义文本，line2=实时时戳（strftime）。
 * 返回行数（0=无内容可画）。 */
static int compose_lines(char *l1, size_t l1sz, char *l2, size_t l2sz)
{
    const cam_config_t *cfg = config_get();
    int n = 0;
    l1[0] = l2[0] = '\0';
    if (cfg->wm_text[0]) {
        strlcpy(l1, cfg->wm_text, l1sz);
        n++;
    }
    if (cfg->wm_time_fmt[0]) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        size_t w = strftime(l2, l2sz, cfg->wm_time_fmt, &tmv);
        if (w > 0) n++;
        else l2[0] = '\0';
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  缓冲管理                                                          */
/* ------------------------------------------------------------------ */

static bool ensure_buffers(size_t rgb_need, size_t jpeg_in_len)
{
    if (s_wm.rgb_cap < rgb_need) {
        free(s_wm.rgb);
        s_wm.rgb = (uint8_t *)heap_caps_malloc(rgb_need, MALLOC_CAP_SPIRAM);
        s_wm.rgb_cap = s_wm.rgb ? rgb_need : 0;
        if (!s_wm.rgb) return false;
        ESP_LOGI(TAG, "RGB buffer: %u bytes", (unsigned)rgb_need);
    }
    size_t out_need = jpeg_in_len + WM_JPEG_OUT_MARGIN;
    if (s_wm.out_cap < out_need) {
        free(s_wm.out);
        s_wm.out = (uint8_t *)heap_caps_malloc(out_need, MALLOC_CAP_SPIRAM);
        s_wm.out_cap = s_wm.out ? out_need : 0;
        if (!s_wm.out) return false;
    }
    if (!s_wm.tjpgd_ws) {
        s_wm.tjpgd_ws = (uint8_t *)heap_caps_malloc(3100 + 512, MALLOC_CAP_SPIRAM);
        if (!s_wm.tjpgd_ws) return false;
    }
    if (!s_wm.mtx) {
        s_wm.mtx = xSemaphoreCreateMutex();
        if (!s_wm.mtx) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  公共接口                                                          */
/* ------------------------------------------------------------------ */

bool watermark_photo_enabled(void)
{
    return config_get()->wm_enable;
}

bool watermark_video_enabled(void)
{
    const cam_config_t *cfg = config_get();
    return cfg->wm_enable && cfg->wm_video;
}

esp_err_t watermark_apply(const uint8_t *jpeg_in, size_t in_len,
                          const uint8_t **jpeg_out, size_t *out_len)
{
    *jpeg_out = jpeg_in;      /* 回退语义：任何失败都写原帧 */
    *out_len = in_len;
    if (!jpeg_in || in_len < 2) return ESP_ERR_INVALID_ARG;

    if (!s_wm.mtx && !ensure_buffers(0, 0)) return ESP_FAIL;
    xSemaphoreTake(s_wm.mtx, portMAX_DELAY);

    esp_err_t ret = ESP_FAIL;
    int64_t t0, t_decode = 0, t_encode = 0;
    size_t rgb_need = 0;
    char l1[40] = {0}, l2[32] = {0};
    int lines = 0;
    const cam_config_t *cfg = NULL;
    int w = 0, h = 0, scale = 1, lh = 10, margin = 6, x0 = 0, y0 = 0;
    jpge::params p;
    int out_size = 0;
    bool ok = false;

    esp_jpeg_image_output_t info = {};
    esp_jpeg_image_cfg_t dec = {};
    dec.indata = (uint8_t *)jpeg_in;
    dec.indata_size = (uint32_t)in_len;
    dec.outbuf = NULL;
    dec.outbuf_size = 0;
    dec.out_format = JPEG_IMAGE_FORMAT_RGB888;
    dec.out_scale = JPEG_IMAGE_SCALE_0;
    dec.advanced.working_buffer = s_wm.tjpgd_ws;
    dec.advanced.working_buffer_size = 3100 + 512;
    if (esp_jpeg_get_image_info(&dec, &info) != ESP_OK) goto out;
    rgb_need = (size_t)info.width * info.height * 3;
    if (rgb_need > WM_RGB_MAX_BYTES) goto out;          /* 超上限：原帧 */
    if (!ensure_buffers(rgb_need, in_len)) goto out;

    dec.outbuf = s_wm.rgb;
    dec.outbuf_size = (uint32_t)s_wm.rgb_cap;

    t0 = esp_timer_get_time();
    if (esp_jpeg_decode(&dec, &info) != ESP_OK) goto out;
    t_decode = esp_timer_get_time() - t0;

    lines = compose_lines(l1, sizeof(l1), l2, sizeof(l2));
    if (lines <= 0) goto out;             /* 无内容：原帧直写 */
    cfg = config_get();
    w = info.width;
    h = info.height;
    scale = (h >= 720) ? 2 : 1;
    lh = 10 * scale;                      /* 8px 字高 + 2px 行距 */
    margin = 6 * scale;
    x0 = margin;
    if (cfg->wm_pos & 0x2) {              /* bit1: 上 */
        y0 = margin;
    } else {
        y0 = h - margin - lines * lh + 2 * scale;
    }
    if (cfg->wm_pos & 0x1) {              /* bit0: 右对齐 */
        int maxw = 0;
        if (l1[0]) maxw = (int)strlen(l1);
        if (l2[0] && (int)strlen(l2) > maxw) maxw = (int)strlen(l2);
        x0 = w - margin - maxw * 8 * scale;
    }
    {
        const char *ln[2] = { l1, l2 };
        for (int i = 0; i < 2; i++) {
            if (!ln[i][0]) continue;
            int y = y0 + i * lh;
            /* 黑描边（4 向偏移）+ 白填充：任意背景可读 */
            for (int d = 0; d < 4; d++) {
                int dx = (d & 1) ? scale : 0, dy = (d & 2) ? scale : 0;
                blit_text_rgb888(s_wm.rgb, w, w, h, ln[i],
                                 x0 + dx, y + dy, scale, 0, 0, 0);
            }
            blit_text_rgb888(s_wm.rgb, w, w, h, ln[i], x0, y, scale,
                             255, 255, 255);
        }
    }

    {
        p.m_quality = config_get()->wm_quality;
        p.m_subsampling = jpge::H2V2;
        out_size = (int)s_wm.out_cap;
        t0 = esp_timer_get_time();
        ok = jpge::compress_image_to_jpeg_file_in_memory(
            s_wm.out, out_size, info.width, info.height, 3, s_wm.rgb, p);
        t_encode = esp_timer_get_time() - t0;
        if (!ok) goto out;
        *jpeg_out = s_wm.out;
        *out_len = (size_t)out_size;
        ret = ESP_OK;
        s_wm.frames++;
        s_wm.decode_us_acc += t_decode;
        s_wm.encode_us_acc += t_encode;
        if (s_wm.frames - s_wm.last_log_frame >= 60) {
            ESP_LOGI(TAG, "stats: frames=%u fallbacks=%u decode=%ums encode=%ums (avg/frm)",
                     (unsigned)s_wm.frames, (unsigned)s_wm.fallbacks,
                     (unsigned)(s_wm.decode_us_acc / s_wm.frames / 1000),
                     (unsigned)(s_wm.encode_us_acc / s_wm.frames / 1000));
            s_wm.last_log_frame = s_wm.frames;
        }
    }

out:
    if (ret != ESP_OK) {
        s_wm.fallbacks++;
        if (s_wm.fallbacks % 100 == 1) {
            ESP_LOGW(TAG, "watermark fallback (total=%u) — writing original frame",
                     (unsigned)s_wm.fallbacks);
        }
    }
    xSemaphoreGive(s_wm.mtx);
    return ret;
}

#else /* !CONFIG_MIBEE_WATERMARK：编译门关闭——零代码路径（回退形态） */

bool watermark_photo_enabled(void) { return false; }
bool watermark_video_enabled(void) { return false; }
esp_err_t watermark_apply(const uint8_t *jpeg_in, size_t in_len,
                          const uint8_t **jpeg_out, size_t *out_len)
{
    *jpeg_out = jpeg_in;
    *out_len = in_len;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_MIBEE_WATERMARK */
