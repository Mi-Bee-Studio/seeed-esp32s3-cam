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
 * @file onvif_service.c
 * @brief Minimal ONVIF SOAP service handlers for NVR discovery.
 *
 * Implements only the SOAP actions required for NVRs to discover and
 * add this camera: GetDeviceInformation, GetCapabilities, GetServices,
 * GetProfiles, GetStreamUri.
 *
 * No XML parser is used — action detection via strstr(), response
 * generation via snprintf().
 */

#include "onvif_service.h"
#include "esp_log.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "ota_updater.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "onvif_svc";

/* Namespace prefixes used in SOAP responses */
#define NS_DEV "http://www.onvif.org/ver10/device/wsdl"
#define NS_MED "http://www.onvif.org/ver10/media/wsdl"

/* Maximum SOAP body size we accept */
#define ONVIF_BODY_MAX 4096
/* Maximum response buffer size */
#define ONVIF_RESP_MAX 4096

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Map internal resolution index (0=VGA, 1=SVGA, 2=XGA) to pixel dimensions.
 */
static void resolution_to_wh(uint8_t res, int *w, int *h)
{
    switch (res) {
        case 0:
            *w = 640;  *h = 480;
            break;
        case 1:
            *w = 800;  *h = 600;
            break;
        case 2:
            *w = 1024; *h = 768;
            break;
        default:
            *w = 640;  *h = 480;
            break;
    }
}

/**
 * @brief Read SOAP request body into a heap buffer (caller frees).
 */
