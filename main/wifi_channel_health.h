/*
 * wifi_channel_health.h — MiBee Cam 家族 Wi-Fi 信道健康感知（契约 v1.7 ①b）
 *
 * Copyright (C) 2026 MiBee Cam Authors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CSI 无关的共享子系统：STA 链路质量采样（RSSI/断连）+ 低频 scan 信道
 * 拥塞估计 + （CSI 门控板）admitted 率探针。第一阶段只感知+报告
 * （/api/status 的 wifi.chan_health 对象 + AT+CHHEALTH?），不做自动
 * 切网/换信道动作——自动动作待 soak 数据标定后放开（根 AGENTS.md 路线②）。
 *
 * 红线：STA 关联期间绝不调 esp_wifi_set_channel（信道由 AP 决定）；
 * scan 有 ~2s 射频离线成本，仅在无活跃观众/非录像时执行（PIT-038 教训）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动采样任务（60s 周期 RSSI + 1h 断连滑窗 + 60min 低频 scan）。
 * 幂等；内部自订阅 WIFI_EVENT（不侵入 wifi_manager）。 */
esp_err_t wifi_channel_health_init(void);

/* 最新健康快照（/api/status "wifi.chan_health" 数据源；未初始化返回 false） */
typedef struct {
    int8_t   rssi_avg;       /* 近 5min RSSI 均值（dBm；未连接 INT8_MIN） */
    int8_t   rssi_min;       /* 近 5min RSSI 最差值 */
    uint8_t  channel;        /* 当前关联信道（0=未知） */
    uint16_t disconnects_1h; /* 近 1h 断连次数（含重连风暴特征） */
    /* scan 面（60min 低频 + 手动触发；scan_ts 为上次成功 scan 的 epoch 秒，
     * 0=尚无数据） */
    uint32_t scan_ts;
    uint8_t  bss_on_chan;    /* 当前信道上可见 BSS 数（拥塞代理指标） */
    uint8_t  bss_total;      /* 全信道 BSS 总数 */
    uint8_t  busy_score;     /* 0-100 拥塞评分（当前信道 BSS 加权 RSSI 占用估计） */
    /* CSI 探针（仅 CONFIG_MIBEE_CSI_MOTION 板填充；其余恒 0） */
    float    csi_adm_pps;    /* 检测器时间栅格 admitted 率（目标 10） */
    float    csi_cb_ratio;   /* cb/tx 比值（≫2 = 自家流量污染，PIT-046） */
} wifi_chan_health_t;

bool wifi_channel_health_get(wifi_chan_health_t *out);

/* 手动触发一次 scan 拥塞测量（AT+CHHEALTH=SCAN / 调试用；遵循同样的
 * 观众/录像避让，不满足条件返回 ESP_ERR_INVALID_STATE） */
esp_err_t wifi_channel_health_scan_now(void);

#ifdef __cplusplus
}
#endif
