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

/**
 * @file rtsp_server.c
 * @brief Minimal RTSP server — MJPEG over RTP, TCP-interleaved only.
 *
 * Supported methods: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER.
 * Max 2 concurrent clients, 60-second idle session timeout.
 * FreeRTOS task with TWDT registration.
 */

#include "rtsp_server.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "camera_driver.h"
#include "wifi_manager.h"
#include "config_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

/* ---- Constants -------------------------------------------------------- */

static const char *TAG = "rtsp";

#define RTSP_PORT             554
#define MAX_CLIENTS           2
#define SESSION_TIMEOUT_SEC   60
#define RX_BUF_SIZE           1024
#define RTP_MTU               1400
#define RTP_HDR_SIZE          12
#define JPEG_HDR_SIZE         8
#define MAX_RTP_PAYLOAD       (RTP_MTU - RTP_HDR_SIZE - JPEG_HDR_SIZE)

/* ---- RTP / RTSP types ------------------------------------------------- */

typedef enum {
    CLIENT_STATE_INIT,
    CLIENT_STATE_READY,     /* SETUP done, waiting for PLAY */
    CLIENT_STATE_PLAYING,
} client_state_t;

typedef struct {
    int            sock;
    client_state_t state;
    char           session_id[17];
    uint16_t       rtp_seq;
    uint32_t       rtp_ts;
    uint32_t       rtp_ssrc;
    uint32_t       frame_count;
    int64_t        last_activity;  /* monotonic seconds */
    TaskHandle_t   task;
} rtsp_client_t;

/* ---- Statics ---------------------------------------------------------- */

static SemaphoreHandle_t s_mutex        = NULL;
static rtsp_client_t     s_clients[MAX_CLIENTS];
static int               s_listen_sock  = -1;
static volatile bool     s_running      = false;
static TaskHandle_t      s_listener_task = NULL;

/* ---- Helpers ---------------------------------------------------------- */

static void close_client(int idx)
{
    if (idx < 0 || idx >= MAX_CLIENTS) return;
    rtsp_client_t *c = &s_clients[idx];
    if (c->sock >= 0) {
        close(c->sock);
        c->sock = -1;
    }
    if (c->task != NULL) {
        vTaskDelete(c->task);
        c->task = NULL;
    }
    c->state = CLIENT_STATE_INIT;
    c->session_id[0] = '\0';
    c->rtp_seq = 0;
    c->rtp_ts = 0;
    c->frame_count = 0;
    c->last_activity = 0;
    ESP_LOGI(TAG, "Client %d closed", idx);
}

static int find_free_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].sock < 0) return i;
    }
    return -1;
}

/* Reserve for future use with session-based routing */

static int64_t get_monotonic_sec(void)
{
    return (int64_t)xTaskGetTickCount() / configTICK_RATE_HZ;
}

static void gen_session_id(char *buf, size_t len)
{
    uint32_t r = esp_random();
    snprintf(buf, len, "%08X%08X", (unsigned)(r), (unsigned)esp_random());
}

static int count_playing_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].state == CLIENT_STATE_PLAYING) n++;
    }
    return n;
}

static int count_connected_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].sock >= 0) n++;
    }
    return n;
}

/* ---- RTP packet builder ----------------------------------------------- */

