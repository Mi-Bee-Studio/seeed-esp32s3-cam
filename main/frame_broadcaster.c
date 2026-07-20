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

#include "frame_broadcaster.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

/* Use GCC/Clang __atomic builtins (not <stdatomic.h>) so the same header
 * compiles cleanly under both C (frame_broadcaster.c, video_recorder.c) and
 * C++ (rtsp_server.cpp) with a consistent ABI. On Xtensa ESP32-S3 dual-core,
 * 32-bit atomic inc/dec compile to LOCK-prefixed instructions (lock-free). */

static const char *TAG = "fbcast";

/* ---- Internal subscriber structure ---- */

struct frame_sub {
    QueueHandle_t    queue;       /* depth-1 queue of frame_msg_t */
    frame_sub_type_t type;
    bool             active;
    uint32_t         drops;       /* consecutive drops (for diagnostics) */
    uint32_t         total_drops; /* lifetime drops */
};

static frame_sub_t s_subs[FBROADCAST_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_mutex;       /* protects subscribe/unsubscribe/publish fan-out */
static SemaphoreHandle_t s_cache_mutex; /* protects latest frame cache */

/* Latest frame cache — holds a reference to the most recently published
 * frame_buf_t, so new subscribers can get a frame immediately without
 * waiting for the next capture. */
static frame_buf_t *s_cache_fb = NULL;
static uint32_t     s_seq = 0;

/* ---- frame_buf_t lifecycle ---- */

frame_buf_t *frame_buf_alloc(const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (!jpeg_data || jpeg_len == 0) return NULL;

    frame_buf_t *fb = heap_caps_malloc(sizeof(*fb), MALLOC_CAP_DEFAULT);
    if (!fb) {
        ESP_LOGW(TAG, "frame_buf metadata alloc failed (%zu B)", sizeof(*fb));
        return NULL;
    }
    fb->data = heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM);
    if (!fb->data) {
        ESP_LOGW(TAG, "frame_buf PSRAM alloc failed (%zu B)", jpeg_len);
        free(fb);
        return NULL;
    }
    memcpy(fb->data, jpeg_data, jpeg_len);
    fb->len = jpeg_len;
    fb->seq = 0;  /* assigned by publish */
    __atomic_store_n(&fb->refcount, 1, __ATOMIC_RELEASE);
    return fb;
}

void frame_buf_release(frame_buf_t *fb)
{
    if (!fb) return;
    /* fetch_sub returns the previous value; if it was 1, we are the last
     * holder and must free the payload + metadata. */
    int32_t prev = __atomic_fetch_sub(&fb->refcount, 1, __ATOMIC_ACQ_REL);
    if (prev == 1) {
        free(fb->data);
        free(fb);
    } else if (prev <= 0) {
        /* Defensive: should never happen (double release). Restore to avoid
         * a dangling free and log loudly. */
        __atomic_store_n(&fb->refcount, 0, __ATOMIC_RELEASE);
        ESP_LOGE(TAG, "frame_buf_release: refcount underflow (prev=%ld)",
                 (long)prev);
    }
}

/* ---- Public API ---- */

esp_err_t fbroadcast_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_cache_mutex = xSemaphoreCreateMutex();
    if (!s_mutex || !s_cache_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return ESP_FAIL;
    }
    memset(s_subs, 0, sizeof(s_subs));
    ESP_LOGI(TAG, "Frame broadcaster initialized (%d subscriber slots)",
             FBROADCAST_MAX_SUBSCRIBERS);
    return ESP_OK;
}

void fbroadcast_deinit(void)
{
    for (int i = 0; i < FBROADCAST_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].active) {
            fbroadcast_unsubscribe(&s_subs[i]);
        }
    }
    if (s_cache_fb) {
        frame_buf_release(s_cache_fb);
        s_cache_fb = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    if (s_cache_mutex) {
        vSemaphoreDelete(s_cache_mutex);
        s_cache_mutex = NULL;
    }
}

