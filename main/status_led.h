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

typedef enum {
    LED_STARTING,         // Solid on
    LED_AP_MODE,          // Slow blink 1s
    LED_WIFI_CONNECTING,  // Fast blink 200ms
    LED_RUNNING,          // Off
    LED_ERROR             // Double blink
} led_status_t;

esp_err_t led_init(void);
void led_set_status(led_status_t status);