static char *onvif_read_body(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > ONVIF_BODY_MAX) {
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

/**
 * @brief Send a SOAP fault response for unrecognized actions.
 */
static esp_err_t send_soap_fault(httpd_req_t *req)
{
    const char *fault =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<s:Fault>"
        "<s:Code>"
        "<s:Value>s:Receiver</s:Value>"
        "<s:Subcode>"
        "<s:Value>ter:ActionNotSupported</s:Value>"
        "</s:Subcode>"
        "</s:Code>"
        "<s:Reason>"
        "<s:Text xml:lang=\"en\">Action not supported</s:Text>"
        "</s:Reason>"
        "</s:Fault>"
        "</s:Body>"
        "</s:Envelope>";

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, fault, strlen(fault));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

/**
 * @brief Handle GetSystemDateAndTime SOAP action.
 * Returns current system date and time. Required by most NVRs during discovery.
 */
static esp_err_t handle_get_system_date_and_time(httpd_req_t *req)
{
    time_t now = time(NULL);
    struct tm utc_tm;
    gmtime_r(&now, &utc_tm);

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tcr:GetSystemDateAndTimeResponse "
        "xmlns:tcr=\"" NS_DEV "\">"
        "<tcr:SystemDateAndTime>"
        "<tcr:DateTimeType>NTP</tcr:DateTimeType>"
        "<tcr:DaylightSavings>false</tcr:DaylightSavings>"
        "<tcr:UTCDateTime>"
        "<tcr:Time>"
        "<tcr:Hour>%d</tcr:Hour>"
        "<tcr:Minute>%d</tcr:Minute>"
        "<tcr:Second>%d</tcr:Second>"
        "</tcr:Time>"
        "<tcr:Date>"
        "<tcr:Year>%d</tcr:Year>"
        "<tcr:Month>%d</tcr:Month>"
        "<tcr:Day>%d</tcr:Day>"
        "</tcr:Date>"
        "</tcr:UTCDateTime>"
        "</tcr:SystemDateAndTime>"
        "</tcr:GetSystemDateAndTimeResponse>"
        "</s:Body>"
        "</s:Envelope>",
        utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
        utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}
/*  Device Service Actions                                            */
/* ------------------------------------------------------------------ */

/**
 * @brief Handle GetDeviceInformation SOAP action.
 * Returns manufacturer, model, firmware version, serial number (MAC), hardware ID.
 */
static esp_err_t handle_get_device_information(httpd_req_t *req)
{
    /* Get serial number from MAC address */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial_no[24];
    snprintf(serial_no, sizeof(serial_no),
             "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tcr:GetDeviceInformationResponse "
        "xmlns:tcr=\"" NS_DEV "\">"
        "<tcr:Manufacturer>MiBee</tcr:Manufacturer>"
        "<tcr:Model>MiBee Cam</tcr:Model>"
        "<tcr:FirmwareVersion>%s</tcr:FirmwareVersion>"
        "<tcr:SerialNumber>%s</tcr:SerialNumber>"
        "<tcr:HardwareId>ESP32-S3</tcr:HardwareId>"
        "</tcr:GetDeviceInformationResponse>"
        "</s:Body>"
        "</s:Envelope>",
        FW_VERSION,
        serial_no);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/**
 * @brief Handle GetCapabilities SOAP action.
 * Returns Device and Media service endpoints.
 */
static esp_err_t handle_get_capabilities(httpd_req_t *req)
{
    char *ip_str = wifi_get_ip_str();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        ip_str = "0.0.0.0";
    }

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tcr:GetCapabilitiesResponse "
        "xmlns:tcr=\"" NS_DEV "\">"
        "<tcr:Capabilities>"
        "<tcr:Device>"
        "<tcr:XAddr>http://%s:80/onvif/device_service</tcr:XAddr>"
        "</tcr:Device>"
        "<tcr:Media>"
        "<tcr:XAddr>http://%s:80/onvif/media_service</tcr:XAddr>"
        "</tcr:Media>"
        "</tcr:Capabilities>"
        "</tcr:GetCapabilitiesResponse>"
        "</s:Body>"
        "</s:Envelope>",
        ip_str, ip_str);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/**
 * @brief Handle GetServices SOAP action.
 * Returns Device and Media service entries with their URLs and versions.
 */
static esp_err_t handle_get_services(httpd_req_t *req)
{
    char *ip_str = wifi_get_ip_str();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        ip_str = "0.0.0.0";
    }

    /* Check if the request includes a category filter */
    /* NVRs typically call GetServices without category, returning all services */

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<tcr:GetServicesResponse "
        "xmlns:tcr=\"" NS_DEV "\">"
        "<tcr:Service>"
        "<tcr:Namespace>" NS_DEV "</tcr:Namespace>"
        "<tcr:XAddr>http://%s:80/onvif/device_service</tcr:XAddr>"
        "<tcr:Version>"
        "<tcr:Major>2</tcr:Major>"
        "<tcr:Minor>0</tcr:Minor>"
        "</tcr:Version>"
        "</tcr:Service>"
        "<tcr:Service>"
        "<tcr:Namespace>" NS_MED "</tcr:Namespace>"
        "<tcr:XAddr>http://%s:80/onvif/media_service</tcr:XAddr>"
        "<tcr:Version>"
        "<tcr:Major>2</tcr:Major>"
        "<tcr:Minor>0</tcr:Minor>"
        "</tcr:Version>"
        "</tcr:Service>"
        "</tcr:GetServicesResponse>"
        "</s:Body>"
        "</s:Envelope>",
        ip_str, ip_str);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Media Service Actions                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Handle GetProfiles SOAP action.
 * Returns one video profile with current resolution and encoder settings.
 */
