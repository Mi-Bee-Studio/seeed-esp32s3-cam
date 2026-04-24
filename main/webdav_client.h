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

typedef struct {
    char url[128];      // e.g. "http://192.168.1.100:5005"
    char user[32];
    char pass[32];
} webdav_config_t;

/**
 * Upload a local file to a remote WebDAV path via HTTP PUT.
 * Includes retry with exponential backoff (up to 3 attempts).
 */
esp_err_t webdav_upload(const webdav_config_t *cfg, const char *remote_path, const char *local_path);

/**
 * Create a remote directory via WebDAV MKCOL.
 * Returns ESP_OK if created or already exists.
 */
esp_err_t webdav_mkdir(const webdav_config_t *cfg, const char *remote_dir);

/**
 * Check if a remote resource exists via HTTP HEAD.
 * Returns ESP_OK if exists (200), ESP_ERR_NOT_FOUND if 404.
 */
esp_err_t webdav_exists(const webdav_config_t *cfg, const char *remote_path);

/**
 * Recursively create all directories in a remote path.
 * e.g. "/ParrotCam/2026-04/24" creates /ParrotCam, /ParrotCam/2026-04, etc.
 */
esp_err_t webdav_mkdir_recursive(const webdav_config_t *cfg, const char *path);
