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

/**
 * @file onvif_events.c
 * @brief ONVIF Pull-Point 事件服务（契约 v1.5）——见 onvif_events.h 总注释。
 */

#include "onvif_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "onvif_events";

/* —— 板级适配（与 n16r8 版本仅此两函数有差异）—— */

static bool motion_alarms_enabled(void)
{
    return config_get()->onvif_events != 0;
}

static const char *device_ip(void)
{
    const char *ip = wifi_get_ip_str();
    return (ip && strcmp(ip, "0.0.0.0") != 0) ? ip : "0.0.0.0";
}

/* —— 常量 —— */

#define ONVIF_EV_BODY_MAX    4096   /* 接受的请求体上限（对齐 onvif_service.c） */
#define ONVIF_EV_QUEUE_MAX     12   /* 每订阅事件队列深度（溢出丢最旧） */
#define ONVIF_EV_PULL_MAX       6   /* 单次 PullMessages 最多吐出（响应缓冲约束） */
#define SUB_LIFETIME_S       3600   /* 授予的 TerminationTime */
#define SUB_IDLE_TIMEOUT_S    120   /* 无 PullMessages 自动过期 */

#define NS_EV  "http://www.onvif.org/ver10/events/wsdl"
#define NS_WSN "http://docs.oasis-open.org/wsn/b-2"
#define NS_WSA "http://www.w3.org/2005/08/addressing"
#define NS_TT  "http://www.onvif.org/ver10/schema"

typedef struct {
    bool    active;
    uint8_t score;
    time_t  utc;
} motion_evt_t;

static struct {
    SemaphoreHandle_t mtx;
    bool     sub_valid;
    time_t   sub_termination;
    time_t   sub_last_pull;
    motion_evt_t q[ONVIF_EV_QUEUE_MAX];
    int      head, count;
    uint32_t generated;    /* 累计入队数（诊断） */
} s_ev;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void iso8601(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static char *ev_read_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > ONVIF_EV_BODY_MAX) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        return NULL;
    }
    buf[ret] = '\0';
    return buf;
}

static esp_err_t ev_send(httpd_req_t *req, const char *xml)
{
    httpd_resp_set_type(req, "application/soap+xml");
    return httpd_resp_send(req, xml, strlen(xml));
}

static esp_err_t ev_fault(httpd_req_t *req, const char *subcode, const char *text)
{
    char resp[768];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body><s:Fault>"
        "<s:Code><s:Value>s:Sender</s:Value>"
        "<s:Subcode><s:Value>%s</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">%s</s:Text></s:Reason>"
        "</s:Fault></s:Body></s:Envelope>",
        subcode, text);
    if (len <= 0 || (size_t)len >= sizeof(resp)) {
        return ESP_FAIL;
    }
    return ev_send(req, resp);
}

