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

    /* NVS 状态日志按时间序拼接（%.*s 精度限界，避免 truncation 警告） */
    char journal[1600] = "";
    if (h) {
        uint8_t jn = 0;
        nvs_get_u8(h, JOURNAL_CNT, &jn);
        uint8_t base = (uint8_t)(jn > JOURNAL_MAX ? jn - JOURNAL_MAX : 0);
        size_t off = 0;
        for (uint8_t i = base; i < jn && off < sizeof(journal) - 40; i++) {
            char key[8];
            snprintf(key, sizeof(key), JOURNAL_KEYP, (unsigned)(i % JOURNAL_MAX));
            size_t rl = 0;
            nvs_get_str(h, key, NULL, &rl);
            if (rl == 0) continue;
            char *rec = (char *)malloc(rl);
            if (!rec) continue;
            if (nvs_get_str(h, key, rec, &rl) == ESP_OK) {
                size_t room = sizeof(journal) - off - 2;
                if (rl > room) rl = room;
                if (rl > 0) {
                    if (off) { journal[off++] = ','; room--; }
                    if (rl > room) rl = room;
                    memcpy(journal + off, rec, rl);
                    off += rl;
                    journal[off] = '\0';
                }
            }
            free(rec);
        }
    }

    /* 报告组装：head_pre + journal + head_post + tail（分段 memcpy，
     * 不用 %s 插大参数——-Wformat-truncation 会拦 head 小缓冲） */
    char head_pre[384];
    snprintf(head_pre, sizeof(head_pre),
        "{\"fw\":\"%s\",\"rst\":%d,\"panic_cnt\":%u,\"limp\":%u,\"try_lan\":%u,"
        "\"uptime_s\":%.0f,\"heap\":%u,\"fs\":%u,\"q\":%u,\"dn\":%u,"
        "\"ir_gpio\":%u,\"ir_br\":%u,\"event\":\"%s\",\"journal\":[",
        esp_app_get_description()->version, (int)rr, cnt, limp, try_lan,
        (double)(esp_timer_get_time() / 1000000LL),
        (unsigned)esp_get_free_heap_size(),
        cfg->cam_framesize, cfg->cam_quality, cfg->day_night_mode,
        cfg->ir_gpio, cfg->ir_brightness,
        event ? event : "boot");
    const char *head_post = "],\"log_tail\":\"";
    size_t journal_len = strlen(journal);
    if (journal_len > 1200) journal_len = 1200;   /* 诊断截断上限 */

    size_t pre_len = strlen(head_pre);
    size_t post_len = strlen(head_post);
    size_t body_len = pre_len + journal_len + post_len +
                      (tail ? tail_len * 6 : 0) + 16;  /* 保守 6x 转义上界 */
    char *body = (char *)malloc(body_len);
    size_t blen = 0;
    if (body) {
        memcpy(body, head_pre, pre_len); blen = pre_len;
        memcpy(body + blen, journal, journal_len); blen += journal_len;
        memcpy(body + blen, head_post, post_len); blen += post_len;
        if (tail) {
            for (size_t i = 0; i < tail_len && blen < body_len - 8; i++) {
                char c = tail[i];
                if (c == '"' || c == '\\') { body[blen++] = '\\'; }
                if (c == '\n') { body[blen++] = '\\'; body[blen++] = 'n'; }
                else if (c == '\r') { }
                else if (c == '\t') { body[blen++] = ' '; }
                else if ((unsigned char)c < 0x20) { }
                else { body[blen++] = c; }
                (void)c;
            }
        }
        body[blen++] = '"';
        body[blen++] = '}';
        body[blen] = '\0';
    }

    esp_http_client_config_t hc = {
        .url = cfg->log_upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    bool ok = false;
    if (cli) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_header(cli, "X-MiBee-Device", "mibee-cam");
        esp_err_t err = esp_http_client_open(cli, (int)blen);
        if (err == ESP_OK) {
            if (body) esp_http_client_write(cli, body, blen);
            int status = esp_http_client_fetch_headers(cli);
            status = esp_http_client_is_chunked_response(cli) ? status : status;
            esp_http_client_close(cli);
            ok = (status >= 0);
            ESP_LOGI(TAG, "uploaded %uB to log endpoint (status=%d, event=%s)",
                     (unsigned)blen, status, event ? event : "boot");
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
