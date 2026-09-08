/*
 * MiBee Cam — ESPectre WiFi CSI motion sensing (optional pilot module)
 *
 * Copyright (C) 2026 MiBee Cam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ESPectre SDK part is GPL-3.0-only (components/espectre/LICENSE), so the
 * combined firmware is distributed under GPLv3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "csi_motion.h"

#if CONFIG_MIBEE_CSI_MOTION

#include <cstdarg>
#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espectre_sdk.h"

/* seeed 桥接（2026-09-06）：CSI MOTION 事件复用现有 WS 运动事件对
 * （motion_started/motion_cleared），SPA 的 ws.* i18n 通道零改动直接显示；
 * data 带 source:csi 供未来区分。回调上下文=runtime loop 任务，ws_broadcast
 * 为快照模式非阻塞（ws_server.c 快照后发送），满足 SDK 有界回调契约。 */
extern "C" void ws_broadcast(const char *type, const char *data);
#include "onvif_events.h"   /* 契约 v1.5：MotionAlarm 扇出（onvif_events 门控） */

static const char *TAG = "csi_motion";

/* Route SDK-internal ESPECTRE_LOGx into the firmware log. W/E only — the
 * runtime's own INFO heartbeat would duplicate the listener heartbeat. */
static bool espectre_log_enabled(void *ctx, espectre::LogLevel level, const char *tag)
{
    (void)ctx; (void)tag;
    return level <= espectre::LogLevel::WARNING;
}

static void espectre_log_write(void *ctx, espectre::LogLevel level, const char *tag,
                               int line, const char *format, va_list args)
{
    (void)ctx;
    char buf[192];
    vsnprintf(buf, sizeof(buf), format, args);
    ESP_LOGW(TAG, "[espectre %s:%d] %s", tag ? tag : "?", line, buf);
}

namespace {

espectre::RuntimeFrontendController s_controller;

/* Pilot listener: log-only. Keep callbacks bounded and non-blocking
 * (SDK threading contract) — real actions (webhook/event_bus) come later. */
class CamCsiListener : public espectre::IRuntimeListener {
public:
    void on_motion_state_changed(const espectre::RuntimeSnapshot &s) override {
        if (!s.ready_to_publish) return;
        ESP_LOGI(TAG, "motion=%s score=%.2f thr=%.2f rssi=%d ch=%u",
                 s.motion_state == espectre::MotionState::MOTION ? "MOTION" : "IDLE",
                 s.movement_metric, s.threshold,
                 (int)s.link_rssi_dbm, (unsigned)s.link_channel);
        /* 契约 §6：motion_* 的 score 为家族 0-100 刻度（与像素运动一致），
         * source:csi 供 UI 区分来源；CSI 精细分值走 csi_status 心跳。 */
        char ws_data[64];
        snprintf(ws_data, sizeof(ws_data), "{\"score\":%.0f,\"source\":\"csi\"}",
                 s.movement_metric * 100.0f);
        ws_broadcast(s.motion_state == espectre::MotionState::MOTION
                         ? "motion_started" : "motion_cleared",
                     ws_data);
        /* 契约 v1.5：同一状态转移扇出到 ONVIF MotionAlarm（NVR 联动） */
        onvif_events_motion(s.motion_state == espectre::MotionState::MOTION,
                            (uint8_t)(s.movement_metric * 100.0f + 0.5f));
    }

    void on_calibration_started(const espectre::RuntimeSnapshot &s) override {
        ESP_LOGI(TAG, "calibration started (target=%u pkts)",
                 (unsigned)s.calibration_target_packets);
    }

    void on_calibration_finished(const espectre::RuntimeSnapshot &s, bool success) override {
        ESP_LOGI(TAG, "calibration %s (thr=%.2f)",
                 success ? "OK" : "FAILED", s.threshold);
    }

    void on_periodic_update(const espectre::RuntimeSnapshot &s,
                            uint32_t packets_received) override {
        const char *st = s.ready_to_publish
                             ? (s.motion_state == espectre::MotionState::MOTION ? "MOTION" : "IDLE")
                             : "warming";
        /* 契约 §6 v1.4：csi_status 心跳（仅 CSI 门控板，~1s 一发）。快照式
         * 非阻塞；score/thr 为 0-1 精细分值，供 SPA 直读实时状态。 */
        char ws_status[96];
        snprintf(ws_status, sizeof(ws_status),
                 "{\"state\":\"%s\",\"score\":%.2f,\"thr\":%.2f}",
                 st, s.movement_metric, s.threshold);
        ws_broadcast("csi_status", ws_status);

        const espectre::RuntimeDiagnosticsSample *d = s_controller.diagnostics_sample();
        if (d != nullptr) {
            ESP_LOGI(TAG,
                     "status: state=%s score=%.2f pkts=%u cal=%u/%u prof=%d | "
                     "diag tx=%.1f cb=%.1f cls=%.1f rej=%.1f acc=%.1f adm=%.1f filt=%.1f",
                     st,
                     s.movement_metric, (unsigned)packets_received,
                     (unsigned)s.calibration_packets,
                     (unsigned)s.calibration_target_packets,
                     (int)s.csi_capture_profile,
                     d->traffic_tx_pps, d->csi_callback_pps, d->csi_classified_pps,
                     d->csi_provenance_rejected_pps, d->csi_accepted_pps,
                     d->csi_admitted_pps, d->csi_filtered_pps);
        } else {
            ESP_LOGI(TAG, "status: state=%s pkts=%u (no diag)",
                     st,
                     (unsigned)packets_received);
        }
    }

    void on_runtime_fault(const char *message) override {
        ESP_LOGW(TAG, "runtime fault: %s", message);
    }
};

CamCsiListener s_listener;

/* Single-owner pump task per SDK threading contract. Core 1 prio 1:
 * lowest user task there, below the streamers (prio 2) — sensing is
 * debounced over seconds, never latency-critical. */
void csi_motion_task(void *unused)
{
    (void)unused;
    espectre::LogSink sink;
    sink.enabled = espectre_log_enabled;
    sink.write = espectre_log_write;
    espectre::set_log_sink(sink);
    espectre::RuntimeConfig config = espectre::make_runtime_sensing_config_from_kconfig();
    config.device_id = espectre::derive_runtime_device_id();
    s_controller.set_config(config);
    if (!s_controller.setup(&s_listener)) {
        ESP_LOGE(TAG, "ESPectre setup failed — sensing disabled");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "ESPectre sensing started (pps=%u mode=%d)",
             (unsigned)config.csi_target_pps, (int)config.csi_traffic_mode);
    while (true) {
        s_controller.loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} /* namespace */

esp_err_t csi_motion_init(void)
{
    if (xTaskCreatePinnedToCore(csi_motion_task, "csi_motion", 6144,
                                nullptr, 1, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create csi_motion task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

#else /* !CONFIG_MIBEE_CSI_MOTION */

esp_err_t csi_motion_init(void)
{
    return ESP_OK;
}

#endif /* CONFIG_MIBEE_CSI_MOTION */
