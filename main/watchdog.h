/*
 * MiBee Cam — 家族共享任务看门狗核心（四仓 md5 锁文件）
 *
 * Copyright (C) 2024-2026 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 能力契约（api-contract v1.10 §5 wdt 对象）：
 *  - 订阅制：长周期任务 watchdog_register_current() 挂上 ESP-IDF TWDT，
 *    循环顶部 watchdog_feed_current()；卡死即停喂，TWDT 到期触发
 *    （CONFIG_ESP_TASK_WDT_PANIC=y 时复位并吐 backtrace——n16#26 类
 *    静默 wedge 的自愈与诊断路径）。
 *  - 观测面：/api/status 挂 "wdt" 对象（enabled/panic/timeout_s/tasks[]），
 *    feeds 计数仅供诊断（宽松计数，非精确）。
 *  - 本模块只做登记与喂狗包装，不改变 TWDT 本身的配置（sdkconfig 所有）。
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "cJSON.h"
#include "esp_err.h"

/** 上电早期调用：横幅打印 TWDT 配置（含 PANIC 与否），清空注册表。 */
void watchdog_init(void);

/**
 * @brief 把当前任务挂上 TWDT 并入注册表（幂等）。
 * @param name 展示名（≤15 字符，超长截断）。
 * @return ESP_OK；订阅失败只告警并返回错误码，绝不致命。
 */
esp_err_t watchdog_register_current(const char *name);

/** 喂狗 + 计数。仅对已注册的调用任务生效，其余为空操作。 */
void watchdog_feed_current(void);

/** 向 /api/status 的 data 对象挂 "wdt" 子对象（契约 v1.10）。 */
void watchdog_attach_status(cJSON *data);

#endif /* WATCHDOG_H */
