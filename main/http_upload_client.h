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
#include <stddef.h>

/** @brief HTTP upload configuration */
typedef struct {
    char url[256];         /* Full upload URL (e.g., https://nas.local/api/upload) */
    char user[64];         /* Basic Auth username (empty = no auth) */
    char pass[64];         /* Basic Auth password */
    bool skip_cert_verify; /* Skip TLS certificate verification */
} http_upload_config_t;

/** @brief Upload a file via HTTP PUT
 * Streams file in 4KB chunks, supports HTTP and HTTPS.
 * @param config Upload configuration
 * @param remote_path Remote path/name for the upload (appended to base URL)
 * @param local_path Local file path to upload
 * @return ESP_OK success, ESP_FAIL failure
 */
esp_err_t http_upload_file(const http_upload_config_t *config,
                           const char *remote_path,
                           const char *local_path);

/** @brief Create remote directory via HTTP POST (best-effort)
 * @param config Upload configuration
 * @param path Remote directory path
 * @return ESP_OK success, ESP_FAIL failure
 */
esp_err_t http_upload_mkdir(const http_upload_config_t *config,
                            const char *path);
