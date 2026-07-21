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
#include "audio_broadcaster.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "abcast";

/* ---- Internal subscriber structure ---- */

struct audio_sub {
    QueueHandle_t    queue;       /* depth-1 queue of audio_frame_t */
    audio_sub_type_t type;
    bool             active;
    uint32_t         drops;       /* consecutive drops (for diagnostics) */
    uint32_t         total_drops; /* lifetime drops */
};

static audio_sub_t s_subs[AUDIO_BCAST_MAX_SUBSCRIBERS];
static SemaphoreHandle_t s_mutex; /* protects subscribe/unsubscribe */

/* ---- Public API ---- */

esp_err_t audio_bcast_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    memset(s_subs, 0, sizeof(s_subs));
    ESP_LOGI(TAG, "Audio broadcaster initialized (%d subscriber slots)",
             AUDIO_BCAST_MAX_SUBSCRIBERS);
    return ESP_OK;
}

void audio_bcast_deinit(void)
{
    for (int i = 0; i < AUDIO_BCAST_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].active) {
            audio_bcast_unsubscribe(&s_subs[i]);
        }
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

audio_sub_t *audio_bcast_subscribe(audio_sub_type_t type)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "audio_bcast_subscribe called before init");
        return NULL;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    audio_sub_t *sub = NULL;
    for (int i = 0; i < AUDIO_BCAST_MAX_SUBSCRIBERS; i++) {
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
    sub->queue = xQueueCreate(1, sizeof(audio_frame_t));
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

    ESP_LOGI(TAG, "Subscriber registered: type=%d", (int)type);
    return sub;
}

void audio_bcast_unsubscribe(audio_sub_t *sub)
{
    if (!sub) return;
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (sub->active) {
        /* Drain and free any pending frame */
        audio_frame_t msg;
        if (xQueueReceive(sub->queue, &msg, 0)) {
            if (msg.data) free(msg.data);
        }
        vQueueDelete(sub->queue);
        sub->queue = NULL;
        sub->active = false;

        ESP_LOGI(TAG, "Subscriber type=%d unregistered (total_drops=%lu)",
                 (int)sub->type, (unsigned long)sub->total_drops);
    }

    xSemaphoreGive(s_mutex);
}

bool audio_bcast_receive(audio_sub_t *sub, audio_frame_t *frame, uint32_t timeout_ms)
{
    if (!sub || !sub->active || !frame) return false;

    if (xQueueReceive(sub->queue, frame, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return false;
    }
    return true;
}

void audio_bcast_release(audio_frame_t *frame)
{
    if (frame && frame->data) {
        free(frame->data);
        frame->data = NULL;
        frame->len = 0;
    }
}

void audio_bcast_publish(const uint8_t *g711_data, size_t len, uint64_t timestamp_us)
{
    if (!g711_data || len == 0) return;

    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Fan-out to each active subscriber */
    for (int i = 0; i < AUDIO_BCAST_MAX_SUBSCRIBERS; i++) {
        audio_sub_t *sub = &s_subs[i];
        if (!sub->active) continue;

        /* Allocate per-subscriber internal RAM copy — faster for 160-byte frames, avoids PSRAM cache contention with JPEG DMA traffic */
        audio_frame_t frame = {
            .len = len,
            .timestamp_us = timestamp_us,
        };
        frame.data = heap_caps_malloc(len, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!frame.data) {
            sub->drops++;
            sub->total_drops++;
            continue;
        }
        memcpy(frame.data, g711_data, len);

        /* Drain old frame if subscriber hasn't consumed it (slow client) */
        audio_frame_t old;
        if (xQueueReceive(sub->queue, &old, 0)) {
            if (old.data) free(old.data);
            sub->drops++;
            sub->total_drops++;
        } else {
            sub->drops = 0; /* reset consecutive counter on successful drain */
        }

        /* Push new frame */
        if (xQueueSend(sub->queue, &frame, 0) != pdPASS) {
            if (frame.data) free(frame.data);
        }
    }

    xSemaphoreGive(s_mutex);
}
