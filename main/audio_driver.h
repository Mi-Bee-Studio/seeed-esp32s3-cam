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
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2S PDM RX for microphone capture.
 *
 * GPIO41 = DATA, GPIO42 = CLK (XIAO ESP32-S3 Sense onboard MEMS mic).
 * Configures 16 kHz / 16-bit / mono PDM-to-PCM capture.
 * @return ESP_OK on success
 */
esp_err_t audio_driver_init(void);

/**
 * @brief Start the audio capture FreeRTOS task.
 *
 * Spawns a task on Core 0 with priority 4 and 4096-word stack.
 * The task reads PCM from I2S, decimates 2:1 to 8 kHz, encodes to
 * G.711 mu-law, and publishes 20 ms frames via audio_bcast_publish().
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t audio_driver_start(void);

/**
 * @brief Stop the audio capture task.
 *
 * Sets the running flag to false and unblocks the capture task.
 * I2S peripheral remains initialized for a potential restart.
 * @return ESP_OK always
 */
esp_err_t audio_driver_stop(void);

/**
 * @brief Deinitialize I2S and release all resources.
 *
 * Stops capture first, then disables and deletes the I2S channel.
 * Safe to call even if not initialized.
 * @return ESP_OK always
 */
esp_err_t audio_driver_deinit(void);

/**
 * @brief Check if audio capture is currently running.
 * @return true if capture task is active
 */
bool audio_driver_is_running(void);

#ifdef __cplusplus
}
#endif
