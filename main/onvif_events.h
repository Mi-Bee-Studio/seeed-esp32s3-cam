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

#ifndef ONVIF_EVENTS_H
#define ONVIF_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file onvif_events.h
 * @brief ONVIF Pull-Point 事件服务（契约 v1.5，NVR 运动报警联动）。
 *
 * 最小 WS-BaseNotification Pull-Point 实现：NVR 向 /onvif/events_service
 * POST CreatePullPointSubscription 建立订阅，随后循环 PullMessages 拉取。
 * CSI 运动状态转移入队为 tns1:VideoSource/MotionAlarm 通知
 * （Source=CSI、State、Score 0-100 家族刻度）——NVR 据此触发"有人才拍摄"
 * 式录像（契约文档 §onvif-events）。
 *
 * 设计约束：
 *  - 单订阅模型：新的 CreatePullPointSubscription 顶替旧订阅；
 *    SUB_IDLE_TIMEOUT_S 内无 PullMessages 自动过期（NVR 重连重建）。
 *  - 无长轮询：PullMessages 立即返回（esp httpd worker 绝不阻塞），
 *    NVR 侧轮询节奏自定。
 *  - 服务常注册（同商用相机）；是否“生成运动报警”由配置键 onvif_events
 *    门控（默认关）——即 NVR 世界里的"运动侦测开关"。
 *  - 生产者上下文 = ESPectre 回调：非阻塞，互斥锁竞争时丢弃事件。
 */

/* CSI 运动状态转移（active=进入运动；score 0-100 家族刻度）。
 * ESPectre 回调上下文可安全调用（非阻塞）。 */
void onvif_events_motion(bool active, uint8_t score);

/* 当前是否存在有效订阅（诊断用）。 */
bool onvif_events_subscribed(void);

/* 注册 /onvif/events_service（由 onvif_register_handlers 调用）。 */
esp_err_t onvif_events_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* ONVIF_EVENTS_H */
