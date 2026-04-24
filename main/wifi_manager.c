/*
 * Copyright (C) 2024 ParrotCam Authors
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

#include "wifi_manager.h"
#include "config_manager.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"

static const char *TAG = "wifi";

/* ---- state ---- */
static wifi_state_t s_state = WIFI_STATE_AP;
static esp_netif_t *s_netif_ap  = NULL;
static esp_netif_t *s_netif_sta = NULL;
static EventGroupHandle_t s_event_group = NULL;
static int s_retry_count = 0;
static char s_ip_str[16] = "0.0.0.0";

/* ---- event group bits ---- */
#define CONNECTED_BIT   BIT0
#define SCAN_DONE_BIT   BIT1

/* ---- reconnect timer (one-shot, 60 s) ---- */
static TimerHandle_t s_reconnect_timer = NULL;

/** @brief WiFi 重连定时器回调函数，60 秒后触发重连 */
static void reconnect_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Reconnect timer fired, retry %d", ++s_retry_count);
    esp_wifi_connect();
}

/* ---- MAC helper for AP SSID ---- */
/** @brief 根据设备 MAC 地址生成 AP 热点 SSID（格式：ParrotCam-XXXX） */
static void get_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    uint16_t suffix = ((uint16_t)mac[4] << 8) | mac[5];
    snprintf(buf, len, "ParrotCam-%04X", suffix);
}

/* ---- event handler ---- */
/** @brief WiFi 事件处理函数，处理连接/断开/扫描/IP 获取等事件 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_state = WIFI_STATE_STA_DISCONNECTED;
            xEventGroupClearBits(s_event_group, CONNECTED_BIT);
            ESP_LOGW(TAG, "WiFi disconnected, retrying in 60 s");
            if (s_reconnect_timer) {
                xTimerReset(s_reconnect_timer, portMAX_DELAY);
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE: {
            xEventGroupSetBits(s_event_group, SCAN_DONE_BIT);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        switch (id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
            s_state = WIFI_STATE_STA_CONNECTED;
            s_retry_count = 0;
            xEventGroupSetBits(s_event_group, CONNECTED_BIT);
            ESP_LOGI(TAG, "WiFi connected, IP=%s", s_ip_str);
            break;
        }
        case IP_EVENT_ASSIGNED_IP_TO_CLIENT: {
            ip_event_ap_staipassigned_t *evt = (ip_event_ap_staipassigned_t *)event_data;
            ESP_LOGI(TAG, "AP client connected, IP=" IPSTR, IP2STR(&evt->ip));
            break;
        }
        default:
            break;
        }
    }
}

/* ---- public API ---- */

/** @brief WiFi 模块初始化入口，根据配置自动选择 AP 或 STA 模式 */
esp_err_t wifi_init(void)
{
    /* Init netif (idempotent-safe after config_init which already did NVS) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Reconnect timer: one-shot, 60 s */
    s_reconnect_timer = xTimerCreate("wifi_recon",
                                     pdMS_TO_TICKS(60000),
                                     pdFALSE,       // one-shot
                                     NULL,
                                     reconnect_timer_cb);
    if (!s_reconnect_timer) {
        ESP_LOGE(TAG, "Failed to create reconnect timer");
        return ESP_ERR_NO_MEM;
    }

    /* Decide mode from config */
    cam_config_t *cfg = config_get();
    if (strlen(cfg->wifi_ssid) == 0) {
        return wifi_start_ap();
    } else {
        return wifi_start_sta();
    }
}

/** @brief 获取当前 WiFi 连接状态 */
wifi_state_t wifi_get_state(void)
{
    return s_state;
}

/** @brief 获取当前 IP 地址字符串（静态缓冲区） */
char *wifi_get_ip_str(void)
{
    return s_ip_str;
}

/** @brief 判断当前是否处于 STA 模式（正在连接或已连接） */
bool wifi_is_sta(void)
{
    return s_state != WIFI_STATE_AP;
}

/** @brief 启动 WiFi AP 热点模式，配置静态 IP 192.168.4.1 */
esp_err_t wifi_start_ap(void)
{
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register AP-related events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                        wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {0};
    get_ap_ssid((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, "12345678", sizeof(ap_config.ap.password));
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Set static IP info for AP netif */
    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_netif_ap);
    esp_netif_set_ip_info(s_netif_ap, &ip_info);
    esp_netif_dhcps_start(s_netif_ap);

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    s_state = WIFI_STATE_AP;

    ESP_LOGI(TAG, "AP mode started, SSID=%s, IP=192.168.4.1", ap_config.ap.ssid);
    return ESP_OK;
}

/** @brief 启动 WiFi STA 客户端模式，连接配置的路由器 */
esp_err_t wifi_start_sta(void)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register STA-related events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                        WIFI_EVENT_STA_DISCONNECTED,
                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_STA_GOT_IP,
                        wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    cam_config_t *config = config_get();
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, config->wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, config->wifi_pass, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = WIFI_STATE_STA_CONNECTING;
    ESP_LOGI(TAG, "STA mode connecting to SSID=%s", config->wifi_ssid);

    ESP_ERROR_CHECK(esp_wifi_connect());
    return ESP_OK;
}

/** @brief 扫描周围 WiFi 热点，返回找到的数量，失败返回 -1 */
int wifi_scan(wifi_ap_info_t *aps, int max_count)
{
    if (!aps || max_count <= 0) {
        return -1;
    }

    /* Clear previous scan result bits */
    xEventGroupClearBits(s_event_group, SCAN_DONE_BIT);

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        return 0;
    }

    /* Use static array — no dynamic allocation */
    static wifi_ap_record_t records[32];
    int to_fetch = (ap_count > (uint16_t)max_count) ? max_count : (int)ap_count;
    if (to_fetch > 32) {
        to_fetch = 32;
    }
    uint16_t fetched = (uint16_t)to_fetch;
    esp_wifi_scan_get_ap_records(&fetched, records);

    for (int i = 0; i < (int)fetched; i++) {
        strlcpy(aps[i].ssid, (const char *)records[i].ssid, sizeof(aps[i].ssid));
        aps[i].rssi = records[i].rssi;
        aps[i].auth_mode = (uint8_t)records[i].authmode;
    }

    ESP_LOGI(TAG, "WiFi scan found %d APs", (int)fetched);
    return (int)fetched;
}
