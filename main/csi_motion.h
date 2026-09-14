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
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the ESPectre sensing runtime (single-owner task). No-op unless
 * CONFIG_MIBEE_CSI_MOTION is set. Must be called after wifi_init() — the
 * runtime hooks WIFI_EVENT/IP_EVENT itself and handles the already-connected
 * case.
 */
esp_err_t csi_motion_init(void);

/* 契约 v1.7：最新感知快照（GET /api/status 的 "csi" 字段，与 /ws csi_status
 * 心跳同源）。CSI 未编译或运行时尚未产出首个周期更新时返回 false。
 * portMUX 单写者快照，非阻塞，可在任意任务上下文调用。
 * v1.6 字段（state/score/thr）向后兼容不变；v1.7 增补诊断字段。 */
typedef struct {
    char  state[10];   /* "MOTION" / "IDLE" / "warming" / "off"（v1.7） */
    float score;       /* 0-1 精细分值 */
    float thr;         /* 0-1 判决阈值 */
    /* ── v1.7 增补（调参/自愈可观测面；CSI-off 板恒缺省） ── */
    uint8_t profile;      /* 检测档：0=Lightweight 1=High-Accuracy */
    bool    thr_locked;   /* true=手动阈值锁定（settle 单边下调已禁用） */
    bool    calibrating;  /* 启动校准进行中（判定暂无效） */
    uint16_t flip_rate;   /* 近 1h 状态翻转次数（自愈触发指标之一） */
    float   tx_pps;       /* 生成器流量速率（自测 ping） */
    float   cb_pps;       /* CSI 回调速率（含板上业务流量；cb>>tx=污染） */
    float   adm_pps;      /* 进检测器时间栅格的速率 */
} csi_motion_status_t;

bool csi_motion_get_status(csi_motion_status_t *out);

/* ── 契约 v1.7：运行时调参面（热生效，无需重启） ──────────────────
 * CSI 未编译（CONFIG_MIBEE_CSI_MOTION=n）或运行时未就绪：
 * 查询类返回 false / 设置类返回 ESP_ERR_NOT_SUPPORTED。
 * 全部幂等；生效值同时持久化由调用方（config 层）负责。 */

/* 0.0=恢复自动（触发重校准、恢复 settle）；(0,1]=手动锁定阈值并禁用
 * settle 单边下调（PIT-041 误报根因的根治开关）。 */
esp_err_t csi_motion_set_threshold(float thr);
/* 去抖命中数：on=连续超阈 N 次判 MOTION，off=连续低于 N 次判 IDLE（1-20） */
esp_err_t csi_motion_set_motion_hits(uint8_t on_hits, uint8_t off_hits);
/* 检测档热切换：0=Lightweight（自适应阈值） 1=High-Accuracy（ML 固定阈值，
 * 结构上无 settle 退化路径；切换后自动重校准） */
esp_err_t csi_motion_set_profile(uint8_t profile);
/* 感知启停（false=暂停 CSI 采样与判定，不动 WiFi 连接；事件停发） */
esp_err_t csi_motion_set_enabled(bool enabled);
/* 立即触发重校准（背景执行；完成后事件恢复） */
esp_err_t csi_motion_recalibrate(void);
/* 从 config 的 csi_* 键热应用全部参数（开机 csi_motion_init 后与
 * POST /api/config 变更后调用；幂等；csi_threshold=0 跳过阈值项，
 * 仅用户显式写 0 时经 set_threshold(0) 走恢复-自动语义） */
void csi_motion_apply_config(void);

#ifdef __cplusplus
}
#endif