static esp_err_t handle_get_profiles(httpd_req_t *req)
{
    cam_config_t *cfg = config_get();
    int width, height;
    resolution_to_wh(cfg->resolution, &width, &height);

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<trt:GetProfilesResponse "
        "xmlns:trt=\"" NS_MED "\">"
        "<trt:Profiles token=\"profile_1\">"
        "<trt:Name>MainStream</trt:Name>"
        "<trt:VideoSourceConfiguration token=\"video_src_1\">"
        "<trt:Name>VideoSource</trt:Name>"
        "<trt:Bounds x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"/>"
        "</trt:VideoSourceConfiguration>"
        "<trt:VideoEncoderConfiguration token=\"video_enc_1\">"
        "<trt:Name>VideoEncoder</trt:Name>"
        "<trt:Encoding>JPEG</trt:Encoding>"
        "<trt:Resolution>"
        "<trt:Width>%d</trt:Width>"
        "<trt:Height>%d</trt:Height>"
        "</trt:Resolution>"
        "<trt:Quality>5</trt:Quality>"
        "<trt:RateControl>"
        "<trt:FrameRateLimit>%d</trt:FrameRateLimit>"
        "</trt:RateControl>"
        "</trt:VideoEncoderConfiguration>"
        "</trt:Profiles>"
        "</trt:GetProfilesResponse>"
        "</s:Body>"
        "</s:Envelope>",
        width, height,
        width, height,
        cfg->fps);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/**
 * @brief Handle GetStreamUri SOAP action.
 * Returns the RTSP stream URI for the camera.
 */
static esp_err_t handle_get_stream_uri(httpd_req_t *req)
{
    char *ip_str = wifi_get_ip_str();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        ip_str = "0.0.0.0";
    }

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body>"
        "<trt:GetStreamUriResponse "
        "xmlns:trt=\"" NS_MED "\">"
        "<trt:MediaUri>"
        "<trt:Uri>rtsp://%s:554/stream</trt:Uri>"
        "</trt:MediaUri>"
        "</trt:GetStreamUriResponse>"
        "</s:Body>"
        "</s:Envelope>",
        ip_str);

    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  SOAP Action Dispatch                                              */
/* ------------------------------------------------------------------ */


/**
 * @brief Handle WS-Discovery Probe received over HTTP (directed discovery).
 * NVRs that know the device IP send Probe via HTTP POST instead of multicast.
 * We must respond with ProbeMatches containing the same info as our UDP response.
 */
static esp_err_t handle_http_probe(httpd_req_t *req, const char *body)
{
    char *ip_str = wifi_get_ip_str();
    if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
        return send_soap_fault(req);
    }

    /* Generate device UUID from MAC */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char device_uuid[64];
    snprintf(device_uuid, sizeof(device_uuid),
             "f472b01e-0000-1000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Extract MessageID from incoming Probe for RelatesTo */
    char relates_to[256] = "";
    const char *mid_tag = strstr(body, "MessageID");
    if (mid_tag) {
        const char *gt = strchr(mid_tag, '>');
        if (gt) {
            gt++;
            const char *lt = strchr(gt, '<');
            if (lt) {
                size_t len = lt - gt;
                if (len > 0 && len < sizeof(relates_to)) {
                    memcpy(relates_to, gt, len);
                    relates_to[len] = '\0';
                }
            }
        }
    }

    char resp[ONVIF_RESP_MAX];
    int len = snprintf(resp, sizeof(resp),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope "
        "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:tns=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<soap:Header>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
        "<wsa:RelatesTo>%s</wsa:RelatesTo>"
        "<wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>"
        "</soap:Header>"
        "<soap:Body>"
        "<wsd:ProbeMatches>"
        "<wsd:ProbeMatch>"
        "<wsa:EndpointReference>"
        "<wsa:Address>urn:uuid:%s</wsa:Address>"
        "</wsa:EndpointReference>"
        "<wsd:Types>tns:NetworkVideoTransmitter</wsd:Types>"
        "<wsd:Scopes>onvif://www.onvif.org/type/video_encoder "
        "onvif://www.onvif.org/type/NetworkVideoTransmitter "
        "onvif://www.onvif.org/hardware/MiBeeCam "
        "onvif://www.onvif.org/name/MiBeeCam "
        "onvif://www.onvif.org/Profile/Streaming</wsd:Scopes>"
        "<wsd:XAddrs>http://%s:80/onvif/device_service</wsd:XAddrs>"
        "<wsd:MetadataVersion>2</wsd:MetadataVersion>"
        "</wsd:ProbeMatch>"
        "</wsd:ProbeMatches>"
        "</soap:Body>"
        "</soap:Envelope>",
        device_uuid,
        relates_to,
        device_uuid,
        ip_str);

    if (len <= 0 || (size_t)len >= sizeof(resp)) {
        return send_soap_fault(req);
    }

    ESP_LOGI(TAG, "HTTP Probe -> ProbeMatches (%d bytes)", len);
    httpd_resp_set_type(req, "application/soap+xml");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

 /**
 * @brief Route a SOAP POST to the appropriate handler based on action name.
 *
 * @param req HTTP request
 * @param body SOAP XML body (null-terminated)
 */
static esp_err_t dispatch_device_action(httpd_req_t *req, const char *body)
{
    if (!body) {
        return send_soap_fault(req);
    }

    /* Handle WS-Discovery Probe over HTTP (directed discovery) */
    /* NVR sends SOAP with Action header containing .../discovery/Probe */
    if (strstr(body, "/Probe") && !strstr(body, "ProbeMatches")) {
        return handle_http_probe(req, body);
    }

    if (strstr(body, "GetDeviceInformation")) {
        return handle_get_device_information(req);
    }
    if (strstr(body, "GetCapabilities")) {
        return handle_get_capabilities(req);
    }
    if (strstr(body, "GetServices")) {
        return handle_get_services(req);
    }
    if (strstr(body, "GetSystemDateAndTime")) {
        return handle_get_system_date_and_time(req);
    }

    /* Unrecognized action — log and return SOAP fault */
    {
        /* Extract action name from SOAP body for debugging */
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
                    ESP_LOGW(TAG, "Unsupported device action: %s", action);
                }
            }
        }
    }
    return send_soap_fault(req);
}

