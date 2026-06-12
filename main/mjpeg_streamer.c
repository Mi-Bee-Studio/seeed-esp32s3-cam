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
 *
 * MJPEG streamer — independent TCP server (port 81)
 * ==================================================
 *
 * Separate listen + per-client FreeRTOS tasks bypass the single-threaded
 * httpd entirely.  The main web server on port 80 stays responsive even
 * when stream clients are connected for hours.
 *
 * Architecture:
 *   mjpeg_streamer_init()
 *     └─ create listen socket on port 81
 *     └─ spawn mjpeg_listen_task (Core 1)
 *          └─ accept() loop
 *               └─ for each client: spawn mjpeg_client_task (Core 1)
 *                    └─ subscribe to frame broadcaster
 *                    └─ loop: fbroadcast_receive → send() chunked JPEG
 *                    └─ cleanup: unsubscribe, close socket, task delete
 */

#include "mjpeg_streamer.h"
#include "esp_log.h"
#include "frame_broadcaster.h"
#include "config_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <errno.h>

static const char *TAG = "mjpeg";

#define STREAM_PORT        81
#define MJPEG_BOUNDARY     "frame"
#define MAX_STREAM_CLIENTS 2
#define CHUNK_SIZE         4096
#define LISTEN_BACKLOG     2
#define CLIENT_TASK_STACK  4096
#define SEND_TIMEOUT_MS    5000

static int s_client_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_listen_task = NULL;
static int s_listen_sock = -1;
static volatile bool s_running = false;

/* Forward declarations */
static void mjpeg_listen_task(void *arg);
static void mjpeg_client_task(void *arg);

/* ------------------------------------------------------------------ */
/*  Client task — serves one MJPEG stream connection                   */
/* ------------------------------------------------------------------ */

static void mjpeg_client_task(void *arg)
{
    int client_sock = (int)(intptr_t)arg;

    /* Set send timeout so a stuck client does not hang the task */
    struct timeval tv = { .tv_sec = SEND_TIMEOUT_MS / 1000,
                          .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Read HTTP request (first 511 bytes is enough to validate) */
    char req_buf[512];
    int req_len = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
    if (req_len <= 0) {
        ESP_LOGW(TAG, "Failed to read HTTP request");
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }
    req_buf[req_len] = '\0';

    /* Validate: must be GET /stream (accept /stream?xxx too) */
    if (strncmp(req_buf, "GET /stream", 11) != 0) {
        ESP_LOGW(TAG, "Unexpected request: %.60s", req_buf);
        const char *resp = "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
        send(client_sock, resp, strlen(resp), 0);
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    /* Send HTTP 200 + multipart/x-mixed-replace headers */
    char headers[512];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n");

    if (send(client_sock, headers, hdr_len, 0) != hdr_len) {
        ESP_LOGW(TAG, "Failed to send response headers");
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    /* Subscribe to frame broadcaster */
    frame_sub_t *sub = fbroadcast_subscribe(FRAMESUB_MJPEG);
    if (!sub) {
        ESP_LOGE(TAG, "Failed to subscribe to frame broadcaster");
        const char *err_resp = "HTTP/1.1 500 Internal Server Error\r\n"
                               "Content-Length: 22\r\n\r\nStream unavailable\r\n";
        send(client_sock, err_resp, strlen(err_resp), 0);
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Stream client started (total %d)", s_client_count);

    /* ---- Stream loop ------------------------------------------------- */
    uint8_t fps;
    TickType_t frame_delay;
    char part_hdr[128];
    int capture_fails = 0;

    while (1) {
        fps = config_get()->fps;
        frame_delay = (fps > 0) ? pdMS_TO_TICKS(1000 / fps) : pdMS_TO_TICKS(100);

        /* Receive frame from broadcaster (up to 3s timeout) */
        frame_msg_t fmsg;
        if (!fbroadcast_receive(sub, &fmsg, 3000)) {
            capture_fails++;
            if (capture_fails >= 30) {
                ESP_LOGW(TAG, "No frames for %d tries, ending stream", capture_fails);
                break;
            }
            continue;
        }
        capture_fails = 0;

        /* Build multipart part header */
        int hdrlen = snprintf(part_hdr, sizeof(part_hdr),
            "\r\n--" MJPEG_BOUNDARY "\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "\r\n", fmsg.len);

        /* Send part header */
        if (send(client_sock, part_hdr, hdrlen, 0) != hdrlen) {
            fbroadcast_release(&fmsg);
            break;
        }

        /* Send JPEG body in CHUNK_SIZE pieces */
        size_t remaining = fmsg.len;
        const uint8_t *ptr = fmsg.data;
        while (remaining > 0) {
            size_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
            int sent = send(client_sock, ptr, chunk, 0);
            if (sent <= 0) {
                fbroadcast_release(&fmsg);
                goto cleanup;
            }
            ptr      += sent;
            remaining -= sent;
        }

        /* Trailing CRLF */
        if (send(client_sock, "\r\n", 2, 0) != 2) {
            fbroadcast_release(&fmsg);
            break;
        }

        fbroadcast_release(&fmsg);

        /* Frame-rate throttle */
        vTaskDelay(frame_delay);
    }

cleanup:
    fbroadcast_unsubscribe(sub);

    /* Send closing boundary (best-effort) */
    send(client_sock, "\r\n--" MJPEG_BOUNDARY "--\r\n", strlen(MJPEG_BOUNDARY) + 8, 0);

    close(client_sock);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_client_count--;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stream client disconnected (total %d)", s_client_count);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Listen task — accepts connections, spawns client tasks             */
/* ------------------------------------------------------------------ */

static void mjpeg_listen_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Listen task started on port %d", STREAM_PORT);

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(s_listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
        if (client_sock < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            /* s_running check — if stopped, exit cleanly */
            if (!s_running) break;
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Enforce client limit */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_client_count >= MAX_STREAM_CLIENTS) {
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "Max stream clients (%d) reached, rejecting", MAX_STREAM_CLIENTS);
            const char *reject = "HTTP/1.1 503 Service Unavailable\r\n"
                                 "Content-Length: 25\r\n\r\nMax stream connections\r\n";
            send(client_sock, reject, strlen(reject), 0);
            close(client_sock);
            continue;
        }
        s_client_count++;
        xSemaphoreGive(s_mutex);

        /* Spawn a dedicated client task (Core 1, priority 2) */
        BaseType_t created = xTaskCreatePinnedToCore(
            mjpeg_client_task,
            "mjpeg_cli",
            CLIENT_TASK_STACK,
            (void *)(intptr_t)client_sock,
            2,
            NULL,
            1);

        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task");
            close(client_sock);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_client_count--;
            xSemaphoreGive(s_mutex);
        }
    }

    ESP_LOGI(TAG, "Listen task exiting");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
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

    /* Create TCP listen socket */
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create listen socket: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(STREAM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind port %d: errno %d", STREAM_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    if (listen(s_listen_sock, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Failed to listen on port %d: errno %d", STREAM_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    s_running = true;

    /* Spawn listen task on Core 1 */
    BaseType_t created = xTaskCreatePinnedToCore(
        mjpeg_listen_task,
        "mjpeg_listen",
        CLIENT_TASK_STACK,
        NULL,
        3,      /* slightly higher than client tasks */
        &s_listen_task,
        1);     /* Core 1 */

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create listen task");
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MJPEG streamer initialized on port %d", STREAM_PORT);
    return ESP_OK;
}

int mjpeg_streamer_client_count(void)
{
    return s_client_count;
}
