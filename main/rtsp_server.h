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
#include <stdint.h>

/** @brief Initialize RTSP server
 * Creates listening task on port 554.
 * @return ESP_OK success, ESP_FAIL failure
 */
esp_err_t rtsp_server_init(void);

/** @brief Get number of currently connected RTSP clients
 * @return Client count (0-2)
 */
int rtsp_server_get_client_count(void);

/** @brief Check if RTSP server is initialized
 * @return true if initialized
 */
bool rtsp_server_is_running(void);
