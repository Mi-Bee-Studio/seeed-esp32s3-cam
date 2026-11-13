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

#include "log_upload.h"

#include "config_manager.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "log_up";

/* ---- RAM 日志环（PSRAM）------------------------------------------ */

#define RING_SIZE (96u * 1024u)
static char *s_ring = NULL;
static size_t s_ring_head = 0;      /* 写入位置 */
static size_t s_ring_used = 0;      /* 有效数据（≤RING_SIZE） */
static vprintf_like_t s_orig_vprintf = NULL;

/* IDF v6：esp_log_set_vprintf 的钩子签名是 vprintf_like_t
 * (int (*)(const char*, va_list))——返回写入字符数。 */

static int ring_tee_vprintf(const char *fmt, va_list args)
{
    /* 先交原处理器（控制台照常输出），再把同内容追加进环 */
    int ret = 0;
    if (s_orig_vprintf) {
        va_list copy;
        va_copy(copy, args);
        ret = s_orig_vprintf(fmt, copy);
        va_end(copy);
    }

    if (s_ring) {
        va_list copy2;
        va_copy(copy2, args);
        int n = vsnprintf(NULL, 0, fmt, copy2);
        va_end(copy2);
        if (n > 0) {
            size_t len = (size_t)n;
            if (len >= RING_SIZE) len = RING_SIZE - 1;   /* 超长行截断 */
            char *tmp = (char *)malloc(len + 1);
            if (tmp) {
                va_list copy3;
                va_copy(copy3, args);
                vsnprintf(tmp, len + 1, fmt, copy3);
                va_end(copy3);
                /* 追加到环（顺序写，满后从头覆盖） */
                for (size_t i = 0; i < len; i++) {
                    s_ring[s_ring_head] = tmp[i];
                    s_ring_head = (s_ring_head + 1) % RING_SIZE;
                }
                if (s_ring_used < RING_SIZE) s_ring_used += len;
                else s_ring_head = s_ring_head % RING_SIZE;
                free(tmp);
            }
        }
    }
    return ret;
}

bool log_upload_ring_init(void)
{
    if (s_ring) return true;
    s_ring = (char *)heap_caps_calloc(1, RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        s_ring = (char *)heap_caps_calloc(1, RING_SIZE, MALLOC_CAP_8BIT);
    }
    if (!s_ring) {
        ESP_LOGE(TAG, "ring alloc failed (96KB)");
        return false;
    }
    s_orig_vprintf = esp_log_set_vprintf(ring_tee_vprintf);
    ESP_LOGI(TAG, "log ring ready (%uKB PSRAM, vprintf tee installed)",
             RING_SIZE / 1024);
    return true;
}

const char *log_upload_ring_data(void) { return s_ring; }
size_t log_upload_ring_size(void) { return RING_SIZE; }
size_t log_upload_ring_used(void) { return s_ring_used; }
size_t log_upload_ring_head(void) { return s_ring_head; }

/* ---- NVS 状态日志（8 条环形，60s/条）------------------------------ */

#define JOURNAL_NS   "cam_brk"
#define JOURNAL_CNT  "j_cnt"
#define JOURNAL_MAX  8
#define JOURNAL_KEYP "j%u"