static int build_rtp_packet(uint8_t *pkt, int max_len,
                            const uint8_t *jpeg_data, size_t jpeg_len,
                            uint16_t seq, uint32_t ts, uint32_t ssrc,
                            int width, int height, int offset,
                            int quality, bool marker)
{
    /* JPEG type DRI (0), quantization table (255 = no quant table) */
    uint8_t jpeg_type = 0;
    uint8_t dri = 0;
    uint8_t q_table = 255;

    if (offset == 0 && max_len >= (int)jpeg_len + RTP_HDR_SIZE + JPEG_HDR_SIZE) {
        /* Whole frame fits in one packet */
        int total = RTP_HDR_SIZE + JPEG_HDR_SIZE + (int)jpeg_len;
        if (total > max_len) return -1;

        /* RTP header */
        pkt[0]  = 0x80;                                       /* V=2, P=0, X=0, CC=0 */
        pkt[1]  = (marker ? 0x80 : 0x00) | 96;               /* M=1, PT=96 (JPEG) */
        pkt[2]  = (seq >> 8) & 0xFF;
        pkt[3]  = seq & 0xFF;
        pkt[4]  = (ts >> 24) & 0xFF;
        pkt[5]  = (ts >> 16) & 0xFF;
        pkt[6]  = (ts >> 8)  & 0xFF;
        pkt[7]  = ts & 0xFF;
        pkt[8]  = (ssrc >> 24) & 0xFF;
        pkt[9]  = (ssrc >> 16) & 0xFF;
        pkt[10] = (ssrc >> 8)  & 0xFF;
        pkt[11] = ssrc & 0xFF;

        /* JPEG header (RFC 2435 sec 3.1) */
        int hdrlen = RTP_HDR_SIZE + JPEG_HDR_SIZE;
        pkt[12] = 0;                          /* Type-specific: offset high bits (0) */
        pkt[13] = 0;                          /* Fragment offset (0) */
        pkt[14] = (jpeg_type << 5) | dri;     /* Type=0, DRI=0 */
        pkt[15] = q_table;                     /* Quantization table (255=none) */
        pkt[16] = width / 8;                   /* Width in 8-pixel blocks */
        pkt[17] = height / 8;                  /* Height in 8-pixel blocks */
        pkt[18] = 0;                          /* Reserved / high byte of offset (0) */
        pkt[19] = 0;                          /* Low byte of fragment offset (0) */

        memcpy(pkt + hdrlen, jpeg_data, jpeg_len);
        return total;
    }

    /* Fragmented packet */
    size_t payload_space = max_len - RTP_HDR_SIZE - JPEG_HDR_SIZE;
    if (payload_space > (size_t)MAX_RTP_PAYLOAD) payload_space = MAX_RTP_PAYLOAD;

    /* Determine if this is the last fragment */
    size_t remaining = jpeg_len - offset;
    bool is_last = (remaining <= payload_space);
    size_t this_chunk = is_last ? remaining : payload_space;
    bool this_marker = is_last && marker;

    int total = RTP_HDR_SIZE + JPEG_HDR_SIZE + (int)this_chunk;

    /* RTP header */
    pkt[0]  = 0x80;
    pkt[1]  = (this_marker ? 0x80 : 0x00) | 96;
    pkt[2]  = (seq >> 8) & 0xFF;
    pkt[3]  = seq & 0xFF;
    pkt[4]  = (ts >> 24) & 0xFF;
    pkt[5]  = (ts >> 16) & 0xFF;
    pkt[6]  = (ts >> 8)  & 0xFF;
    pkt[7]  = ts & 0xFF;
    pkt[8]  = (ssrc >> 24) & 0xFF;
    pkt[9]  = (ssrc >> 16) & 0xFF;
    pkt[10] = (ssrc >> 8)  & 0xFF;
    pkt[11] = ssrc & 0xFF;

    /* JPEG header */
    pkt[12] = 0;
    pkt[13] = (offset >> 16) & 0xFF;
    pkt[14] = (jpeg_type << 5) | dri;
    pkt[15] = q_table;
    pkt[16] = width / 8;
    pkt[17] = height / 8;
    pkt[18] = (offset >> 8)  & 0xFF;
    pkt[19] = offset & 0xFF;

    memcpy(pkt + RTP_HDR_SIZE + JPEG_HDR_SIZE, jpeg_data + offset, this_chunk);
    return total;
}

