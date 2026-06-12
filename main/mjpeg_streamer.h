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

/**
 * @brief Initialize the MJPEG streamer (creates TCP server + listen task).
 * Starts an independent TCP server on port 81, completely separate from
 * the main httpd (port 80), so streaming does not block API/UI requests.
 * Call once after frame broadcaster init, before web server start.
 * @return ESP_OK on success
 */
esp_err_t mjpeg_streamer_init(void);

/**
 * @brief Return the number of currently connected streaming clients.
 */
int mjpeg_streamer_client_count(void);
