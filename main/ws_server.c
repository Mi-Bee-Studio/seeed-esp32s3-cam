/*
 * Copyright (C) 2024 MiBeeHomeCam Authors
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
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ws";

#define MAX_WS_CLIENTS 10

/* ------------------------------------------------------------------ */
/*  Client tracking                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    int fd;
} ws_client_t;

static ws_client_t s_clients[MAX_WS_CLIENTS];
static int s_client_count = 0;

static void add_client(httpd_req_t *req)
{
    if (s_client_count >= MAX_WS_CLIENTS) {
        ESP_LOGW(TAG, "Max WS clients reached (%d)", MAX_WS_CLIENTS);
        return;
    }
    s_clients[s_client_count].req = req;
    s_clients[s_client_count].fd  = httpd_req_to_sockfd(req);
    s_client_count++;
    ESP_LOGI(TAG, "WS client added, fd=%d (total=%d)",
             s_clients[s_client_count - 1].fd, s_client_count);
}

static void remove_client(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    for (int i = 0; i < s_client_count; i++) {
        if (s_clients[i].fd == fd) {
            s_clients[i] = s_clients[--s_client_count];
            ESP_LOGI(TAG, "WS client removed, fd=%d (total=%d)", fd, s_client_count);
            return;
        }
    }
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

esp_err_t ws_server_init(httpd_handle_t server)
{
    httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .user_ctx     = NULL,
        .is_websocket = true,
    };
    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /ws: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WebSocket handler registered at /ws");
    }
    return ret;
}

void ws_broadcast(const char *type, const char *data)
{
    if (s_client_count == 0) {
        return;
    }

    char buf[512];
    int len = snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"data\":%s}", type, data);
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }

    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = len,
    };

    /* Iterate backwards so swap-with-last removal doesn't skip anyone */
    for (int i = s_client_count - 1; i >= 0; i--) {
        esp_err_t ret = httpd_ws_send_frame(s_clients[i].req, &pkt);
        if (ret != ESP_OK) {
            /* Client disconnected — remove silently */
            s_clients[i] = s_clients[--s_client_count];
        }
    }
}
