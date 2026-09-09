/*
 * MiBee Cam — ESPectre WiFi CSI motion sensing (optional pilot module)
 *
 * Copyright (C) 2026 MiBee Cam Authors *
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
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_manager.h"
#include "espectre_sdk.h"

/* seeed 桥接（2026-09-06）：CSI MOTION 事件复用现有 WS 运动事件对
 * （motion_started/motion_cleared），SPA 的 ws.* i18n 通道零改动直接显示；
 * data 带 source:csi 供未来区分。回调上下文=runtime loop 任务（即本模块
 * 的 pump 任务，controller.loop() 在此驱动 listener），ws_broadcast
 * 为快照模式非阻塞（ws_server.c 快照后发送），满足 SDK 有界回调契约。 */
extern "C" void ws_broadcast(const char *type, const char *data);
#include "onvif_events.h"   /* 契约 v1.5：MotionAlarm 扇出（onvif_events 门控） */

static const char *TAG = "csi_motion";

/* ── 自愈参数（契约 v1.7；编译期常量，标定依据 PIT-041 soak 数据：
 * 健康校准 thr 0.38-0.92，退化期 <0.10 + 翻转 200-400/h，有人在家基线
 * 10-20/h。阈值取分界保守侧） ─────────────────────────────────── */
#define CSI_HEAL_THR_FLOOR        0.10f   /* thr 低于此视为退化（settle 崩塌特征） */
#define CSI_HEAL_FLIP_LIMIT_H     60u     /* 1h 翻转超此视为退化（误报风暴特征） */
#define CSI_HEAL_SUSTAIN_S        300     /* 条件持续 5min 才动作（防瞬时波动） */
#define CSI_HEAL_RECAL_COOLDOWN_S 1800    /* 重校准间隔 ≥30min（防风暴） */
#define CSI_HEAL_LOCK_THR         0.15f   /* 二次退化→锁定阈值（断 settle 根治） */
#define CSI_HEAL_CH_COOLDOWN_S    600     /* 信道变化重校准冷却 10min */

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

/* 契约 v1.6/v1.7：最新快照。写者 = pump 任务（on_periodic_update ~1Hz +
 * 自愈动作）与外部 setter（httpd 上下文，set_enabled 即时改 state），统一
 * portMUX；读侧（/api/status）临界区仅结构拷贝，永不阻塞感知回调。 */
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_snap_valid = false;
static csi_motion_status_t s_snap;

/* ── 观测/自愈状态：仅 pump 任务上下文 touch（listener 回调即 pump 上下文，
 * setter 里的简单标量读写为 32bit 对齐单字，无需加锁） ── */
static uint16_t s_flip_buckets[60];   /* 1h 滚动窗：60 桶 × 1min 翻转计数 */
static uint8_t  s_flip_min = 0;
static int64_t  s_flip_base_us = 0;
static bool     s_thr_locked = false; /* 手动阈值锁定中（settle 已禁用） */
static bool     s_enabled = true;     /* csi_enabled=0 时暂停感知 */
static uint8_t  s_profile = 0;        /* 0=LW 1=HA */
static uint8_t  s_last_channel = 0;
static int64_t  s_deg_since_us = 0;
static int64_t  s_last_recal_us = 0;
static uint8_t  s_heal_rounds = 0;
static int64_t  s_last_heal_check_us = 0;

static void snap_write_state(const char *st)
{
    portENTER_CRITICAL(&s_snap_mux);
    strlcpy(s_snap.state, st, sizeof(s_snap.state));
    portEXIT_CRITICAL(&s_snap_mux);
}

static void flip_advance(int64_t now_us)
{
    if (s_flip_base_us == 0) {
        s_flip_base_us = now_us;
        return;
    }
    int64_t mins = (now_us - s_flip_base_us) / 60000000LL;
    while (mins-- > 0) {
        s_flip_min = (uint8_t)((s_flip_min + 1) % 60);
        s_flip_buckets[s_flip_min] = 0;
        s_flip_base_us += 60000000LL;
    }
}

static uint16_t flip_rate_sum(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < 60; i++) sum += s_flip_buckets[i];
    return (uint16_t)(sum > 0xFFFF ? 0xFFFF : sum);
}

