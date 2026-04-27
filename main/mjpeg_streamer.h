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
#include "esp_http_server.h"

/** @brief 初始化 MJPEG 流服务
 *
 * 创建互斥锁并重置客户端计数，必须在注册到 HTTP 服务器之前调用。
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 内存不足
 */
/**
 * Initialize the MJPEG streamer (creates mutex, resets client count).
 * Call once before registering with a server.
 */
esp_err_t mjpeg_streamer_init(void);

/** @brief 在指定 HTTP 服务器上注册 /stream URI 处理器
 *
 * 将 MJPEG 流端点绑定到 HTTP 服务器。
 * @param server HTTP 服务器句柄
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效
 */
/**
 * Register the /stream URI handler on the given HTTP server.
 */
esp_err_t mjpeg_streamer_register(httpd_handle_t server);

/** @brief 获取当前连接的流客户端数量
 * @return 已连接的客户端数量
 */
/**
 * Return the number of currently connected streaming clients.
 */
int mjpeg_streamer_client_count(void);

/** @brief MJPEG 流处理器（GET /stream）
 *
 * 处理 HTTP multipart/x-mixed-replace MJPEG 实时视频流请求。
 * 此函数通过 web_server.c 的 s_uris[] 表注册到 /stream 端点。
 * @param req HTTP 请求对象
 * @return ESP_OK 流结束，ESP_FAIL 连接错误
 */
esp_err_t mjpeg_stream_handler(httpd_req_t *req);
