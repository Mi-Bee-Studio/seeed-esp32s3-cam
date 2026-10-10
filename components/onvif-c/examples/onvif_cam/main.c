/*
 * onvif-c minimal example — static identity, no camera.
 * Build: put onvif-c on EXTRA_COMPONENT_DIRS, add onvif-c to REQUIRES.
 */
#include "onvif_c.h"
#include "esp_http_server.h"
#include "esp_wifi.h"

static httpd_handle_t s_httpd;

static const char *my_ip(void)
{
    static char         ip[16] = "0.0.0.0";
    esp_netif_ip_info_t info;
    esp_netif_t        *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    return ip;
}

static const char *my_stream_uri(void)
{
    static char uri[64];
    snprintf(uri, sizeof(uri), "http://%s:81/stream", my_ip());
    return uri;
}

static bool my_events_gate(void)
{
    return true;
}

void app_main(void)
{
    /* ... start WiFi + esp_http_server first (s_httpd = ...) ... */
    const onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "ESP32-S3",
        .firmware_version = "v0.1.0",
        .serial           = "aabbccddeeff",
        .uuid             = "f472b01e-0000-1000-8000-aabbccddeeff",
        .ip               = my_ip,
        .stream_uri       = my_stream_uri,
        .events_enabled   = my_events_gate,
        .mdns_hostname    = "mibeecam-demo",
    };
    onvif_c_start(s_httpd, &cfg);
}
