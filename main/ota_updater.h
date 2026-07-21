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

#include "esp_err.h"
#include "esp_http_server.h"

#define FW_VERSION "0.3.0"

/**
 * @brief Initialize OTA module — logs firmware version, creates mutex.
 */
esp_err_t ota_updater_init(void);

/**
 * @brief HTTP handler for POST /api/ota — parses {"url":"..."}, performs update.
 */
esp_err_t api_ota_handler(httpd_req_t *req);

/** @brief GET /api/ota/info — 固件版本和分区信息 */
esp_err_t api_ota_info_handler(httpd_req_t *req);
/** @brief POST /api/ota/upload — 流式上传固件二进制 */
esp_err_t api_ota_upload_handler(httpd_req_t *req);
/** @brief POST /api/ota/spiffs — 流式上传 SPIFFS 镜像 */
esp_err_t api_ota_spiffs_handler(httpd_req_t *req);