/* 自愈检查（pump 任务 30s 节流调用）。两级动作：
 * 退化（thr 崩塌或翻转风暴，持续 5min）→ 重校准（30min 冷却）；
 * 冷却窗内二次退化 → 锁定阈值（manual override 断 settle，PIT-041 根治）。
 * 手动锁定（csi_threshold>0）时让位用户，不自愈。 */
static void autoheal_check(int64_t now_us, bool heal_enabled)
{
    const espectre::RuntimeSnapshot &s = s_controller.snapshot();

    /* AP 信道变化（ch11→ch7 实测）→ metric 分布漂移，重校准一次 */
    if (s.link_channel != 0 && s.link_channel != s_last_channel) {
        if (s_last_channel != 0 && now_us - s_last_recal_us > CSI_HEAL_CH_COOLDOWN_S * 1000000LL) {
            ESP_LOGW(TAG, "heal: link channel %u->%u, recalibrating",
                     (unsigned)s_last_channel, (unsigned)s.link_channel);
            s_controller.trigger_recalibration();
            s_last_recal_us = now_us;
        }
        s_last_channel = s.link_channel;
    }

    if (!heal_enabled || s_thr_locked || !s_enabled ||
        s.calibrating || !s.ready_to_publish) {
        s_deg_since_us = 0;
        return;
    }

    const uint16_t flips = flip_rate_sum();
    const bool degraded = (s.threshold < CSI_HEAL_THR_FLOOR) ||
                          (flips > CSI_HEAL_FLIP_LIMIT_H);
    if (!degraded) {
        s_deg_since_us = 0;
        /* 冷却窗过后且已恢复健康 → 重置动作级数 */
        if (now_us - s_last_recal_us > CSI_HEAL_RECAL_COOLDOWN_S * 1000000LL) {
            s_heal_rounds = 0;
        }
        return;
    }
    if (s_deg_since_us == 0) {
        s_deg_since_us = now_us;
        return;
    }
    if (now_us - s_deg_since_us < CSI_HEAL_SUSTAIN_S * 1000000LL) return;
    if (now_us - s_last_recal_us < CSI_HEAL_RECAL_COOLDOWN_S * 1000000LL) return;

    s_heal_rounds++;
    if (s_heal_rounds >= 2) {
        ESP_LOGW(TAG, "heal: degradation persists after recalibration — "
                 "locking threshold to %.2f (settle disabled)", CSI_HEAL_LOCK_THR);
        if (s_controller.set_threshold_runtime(CSI_HEAL_LOCK_THR)) {
            s_thr_locked = true;
            portENTER_CRITICAL(&s_snap_mux);
            s_snap.thr = CSI_HEAL_LOCK_THR;
            s_snap.thr_locked = true;
            portEXIT_CRITICAL(&s_snap_mux);
        }
    } else {
        ESP_LOGW(TAG, "heal: thr=%.3f flip/h=%u degraded %.0fs — recalibrating",
                 s.threshold, (unsigned)flips,
                 (double)((now_us - s_deg_since_us) / 1000000LL));
        s_controller.trigger_recalibration();
    }
    s_last_recal_us = now_us;
    s_deg_since_us = 0;
}

/* Pilot listener: log + WS/ONVIF 扇出。Keep callbacks bounded and non-blocking
 * (SDK threading contract). */
class CamCsiListener : public espectre::IRuntimeListener {
public:
    void on_motion_state_changed(const espectre::RuntimeSnapshot &s) override {
        if (!s.ready_to_publish) return;
        s_flip_buckets[s_flip_min]++;   /* 翻转计数（自愈指标） */
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
        /* 重校准走 SDK 完整路径：on_startup_calibration_begin 会清
         * manual_threshold_override_ —— 手动锁定若非用户显式（自愈锁定），
         * 随重校准自动解除。自愈锁定的解除同样生效。 */
        if (s_thr_locked) {
            ESP_LOGI(TAG, "manual threshold lock released by recalibration");
        }
        s_thr_locked = false;
        s_heal_rounds = 0;
        portENTER_CRITICAL(&s_snap_mux);
        s_snap.thr_locked = false;
        s_snap.calibrating = false;
        portEXIT_CRITICAL(&s_snap_mux);
    }

