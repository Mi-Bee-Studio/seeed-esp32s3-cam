/*
 * MiBee Cam — ONVIF 板级适配层（onvif-c 组件接缝）
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 唯一的板级差异面：把固件的 wifi/config/ota/device_id 符号接进
 * onvif_c_config_t 回调（组件本身零板级 include）。原
 * onvif_service/discovery/events 三件的启动语义在此合一：
 * SOAP 处理器注册（原 web_server_start 内）+ WS-Discovery（原
 * main.c 第 15 步）+ onvif_enable 门控（原两处门控收拢于此）。
 */

#include "onvif_port.h"
#include "onvif_c.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "ota_updater.h"   /* FW_VERSION（原 onvif_service.c 同源） */
#include "device_id.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "web_server.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "onvif_port";

static const char *port_serial(void)
{
    return device_get_serial();
}

static const char *port_uuid(void)
{
    return device_get_uuid();
}

static const char *port_ip(void)
{
    return wifi_get_ip_str();
}

/* 原 onvif_service.c GetStreamUri 的默认分支：HTTP MJPEG :81。
 * （旧实现对请求体含 RTP-RTSP/RTSP 的客户端回 rtsp://ip:554/stream；
 * 组件回调拿不到请求体，统一回 MJPEG URI —— 本板 :81 流无鉴权、
 * NVR 拉流行为不变。） */
static const char *port_stream_uri(void)
{
    static char uri[64];
    const char *ip = wifi_get_ip_str();
    if (!ip || strcmp(ip, "0.0.0.0") == 0) {
        ip = "0.0.0.0";
    }
    snprintf(uri, sizeof(uri), "http://%s:81/stream", ip);
    return uri;
}

static uint8_t port_frame_rate(void)
{
    return config_get()->cam_fps;   /* 契约 §3.1 cam_fps 消费者 */
}

static bool port_events_enabled(void)
{
    return config_get()->onvif_events != 0;   /* 契约 v1.5：MotionAlarm 生成开关 */
}

esp_err_t onvif_port_start(void)
{
    /* 运行时开关（原 web_server_start/main.c 两处 onvif_enable 门的合一） */
    if (!config_get()->onvif_enable) {
        ESP_LOGI(TAG, "ONVIF disabled by config (onvif_enable=0)");
        return ESP_OK;
    }

    const onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "XIAO-ESP32S3-SENSE",
        .firmware_version = FW_VERSION,
        .serial           = port_serial,
        .uuid             = port_uuid,
        .ip               = port_ip,
        .stream_uri       = port_stream_uri,
        .frame_rate       = port_frame_rate,
        .events_enabled   = port_events_enabled,
        .http_port        = 80,
        /* mDNS 归 wifi_manager 所有（mibee_cam-XXXX + _http._tcp）：
         * 组件再 init 会与既有实例冲突并翻主机名，故置 NULL 跳过。 */
        .mdns_hostname    = NULL,
        .mdns_instance    = NULL,
    };

    httpd_handle_t server = web_server_get_handle();
    if (!server) {
        ESP_LOGW(TAG, "Web server not available, ONVIF skipped");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = onvif_c_start(server, &cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ONVIF services started (onvif-c component)");
    }
    return err;
}
