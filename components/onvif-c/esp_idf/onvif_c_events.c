/*
 * onvif-c ESP-IDF port — Pull-Point events service.
 *
 * Extracted from the MiBee Cam firmware (main/onvif_events.c, contract
 * v1.5 semantics): single subscription (new replaces old), 1h granted
 * TerminationTime, 120s idle expiry, no long polling — PullMessages
 * returns immediately, pacing is the client's business.
 */

#include "onvif_c_events.h"
#include "onvif_c_port.h"
#include "../core/onvif_xml.h"
#include "../core/onvif_events_ring.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "onvif_c_ev";

#define ONVIF_EV_BODY_MAX  4096 /* accepted request body ceiling */
#define ONVIF_EV_RESP_MAX  4096 /* PullMessages response buffer */
#define ONVIF_EV_PULL_MAX  6    /* max events per PullMessages (buffer) */
#define SUB_LIFETIME_S     3600 /* granted TerminationTime */
#define SUB_IDLE_TIMEOUT_S 120  /* auto-expire without pulls */

static struct {
    SemaphoreHandle_t    mtx;
    bool                 sub_valid;
    time_t               sub_termination;
    time_t               sub_last_pull;
    onvif_c_event_ring_t ring;
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
    /* Loop: a partial recv is one TCP segment, not a dead request. */
    size_t got = 0;
    while (got < len) {
        int ret = httpd_req_recv(req, buf + got, len - got);
        if (ret <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)ret;
    }
    buf[len] = '\0';
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
    int  len = onvif_xml_events_fault(resp, sizeof(resp), subcode, text);
    if (len <= 0 || (size_t)len >= sizeof(resp)) {
        return ESP_FAIL;
    }
    return ev_send(req, resp);
}

/* Subscription validity (with expiry convergence). Caller holds the lock. */
static bool sub_alive(time_t now)
{
    if (!s_ev.sub_valid) {
        return false;
    }
    if (now > s_ev.sub_termination || (now - s_ev.sub_last_pull) > SUB_IDLE_TIMEOUT_S) {
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
    bool replaced        = s_ev.sub_valid;
    s_ev.sub_valid       = true;
    s_ev.sub_termination = now + SUB_LIFETIME_S;
    s_ev.sub_last_pull   = now;
    onvif_c_ring_reset(&s_ev.ring);
    xSemaphoreGive(s_ev.mtx);
    ESP_LOGI(TAG, "Pull-Point subscription created%s", replaced ? " (replaced previous)" : "");

    char now_s[24], term_s[24];
    iso8601(now, now_s, sizeof(now_s));
    iso8601(now + SUB_LIFETIME_S, term_s, sizeof(term_s));

    char resp[1024];
    int  len = onvif_xml_create_pull_point_response(resp, sizeof(resp), onvif_c_cfg_ip(),
                                                    onvif_c_cfg()->http_port, now_s, term_s);
    if (len <= 0 || (size_t)len >= sizeof(resp)) {
        return ev_fault(req, "ter:ActionNotSupported", "response overflow");
    }
    return ev_send(req, resp);
}

static esp_err_t handle_pull_messages(httpd_req_t *req, const char *body)
{
    /* MessageLimit (optional): default/cap ONVIF_EV_PULL_MAX */
    int         limit = ONVIF_EV_PULL_MAX;
    const char *ml    = body ? strstr(body, "MessageLimit") : NULL;
    if (ml) {
        const char *gt = strchr(ml, '>');
        int         v  = gt ? atoi(gt + 1) : 0;
        if (v > 0 && v < limit) {
            limit = v;
        }
    }

    time_t now = time(NULL);
    char   now_s[24], term_s[24];
    iso8601(now, now_s, sizeof(now_s));

    /* Response assembled dynamically (max 6 events x ~440B + envelope). */
    size_t cap  = ONVIF_EV_RESP_MAX;
    char  *resp = malloc(cap);
    if (!resp) {
        return ev_fault(req, "ter:ActionNotSupported", "oom");
    }
    int  off       = onvif_xml_pull_open(resp, cap);
    bool truncated = off <= 0 || (size_t)off >= cap;

    int delivered = 0;
    xSemaphoreTake(s_ev.mtx, portMAX_DELAY);
    bool alive = sub_alive(now);
    if (alive) {
        s_ev.sub_last_pull = now;
        iso8601(s_ev.sub_termination, term_s, sizeof(term_s));
        onvif_c_event_t e;
        while (!truncated && delivered < limit && onvif_c_ring_pop(&s_ev.ring, &e)) {
            char ts[24];
            iso8601((time_t)e.utc, ts, sizeof(ts));
            int w = onvif_xml_pull_event(resp + off, cap - off, ts, e.active, e.score);
            if (w <= 0 || (size_t)w >= cap - off) {
                truncated = true;
                break;
            }
            off += w;
            delivered++;
        }
    }
    xSemaphoreGive(s_ev.mtx);

    if (!alive) {
        free(resp);
        return ev_fault(req, "ter:SubscriptionReferenceDereferenced",
                        "no active subscription (expired)");
    }

    if (truncated) {
        free(resp);
        return ev_fault(req, "ter:ActionNotSupported", "response overflow");
    }
    int wc = onvif_xml_pull_close(resp + off, cap - off, now_s, term_s);
    if (wc <= 0 || (size_t)wc >= cap - off) {
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

    char term_s[24];
    iso8601(now + SUB_LIFETIME_S, term_s, sizeof(term_s));

    char resp[512];
    int  len = onvif_xml_renew_response(resp, sizeof(resp), term_s);
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

    return ev_send(req, onvif_xml_unsubscribe_response());
}

/* ------------------------------------------------------------------ */
/*  HTTP handler                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t events_service_handler(httpd_req_t *req)
{
    char     *body = ev_read_body(req);
    esp_err_t ret;
    if (!body) {
        return ev_fault(req, "ter:ActionNotSupported", "empty/oversized body");
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

void onvif_c_motion(bool active, uint8_t score)
{
    if (!s_ev.mtx) {
        return;
    }
    const onvif_c_config_t *cfg = onvif_c_cfg();
    if (cfg->events_enabled && !cfg->events_enabled()) {
        return;
    }
    /* Producer contract: never block; drop on lock contention. */
    if (xSemaphoreTake(s_ev.mtx, 0) != pdTRUE) {
        return;
    }
    if (s_ev.sub_valid) {
        onvif_c_ring_push(&s_ev.ring, active, score, (int64_t)time(NULL));
    }
    xSemaphoreGive(s_ev.mtx);
}

bool onvif_c_events_subscribed(void)
{
    return s_ev.mtx && s_ev.sub_valid;
}

esp_err_t onvif_c_events_register(httpd_handle_t server)
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
        ESP_LOGE(TAG, "Failed to register events service: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Registered /onvif/events_service (Pull-Point, MotionAlarm)");
    return ESP_OK;
}
