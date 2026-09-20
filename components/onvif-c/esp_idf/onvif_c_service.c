/*
 * onvif-c ESP-IDF port — SOAP Device/Media service handlers + lifecycle.
 *
 * Extracted from the MiBee Cam firmware (esp32s3-n16r8-cam main/onvif_service.c);
 * board symbols replaced by the onvif_c_config_t callback seam.
 */

#include "onvif_c_port.h"
#include "onvif_c_events.h"
#include "../core/onvif_xml.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "onvif_c_svc";

#define ONVIF_C_BODY_MAX  4096
#define ONVIF_C_RESP_MAX  4096

static onvif_c_config_t s_cfg;

/* ------------------------------------------------------------------ */
/*  Config seam                                                        */
/* ------------------------------------------------------------------ */

const onvif_c_config_t *onvif_c_cfg(void)
{
    return &s_cfg;
}

void onvif_c_cfg_set(const onvif_c_config_t *cfg)
{
    s_cfg = *cfg;
    if (!s_cfg.manufacturer)     s_cfg.manufacturer = "MiBee";
    if (!s_cfg.model)            s_cfg.model = "MiBeeCam";
    if (!s_cfg.hardware_id)      s_cfg.hardware_id = "ESP32";
    if (!s_cfg.firmware_version) s_cfg.firmware_version = "v0.1.0";
    if (s_cfg.http_port == 0)    s_cfg.http_port = 80;
}

const char *onvif_c_cfg_ip(void)
{
    const char *ip = s_cfg.ip ? s_cfg.ip() : NULL;
    return (ip && strcmp(ip, "0.0.0.0") != 0) ? ip : "0.0.0.0";
}

const char *onvif_c_cfg_scopes(void)
{
    if (s_cfg.scopes) {
        return s_cfg.scopes;
    }
    static char buf[256];
    snprintf(buf, sizeof(buf),
             "onvif://www.onvif.org/type/video_encoder "
             "onvif://www.onvif.org/type/NetworkVideoTransmitter "
             "onvif://www.onvif.org/hardware/%s "
             "onvif://www.onvif.org/name/%s "
             "onvif://www.onvif.org/Profile/Streaming",
             s_cfg.model, s_cfg.model);
    return buf;
}