/* 订阅有效性检查（含过期收敛）。调用方持锁。 */
static bool sub_alive(time_t now)
{
    if (!s_ev.sub_valid) {
        return false;
    }
    if (now > s_ev.sub_termination ||
        (now - s_ev.sub_last_pull) > SUB_IDLE_TIMEOUT_S) {
        s_ev.sub_valid = false;
        ESP_LOGI(TAG, "Subscription expired (idle/termination)");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  SOAP actions                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handle_create_pull_point(httpd_req_t *req)
{
    time_t now = time(NULL);
    xSemaphoreTake(s_ev.mtx, portMAX_DELAY);
    bool replaced = s_ev.sub_valid;
    s_ev.sub_valid = true;
    s_ev.sub_termination = now + SUB_LIFETIME_S;
    s_ev.sub_last_pull = now;
    s_ev.head = 0;
    s_ev.count = 0;
    xSemaphoreGive(s_ev.mtx);
    ESP_LOGI(TAG, "Pull-Point subscription created%s",
             replaced ? " (replaced previous)" : "");

    char now_s[24], term_s[24];
    iso8601(now, now_s, sizeof(now_s));
    iso8601(now + SUB_LIFETIME_S, term_s, sizeof(term_s));

    char resp[1024];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tev:CreatePullPointSubscriptionResponse xmlns:tev=\"" NS_EV "\" "
        "xmlns:wsa=\"" NS_WSA "\" xmlns:wsnt=\"" NS_WSN "\">"
        "<tev:SubscriptionReference>"
        "<wsa:Address>http://%s:80/onvif/events_service</wsa:Address>"
        "</tev:SubscriptionReference>"
        "<wsnt:CurrentTime>%s</wsnt:CurrentTime>"
        "<wsnt:TerminationTime>%s</wsnt:TerminationTime>"
        "</tev:CreatePullPointSubscriptionResponse>"
        "</s:Body></s:Envelope>",
        device_ip(), now_s, term_s);
    if (len <= 0 || (size_t)len >= sizeof(resp)) {
        return ev_fault(req, "ter:ActionNotSupported", "response overflow");
    }
    return ev_send(req, resp);
}

static esp_err_t handle_pull_messages(httpd_req_t *req, const char *body)
{
    /* MessageLimit（可选）：默认/封顶 ONVIF_EV_PULL_MAX */
    int limit = ONVIF_EV_PULL_MAX;
    const char *ml = body ? strstr(body, "MessageLimit") : NULL;
    if (ml) {
        const char *gt = strchr(ml, '>');
        int v = gt ? atoi(gt + 1) : 0;
        if (v > 0 && v < limit) {
            limit = v;
        }
    }

    time_t now = time(NULL);
    char now_s[24], term_s[24];
    iso8601(now, now_s, sizeof(now_s));

    /* 响应动态拼装（最多 6 条 × ~440B + 外壳） */
    size_t cap = 4096;
    char *resp = malloc(cap);
    if (!resp) {
        return ev_fault(req, "ter:ActionNotSupported", "oom");
    }
    int off = snprintf(resp, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tev:PullMessagesResponse xmlns:tev=\"" NS_EV "\" "
        "xmlns:wsnt=\"" NS_WSN "\" xmlns:tt=\"" NS_TT "\">");

    int delivered = 0;
    xSemaphoreTake(s_ev.mtx, portMAX_DELAY);
    bool alive = sub_alive(now);
    if (alive) {
        s_ev.sub_last_pull = now;
        iso8601(s_ev.sub_termination, term_s, sizeof(term_s));
        while (delivered < limit && s_ev.count > 0) {
            const motion_evt_t *e = &s_ev.q[s_ev.head];
            char ts[24];
            iso8601(e->utc, ts, sizeof(ts));
            off += snprintf(resp + off, cap - off,
                "<wsnt:NotificationMessage>"
                "<wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">"
                "tns1:VideoSource/MotionAlarm</wsnt:Topic>"
                "<wsnt:Message><tt:Message UtcTime=\"%s\">"
                "<tt:Source><tt:SimpleItem Name=\"Source\" Value=\"CSI\"/></tt:Source>"
                "<tt:Data>"
                "<tt:SimpleItem Name=\"State\" Value=\"%s\"/>"
                "<tt:SimpleItem Name=\"Score\" Value=\"%u\"/>"
                "</tt:Data></tt:Message></wsnt:Message>"
                "</wsnt:NotificationMessage>",
                ts, e->active ? "true" : "false", e->score);
            s_ev.head = (s_ev.head + 1) % ONVIF_EV_QUEUE_MAX;
            s_ev.count--;
            delivered++;
        }
    }
    xSemaphoreGive(s_ev.mtx);

    if (!alive) {
        free(resp);
        return ev_fault(req, "ter:SubscriptionReferenceDereferenced",
                        "no active subscription (expired)");
    }

    off += snprintf(resp + off, cap - off,
        "<tev:CurrentTime>%s</tev:CurrentTime>"
        "<tev:TerminationTime>%s</tev:TerminationTime>"
        "</tev:PullMessagesResponse></s:Body></s:Envelope>",
        now_s, term_s);
    if (off <= 0 || (size_t)off >= cap) {
        free(resp);
        return ev_fault(req, "ter:ActionNotSupported", "response overflow");
    }
    esp_err_t r = ev_send(req, resp);
    free(resp);
    return r;
}

static esp_err_t handle_renew(httpd_req_t *req)
{
    time_t now = time(NULL);
    xSemaphoreTake(s_ev.mtx, portMAX_DELAY);
    bool alive = sub_alive(now);
    if (alive) {
        s_ev.sub_termination = now + SUB_LIFETIME_S;
    }
    xSemaphoreGive(s_ev.mtx);
    if (!alive) {
        return ev_fault(req, "ter:SubscriptionReferenceDereferenced",
                        "no active subscription (expired)");
    }

    char now_s[24], term_s[24];
    iso8601(now, now_s, sizeof(now_s));
    iso8601(now + SUB_LIFETIME_S, term_s, sizeof(term_s));

    char resp[512];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<wsnt:RenewResponse xmlns:wsnt=\"" NS_WSN "\">"
        "<wsnt:TerminationTime>%s</wsnt:TerminationTime>"
        "</wsnt:RenewResponse>"
        "</s:Body></s:Envelope>",
        term_s);
    if (len <= 0 || (size_t)len >= sizeof(resp)) {
        return ESP_FAIL;
    }
    return ev_send(req, resp);
}

