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

#define FBROADCAST_MAX_SUBSCRIBERS  4   /* up to 2 MJPEG + 1 RTSP + 1 motion detector */

/** Subscriber type for logging and diagnostics. */
typedef enum {
    FRAMESUB_MJPEG,
    FRAMESUB_MOTION,   /* motion detector async task */
} frame_sub_type_t;

/**
 * @brief Reference-counted JPEG buffer shared between subscribers.
 *
 * The producer allocates one frame_buf_t per captured frame (single PSRAM
 * copy). Every active subscriber + the latest-frame cache hold a reference;
 * the last release frees the underlying PSRAM buffer. Thread-safe via
 * atomic refcount operations (safe across FreeRTOS tasks on both cores).
 *
 * Access the JPEG payload via fb->data / fb->len; do NOT free fb->data
 * directly — call frame_buf_release() instead.
 */
typedef struct frame_buf {
    volatile int32_t refcount;  /* atomic; do not read/write directly */
    uint8_t       *data;        /* PSRAM JPEG payload */
    size_t         len;         /* JPEG size in bytes */
    uint32_t       seq;         /* monotonic sequence number */
} frame_buf_t;

/**
 * @brief Frame message handed to subscribers. Holds a reference to a
 *        shared frame_buf_t; release with fbroadcast_release().
 */
typedef struct {
    frame_buf_t *fb;   /* NULL when empty */
} frame_msg_t;

/** Opaque subscriber handle. */
typedef struct frame_sub frame_sub_t;

/**
 * @brief Allocate a frame_buf_t and copy jpeg_data into PSRAM.
 *
 * The returned buffer has refcount=1 (caller owns that reference). Release
 * with frame_buf_release() when done, or transfer ownership to
 * fbroadcast_publish_buf().
 *
 * @param jpeg_data Source JPEG bytes (copied; caller retains ownership)
 * @param jpeg_len  Length of jpeg_data
 * @return New frame_buf_t (refcount=1), or NULL on allocation failure
 */
frame_buf_t *frame_buf_alloc(const uint8_t *jpeg_data, size_t jpeg_len);

/**
 * @brief Atomically decrement refcount; free the buffer when it reaches 0.
 * @param fb Buffer (may be NULL — safe no-op)
 */
void frame_buf_release(frame_buf_t *fb);

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
 * @param msg      Output frame message (caller must fbroadcast_release)
 * @param timeout_ms Max wait time in ms
 * @return true if frame available, false on timeout
 */
bool fbroadcast_receive(frame_sub_t *sub, frame_msg_t *msg, uint32_t timeout_ms);

/**
 * @brief Release a received frame (drops one reference; frees when last).
 * @param msg Frame message previously returned by fbroadcast_receive()
 *            or fbroadcast_get_latest(). Safe on zeroed msg.
 */
void fbroadcast_release(frame_msg_t *msg);

/**
 * @brief Publish a frame by transferring ownership of a frame_buf_t.
 *
 * The broadcaster takes a reference for the latest-frame cache and one
 * reference per active subscriber. The caller's own reference is NOT
 * released — caller may keep using fb->data and must release when done.
 * Typical producer pattern:
 *   frame_buf_t *fb = frame_buf_alloc(src, len);
 *   fbroadcast_publish_buf(fb);   // shares with subscribers
 *   ... use fb->data ...
 *   frame_buf_release(fb);        // drop producer's own reference
 *
 * @param fb Buffer allocated with frame_buf_alloc(). Must not be NULL.
 */
void fbroadcast_publish_buf(frame_buf_t *fb);

/**
 * @brief Publish a frame to all subscribers (convenience wrapper).
 * Equivalent to: frame_buf_alloc + fbroadcast_publish_buf + frame_buf_release.
 * Use fbroadcast_publish_buf() directly when the producer wants to keep
 * using the buffer after publishing (avoids a redundant copy).
 * @param jpeg_data JPEG frame data (copied once into PSRAM)
 * @param jpeg_len  JPEG frame length
 */
void fbroadcast_publish(const uint8_t *jpeg_data, size_t jpeg_len);

/**
 * @brief Get the latest cached frame (for immediate use by new connections).
 * Returns a new reference to the shared cache buffer — zero copy.
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

/**
 * @brief 计算推荐的帧间延迟（毫秒）。
 * 无 MJPEG 客户端时降到 2fps 节省资源；有客户端用配置 fps。
 * @return 延迟毫秒数
 */
int broker_frame_delay(void);

#ifdef __cplusplus
}
#endif
