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

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD card structured event logging module.
 * Creates the /sdcard/logs/ directory and prepares the write mutex.
 * Call once at boot after storage_init().
 */
esp_err_t sd_log_init(void);

/**
 * @brief Deinitialize SD log module.
 * Flushes and closes the current log file, deletes the mutex.
 */
void sd_log_deinit(void);

/**
 * @brief Write one structured log event line to the SD card.
 *
 * Called by logging.c log_event_impl() after serial output.
 * Line format: the pre-formatted JSON line (already contains type/ts/msg).
 *
 * Non-blocking: silently drops if SD unavailable, mutex busy, or write fails.
 *
 * @param json_line  Pre-formatted JSON log line (without trailing newline).
 */
void sd_log_write(const char *json_line);

/**
 * @brief Pause SD card logging.
 * Closes the current file. Called on SD card removal.
 */
void sd_log_pause(void);

/**
 * @brief Resume SD card logging.
 * Reopens the log file on the next write. Called on SD card insertion.
 */
void sd_log_resume(void);

#ifdef __cplusplus
}
#endif