frame_sub_t *fbroadcast_subscribe(frame_sub_type_t type)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "fbroadcast_subscribe called before init");
        return NULL;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    frame_sub_t *sub = NULL;
    for (int i = 0; i < FBROADCAST_MAX_SUBSCRIBERS; i++) {
        if (!s_subs[i].active) {
            sub = &s_subs[i];
            break;
        }
    }

    if (!sub) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "No free subscriber slots");
        return NULL;
    }

    /* depth-1 queue: xQueueOverwrite semantics — always holds the latest frame */
    sub->queue = xQueueCreate(1, sizeof(frame_msg_t));
    if (!sub->queue) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Failed to create subscriber queue");
        return NULL;
    }

    sub->type = type;
    sub->active = true;
    sub->drops = 0;
    sub->total_drops = 0;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Subscriber registered: %s",
             type == FRAMESUB_MOTION ? "MOTION" : "MJPEG");
    return sub;
}

void fbroadcast_unsubscribe(frame_sub_t *sub)
{
    if (!sub) return;

    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (sub->active) {
        /* Drain and release any pending frame reference */
        frame_msg_t msg;
        if (xQueueReceive(sub->queue, &msg, 0)) {
            if (msg.fb) frame_buf_release(msg.fb);
        }
        vQueueDelete(sub->queue);
        sub->queue = NULL;
        sub->active = false;

        ESP_LOGI(TAG, "Subscriber %s unregistered (total_drops=%lu)",
                 sub->type == FRAMESUB_MOTION ? "MOTION" : "MJPEG",
                 (unsigned long)sub->total_drops);
    }

    xSemaphoreGive(s_mutex);
}

bool fbroadcast_receive(frame_sub_t *sub, frame_msg_t *msg, uint32_t timeout_ms)
{
    if (!sub || !sub->active || !msg) return false;

    if (xQueueReceive(sub->queue, msg, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return false;
    }
    return true;
}

void fbroadcast_release(frame_msg_t *msg)
{
    if (msg && msg->fb) {
        frame_buf_release(msg->fb);
        msg->fb = NULL;
    }
}

void fbroadcast_publish_buf(frame_buf_t *fb)
{
    if (!fb) return;

    s_seq++;
    fb->seq = s_seq;

    /* Update latest frame cache: replace old reference with this buffer's. */
    if (!s_cache_mutex) return;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    frame_buf_t *old_cache = s_cache_fb;
    s_cache_fb = fb;
    __atomic_fetch_add(&fb->refcount, 1, __ATOMIC_ACQ_REL);
    xSemaphoreGive(s_cache_mutex);
    if (old_cache) {
        frame_buf_release(old_cache);
    }

    /* Fan-out to each active subscriber — share a reference, no per-sub copy. */
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < FBROADCAST_MAX_SUBSCRIBERS; i++) {
        frame_sub_t *sub = &s_subs[i];
        if (!sub->active) continue;

        /* Drain old frame if subscriber hasn't consumed it (slow client). */
        frame_msg_t old;
        if (xQueueReceive(sub->queue, &old, 0)) {
            if (old.fb) frame_buf_release(old.fb);
            sub->drops++;
            sub->total_drops++;
        } else {
            sub->drops = 0;  /* reset consecutive counter on successful drain */
        }

        /* Hand a shared reference to this subscriber. If the queue is full
         * (shouldn't happen since we just drained), drop the new frame. */
        frame_msg_t msg = { .fb = fb };
        if (xQueueSend(sub->queue, &msg, 0) != pdPASS) {
            sub->drops++;
            sub->total_drops++;
        } else {
            __atomic_fetch_add(&fb->refcount, 1, __ATOMIC_ACQ_REL);
        }
    }

    xSemaphoreGive(s_mutex);
}

void fbroadcast_publish(const uint8_t *jpeg_data, size_t jpeg_len)
{
    frame_buf_t *fb = frame_buf_alloc(jpeg_data, jpeg_len);
    if (!fb) return;
    /* publish_buf takes references for cache + subscribers; the alloc gave
     * us refcount=1, which we drop here (publisher does not keep it). */
    fbroadcast_publish_buf(fb);
    frame_buf_release(fb);
}

bool fbroadcast_get_latest(frame_msg_t *msg)
{
    if (!msg) return false;
    msg->fb = NULL;

    if (!s_cache_mutex) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (!s_cache_fb) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }

    /* Return a new reference to the shared cache buffer — zero copy. */
    __atomic_fetch_add(&s_cache_fb->refcount, 1, __ATOMIC_ACQ_REL);
    msg->fb = s_cache_fb;

    xSemaphoreGive(s_cache_mutex);
    return true;
}