static int tcp_send_all(int sock, const uint8_t *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static int send_rtp_interleaved(int sock, uint8_t channel,
                                const uint8_t *pkt, int pkt_len)
{
    /* TCP interleaved framing: $channel(1) len_hi(1) len_lo(1) data(len) */
    uint8_t hdr[4];
    hdr[0] = '$';
    hdr[1] = channel;
    hdr[2] = (pkt_len >> 8) & 0xFF;
    hdr[3] = pkt_len & 0xFF;

    if (tcp_send_all(sock, hdr, 4) < 0) return -1;
    if (tcp_send_all(sock, pkt, pkt_len) < 0) return -1;
    return pkt_len;
}

static int send_rtsp_response(int sock, int code, const char *status,
                              const char *extra_headers, const char *body)
{
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "RTSP/1.0 %d %s\r\n"
        "CSeq: 0\r\n"
        "%s"
        "\r\n",
        code, status, extra_headers ? extra_headers : "");

    if (tcp_send_all(sock, (const uint8_t *)hdr, hlen) < 0) return -1;
    if (body && body[0]) {
        int blen = strlen(body);
        if (tcp_send_all(sock, (const uint8_t *)body, blen) < 0) return -1;
    }
    return 0;
}

/* ---- RTSP method handlers --------------------------------------------- */

static void handle_options(int sock)
{
    send_rtsp_response(sock, 200, "OK",
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
        NULL);
}

static void handle_describe(int sock)
{
    const char *ip = wifi_get_ip_str();
    if (!ip || ip[0] == '\0') ip = "0.0.0.0";

    char sdp[512];
    int slen = snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=MiBeeHomeCam\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 JPEG/90000\r\n"
        "a=control:stream\r\n",
        ip);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n",
        slen);

    send_rtsp_response(sock, 200, "OK", hdr, sdp);
}

static void handle_setup(int sock, const char *request, int client_idx)
{
    rtsp_client_t *c = &s_clients[client_idx];
    gen_session_id(c->session_id, sizeof(c->session_id));
    c->rtp_ssrc = esp_random();
    c->rtp_seq = 0;
    c->rtp_ts = 0;
    c->frame_count = 0;
    c->state = CLIENT_STATE_READY;

    char transport[128];
    snprintf(transport, sizeof(transport),
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "Session: %s\r\n",
        c->session_id);

    send_rtsp_response(sock, 200, "OK", transport, NULL);
    ESP_LOGI(TAG, "Client %d SETUP session=%s", client_idx, c->session_id);
}

static void handle_play(int sock, int client_idx)
{
    rtsp_client_t *c = &s_clients[client_idx];
    if (c->state != CLIENT_STATE_READY) {
        send_rtsp_response(sock, 455, "Method Not Valid in This State", NULL, NULL);
        return;
    }
    c->state = CLIENT_STATE_PLAYING;

    char hdr[128];
    snprintf(hdr, sizeof(hdr),
        "Session: %s\r\n"
        "Range: npt=0.000-\r\n",
        c->session_id);

    send_rtsp_response(sock, 200, "OK", hdr, NULL);
    ESP_LOGI(TAG, "Client %d PLAYING", client_idx);
}

static void handle_teardown(int sock, int client_idx)
{
    rtsp_client_t *c = &s_clients[client_idx];
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Session: %s\r\n", c->session_id);
    send_rtsp_response(sock, 200, "OK", hdr, NULL);
    ESP_LOGI(TAG, "Client %d TEARDOWN session=%s", client_idx, c->session_id);
    close_client(client_idx);
}

static void handle_get_parameter(int sock, int client_idx)
{
    rtsp_client_t *c = &s_clients[client_idx];
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Session: %s\r\n", c->session_id);
    send_rtsp_response(sock, 200, "OK", hdr, NULL);
}

/* ---- RTSP request parser ---------------------------------------------- */

