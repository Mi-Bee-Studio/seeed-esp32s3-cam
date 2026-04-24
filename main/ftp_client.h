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

typedef struct {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[32];
} ftp_config_t;

esp_err_t ftp_connect(const ftp_config_t *cfg);
esp_err_t ftp_upload(const char *remote_path, const char *local_path);
esp_err_t ftp_mkdir_recursive(const char *path);
void ftp_disconnect(void);
bool ftp_is_connected(void);
