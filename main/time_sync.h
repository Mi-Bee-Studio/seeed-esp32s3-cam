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
#include <time.h>

/**
 * Start SNTP time synchronization (call after WiFi connected).
 * Blocks up to 5 seconds waiting for initial sync, then returns.
 * Async sync continues in background if not yet synced.
 */
esp_err_t time_sync_init(void);

/** True if NTP or manual time has been set successfully. */
bool time_is_synced(void);

/** Format current time as "YYYY-MM-DD HH:MM:SS" into buf. */
void time_get_str(char *buf, size_t len);

/** Manually set system time (useful when no NTP available). */
esp_err_t time_set_manual(int year, int month, int day, int hour, int min, int sec);
