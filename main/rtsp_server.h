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

#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the RTSP server (allocate resources, register tracks).
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_init(void);

/**
 * @brief Start the RTSP server (bind socket, begin accepting clients).
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_start(void);

/**
 * @brief Stop the RTSP server and all feed tasks.
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_stop(void);

/**
 * @brief Get the number of currently connected RTSP clients.
 * @return Number of active RTSP sessions
 */
int rtsp_server_client_count(void);

#ifdef __cplusplus
}
#endif
