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

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 崩溃断路器（PIT-062 自愈，2026-11-10）。
 *
 * 背景：捕获管线存在潜伏堆越界写（FreeRTOS 定时器队列被腐坏 → LoadProhibited
 * 复位循环），触发与"生产捕获档（HD/自动夜视/IR）+ 局域网流客户端负载"强相关；
 * 编队管理面（Steward 心跳/NVR 拉流）保证设备每次重启 ~10 秒内就重新带负载，
 * 循环无法自愈。本模块不解决越界者本体（需 heap-trace 台架），而是把死亡螺旋
 * 改写为自愈降级：
 *
 *   - NVS 持续计数 panic/看门狗/欠压复位；连续 ≥3 次 → 本次开机强制
 *     "已知稳定捕获档"（SVGA/q18/fps12/彩色/IR 熄，2026-11-10 实测该档在
 *     LAN+RTSP 负载下存活）；
 *   - 进入安全档时把用户捕获配置备份进 NVS（只备份一次）；
 *   - 安全档下单次开机干净运行 30 分钟 → 自动还原用户配置并重启（SW 复位，
 *     计数清零）→ 回到全功能；若仍崩则再次进入安全档——设备永远在线，
 *     最坏情况 = 降级画质运行。
 *
 * 用户随时可通过 Web/API 改回自己的配置（运行时热生效）；若再次连续 panic，
 * 下次开机仍会强制安全档（自愈优先于用户偏好，日志显式声明）。 */

/** 开机早期调用（config_init 之后、camera_init 之前）：计数、判档、必要时
 *  备份用户捕获配置并强制安全档。 */
bool crash_breaker_check_and_arm(void);

/** 健康监控循环周期调用：状态机出口。
 *  - 安全态（AP-only）：干净运行 1 小时 → 置 LAN 试运行标志并重启；
 *  - LAN 试运行：干净运行 30 分钟 → 还原用户捕获配置并重启。 */
void crash_breaker_recovery_poll(uint32_t uptime_s);

/** SD 卡配置覆盖（step 7）之后调用：若安全档生效且 SD 覆盖改写了捕获字段，
 *  重新强制安全档（板载无 SD 时为无害空转）。 */
void crash_breaker_reassert(void);

/** 当前是否处于安全档（limp）。安全态下调用方应跳过 STA 入网（AP-only
 *  生命线，NVS 凭据保留），除非同时处于 LAN 试运行态。 */
bool crash_breaker_limp_active(void);

/** 当前是否为 LAN 试运行开机（limp 下的每小时一次回归尝试；
 *  例外：满 30 分钟干净运行后已自动还原用户配置，limp 同时解除） */
bool crash_breaker_lan_try_active(void);
