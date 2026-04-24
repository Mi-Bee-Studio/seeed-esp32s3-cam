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

/**
 * Initialize the MJPEG streamer (creates mutex, resets client count).
 * Call once before registering with a server.
 */
esp_err_t mjpeg_streamer_init(void);

/**
 * Register the /stream URI handler on the given HTTP server.
 */
esp_err_t mjpeg_streamer_register(httpd_handle_t server);

/**
 * Return the number of currently connected streaming clients.
 */
int mjpeg_streamer_client_count(void);
