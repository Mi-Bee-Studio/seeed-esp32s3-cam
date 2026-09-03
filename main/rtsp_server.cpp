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
 * RTSP server — dual-track MJPEG + G.711 µ-law over RTP/UDP
 * ========================================================
 *
 * Uses espp/rtsp (v1.1.3) RtspServer with:
 *   Track 0 — MJPEG video (payload type 26, RFC 2435)
 *   Track 1 — PCMU audio (payload type 0, RFC 3551)
 *
 * Digest authentication via config web_password.
 *
 * Architecture:
 *   rtsp_server_init()
 *     └─ create RtspServer with MJPEG + PCMU tracks
 *   rtsp_server_start()
 *     └─ start RtspServer::start()
 *     └─ spawn video_feed_task (Core 1)
 *          └─ fbroadcast_subscribe → send_frame(track=0)
 *     └─ spawn audio_feed_task (Core 1)
 *          └─ audio_bcast_subscribe → send_frame(track=1)
 *   rtsp_server_stop()
 *     └─ stop feed tasks → stop RtspServer
 */

#include "rtsp_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "frame_broadcaster.h"
#include "audio_broadcaster.h"
#include "audio_common.h"
#include "config_manager.h"
#include "video_recorder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* espp/rtsp C++ headers */
#include "rtsp_server.hpp"
#include "mjpeg_packetizer.hpp"
#include "generic_packetizer.hpp"

#include <memory>
#include <cstdio>
#include <cstring>
#include <string>

static const char *TAG = "rtsp";

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define RTSP_PORT           554
#define RTSP_PATH           "/stream"
#define MAX_RTSP_CLIENTS    2
#define VIDEO_TASK_PRIORITY 2
#define AUDIO_TASK_PRIORITY 2
#define VIDEO_TASK_STACK    4096
#define AUDIO_TASK_STACK    4096
#define FEED_TIMEOUT_MS     1000

/* ------------------------------------------------------------------ */
/*  Static state                                                       */
/* ------------------------------------------------------------------ */

static std::unique_ptr<espp::RtspServer> s_server;
static TaskHandle_t s_video_task = NULL;
static TaskHandle_t s_audio_task = NULL;
static volatile bool s_running = false;

/* ------------------------------------------------------------------ */
/*  Helper — get WiFi/AP IP as string                                  */
/* ------------------------------------------------------------------ */

static std::string get_ip_string(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    }
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            return std::string(buf);
        }
    }
    return "0.0.0.0";
}

/* ------------------------------------------------------------------ */
/*  Video feed task — pulls JPEG frames from broadcaster               */
/* ------------------------------------------------------------------ */

