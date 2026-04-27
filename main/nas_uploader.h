/*
 * Copyright (C) 2024 MiBeeHomeCam Authors
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

/**
 * @brief 初始化NAS上传模块
 * 创建上传队列和后台上传任务（绑定到核心1，优先级3）
 * @return ESP_OK 成功，ESP_FAIL 失败
 */
esp_err_t nas_uploader_init(void);
/**
 * @brief 将文件路径加入上传队列
 * @param filepath 待上传文件的完整路径
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，ESP_ERR_NO_MEM 队列已满
 */
esp_err_t nas_uploader_enqueue(const char *filepath);
/**
 * @brief 获取上传模块当前状态
 * @param last_upload 输出上次成功上传时间字符串
 * @param len last_upload缓冲区长度
 * @param queue_count 输出当前队列中的文件数量
 * @param paused 输出是否因连续失败而暂停
 */
void nas_uploader_get_status(char *last_upload, size_t len, int *queue_count, bool *paused);
/**
 * @brief 获取上传任务的栈高水位标记
 * @return 剩余栈空间（字节）
 */
uint32_t nas_uploader_get_stack_hwm(void);