static void parse_and_handle(int sock, char *buf, int len, int client_idx)
{
    (void)len;

    /* Extract method */
    char method[32] = {0};
    if (sscanf(buf, "%31s", method) != 1) {
        send_rtsp_response(sock, 400, "Bad Request", NULL, NULL);
        return;
    }

    if (strcmp(method, "OPTIONS") == 0) {
        handle_options(sock);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        handle_describe(sock);
    } else if (strcmp(method, "SETUP") == 0) {
        handle_setup(sock, buf, client_idx);
    } else if (strcmp(method, "PLAY") == 0) {
        handle_play(sock, client_idx);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        handle_teardown(sock, client_idx);
    } else if (strcmp(method, "GET_PARAMETER") == 0) {
        handle_get_parameter(sock, client_idx);
    } else {
        send_rtsp_response(sock, 405, "Method Not Allowed",
            "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
            NULL);
    }
}

/* ---- Per-client receive/timeout task ---------------------------------- */

static void rtsp_client_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    rtsp_client_t *c = &s_clients[idx];
    char rx_buf[RX_BUF_SIZE];

    ESP_LOGI(TAG, "Client %d task started", idx);

    esp_task_wdt_add(NULL);

    while (c->sock >= 0 && s_running) {
        esp_task_wdt_reset();

        /* Check session timeout for non-INIT clients */
        if (c->state != CLIENT_STATE_INIT) {
            int64_t now = get_monotonic_sec();
            if (now - c->last_activity > SESSION_TIMEOUT_SEC) {
                ESP_LOGW(TAG, "Client %d session timeout", idx);
                break;
            }
        }

        /* Non-blocking recv with 1s timeout */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int n = recv(c->sock, rx_buf, sizeof(rx_buf) - 1, 0);
        if (n > 0) {
            rx_buf[n] = '\0';
            c->last_activity = get_monotonic_sec();

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            parse_and_handle(c->sock, rx_buf, n, idx);
            xSemaphoreGive(s_mutex);

            /* Check if TEARDOWN closed the client */
            if (c->sock < 0) break;
        } else if (n == 0) {
            ESP_LOGI(TAG, "Client %d disconnected", idx);
            break;
        } else {
            /* Timeout or error — check if socket is still valid */
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "Client %d recv error: %d", idx, errno);
                break;
            }
        }
    }

    esp_task_wdt_delete(NULL);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    close_client(idx);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Client %d task exiting", idx);
    vTaskDelete(NULL);
}

/* ---- Frame streaming (called from listener task) --------------------- */

static void stream_frame_to_clients(camera_frame_t *frame, int width, int height)
{
    uint8_t pkt_buf[RTP_MTU + 4]; /* interleaved header + RTP packet */

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        rtsp_client_t *c = &s_clients[i];
        if (c->state != CLIENT_STATE_PLAYING) continue;

        size_t offset = 0;
        bool first = true;
        while (offset < frame->len) {
            bool marker = false; /* marker only on last fragment */

            int pkt_len = build_rtp_packet(
                pkt_buf + 4, RTP_MTU,
                frame->buf, frame->len,
                c->rtp_seq, c->rtp_ts, c->rtp_ssrc,
                width, height,
                (int)offset,
                0,       /* quality (0 = use frame's embedded quantization) */
                marker);

            if (pkt_len < 0) break;

            /* Determine if this is the last fragment */
            size_t remaining = frame->len - offset;
            if ((size_t)(pkt_len - RTP_HDR_SIZE - JPEG_HDR_SIZE) >= remaining || first) {
                /* Check if it's truly the last packet */
                if (offset + (pkt_len - RTP_HDR_SIZE - JPEG_HDR_SIZE) >= frame->len) {
                    pkt_buf[4 + 1] |= 0x80; /* Set marker bit */
                }
            }

            if (send_rtp_interleaved(c->sock, 0, pkt_buf + 4, pkt_len) < 0) {
                ESP_LOGW(TAG, "Client %d RTP send failed, closing", i);
                close_client(i);
                break;
            }

            c->rtp_seq++;
            offset += (pkt_len - RTP_HDR_SIZE - JPEG_HDR_SIZE);
            first = false;
        }

        c->rtp_ts += (90000 / config_get()->fps);
        c->frame_count++;
    }

    xSemaphoreGive(s_mutex);
}