bool onvif_c_cfg_has_events(void)
{
    return s_cfg.events_enabled != NULL;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static char *read_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > ONVIF_C_BODY_MAX) {
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

/** Build + send: fn writes into a heap buffer of ONVIF_C_RESP_MAX. */
#define SEND_BUILT(req, build)                                        \
    do {                                                              \
        char *resp_ = malloc(ONVIF_C_RESP_MAX);                       \
        if (!resp_) return ESP_FAIL;                                  \
        int len_ = (build);                                           \
        httpd_resp_set_type(req, "application/soap+xml");             \
        httpd_resp_send(req, resp_, len_ > 0 ? len_ : 0);             \
        free(resp_);                                                  \
        return ESP_OK;                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Device service actions                                             */
/* ------------------------------------------------------------------ */

static esp_err_t handle_get_system_date_and_time(httpd_req_t *req)
{
    time_t now = time(NULL);
    struct tm utc_tm;
    gmtime_r(&now, &utc_tm);
    SEND_BUILT(req, onvif_xml_system_date_and_time(resp_, ONVIF_C_RESP_MAX,
                                                   &utc_tm));
}

static esp_err_t handle_get_device_information(httpd_req_t *req)
{
    SEND_BUILT(req, onvif_xml_device_information(
        resp_, ONVIF_C_RESP_MAX,
        s_cfg.manufacturer, s_cfg.model, s_cfg.firmware_version,
        s_cfg.serial ? s_cfg.serial() : "000000000000", s_cfg.hardware_id));
}

static esp_err_t handle_get_capabilities(httpd_req_t *req)
{
    SEND_BUILT(req, onvif_xml_capabilities(resp_, ONVIF_C_RESP_MAX,
                                           onvif_c_cfg_ip(),
                                           onvif_c_cfg_has_events()));
}

/* ------------------------------------------------------------------ */
/*  Media service actions                                              */
/* ------------------------------------------------------------------ */

static esp_err_t handle_get_profiles(httpd_req_t *req)
{
    int fps = s_cfg.frame_rate ? s_cfg.frame_rate() : 15;
    SEND_BUILT(req, onvif_xml_profiles(resp_, ONVIF_C_RESP_MAX, fps));
}

static esp_err_t handle_get_stream_uri(httpd_req_t *req)
{
    const char *uri = s_cfg.stream_uri ? s_cfg.stream_uri()
                                       : "rtsp://0.0.0.0:554/stream";
    SEND_BUILT(req, onvif_xml_stream_uri(resp_, ONVIF_C_RESP_MAX, uri));
}

static esp_err_t handle_get_snapshot(httpd_req_t *req)
{
    if (s_cfg.snapshot_uri) {
        SEND_BUILT(req, onvif_xml_snapshot_uri(resp_, ONVIF_C_RESP_MAX,
                                               s_cfg.snapshot_uri()));
    }
    char uri[128];
    snprintf(uri, sizeof(uri), "http://%s:%u/api/capture",
             onvif_c_cfg_ip(), (unsigned)s_cfg.http_port);
    SEND_BUILT(req, onvif_xml_snapshot_uri(resp_, ONVIF_C_RESP_MAX, uri));
}

/* ------------------------------------------------------------------ */
/*  SOAP action dispatch                                               */
/* ------------------------------------------------------------------ */

static void log_unsupported(const char *body)
{
    const char *act_start = strstr(body, ":Body>");
    if (act_start) {
        act_start = strchr(act_start + 6, '<');
        if (act_start) {
            act_start++;
            const char *act_end = strchr(act_start, ' ');
            if (!act_end) act_end = strchr(act_start, '>');
            if (act_end) {
                char action[64] = {0};
                int len = act_end - act_start;
                if (len > 63) len = 63;
                memcpy(action, act_start, len);
                ESP_LOGW(TAG, "Unsupported action: %s", action);
            }
        }
    }
}

static esp_err_t send_fault(httpd_req_t *req)
{
    char *resp = malloc(ONVIF_C_RESP_MAX);
    if (!resp) {
        return ESP_FAIL;
    }
    int len = onvif_xml_fault_action_not_supported(resp, ONVIF_C_RESP_MAX);
    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len > 0 ? len : 0);
    free(resp);
    return ESP_OK;
}

static esp_err_t dispatch_device_action(httpd_req_t *req, const char *body)
{
    if (!body) {
        return send_fault(req);
    }
    if (strstr(body, "GetSystemDateAndTime")) {
        return handle_get_system_date_and_time(req);
    }
    if (strstr(body, "GetDeviceInformation")) {
        return handle_get_device_information(req);
    }
    if (strstr(body, "GetCapabilities")) {
        return handle_get_capabilities(req);
    }
    log_unsupported(body);
    return send_fault(req);
}

static esp_err_t dispatch_media_action(httpd_req_t *req, const char *body)
{
    if (!body) {
        return send_fault(req);
    }
    if (strstr(body, "GetProfiles")) {
        return handle_get_profiles(req);
    }
    if (strstr(body, "GetStreamUri")) {
        return handle_get_stream_uri(req);
    }
    if (strstr(body, "GetSnapshotUri") || strstr(body, "GetSnapshot")) {
        return handle_get_snapshot(req);
    }
    log_unsupported(body);
    return send_fault(req);
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t device_service_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body) {
        ESP_LOGI(TAG, "DEVICE REQ [first 200]: %.200s", body);
    }
    esp_err_t ret = dispatch_device_action(req, body);
    free(body);
    return ret;
}

static esp_err_t media_service_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body) {
        ESP_LOGI(TAG, "MEDIA REQ [first 200]: %.200s", body);
    }
    esp_err_t ret = dispatch_media_action(req, body);
    free(body);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

static esp_err_t register_one(httpd_handle_t server, const char *uri,
                              esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {
        .uri      = uri,
        .method   = HTTP_POST,
        .handler  = handler,
        .user_ctx = NULL,
    };
    esp_err_t ret = httpd_register_uri_handler(server, &u);
    if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        ESP_LOGW(TAG, "%s handler already registered", uri);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s: %s", uri, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Registered %s", uri);
    return ESP_OK;
}

esp_err_t onvif_c_start(httpd_handle_t httpd, const onvif_c_config_t *cfg)
{
    if (!httpd || !cfg || !cfg->serial || !cfg->uuid || !cfg->ip ||
        !cfg->stream_uri) {
        return ESP_ERR_INVALID_ARG;
    }
    onvif_c_cfg_set(cfg);

    esp_err_t ret = register_one(httpd, "/onvif/device_service",
                                 device_service_handler);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = register_one(httpd, "/onvif/media_service", media_service_handler);
    if (ret != ESP_OK) {
        return ret;
    }

    if (onvif_c_cfg_has_events()) {
        ret = onvif_c_events_register(httpd);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return onvif_c_discovery_start();
}

esp_err_t onvif_c_stop(void)
{
    return onvif_c_discovery_stop();
}

int onvif_c_version(void)
{
    return ONVIF_C_VERSION;
}