static void journal_put(const char *rec)
{
    nvs_handle_t h;
    if (nvs_open(JOURNAL_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t jn = 0;
    nvs_get_u8(h, JOURNAL_CNT, &jn);
    char key[8];
    snprintf(key, sizeof(key), JOURNAL_KEYP, (unsigned)(jn % JOURNAL_MAX));
    nvs_set_str(h, key, rec);
    nvs_set_u8(h, JOURNAL_CNT, (uint8_t)(jn + 1));
    nvs_commit(h);
    nvs_close(h);
}

void log_upload_journal_tick(float chip_temp)
{
    static int64_t last = 0;
    int64_t now = esp_timer_get_time();
    if (now - last < 55 * 1000000LL) return;   /* 60s 节流 */
    last = now;

    char rec[160];
    snprintf(rec, sizeof(rec),
             "{\"t\":%.0f,\"heap\":%u,\"psram_l\":%u,\"temp\":%.1f}",
             (double)(now / 1000000LL),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             chip_temp);
    journal_put(rec);
}

/* ---- 上报客户端 ---------------------------------------------------- */

static volatile bool s_upload_busy = false;

/* JSON 字符串转义追加：剥离 ANSI 色码（ESP 控制台色码污染检索），
 * 转义引号/反斜杠/控制字符。返回写入长度。 */
static size_t vl_escape_append(char *dst, size_t cap, const char *src, size_t srclen)
{
    size_t o = 0;
    int ansi = 0;
    for (size_t i = 0; i < srclen && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (ansi) {                       /* ESC [ ... 参数/终结字母 */
            if (c >= 0x40 && c <= 0x7e) ansi = 0;
            continue;
        }
        if (c == 0x1b) { ansi = 1; continue; }
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { }
        else if (c == '\t') { dst[o++] = ' '; }
        else if (c < 0x20) { }
        else dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return o;
}

/* 追加一行 VictoriaLogs jsonline：{"device":..,"fw":..,"kind":..,"_msg":".."} */
static size_t vl_append_line(char *body, size_t cap, size_t blen,
                             const char *fw, const char *kind,
                             const char *msg, size_t msg_len)
{
    static const char *pre = "{\"device\":\"mibee-cam\",\"fw\":\"";
    static const char *mid = "\",\"kind\":\"";
    static const char *msgf = "\",\"_msg\":\"";
    size_t need = strlen(pre) + strlen(fw) * 2 + strlen(mid) + strlen(kind) +
                  strlen(msgf) + msg_len * 2 + 8;
    if (blen + need >= cap) return blen;   /* 容量不足：放弃该行（诊断截断可接受） */
    size_t o = blen;
    o += (size_t)snprintf(body + o, cap - o, "%s", pre);
    o += vl_escape_append(body + o, cap - o, fw, strlen(fw));
    o += (size_t)snprintf(body + o, cap - o, "%s", mid);
    o += vl_escape_append(body + o, cap - o, kind, strlen(kind));
    o += (size_t)snprintf(body + o, cap - o, "%s", msgf);
    o += vl_escape_append(body + o, cap - o, msg, msg_len);
    o += (size_t)snprintf(body + o, cap - o, "\"}\n");
    return o;
}

static void upload_task(void *arg)
{
    char *event = (char *)arg;   /* NULL = boot report；否则事件名 */

    const cam_config_t *cfg = config_get();
    if (!cfg->log_upload_en || cfg->log_upload_url[0] == '\0') {
        free(event);
        s_upload_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* 开机报告：等 STA 就绪（SAFE 态 AP-only 时 90s 超时放弃——无路由可走） */
    if (!event) {
        int waited = 0;
        while (wifi_get_state() != WIFI_STATE_STA_CONNECTED && waited < 90) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited++;
        }
        if (wifi_get_state() != WIFI_STATE_STA_CONNECTED) {
            ESP_LOGW(TAG, "boot report skipped (no STA within 90s — SAFE/AP mode?)");
            free(event);
            s_upload_busy = false;
            vTaskDelete(NULL);
            return;
        }
    }

    /* 组装 JSON 报告：诊断头 + NVS 状态日志全量 + 日志环尾（32KB） */
    nvs_handle_t h = 0;
    nvs_open(JOURNAL_NS, NVS_READONLY, &h);

    esp_reset_reason_t rr = esp_reset_reason();
    /* 断路器计数（cam_brk 命名空间与 crash_breaker 共享） */
    uint8_t cnt = 0, limp = 0, try_lan = 0;
    if (h) {
        nvs_get_u8(h, "cnt", &cnt);
        nvs_get_u8(h, "limp", &limp);
        nvs_get_u8(h, "try_lan", &try_lan);
    }

    const size_t tail_max = 32u * 1024u;
    size_t tail_len = 0;
    char *tail = NULL;
    if (s_ring && s_ring_used > 0) {
        tail_len = s_ring_used > tail_max ? tail_max : s_ring_used;
        tail = (char *)malloc(tail_len + 1);
        if (tail) {
            if (s_ring_used <= RING_SIZE) {
                /* 未回绕：head 之后是旧数据、0..head 是新数据 → 取最后 tail_len */
                size_t start = (s_ring_head > tail_len) ? s_ring_head - tail_len : 0;
                tail_len = s_ring_head - start;
                memcpy(tail, s_ring + start, tail_len);
            } else {
                /* 已回绕：head 前的 tail_len 字节即最新（跨越环尾） */
                size_t start = (s_ring_head + RING_SIZE - tail_len) % RING_SIZE;
                for (size_t i = 0; i < tail_len; i++) {
                    tail[i] = s_ring[(start + i) % RING_SIZE];
                }
            }
            tail[tail_len] = '\0';
        }
    }

    /* VictoriaLogs jsonline（NDJSON）组装：
     * 行 1 = 诊断报告（结构化字段）；行 2.. = NVS 状态日志逐条；之后 = 日志环逐行 */
    const char *fwv = esp_app_get_description()->version;
    time_t now_t = time(NULL);
    char time_iso[32] = "";
    struct tm tmv;
    if (localtime_r(&now_t, &tmv) && tmv.tm_year > 120) {
        strftime(time_iso, sizeof(time_iso), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    }

    char rep[320];
    int rep_n = snprintf(rep, sizeof(rep),
        "event=%s rst=%d panic_cnt=%u limp=%u try_lan=%u uptime_s=%.0f "
        "heap=%u fs=%u q=%u dn=%u ir_gpio=%u ir_br=%u",
        event ? event : "boot", (int)rr, cnt, limp, try_lan,
        (double)(esp_timer_get_time() / 1000000LL),
        (unsigned)esp_get_free_heap_size(),
        cfg->cam_framesize, cfg->cam_quality, cfg->day_night_mode,
        cfg->ir_gpio, cfg->ir_brightness);

    size_t body_cap = 4096 + tail_len * 3 + 512;
    char *body = (char *)malloc(body_cap);
    size_t blen = 0;
    if (body) {
        /* 行 1：诊断报告（结构化字段 + _msg 摘要） */
        blen += (size_t)snprintf(body + blen, body_cap - blen,
            "{\"_time\":\"%s\",\"_msg\":\"%.*s\",\"device\":\"mibee-cam\","
            "\"fw\":\"%s\",\"rst\":%d,\"panic_cnt\":%u,\"limp\":%u,"
            "\"try_lan\":%u,\"fs\":%u,\"q\":%u,\"dn\":%u,"
            "\"ir_gpio\":%u,\"ir_br\":%u}\n",
            time_iso, rep_n > 0 ? rep_n : 0, rep,
            fwv, (int)rr, cnt, limp, try_lan,
            cfg->cam_framesize, cfg->cam_quality, cfg->day_night_mode,
            cfg->ir_gpio, cfg->ir_brightness);

        /* 行 2..：NVS 状态日志逐条 */
        if (h) {
            uint8_t jn = 0;
            nvs_get_u8(h, JOURNAL_CNT, &jn);
            uint8_t base = (uint8_t)(jn > JOURNAL_MAX ? jn - JOURNAL_MAX : 0);
            for (uint8_t i = base; i < jn && blen < body_cap - 256; i++) {
                char key[8];
                snprintf(key, sizeof(key), JOURNAL_KEYP, (unsigned)(i % JOURNAL_MAX));
                size_t rl = 0;
                nvs_get_str(h, key, NULL, &rl);
                if (rl == 0) continue;
                char *rec = (char *)malloc(rl);
                if (!rec) continue;
                if (nvs_get_str(h, key, rec, &rl) == ESP_OK) {
                    blen = vl_append_line(body, body_cap, blen, fwv,
                                          "journal", rec, rl);
                }
                free(rec);
            }
        }

        /* 行 ..：RAM 日志环逐行（剥离 ANSI 色码由转义器完成） */
        if (tail) {
            size_t line_start = 0;
            for (size_t i = 0; i < tail_len && blen < body_cap - 256; i++) {
                if (tail[i] == '\n' || i == tail_len - 1) {
                    size_t ll = i - line_start + (tail[i] == '\n' ? 0 : 1);
                    if (ll > 2) {
                        blen = vl_append_line(body, body_cap, blen, fwv,
                                              "log", tail + line_start, ll);
                    }
                    line_start = i + 1;
                }
            }
        }
    }

    /* POST NDJSON 到 VictoriaLogs /insert/jsonline */
    esp_http_client_config_t hc = {
        .url = cfg->log_upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (cli) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_err_t err = esp_http_client_open(cli, (int)blen);
        if (err == ESP_OK) {
            if (body) esp_http_client_write(cli, body, blen);
            esp_http_client_fetch_headers(cli);
            esp_http_client_close(cli);
            ESP_LOGI(TAG, "uploaded %uB jsonline to %s (event=%s)",
                     (unsigned)blen, cfg->log_upload_url, event ? event : "boot");
        } else {
            ESP_LOGW(TAG, "upload open failed: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(cli);
    }

    free(body);
    free(tail);
    if (h) nvs_close(h);
    free(event);
    s_upload_busy = false;
    vTaskDelete(NULL);
}

static void spawn_upload(const char *event)
{
    if (s_upload_busy) return;   /* 上一次还在传 */
    s_upload_busy = true;
    char *ev = NULL;
    if (event) {
        ev = strdup(event);
        if (!ev) { s_upload_busy = false; return; }
    }
    if (xTaskCreate(upload_task, "log_up", 6144, ev, 1, NULL) != pdPASS) {
        free(ev);
        s_upload_busy = false;
    }
}

void log_upload_boot_report(void)
{
    spawn_upload(NULL);
}

void log_upload_notify_event(const char *event)
{
    if (event) spawn_upload(event);
}

bool log_upload_manual_upload(void)
{
    spawn_upload("manual");
    return true;
}
