/*
 * wifi_channel_health.c — Wi-Fi 信道健康感知实现（家族共享，CSI 无关）
 *
 * Copyright (C) 2026 MiBee Cam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * 采样模型：
 *  - RSSI：60s 周期读 esp_wifi_sta_get_ap_info，5min 环形（300 槽过重，
 *    用 5 槽×60s 滑动均值代理；min 保留全窗最小）。
 *  - 断连：WIFI_EVENT STA 断开事件计数，60min 滚动（60 桶×60s，同
 *    csi_motion 的翻转桶模式）。
 *  - scan：60min 周期 + 手动触发。执行条件（PIT-038：scan 断流 ~2s）：
 *    STA 已连接 && 无 MJPEG 观众 && 未在录像。统计当前信道 BSS 数与
 *    全信道总数；busy_score = 当前信道各 BSS 的 RSSI 权重占用和（每 BSS
 *    贡献 ~100+RSSI/40，钳 0-100），是"同信道竞争强度"的粗代理。
 *  - CSI 探针：门控板从 csi_motion_get_status 读取（零耦合），非门控板
 *    字段恒 0。
 */
#include "wifi_channel_health.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "csi_motion.h"
#include "mjpeg_streamer.h"   /* 观众/录像避让（PIT-038：scan 断流 ~2s） */
#include "video_recorder.h"

static const char *TAG = "chan_health";

#define CH_SAMPLE_PERIOD_S   60      /* RSSI 采样周期 */
#define CH_RSSI_WINDOW       5       /* 5 槽 × 60s */
#define CH_DISCONNECT_BUCKETS 60     /* 60min 滚动 */
#define CH_SCAN_PERIOD_S     3600    /* 低频 scan */
#define CH_TASK_STACK        4096

/* 快照（portMUX 单写者=采样任务；/api/status 读者任意上下文） */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_valid = false;
static wifi_chan_health_t s_snap;

/* 仅采样任务上下文 */
static int8_t   s_rssi_win[CH_RSSI_WINDOW];
static uint8_t  s_rssi_idx = 0, s_rssi_n = 0;
static uint16_t s_disc_buckets[CH_DISCONNECT_BUCKETS];
static uint8_t  s_disc_min = 0;
static int64_t  s_disc_base_us = 0;
static int64_t  s_last_scan_us = -((int64_t)CH_SCAN_PERIOD_S * 1000000LL);
static volatile bool s_scan_requested = false;

static void disc_advance(int64_t now_us)
{
    if (s_disc_base_us == 0) {
        s_disc_base_us = now_us;
        return;
    }
    int64_t mins = (now_us - s_disc_base_us) / 60000000LL;
    while (mins-- > 0) {
        s_disc_min = (uint8_t)((s_disc_min + 1) % CH_DISCONNECT_BUCKETS);
        s_disc_buckets[s_disc_min] = 0;
        s_disc_base_us += 60000000LL;
    }
}

static uint16_t disc_sum(void)
{
    uint32_t s = 0;
    for (int i = 0; i < CH_DISCONNECT_BUCKETS; i++) s += s_disc_buckets[i];
    return (uint16_t)(s > 0xFFFF ? 0xFFFF : s);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED ||
                               id == WIFI_EVENT_STA_BEACON_TIMEOUT)) {
        /* 桶写者=事件任务；与采样任务的分钟推进不同时发生（推进持
           s_mux 期间事件侧只做单槽 ++，32bit 对齐原子，可接受竞界） */
        s_disc_buckets[s_disc_min]++;
    }
}

