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

#include "mjpeg_streamer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "camera_driver.h"
#include "config_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mjpeg";

#define MJPEG_BOUNDARY      "frame"
#define MAX_STREAM_CLIENTS  2
#define CHUNK_SIZE          4096

static int s_client_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* ------------------------------------------------------------------ */
/*  Stream handler (GET /stream)                                      */
/* ------------------------------------------------------------------ */
static esp_err_t mjpeg_stream_handler(httpd_req_t *req)
{
    /* --- Client limit check --- */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_count >= MAX_STREAM_CLIENTS) {
        xSemaphoreGive(s_mutex);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Max stream connections reached", -1);
        return ESP_FAIL;
    }
    s_client_count++;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stream client connected (total %d)", s_client_count);

    /* --- Response headers --- */
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    const uint8_t fps = config_get()->fps;
    const TickType_t frame_delay = (fps > 0) ? pdMS_TO_TICKS(1000 / fps)
                                              : pdMS_TO_TICKS(100);

    camera_frame_t frame;
    char part_hdr[128];

    while (1) {
        /* Capture */
        esp_err_t ret = camera_capture(&frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Capture failed, ending stream");
            break;
        }

        /* Build multipart part header */
        int hdrlen = snprintf(part_hdr, sizeof(part_hdr),
            "\r\n--" MJPEG_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", frame.len);

        /* Send part header */
        if (httpd_resp_send_chunk(req, part_hdr, hdrlen) != ESP_OK) {
            camera_return_fb(&frame);
            break;
        }

        /* Send JPEG body in CHUNK_SIZE pieces */
        size_t remaining = frame.len;
        const uint8_t *ptr = frame.buf;
        bool send_ok = true;
        while (remaining > 0) {
            size_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
            if (httpd_resp_send_chunk(req, (const char *)ptr, chunk) != ESP_OK) {
                send_ok = false;
                break;
            }
            ptr      += chunk;
            remaining -= chunk;
        }

        /* Trailing CRLF */
        if (send_ok) {
            if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
                send_ok = false;
            }
        }

        camera_return_fb(&frame);

        if (!send_ok) {
            break;
        }

        /* Frame-rate throttle */
        vTaskDelay(frame_delay);
    }

    /* --- Cleanup --- */
    httpd_resp_send_chunk(req, NULL, 0);   /* end chunked response */

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_client_count--;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stream client disconnected (total %d)", s_client_count);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t mjpeg_streamer_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_client_count = 0;
    return ESP_OK;
}

esp_err_t mjpeg_streamer_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t stream_uri = {
        .uri      = "/stream",
        .method   = HTTP_GET,
        .handler  = mjpeg_stream_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &stream_uri);
}

int mjpeg_streamer_client_count(void)
{
    return s_client_count;
}
