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

/*
 * IDF v6 客户端追踪只存 fd，广播/收割统一走 httpd_ws_send_frame_async——
 * 不再持有 httpd_req_t*：v6 上握手请求的 req 在回调返回后即失效，
 * 存指针跨任务发送 = LoadProhibited（2026-09-07 实测 Guru Meditation
 * Core 1，ws_broadcast 首发即崩）。发送经 s_send_mutex 串行化，避免
 * csi_motion 心跳与收割 PING 对同一 socket 交错写。
 */

#include "ws_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/semphr.h"
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *TAG = "ws";

#define MAX_WS_CLIENTS 4  /* 4 is realistic for ESP32-S3 alongside streaming; was 10 */

/* ------------------------------------------------------------------ */
/*  Client tracking (fd only — safe across tasks on IDF v6)            */
/* ------------------------------------------------------------------ */

static int s_client_fds[MAX_WS_CLIENTS];
static int s_client_count = 0;
static SemaphoreHandle_t s_mutex = NULL;    /* protects the fd list */
static SemaphoreHandle_t s_send_mutex = NULL; /* serializes socket writes */
static httpd_handle_t s_server = NULL;

static void add_client(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* 幂等去重：入册路径有两条（v5 握手后以 GET 进 handler / v6 走
     * ws_post_handshake_cb），同一 fd 只入册一次。 */
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    if (s_client_count >= MAX_WS_CLIENTS) {
        ESP_LOGW(TAG, "Max WS clients reached (%d)", MAX_WS_CLIENTS);
        xSemaphoreGive(s_mutex);
        return;
    }
    s_client_fds[s_client_count++] = fd;

    /* 记录对端 IP：2026-09-04 WS 洪水事件（未掩码帧 50Hz 打满 httpd → TWDT
     * 复位 ×19）时无法定位凶手，此后每个 WS 连接都可追溯 */
    char ipstr[INET_ADDRSTRLEN] = "?";
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, ipstr, sizeof(ipstr));
    }
    ESP_LOGI(TAG, "WS client added, fd=%d ip=%s (total=%d)", fd, ipstr, s_client_count);
    xSemaphoreGive(s_mutex);
}

static void remove_client_fd(int fd)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_client_count; i++) {
        if (s_client_fds[i] == fd) {
            s_client_fds[i] = s_client_fds[--s_client_count];
            ESP_LOGI(TAG, "WS client removed, fd=%d (total=%d)", fd, s_client_count);
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

/* ------------------------------------------------------------------ */
/*  WebSocket handler                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* IDF v5：握手完成后以 GET 进 handler（v6 不再走此分支）。 */
        add_client(req);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = { 0 };
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        /* 协议违例/断连（含“WS frame is not properly masked”）。
         * 必须 return 非 OK 让 httpd 关闭该会话——返回 ESP_OK 会让坏客户端
         * 无限重发坏帧（2026-09-04 实测 50Hz 打满 httpd CPU0 → IDLE0 饿死
         * → TWDT 复位循环）。 */
        remove_client_fd(httpd_req_to_sockfd(req));
        return ESP_FAIL;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        remove_client_fd(httpd_req_to_sockfd(req));
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Forward decl — defined below init */
static void reaper_task(void *arg);

#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
/* IDF v6 起握手请求不再调用 uri handler（httpd_uri.c 握手分支直接 return OK），
 * GET 分支永不执行 → 客户端列表恒空 → 广播静默失效（2026-09-07 排查）。
 * 客户端入册挂到握手完成回调；v5 的 GET 分支保留，add_client 幂等去重。 */
static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    add_client(req);
    return ESP_OK;
}
#endif /* CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT */

esp_err_t ws_server_init(httpd_handle_t server)
{
    static bool s_initialized = false;
    if (s_initialized) return ESP_OK;
    s_initialized = true;

    s_server = server;
    s_mutex = xSemaphoreCreateMutex();
    s_send_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL || s_send_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    /* 死客户端回收器：每 30s 向所有客户端发 WS ping。
     * 浏览器异常断开（关标签页/断网）不会发 CLOSE 帧，死连接会一直占着
     * httpd 的 open socket；ping 失败即摘除，防止套接字被慢性耗尽。 */
    xTaskCreate(reaper_task, "ws_reaper", 3072, NULL, 1, NULL);

    httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .user_ctx     = NULL,
        .is_websocket = true,
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
        .ws_post_handshake_cb = ws_post_handshake,
#endif
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
    httpd_ws_frame_t ping = {
        .type    = HTTPD_WS_TYPE_PING,
        .payload = NULL,
        .len     = 0,
    };

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        int fds[MAX_WS_CLIENTS];
        int count = s_client_count;
        for (int i = 0; i < count; i++) fds[i] = s_client_fds[i];
        xSemaphoreGive(s_mutex);

        for (int i = 0; i < count; i++) {
            esp_err_t ret;
            xSemaphoreTake(s_send_mutex, portMAX_DELAY);
            ret = httpd_ws_send_frame_async(s_server, fds[i], &ping);
            xSemaphoreGive(s_send_mutex);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Reaped dead WS client fd=%d (total=%d)", fds[i], s_client_count - 1);
                remove_client_fd(fds[i]);
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

    /* Snapshot fd list under mutex, then send without holding list mutex
     * (s_send_mutex serializes the actual socket writes). */
    int fds[MAX_WS_CLIENTS];
    int count;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    count = s_client_count;
    for (int i = 0; i < count; i++) {
        fds[i] = s_client_fds[i];
    }
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < count; i++) {
        esp_err_t ret;
        xSemaphoreTake(s_send_mutex, portMAX_DELAY);
        ret = httpd_ws_send_frame_async(s_server, fds[i], &pkt);
        xSemaphoreGive(s_send_mutex);
        if (ret != ESP_OK) {
            /* Client disconnected — drop from list */
            remove_client_fd(fds[i]);
        }
    }
}