static esp_err_t dispatch_media_action(httpd_req_t *req, const char *body)
{
    if (!body) {
        return send_soap_fault(req);
    }

    if (strstr(body, "GetProfiles")) {
        return handle_get_profiles(req);
    }
    if (strstr(body, "GetStreamUri")) {
        return handle_get_stream_uri(req);
    }

    /* Unrecognized action — log and return SOAP fault */
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
                    ESP_LOGW(TAG, "Unsupported media action: %s", action);
                }
            }
        }
    }
    return send_soap_fault(req);
}

/* ------------------------------------------------------------------ */
/*  HTTP Handlers                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief POST /onvif/device_service handler.
 */
static esp_err_t device_service_handler(httpd_req_t *req)
{
    char *body = onvif_read_body(req);
    if (body) {
        ESP_LOGI(TAG, "DEV REQ [first 300]: %.300s", body);
    }
    esp_err_t ret = dispatch_device_action(req, body);
    free(body);
    return ret;
}

/**
 * @brief POST /onvif/media_service handler.
 */
static esp_err_t media_service_handler(httpd_req_t *req)
{
    char *body = onvif_read_body(req);
    if (body) {
        ESP_LOGI(TAG, "MEDIA REQ [first 300]: %.300s", body);
    }
    esp_err_t ret = dispatch_media_action(req, body);
    free(body);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t onvif_register_handlers(httpd_handle_t server)
{
    if (!server) {
        ESP_LOGE(TAG, "Invalid server handle");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    /* Register device service handler */
    httpd_uri_t device_uri = {
        .uri      = "/onvif/device_service",
        .method   = HTTP_POST,
        .handler  = device_service_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &device_uri);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "Device service handler already registered");
        } else {
            ESP_LOGE(TAG, "Failed to register device service handler: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Registered /onvif/device_service");
    }

    /* Register media service handler */
    httpd_uri_t media_uri = {
        .uri      = "/onvif/media_service",
        .method   = HTTP_POST,
        .handler  = media_service_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &media_uri);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "Media service handler already registered");
        } else {
            ESP_LOGE(TAG, "Failed to register media service handler: %s",
                     esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Registered /onvif/media_service");
    }

    return ESP_OK;
}
