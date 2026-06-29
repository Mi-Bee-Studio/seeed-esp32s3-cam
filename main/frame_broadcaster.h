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
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FBROADCAST_MAX_SUBSCRIBERS  3   /* MJPEG + RTSP */

/** Subscriber type for logging and diagnostics. */
typedef enum {
    FRAMESUB_MJPEG,
} frame_sub_type_t;

/** Owned JPEG buffer — subscriber must call fbroadcast_release() when done. */
typedef struct {
    uint8_t *data;     /* heap_caps_malloc'd in PSRAM */
    size_t   len;      /* JPEG size in bytes */
    uint32_t seq;      /* monotonic sequence number */
} frame_msg_t;

/** Opaque subscriber handle. */
typedef struct frame_sub frame_sub_t;

/**
 * @brief Subscribe to frame broadcasts.
 * @param type Subscriber type (MJPEG or RTSP)
 * @return Handle, or NULL if no free slots
 */
frame_sub_t *fbroadcast_subscribe(frame_sub_type_t type);

/**
 * @brief Unsubscribe and release all resources.
 * @param sub Handle returned by fbroadcast_subscribe()
 */
void fbroadcast_unsubscribe(frame_sub_t *sub);

/**
 * @brief Receive next available frame.
 * Blocks up to timeout_ms waiting for a frame from the producer.
 * @param sub      Subscriber handle
 * @param msg      Output frame message
 * @param timeout_ms Max wait time in ms
 * @return true if frame available, false on timeout
 */
bool fbroadcast_receive(frame_sub_t *sub, frame_msg_t *msg, uint32_t timeout_ms);

/**
 * @brief Release a received frame buffer (frees PSRAM copy).
 * @param msg Frame message previously returned by fbroadcast_receive()
 */
void fbroadcast_release(frame_msg_t *msg);

/**
 * @brief Publish a frame to all subscribers.
 * Called by the sole frame producer (recorder task) after capturing a frame.
 * Each active subscriber receives its own PSRAM copy.
 * @param jpeg_data JPEG frame data
 * @param jpeg_len  JPEG frame length
 */
void fbroadcast_publish(const uint8_t *jpeg_data, size_t jpeg_len);

/**
 * @brief Get the latest cached frame (for immediate use by new connections).
 * Caller must call fbroadcast_release() when done.
 * @param msg Output frame message
 * @return true if a cached frame exists
 */
bool fbroadcast_get_latest(frame_msg_t *msg);

/**
 * @brief Initialize the frame broadcaster.
 * Call once after camera init, before recorder/streamer init.
 * @return ESP_OK on success
 */
esp_err_t fbroadcast_init(void);

/**
 * @brief Shut down the frame broadcaster and free all resources.
 */
void fbroadcast_deinit(void);

#ifdef __cplusplus
}
#endif