    void on_periodic_update(const espectre::RuntimeSnapshot &s,
                            uint32_t packets_received) override {
        const bool armed = s_enabled;
        const char *st = !armed ? "off"
                       : s.ready_to_publish
                             ? (s.motion_state == espectre::MotionState::MOTION ? "MOTION" : "IDLE")
                             : "warming";
        /* diag 三速率：cb>>tx 即自家流量污染信号（PIT-041），进快照供
         * SPA/AT 观测。sample 由 pump 上下文持有，此处直接读安全。 */
        float tx = 0, cb = 0, adm = 0;
        const espectre::RuntimeDiagnosticsSample *d = s_controller.diagnostics_sample();
        if (d != nullptr) {
            tx = d->traffic_tx_pps; cb = d->csi_callback_pps; adm = d->csi_admitted_pps;
        }
        /* 契约 v1.6：先落快照（/api/status "csi" 字段的数据源），再广播。 */
        portENTER_CRITICAL(&s_snap_mux);
        s_snap_valid = true;
        strlcpy(s_snap.state, st, sizeof(s_snap.state));
        s_snap.score = s.movement_metric;
        s_snap.thr = s.threshold;
        s_snap.profile = s_profile;
        s_snap.thr_locked = s_thr_locked;
        s_snap.calibrating = s.calibrating;
        s_snap.flip_rate = flip_rate_sum();
        s_snap.tx_pps = tx; s_snap.cb_pps = cb; s_snap.adm_pps = adm;
        portEXIT_CRITICAL(&s_snap_mux);
        /* 契约 §6 v1.4：csi_status 心跳（仅 CSI 门控板，~1s 一发；v1.7 增补
         * profile/thr_locked/calibrating/flip_rate，向后兼容）。快照式
         * 非阻塞；score/thr 为 0-1 精细分值，供 SPA 直读实时状态。 */
        char ws_status[160];
        snprintf(ws_status, sizeof(ws_status),
                 "{\"state\":\"%s\",\"score\":%.2f,\"thr\":%.2f,"
                 "\"profile\":%u,\"locked\":%s,\"calibrating\":%s,\"flip_rate\":%u}",
                 st, s.movement_metric, s.threshold,
                 (unsigned)s_profile, s_thr_locked ? "true" : "false",
                 s.calibrating ? "true" : "false", (unsigned)flip_rate_sum());
        ws_broadcast("csi_status", ws_status);

        if (d != nullptr) {
            ESP_LOGI(TAG,
                     "status: state=%s score=%.2f pkts=%u cal=%u/%u prof=%d flip/h=%u | "
                     "diag tx=%.1f cb=%.1f cls=%.1f rej=%.1f acc=%.1f adm=%.1f filt=%.1f",
                     st,
                     s.movement_metric, (unsigned)packets_received,
                     (unsigned)s.calibration_packets,
                     (unsigned)s.calibration_target_packets,
                     (int)s.csi_capture_profile,
                     (unsigned)flip_rate_sum(),
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
    memset(s_flip_buckets, 0, sizeof(s_flip_buckets));
    /* 开机应用 csi_* 配置（契约 v1.7；幂等） */
    csi_motion_apply_config();
    ESP_LOGI(TAG, "ESPectre sensing started (pps=%u mode=%d)",
             (unsigned)config.csi_target_pps, (int)config.csi_traffic_mode);
    while (true) {
        s_controller.loop();
        const int64_t now_us = esp_timer_get_time();
        flip_advance(now_us);
        if (now_us - s_last_heal_check_us > 30000000LL) {   /* 30s 节流 */
            s_last_heal_check_us = now_us;
            const cam_config_t *cfg = config_get();
            autoheal_check(now_us, cfg != nullptr && cfg->csi_auto_heal != 0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} /* namespace */

bool csi_motion_get_status(csi_motion_status_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_snap_mux);
    const bool valid = s_snap_valid;
    if (valid) *out = s_snap;
    portEXIT_CRITICAL(&s_snap_mux);
    return valid;
}

esp_err_t csi_motion_init(void)
{
    if (xTaskCreatePinnedToCore(csi_motion_task, "csi_motion", 6144,
                                nullptr, 1, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create csi_motion task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── 契约 v1.7：运行时调参面。controller setter 为 frontend 语义
 * （httpd 上下文安全）；简单标量同步进 pump 侧状态。 ── */

esp_err_t csi_motion_set_threshold(float thr)
{
    if (thr < 0.0f || thr > 1.0f) return ESP_ERR_INVALID_ARG;
    if (thr == 0.0f) {
        /* 恢复自动：重校准走 SDK 完整路径（清 manual override + 恢复 settle） */
        if (!s_controller.trigger_recalibration()) return ESP_FAIL;
        s_thr_locked = false;
        ESP_LOGI(TAG, "threshold: auto (recalibrating)");
        return ESP_OK;
    }
    if (!s_controller.set_threshold_runtime(thr)) return ESP_FAIL;
    s_thr_locked = true;
    portENTER_CRITICAL(&s_snap_mux);
    s_snap.thr = thr;
    s_snap.thr_locked = true;
    portEXIT_CRITICAL(&s_snap_mux);
    ESP_LOGI(TAG, "threshold: locked at %.2f (settle disabled)", thr);
    return ESP_OK;
}

esp_err_t csi_motion_set_motion_hits(uint8_t on_hits, uint8_t off_hits)
{
    if (on_hits < 1 || on_hits > 20 || off_hits < 1 || off_hits > 20) {
        return ESP_ERR_INVALID_ARG;
    }
    return s_controller.set_motion_hits_runtime(on_hits, off_hits) ? ESP_OK : ESP_FAIL;
}

esp_err_t csi_motion_set_profile(uint8_t profile)
{
    if (profile > 1) return ESP_ERR_INVALID_ARG;
    const espectre::DetectionAlgorithm algo = profile == 1
        ? espectre::DetectionAlgorithm::HIGH_ACCURACY
        : espectre::DetectionAlgorithm::LIGHTWEIGHT;
    if (!s_controller.set_detection_algorithm_runtime(algo)) return ESP_FAIL;
    s_profile = profile;
    s_thr_locked = false;   /* 换检测器重校准，锁定态随之解除 */
    portENTER_CRITICAL(&s_snap_mux);
    s_snap.profile = profile;
    s_snap.thr_locked = false;
    portEXIT_CRITICAL(&s_snap_mux);
    ESP_LOGI(TAG, "detector profile: %s", profile == 1 ? "high-accuracy" : "lightweight");
    return ESP_OK;
}

esp_err_t csi_motion_set_enabled(bool enabled)
{
    /* armed=false：SDK 语义为暂停感知服务（不动 WiFi），事件停发 */
    s_controller.set_services_armed(enabled);
    s_enabled = enabled;
    if (!enabled) snap_write_state("off");
    ESP_LOGI(TAG, "sensing %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t csi_motion_recalibrate(void)
{
    return s_controller.trigger_recalibration() ? ESP_OK : ESP_FAIL;
}

void csi_motion_apply_config(void)
{
    const cam_config_t *cfg = config_get();
    if (cfg == nullptr) return;
    csi_motion_set_enabled(cfg->csi_enabled != 0);
    /* threshold=0 是"自动"默认：开机跳过（避免无谓重校准）；用户显式
     * 从锁定改回 0 时由 config 写路径调 set_threshold(0) 走恢复语义 */
    if (cfg->csi_threshold > 0.0f) csi_motion_set_threshold(cfg->csi_threshold);
    csi_motion_set_motion_hits(cfg->csi_on_hits, cfg->csi_off_hits);
    if (cfg->csi_profile != s_profile) csi_motion_set_profile(cfg->csi_profile);
}

#else /* !CONFIG_MIBEE_CSI_MOTION */

esp_err_t csi_motion_init(void)
{
    return ESP_OK;
}

bool csi_motion_get_status(csi_motion_status_t *out)
{
    (void)out;
    return false;
}

esp_err_t csi_motion_set_threshold(float thr)    { (void)thr; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t csi_motion_set_motion_hits(uint8_t on, uint8_t off)
{ (void)on; (void)off; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t csi_motion_set_profile(uint8_t profile){ (void)profile; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t csi_motion_set_enabled(bool enabled)   { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t csi_motion_recalibrate(void)           { return ESP_ERR_NOT_SUPPORTED; }
void csi_motion_apply_config(void)               {}

#endif /* CONFIG_MIBEE_CSI_MOTION */
