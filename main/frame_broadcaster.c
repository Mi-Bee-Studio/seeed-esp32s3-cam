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
static SemaphoreHandle_t s_mutex;       /* protects subscribe/unsubscribe */
static SemaphoreHandle_t s_cache_mutex; /* protects latest frame cache */

/* Latest frame cache — allows new subscribers to get a frame immediately */
static frame_msg_t s_cache = {0};
static uint32_t    s_seq = 0;

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
    if (s_cache.data) {
        free(s_cache.data);
        s_cache.data = NULL;
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
             "MJPEG");
    return sub;
}

void fbroadcast_unsubscribe(frame_sub_t *sub)
{
    if (!sub) return;

    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (sub->active) {
        /* Drain and free any pending frame */
        frame_msg_t msg;
        if (xQueueReceive(sub->queue, &msg, 0)) {
            if (msg.data) free(msg.data);
        }
        vQueueDelete(sub->queue);
        sub->queue = NULL;
        sub->active = false;

        ESP_LOGI(TAG, "Subscriber %s unregistered (total_drops=%lu)",
                 "MJPEG",
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
    if (msg && msg->data) {
        free(msg->data);
        msg->data = NULL;
        msg->len = 0;
    }
}

void fbroadcast_publish(const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (!jpeg_data || jpeg_len == 0) return;

    s_seq++;

    /* Update latest frame cache */
    if (!s_cache_mutex) return;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (s_cache.data) {
        free(s_cache.data);
        s_cache.data = NULL;
    }
    s_cache.data = heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM);
    if (s_cache.data) {
        memcpy(s_cache.data, jpeg_data, jpeg_len);
        s_cache.len = jpeg_len;
        s_cache.seq = s_seq;
    }
    xSemaphoreGive(s_cache_mutex);

    /* Fan-out to each active subscriber */
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < FBROADCAST_MAX_SUBSCRIBERS; i++) {
        frame_sub_t *sub = &s_subs[i];
        if (!sub->active) continue;

        /* Allocate per-subscriber PSRAM copy */
        frame_msg_t msg = {
            .len = jpeg_len,
            .seq = s_seq,
        };
        msg.data = heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM);
        if (!msg.data) {
            sub->drops++;
            sub->total_drops++;
            continue;
        }
        memcpy(msg.data, jpeg_data, jpeg_len);

        /* Drain old frame if subscriber hasn't consumed it (slow client) */
        frame_msg_t old;
        if (xQueueReceive(sub->queue, &old, 0)) {
            if (old.data) free(old.data);
            sub->drops++;
            sub->total_drops++;
        } else {
            sub->drops = 0;  /* reset consecutive counter on successful drain */
        }

        /* Push new frame */
        if (xQueueSend(sub->queue, &msg, 0) != pdPASS) {
            if (msg.data) free(msg.data);
        }
    }

    xSemaphoreGive(s_mutex);
}

bool fbroadcast_get_latest(frame_msg_t *msg)
{
    if (!msg) return false;

    if (!s_cache_mutex) return false;
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (!s_cache.data) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }

    msg->data = heap_caps_malloc(s_cache.len, MALLOC_CAP_SPIRAM);
    if (!msg->data) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }
    memcpy(msg->data, s_cache.data, s_cache.len);
    msg->len = s_cache.len;
    msg->seq = s_cache.seq;

    xSemaphoreGive(s_cache_mutex);
    return true;
}
