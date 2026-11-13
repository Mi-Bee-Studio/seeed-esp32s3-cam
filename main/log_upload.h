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
#include <stddef.h>

/* 远程日志诊断（PIT-062 配套，2026-11-13）。
 *
 * 生产部署后串口不可达，崩溃现场只能远程取证。三层数据：
 *
 * 1. RAM 日志环（PSRAM 96KB）：esp_log vprintf 分流，缓存最近日志行。
 *    GET /api/logs?tail_bytes=N 远程拉取（活窗内诊断）。
 * 2. NVS 状态日志（8 条环形，60s/条）：heap/温度/RTSP 客户端/断路器态。
 *    崩溃重启后存活——开机诊断报告携带，用于还原"死前最后状态"。
 * 3. 上报：配置 log_upload_url（≤256）+ log_upload_en 后，
 *    - 开机（STA 就绪）POST 诊断报告（含 NVS 状态日志全量 + 复位原因 +
 *      断路器计数——跨崩溃存活）
 *    - 断路器状态迁移（limp_arm/lan_try/restore）POST 事件 + 当前日志尾
 *    - POST /api/logs/upload 手动触发
 *
 * URL 未配置或上传失败均静默（不影响主流程）。 */

/** 安装日志分流钩子（必须在任何 ESP_LOG 产生价值前调用，app_main 最早处）。
 *  返回 ring 缓冲指针/容量供 /api/logs 读取。 */
bool log_upload_ring_init(void);
const char *log_upload_ring_data(void);
size_t log_upload_ring_size(void);      /* 环总容量 */
size_t log_upload_ring_used(void);      /* 有效字节数（顺序写，满后回绕） */
size_t log_upload_ring_head(void);      /* 写入位置偏移 */

/** NVS 状态日志追加（健康循环 60s 调一次；内部自带节流） */
void log_upload_journal_tick(float chip_temp);

/** 开机诊断报告（STA 就绪后调用一次；异步任务，立即返回） */
void log_upload_boot_report(void);

/** 断路器状态迁移事件上报（异步任务；event 如 "limp_arm"/"lan_try"/"restore"） */
void log_upload_notify_event(const char *event);

/** 手动触发上传（/api/logs/upload）—— true=已提交异步任务 */
bool log_upload_manual_upload(void);