/* ---- Main listener task ----------------------------------------------- */

static void rtsp_listener_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr;

    ESP_LOGI(TAG, "RTSP server starting on port %d", RTSP_PORT);

    /* Create TCP socket */
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(RTSP_PORT);

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind port %d: %d", RTSP_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_listen_sock, MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "Failed to listen: %d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
    s_running = true;

    esp_task_wdt_add(NULL);

    /* Resolution for RTP header dimensions */
    int width = 640, height = 480;
    camera_res_t res = camera_get_resolution();
    switch (res) {
        case CAMERA_RES_SVGA: width = 800;  height = 600; break;
        case CAMERA_RES_XGA:  width = 1024; height = 768; break;
        default: break;
    }

    while (s_running) {
        esp_task_wdt_reset();

        /* ---- Accept new connections (non-blocking) ---- */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(s_listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(s_listen_sock, (struct sockaddr *)&client_addr, &client_len);

        if (client_sock >= 0) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            int idx = find_free_slot();
            if (idx >= 0) {
                s_clients[idx].sock = client_sock;
                s_clients[idx].state = CLIENT_STATE_INIT;
                s_clients[idx].last_activity = get_monotonic_sec();
                s_clients[idx].task = NULL;

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                ESP_LOGI(TAG, "Client %d connected from %s (total %d)",
                         idx, client_ip, count_connected_clients());

                /* Spawn per-client receive task */
                char task_name[20];
                snprintf(task_name, sizeof(task_name), "rtsp_cli%d", idx);
                xTaskCreate(rtsp_client_task, task_name, 4096,
                            (void *)(intptr_t)idx, 2, &s_clients[idx].task);
            } else {
                ESP_LOGW(TAG, "Max clients reached, rejecting connection");
                const char *msg = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
                send(client_sock, msg, strlen(msg), 0);
                close(client_sock);
            }
            xSemaphoreGive(s_mutex);
        }

        /* ---- Stream frames to playing clients ---- */
        if (count_playing_clients() > 0) {
            camera_frame_t frame;
            if (camera_capture(&frame) == ESP_OK) {
                stream_frame_to_clients(&frame, width, height);
                camera_return_fb(&frame);
            }
            const uint8_t fps = config_get()->fps;
            vTaskDelay(pdMS_TO_TICKS(1000 / (fps > 0 ? fps : 15)));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* Update resolution in case it changed */
        camera_res_t new_res = camera_get_resolution();
        if (new_res != res) {
            res = new_res;
            switch (res) {
                case CAMERA_RES_SVGA: width = 800;  height = 600; break;
                case CAMERA_RES_XGA:  width = 1024; height = 768; break;
                default: width = 640; height = 480; break;
            }
        }
    }

    esp_task_wdt_delete(NULL);

    /* Cleanup all clients */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        close_client(i);
    }
    xSemaphoreGive(s_mutex);

    if (s_listen_sock >= 0) {
        close(s_listen_sock);
        s_listen_sock = -1;
    }

    ESP_LOGI(TAG, "RTSP server stopped");
    vTaskDelete(NULL);
}

/* ---- Public API ------------------------------------------------------- */

esp_err_t rtsp_server_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        s_clients[i].sock = -1;
    }

    s_running = false;

    BaseType_t ret = xTaskCreatePinnedToCore(
        rtsp_listener_task, "rtsp_srv", 8192, NULL, 3, &s_listener_task, 1);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RTSP server task");
        return ESP_FAIL;
    }

    /* Wait a moment for the task to start and set s_running */
    vTaskDelay(pdMS_TO_TICKS(100));
    return s_running ? ESP_OK : ESP_FAIL;
}

int rtsp_server_get_client_count(void)
{
    int count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    count = count_connected_clients();
    xSemaphoreGive(s_mutex);
    return count;
}

bool rtsp_server_is_running(void)
{
    return s_running;
}
