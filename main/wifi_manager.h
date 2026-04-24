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
    char ssid[33];
    int8_t rssi;
    uint8_t auth_mode;  // wifi_auth_mode_t value
} wifi_ap_info_t;

typedef enum {
    WIFI_STATE_AP,                  // AP mode active (no STA config)
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_STA_DISCONNECTED,
} wifi_state_t;

esp_err_t wifi_init(void);          // Decides AP/STA based on config
wifi_state_t wifi_get_state(void);
char *wifi_get_ip_str(void);        // Returns current IP string (static buffer)
esp_err_t wifi_start_ap(void);
esp_err_t wifi_start_sta(void);
int wifi_scan(wifi_ap_info_t *aps, int max_count);   // Returns number of APs found, or -1 on error
bool wifi_is_sta(void);             // true if in STA mode (or trying)