/* scan 拥塞测量：返回是否成功执行（条件不满足返回 false 不报错） */
static bool scan_congestion(wifi_chan_health_t *snap)
{
    /* 避让：活跃推流/录像期间不做（PIT-038：scan 断流 ~2s） */
    if (recorder_get_state() == RECORDER_RECORDING) {
        ESP_LOGI(TAG, "scan skipped: recording active");
        return false;
    }
    if (mjpeg_streamer_client_count() > 0) {
        ESP_LOGI(TAG, "scan skipped: %d stream client(s) watching",
                 mjpeg_streamer_client_count());
        return false;
    }
    wifi_scan_config_t cfg = { .show_hidden = false };
    esp_err_t ret = esp_wifi_scan_start(&cfg, true /*blocking*/);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(ret));
        return false;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return false;
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * n);
    if (!recs) { esp_wifi_clear_ap_list(); return false; }
    esp_wifi_scan_get_ap_records(&n, recs);

    uint8_t cur_chan = snap->channel;
    uint8_t on_chan = 0;
    uint16_t busy = 0;
    for (int i = 0; i < n; i++) {
        if (recs[i].primary == cur_chan) {
            on_chan++;
            /* 每 BSS 占用代理：强信号 AP 贡献更多信道时间 */
            int32_t contrib = 100 + (int32_t)recs[i].rssi / 2;  /* -90→55, -50→75 */
            busy += contrib > 0 ? (uint16_t)contrib : 0;
        }
    }
    free(recs);
    portENTER_CRITICAL(&s_mux);
    s_snap.bss_on_chan = on_chan;
    s_snap.bss_total = (uint8_t)(n > 255 ? 255 : n);
    s_snap.busy_score = (uint8_t)(on_chan == 0 ? 0 : (busy / on_chan > 100 ? 100 : busy / on_chan));
    s_snap.scan_ts = (uint32_t)(time(NULL));
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "scan: chan=%u bss_on_chan=%u total=%u busy=%u%%",
             cur_chan, on_chan, n, s_snap.busy_score);
    return true;
}

static void chan_health_task(void *unused)
{
    (void)unused;
    int64_t next_sample_us = 0;
    while (true) {
        const int64_t now_us = esp_timer_get_time();
        disc_advance(now_us);

        if (now_us >= next_sample_us) {
            next_sample_us = now_us + CH_SAMPLE_PERIOD_S * 1000000LL;
            wifi_ap_record_t ap;
            const bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            if (connected) {
                s_rssi_win[s_rssi_idx] = ap.rssi;
                s_rssi_idx = (uint8_t)((s_rssi_idx + 1) % CH_RSSI_WINDOW);
                if (s_rssi_n < CH_RSSI_WINDOW) s_rssi_n++;
            }
            int32_t sum = 0;
            int8_t mn = 0;
            for (uint8_t i = 0; i < s_rssi_n; i++) {
                sum += s_rssi_win[i];
                if (mn == 0 || s_rssi_win[i] < mn) mn = s_rssi_win[i];
            }
            portENTER_CRITICAL(&s_mux);
            s_valid = true;
            if (connected) {
                s_snap.rssi_avg = s_rssi_n ? (int8_t)(sum / s_rssi_n) : ap.rssi;
                s_snap.rssi_min = mn;
                s_snap.channel = ap.primary;
            } else {
                s_snap.rssi_avg = INT8_MIN;
                s_snap.rssi_min = INT8_MIN;
                s_snap.channel = 0;
            }
            s_snap.disconnects_1h = disc_sum();
            /* CSI 探针（门控板才有值；CSI-off 恒 0） */
            csi_motion_status_t csi;
            if (csi_motion_get_status(&csi)) {
                s_snap.csi_adm_pps = csi.adm_pps;
                s_snap.csi_cb_ratio = csi.tx_pps > 0.01f ? csi.cb_pps / csi.tx_pps : 0.0f;
            }
            portEXIT_CRITICAL(&s_mux);
        }

        const bool scan_due = (now_us - s_last_scan_us) >= (int64_t)CH_SCAN_PERIOD_S * 1000000LL;
        if (scan_due || s_scan_requested) {
            if (scan_congestion(&s_snap)) {
                s_last_scan_us = now_us;
            } else {
                /* 条件不满足时 5min 后重试，不空转 */
                s_last_scan_us = now_us - (int64_t)(CH_SCAN_PERIOD_S - 300) * 1000000LL;
            }
            s_scan_requested = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t wifi_channel_health_init(void)
{
    static bool s_inited = false;
    if (s_inited) return ESP_OK;

    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.rssi_avg = INT8_MIN;
    s_snap.rssi_min = INT8_MIN;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));
    if (xTaskCreate(chan_health_task, "chan_health", CH_TASK_STACK,
                    nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    s_inited = true;
    ESP_LOGI(TAG, "channel health sensing started");
    return ESP_OK;
}

bool wifi_channel_health_get(wifi_chan_health_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_mux);
    const bool valid = s_valid;
    if (valid) *out = s_snap;
    portEXIT_CRITICAL(&s_mux);
    return valid;
}

esp_err_t wifi_channel_health_scan_now(void)
{
    if (recorder_get_state() == RECORDER_RECORDING ||
        mjpeg_streamer_client_count() > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_requested = true;
    return ESP_OK;
}
