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
#include "audio_common.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_BCAST_MAX_SUBSCRIBERS  4

/** Opaque subscriber handle. */
typedef struct audio_sub audio_sub_t;

/**
 * @brief Subscribe to audio frame broadcasts.
 * @param type Subscriber type (RTSP or AVI muxer)
 * @return Handle, or NULL if no free slots
 */
audio_sub_t *audio_bcast_subscribe(audio_sub_type_t type);

/**
 * @brief Unsubscribe and release all resources.
 * @param sub Handle returned by audio_bcast_subscribe()
 */
void audio_bcast_unsubscribe(audio_sub_t *sub);

/**
 * @brief Receive next available audio frame.
 *        Blocks up to timeout_ms waiting for a frame from the producer.
 * @param sub        Subscriber handle
 * @param frame      Output audio frame
 * @param timeout_ms Max wait time in ms
 * @return true if frame available, false on timeout
 */
bool audio_bcast_receive(audio_sub_t *sub, audio_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief Release a received frame buffer (frees PSRAM copy).
 * @param frame Audio frame previously returned by audio_bcast_receive()
 */
void audio_bcast_release(audio_frame_t *frame);

/**
 * @brief Publish a G.711 audio frame to all subscribers.
 *        Called by the audio driver after capturing a frame.
 *        Each active subscriber receives its own PSRAM copy.
 * @param g711_data    G.711 mu-law encoded audio data
 * @param len          Data length in bytes
 * @param timestamp_us Capture timestamp in microseconds
 */
void audio_bcast_publish(const uint8_t *g711_data, size_t len, uint64_t timestamp_us);

/**
 * @brief Initialize the audio broadcaster.
 *        Call once after audio driver init, before subscribing consumers.
 * @return ESP_OK on success
 */
esp_err_t audio_bcast_init(void);

/**
 * @brief Shut down the audio broadcaster and free all resources.
 */
void audio_bcast_deinit(void);

#ifdef __cplusplus
}
#endif
