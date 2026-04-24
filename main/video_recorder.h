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

typedef enum {
    RECORDER_IDLE = 0,
    RECORDER_RECORDING,
    RECORDER_PAUSED,
    RECORDER_ERROR,
} recorder_state_t;

/** Callback invoked when a recording segment is finalized on SD card. */
typedef void (*recorder_segment_cb_t)(const char *filepath, size_t size);

esp_err_t recorder_init(void);
esp_err_t recorder_start(void);
esp_err_t recorder_stop(void);
esp_err_t recorder_pause(void);
recorder_state_t recorder_get_state(void);
void recorder_set_segment_cb(recorder_segment_cb_t cb);
const char *recorder_get_current_file(void);
void recorder_watchdog_feed(void);
void recorder_cleanup_incomplete(void);
uint32_t recorder_get_stack_hwm(void);