extern "C" void video_feed_task(void *arg)
{
    (void)arg;

    frame_sub_t *vsub = fbroadcast_subscribe(FRAMESUB_MJPEG);
    if (!vsub) {
        ESP_LOGE(TAG, "Video: failed to subscribe to frame broadcaster");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Video feed task started");

    /* PIT-020：RTSP 会话存在时维持采集任务（与 MJPEG 的 preview 引用同机制）。
     * 每轮最多滞后 FEED_TIMEOUT_MS 检测一次会话数变化。 */
    bool holding_preview = false;

    /* Send cached frame immediately */
    frame_msg_t cache;
    if (fbroadcast_get_latest(&cache)) {
        s_server->send_frame(0, std::span<const uint8_t>(cache.fb->data, cache.fb->len));
        fbroadcast_release(&cache);
    }

    while (s_running) {
        int sessions = s_server ? (int)s_server->get_num_sessions() : 0;
        if (sessions > 0 && !holding_preview) {
            recorder_preview_acquire("rtsp");
            holding_preview = true;
        } else if (sessions == 0 && holding_preview) {
            recorder_preview_release("rtsp");
            holding_preview = false;
        }
        frame_msg_t msg;
        if (fbroadcast_receive(vsub, &msg, FEED_TIMEOUT_MS)) {
            s_server->send_frame(0, std::span<const uint8_t>(msg.fb->data, msg.fb->len));
            fbroadcast_release(&msg);
        }
    }

    if (holding_preview) {
        recorder_preview_release("rtsp");
    }

    fbroadcast_unsubscribe(vsub);
    ESP_LOGI(TAG, "Video feed task exiting");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Audio feed task — pulls G.711 frames from broadcaster              */
/* ------------------------------------------------------------------ */

extern "C" void audio_feed_task(void *arg)
{
    (void)arg;

    audio_sub_t *asub = audio_bcast_subscribe(AUDIO_SUB_RTSP);
    if (!asub) {
        ESP_LOGE(TAG, "Audio: failed to subscribe to audio broadcaster");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Audio feed task started");

    while (s_running) {
        audio_frame_t frame;
        if (audio_bcast_receive(asub, &frame, FEED_TIMEOUT_MS)) {
            s_server->send_frame(1, std::span<const uint8_t>(frame.data, frame.len));
            audio_bcast_release(&frame);
        }
    }

    audio_bcast_unsubscribe(asub);
    ESP_LOGI(TAG, "Audio feed task exiting");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public C API                                                       */
/* ------------------------------------------------------------------ */

extern "C" esp_err_t rtsp_server_init(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

        cam_config_t *cfg = config_get();
        std::string ip = get_ip_string();

        ESP_LOGI(TAG, "Creating RTSP server on %s:%d", ip.c_str(), RTSP_PORT);

        s_server = std::make_unique<espp::RtspServer>(espp::RtspServer::Config{
            .server_address = ip,
            .port = RTSP_PORT,
            .path = RTSP_PATH,
            .max_data_size = 1400,
            .log_level = espp::Logger::Verbosity::WARN,
            .accept_task_stack_size_bytes = 4096,
            .session_task_stack_size_bytes = 8192,
            .control_task_stack_size_bytes = 8192,
            .auth_username = "admin",
            .auth_password = cfg->web_password,
            .auth_realm = "MiBee Cam",
            .max_sessions = MAX_RTSP_CLIENTS,
        });

        /* Track 0 — MJPEG video (payload type 26, RFC 2435) */
        auto mjpeg_packer = std::make_shared<espp::MjpegPacketizer>(
            espp::MjpegPacketizer::Config{
                .max_payload_size = 1400,
            });
        s_server->add_track(espp::RtspServer::TrackConfig{
            .track_id = 0,
            .packetizer = mjpeg_packer,
        });

        /* Track 1 — PCMU audio (payload type 0, G.711 μ-law @ 8kHz) */
        auto audio_packer = std::make_shared<espp::GenericPacketizer>(
            espp::GenericPacketizer::Config{
                .max_payload_size = 1400,
                .payload_type = 0,
                .clock_rate = G711_SAMPLE_RATE,
                .encoding_name = "PCMU",
                .channels = 1,
                .fmtp = {},
                .media_type = espp::MediaType::AUDIO,
            });
        s_server->add_track(espp::RtspServer::TrackConfig{
            .track_id = 1,
            .packetizer = audio_packer,
        });

        ESP_LOGI(TAG, "RTSP server initialized: rtsp://admin:***@%s:%d%s",
                 ip.c_str(), RTSP_PORT, RTSP_PATH);
        return ESP_OK;

}

extern "C" esp_err_t rtsp_server_start(void)
{
    if (!s_server) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

        /* Refresh IP in case WiFi connected after init */
        std::string ip = get_ip_string();
        if (ip != "0.0.0.0") {
            ESP_LOGI(TAG, "RTSP server IP: %s", ip.c_str());
        }

        if (!s_server->start()) {
            ESP_LOGE(TAG, "Failed to start RTSP server");
            return ESP_FAIL;
        }

        s_running = true;

        /* Spawn video feed task on Core 1 */
        BaseType_t created = xTaskCreatePinnedToCore(
            video_feed_task,
            "rtsp_video",
            VIDEO_TASK_STACK,
            NULL,
            VIDEO_TASK_PRIORITY,
            &s_video_task,
            1);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create video feed task");
            s_server->stop();
            s_running = false;
            return ESP_FAIL;
        }

        /* Spawn audio feed task on Core 0 — low-latency audio near I2S capture */
        created = xTaskCreatePinnedToCore(
            audio_feed_task,
            "rtsp_audio",
            AUDIO_TASK_STACK,
            NULL,
            AUDIO_TASK_PRIORITY,
            &s_audio_task,
            0);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create audio feed task");
            s_running = false;
            vTaskDelete(s_video_task);
            s_video_task = NULL;
            s_server->stop();
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "RTSP server started");
        return ESP_OK;
}

extern "C" esp_err_t rtsp_server_stop(void)
{
    s_running = false;

    /* Let feed tasks exit gracefully so they can release any held frame
     * reference. Previously vTaskDelete() leaked the in-flight frame_buf
     * (~30-80KB PSRAM per task). Each feed_task's fbroadcast_receive /
     * audio_bcast_receive times out within FEED_TIMEOUT_MS (1000ms), so the
     * loop re-checks s_running and exits promptly. */
    for (int i = 0; i < 20 && (s_video_task || s_audio_task); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_video_task) {
        /* Timed out (shouldn't happen) — force-kill as last resort. Any
         * held frame_buf leaks but is bounded to one frame per task. */
        ESP_LOGW(TAG, "video feed task did not exit in time, force-killing");
        vTaskDelete(s_video_task);
        s_video_task = NULL;
    }
    if (s_audio_task) {
        ESP_LOGW(TAG, "audio feed task did not exit in time, force-killing");
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }

    if (s_server) {
        s_server->stop();
    }

    ESP_LOGI(TAG, "RTSP server stopped");
    return ESP_OK;
}

extern "C" int rtsp_server_client_count(void)
{
    if (!s_server) {
        return 0;
    }
    return (int)s_server->get_num_sessions();
}
