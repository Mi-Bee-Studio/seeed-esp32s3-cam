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

#include "ws_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/semphr.h"
#include <time.h>

static const char *TAG = "ws";

#define MAX_WS_CLIENTS 4  /* 4 is realistic for ESP32-S3 alongside streaming; was 10 */

/* ------------------------------------------------------------------ */
/*  Client tracking                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    int fd;
} ws_client_t;

static ws_client_t s_clients[MAX_WS_CLIENTS];
static int s_client_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static void add_client(httpd_req_t *req)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_count >= MAX_WS_CLIENTS) {
        ESP_LOGW(TAG, "Max WS clients reached (%d)", MAX_WS_CLIENTS);
        xSemaphoreGive(s_mutex);
        return;
    }
    s_clients[s_client_count].req = req;
    s_clients[s_client_count].fd  = httpd_req_to_sockfd(req);
    s_client_count++;
    ESP_LOGI(TAG, "WS client added, fd=%d (total=%d)",
             s_clients[s_client_count - 1].fd, s_client_count);
    xSemaphoreGive(s_mutex);
}

static void remove_client(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i] = s_clients[--s_client_count];
            ESP_LOGI(TAG, "WS client removed, fd=%d (total=%d)", fd, s_client_count);
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    xSemaphoreGive(s_mutex);
}

/* ------------------------------------------------------------------ */
/*  WebSocket handler                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket upgrade handshake — client connecting */
        add_client(req);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = { 0 };
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        /* Client disconnected */
        remove_client(req);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        remove_client(req);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/* Forward decl — defined below init */
static void reaper_task(void *arg);

esp_err_t ws_server_init(httpd_handle_t server)
{
    static bool s_initialized = false;
    if (s_initialized) return ESP_OK;
    s_initialized = true;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    /* 死客户端回收器：每 30s 向所有客户端发 WS ping。
     * 浏览器异常断开（关标签页/断网）不会发 CLOSE 帧，死连接会一直占着
     * httpd 的 open socket；ping 失败即摘除，防止套接字被慢性耗尽。 */
    xTaskCreate(reaper_task, "ws_reaper", 3072, server, 1, NULL);

    httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .user_ctx     = NULL,
        .is_websocket = true,
    };
    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGD(TAG, "/ws already registered — idempotent");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "WebSocket handler registered at /ws");
    return ESP_OK;
}

static void reaper_task(void *arg)
{
    httpd_handle_t server = (httpd_handle_t)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        httpd_ws_frame_t ping = {
            .type    = HTTPD_WS_TYPE_PING,
            .payload = NULL,
            .len     = 0,
        };

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        int count = s_client_count;
        httpd_req_t *reqs[MAX_WS_CLIENTS];
        for (int i = 0; i < count; i++) reqs[i] = s_clients[i].req;
        xSemaphoreGive(s_mutex);

        for (int i = 0; i < count; i++) {
            if (httpd_ws_send_frame(reqs[i], &ping) != ESP_OK) {
                int fd = httpd_req_to_sockfd(reqs[i]);
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                for (int j = 0; j < s_client_count; j++) {
                    if (s_clients[j].fd == fd) {
                        s_clients[j] = s_clients[--s_client_count];
                        ESP_LOGW(TAG, "Reaped dead WS client fd=%d (total=%d)", fd, s_client_count);
                        break;
                    }
                }
                xSemaphoreGive(s_mutex);
            }
        }
    }
}

void ws_broadcast(const char *type, const char *data)
{
    if (s_client_count == 0) {
        return;
    }

    /* 契约 v1.0 统一事件格式: {"type":"...","timestamp":<unix>,"data":{...}} */
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"timestamp\":%lld,\"data\":%s}",
                       type, (long long)time(NULL), (data && data[0]) ? data : "{}");
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }

    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = len,
    };

    /* Snapshot client list under mutex, then send without holding mutex */
    httpd_req_t *reqs[MAX_WS_CLIENTS];
    int count;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    count = s_client_count;
    for (int i = 0; i < count; i++) {
        reqs[i] = s_clients[i].req;
    }
    xSemaphoreGive(s_mutex);

    /* Send to all snapshot clients without holding mutex */
    for (int i = 0; i < count; i++) {
        esp_err_t ret = httpd_ws_send_frame(reqs[i], &pkt);
        if (ret != ESP_OK) {
            /* Client disconnected — remove under mutex */
            int fd = httpd_req_to_sockfd(reqs[i]);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            for (int j = 0; j < s_client_count; j++) {
                if (s_clients[j].fd == fd) {
                    s_clients[j] = s_clients[--s_client_count];
                    ESP_LOGI(TAG, "WS client removed (send failed), fd=%d (total=%d)", fd, s_client_count);
                    break;
                }
            }
            xSemaphoreGive(s_mutex);
        }
    }
}
