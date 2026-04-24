/*
 * Copyright (C) 2024 ParrotCam Authors
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

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 录像机状态枚举，表示录像模块的当前工作状态
 *
 * RECORDER_IDLE:   空闲状态，未在录像
 * RECORDER_RECORDING: 正在录像中
 * RECORDER_PAUSED:   录像已暂停
 * RECORDER_ERROR:    录像出错
 */
typedef enum {
    RECORDER_IDLE = 0,
    RECORDER_RECORDING,
    RECORDER_PAUSED,
    RECORDER_ERROR,
} recorder_state_t;

/** Callback invoked when a recording segment is finalized on SD card. */
typedef void (*recorder_segment_cb_t)(const char *filepath, size_t size);

/** @brief 初始化录像模块，创建互斥锁 */
esp_err_t recorder_init(void);
/** @brief 开始录像，创建录像任务（核心0，优先级5） */
esp_err_t recorder_start(void);
/** @brief 停止录像，等待任务结束后清理 */
esp_err_t recorder_stop(void);
/** @brief 暂停录像 */
esp_err_t recorder_pause(void);
/** @brief 获取当前录像状态 */
recorder_state_t recorder_get_state(void);
/** @brief 设置分段录像完成时的回调函数 */
void recorder_set_segment_cb(recorder_segment_cb_t cb);
/** @brief 获取当前正在写入的录像文件路径 */
const char *recorder_get_current_file(void);
/** @brief 喂任务看门狗，防止录像任务被复位 */
void recorder_watchdog_feed(void);
/** @brief 清理启动时发现的不完整录像文件（RIFF大小为0） */
void recorder_cleanup_incomplete(void);
/** @brief 获取录像任务的栈高水位标记（字节） */
uint32_t recorder_get_stack_hwm(void);
