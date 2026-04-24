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
#include "esp_http_server.h"
#include <stdint.h>

/** @brief 启动HTTP Web服务器，注册所有URI处理程序 */
esp_err_t web_server_start(uint16_t port);
/** @brief 停止HTTP Web服务器并释放资源 */
void web_server_stop(void);
/** @brief 获取当前Web服务器的句柄 */
httpd_handle_t web_server_get_handle(void);