static esp_err_t handle_unsubscribe(httpd_req_t *req)
{
    xSemaphoreTake(s_ev.mtx, portMAX_DELAY);
    s_ev.sub_valid = false;
    xSemaphoreGive(s_ev.mtx);
    ESP_LOGI(TAG, "Subscription closed by client");

    const char *resp =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<wsnt:UnsubscribeResponse xmlns:wsnt=\"" NS_WSN "\"/>"
        "</s:Body></s:Envelope>";
    return ev_send(req, resp);
}

/* ------------------------------------------------------------------ */
/*  HTTP handler                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t events_service_handler(httpd_req_t *req)
{
    char *body = ev_read_body(req);
    esp_err_t ret;
    if (!body) {
        ret = ev_fault(req, "ter:ActionNotSupported", "empty/oversized body");
        return ret;
    }
    if (strstr(body, "CreatePullPointSubscription")) {
        ret = handle_create_pull_point(req);
    } else if (strstr(body, "PullMessages")) {
        ret = handle_pull_messages(req, body);
    } else if (strstr(body, "Renew")) {
        ret = handle_renew(req);
    } else if (strstr(body, "Unsubscribe")) {
        ret = handle_unsubscribe(req);
    } else {
        ESP_LOGW(TAG, "Unsupported events action");
        ret = ev_fault(req, "ter:ActionNotSupported", "Action not supported");
    }
    free(body);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void onvif_events_motion(bool active, uint8_t score)
{
    if (!s_ev.mtx || !motion_alarms_enabled()) {
        return;
    }
    /* ESPectre 回调契约：非阻塞；锁竞争即丢弃（事件可丢） */
    if (xSemaphoreTake(s_ev.mtx, 0) != pdTRUE) {
        return;
    }
    if (s_ev.sub_valid) {
        if (s_ev.count == ONVIF_EV_QUEUE_MAX) {
            s_ev.head = (s_ev.head + 1) % ONVIF_EV_QUEUE_MAX;
            s_ev.count--;
        }
        motion_evt_t *e = &s_ev.q[(s_ev.head + s_ev.count) % ONVIF_EV_QUEUE_MAX];
        e->active = active;
        e->score = score;
        e->utc = time(NULL);
        s_ev.count++;
        s_ev.generated++;
    }
    xSemaphoreGive(s_ev.mtx);
}

bool onvif_events_subscribed(void)
{
    return s_ev.mtx && s_ev.sub_valid;
}

esp_err_t onvif_events_register(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ev.mtx) {
        s_ev.mtx = xSemaphoreCreateMutex();
        if (!s_ev.mtx) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_uri_t uri = {
        .uri      = "/onvif/events_service",
        .method   = HTTP_POST,
        .handler  = events_service_handler,
        .user_ctx = NULL,
    };
    esp_err_t ret = httpd_register_uri_handler(server, &uri);
    if (ret != ESP_OK && ret != ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGE(TAG, "Failed to register events service: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Registered /onvif/events_service (Pull-Point, MotionAlarm)");
    return ESP_OK;
}
