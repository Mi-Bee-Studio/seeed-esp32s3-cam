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

#include "wifi_manager.h"
#include "config_manager.h"
#include "status_led.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "lwip/ip_addr.h"
#include "mdns.h"
#include "logging.h"
#include "ws_server.h"

static const char *TAG = "wifi";

/* ---- state ---- */
static wifi_state_t s_state = WIFI_STATE_AP;
static esp_netif_t *s_netif_ap  = NULL;
static esp_netif_t *s_netif_sta = NULL;
static EventGroupHandle_t s_event_group = NULL;
static int s_retry_count = 0;
static bool s_using_backup_wifi = false;  // true when using wifi_ssid_2
static int s_network_failures = 0;  // count failures on current network
static char s_current_ssid[33] = "";  // track current connected SSID
static char s_ip_str[16] = "0.0.0.0";
static bool s_mdns_initialized = false;
static char s_mdns_hostname[32] = "";
static bool s_stopping_sta = false;

/* ---- event group bits ---- */
#define CONNECTED_BIT   BIT0
#define SCAN_DONE_BIT   BIT1

/* ---- reconnect timer with exponential backoff ---- */
#define WIFI_RETRY_MIN_MS   3000
#define WIFI_RETRY_MAX_MS   60000
static TimerHandle_t s_reconnect_timer = NULL;
static uint32_t s_retry_interval_ms = WIFI_RETRY_MIN_MS;

/* Forward declarations */
static void get_ap_ssid(char *buf, size_t len);
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data);
static void start_mdns_service(void);

/** @brief 填充 STA 配置，认证方式自动适配路由器（OPEN ~ WPA3 均可） */
static void fill_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy((char *)cfg->sta.ssid, ssid, sizeof(cfg->sta.ssid));
    strlcpy((char *)cfg->sta.password, pass, sizeof(cfg->sta.password));
    cfg->sta.threshold.authmode = WIFI_AUTH_OPEN;  // auto-negotiate: WPA2/WPA3/mixed/WPA/WEP/OPEN
    cfg->sta.pmf_cfg.capable = true;               // advertise PMF capability
    cfg->sta.pmf_cfg.required = false;              // don't require PMF (compat with older APs)
    cfg->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;      // WPA3: try both H2E and hunting-and-pecking
    cfg->sta.listen_interval = 3;                  // lower = better multicast reception (default 10)
}

/** @brief WiFi 重连定时器回调函数，实现主备 WiFi 自动切换 */
static void reconnect_timer_cb(TimerHandle_t timer)
{
    s_retry_count++;
    s_network_failures++;
    
    ESP_LOGI(TAG, "Retry %d (network failures: %d, interval %lu ms)",
             s_retry_count, s_network_failures, (unsigned long)s_retry_interval_ms);
    
    /* Use read-only pointer instead of copying the full config struct */
    cam_config_t *cfg = config_get();
    
    /* Failover logic: 3 failures on current network → try backup */
    if (s_network_failures >= 3) {
        ESP_LOGW(TAG, "WiFi network failed 3 times");
        s_network_failures = 0;
        
        if (!s_using_backup_wifi && strlen(cfg->wifi_ssid_2) > 0) {
            /* Switch to backup WiFi */
            s_using_backup_wifi = true;
            ESP_LOGI(TAG, "Switching to backup WiFi: %s", cfg->wifi_ssid_2);
            wifi_config_t sta_config;
            fill_sta_config(&sta_config, cfg->wifi_ssid_2, cfg->wifi_pass_2);
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        } else if (s_using_backup_wifi) {
            /* Backup WiFi also failed */
            if (cfg->allow_ap_fallback) {
                /* Allow AP fallback — stop timer, signal AP switch */
                ESP_LOGW(TAG, "Both WiFi networks failed, allowing AP fallback");
                if (s_reconnect_timer) {
                    xTimerStop(s_reconnect_timer, 0);
                }
                s_stopping_sta = true;
                esp_wifi_stop();
                wifi_start_ap();
                return;
            } else {
                /* Never AP fallback - retry from primary */
                ESP_LOGW(TAG, "Both WiFi networks failed, AP fallback disabled, retrying primary");
                s_using_backup_wifi = false;
                wifi_config_t sta_config;
                fill_sta_config(&sta_config, cfg->wifi_ssid, cfg->wifi_pass);
                esp_wifi_set_config(WIFI_IF_STA, &sta_config);
            }
        }
    }
    
    /* Exponential backoff: double interval each retry, cap at 60000 ms */
    s_retry_interval_ms *= 2;
    if (s_retry_interval_ms > WIFI_RETRY_MAX_MS) {
        s_retry_interval_ms = WIFI_RETRY_MAX_MS;
    }
    xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_retry_interval_ms), portMAX_DELAY);
    
    esp_wifi_connect();
}

/* ---- MAC helper for AP SSID ---- */
/** @brief 根据设备 MAC 地址生成 AP 热点 SSID（格式：MiBee Cam-XXXX） */
static void get_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    uint16_t suffix = ((uint16_t)mac[4] << 8) | mac[5];
    snprintf(buf, len, "MiBee Cam-%04X", suffix);
}

/* ---- mDNS helper ---- */
/** @brief Start mDNS service with hostname mibee_cam-XXXX,
 *         advertising _http._tcp (port 80) and _rtsp._tcp (port 554).
 *         Idempotent - only initializes once. */
static void start_mdns_service(void)
{
    if (s_mdns_initialized) {
        return;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint16_t suffix = ((uint16_t)mac[4] << 8) | mac[5];

    char hostname[32];
    snprintf(hostname, sizeof(hostname), "mibee_cam-%04X", suffix);

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    mdns_hostname_set(hostname);
    strlcpy(s_mdns_hostname, hostname, sizeof(s_mdns_hostname));
    ESP_LOGI(TAG, "mDNS hostname: %s.local", hostname);

    /* Register _http._tcp on port 80 */
    mdns_txt_item_t http_txt[] = { { (char *)"path", (char *)"/" } };
    err = mdns_service_add("MiBee Cam Web Server", "_http", "_tcp", 80,
                             http_txt, sizeof(http_txt) / sizeof(http_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _http service add failed: %s", esp_err_to_name(err));
    }


    s_mdns_initialized = true;
    ESP_LOGI(TAG, "mDNS services started (http:80)");
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
            ws_broadcast("wifi_state_changed", "{\"state\":\"disconnected\"}");
            if (s_stopping_sta) {
                s_stopping_sta = false;
                return;
            }
            ESP_LOGW(TAG, "WiFi disconnected, retrying in 5 s");
            LOG_EVENT(LOG_EVENT_WIFI_DISCONNECTED, "retry_count=%d", s_retry_count);
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
            s_network_failures = 0;
            s_retry_interval_ms = WIFI_RETRY_MIN_MS;
            xEventGroupSetBits(s_event_group, CONNECTED_BIT);
            ws_broadcast("wifi_state_changed", "{\"state\":\"connected\"}");
            
            /* Track current connected SSID */
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                strlcpy(s_current_ssid, (char *)ap_info.ssid, sizeof(s_current_ssid));
                ESP_LOGI(TAG, "WiFi connected to %s, IP=%s", s_current_ssid, s_ip_str);
                LOG_EVENT(LOG_EVENT_WIFI_CONNECTED, "ssid=%s ip=%s", s_current_ssid, s_ip_str);
            } else {
                ESP_LOGI(TAG, "WiFi connected, IP=%s", s_ip_str);
            }
            
            start_mdns_service();
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
    esp_netif_init();
    /* Tolerate already-created event loop (ESP_ERR_INVALID_STATE = already exists) */
    esp_err_t ev_err = esp_event_loop_create_default();
    if (ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ev_err));
        return ev_err;
    }

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    /* Reconnect timer: one-shot, 10 s */
    s_reconnect_timer = xTimerCreate("wifi_recon",
                                     pdMS_TO_TICKS(3000),
                                     pdFALSE,       // one-shot
                                     NULL,
                                     reconnect_timer_cb);
    if (!s_reconnect_timer) {
        ESP_LOGE(TAG, "Failed to create reconnect timer");
        return ESP_ERR_NO_MEM;
    }

    /* Decide mode from config */
    cam_config_t *cfg = config_get();
    /* If both WiFi configs are empty, start AP mode */
    if (strlen(cfg->wifi_ssid) == 0) {
        return wifi_start_ap();
    } else {
        /* At least primary WiFi is configured, start STA mode */
        s_using_backup_wifi = false;  /* Start with primary WiFi */
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

/** @brief 获取 STA 模式的 netif 句柄（用于获取 WiFi 网关信息） */
esp_netif_t *wifi_get_sta_netif(void)
{
    return s_netif_sta;
}

esp_err_t wifi_start_ap(void)
{
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.espnow_max_encrypt_num = 0;  // Free all connection slots for AP clients
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register AP-related events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                        wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {0};
    get_ap_ssid((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, "mibeecam2026", sizeof(ap_config.ap.password));
    ap_config.ap.channel = 6;  // Avoid channel 1 on ESP32-S3 (known issue)
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);  // HT20 for better AP visibility

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
    ws_broadcast("wifi_state_changed", "{\"state\":\"ap\"}");

    ESP_LOGI(TAG, "AP mode started, SSID=%s, IP=192.168.4.1", ap_config.ap.ssid);
    LOG_EVENT(LOG_EVENT_WIFI_AP_STARTED, "ssid=%s", ap_config.ap.ssid);
    start_mdns_service();
    return ESP_OK;
}

/** @brief 启动 WiFi STA 客户端模式，连接配置的路由器 */
esp_err_t wifi_start_sta(void)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    /* Set DHCP hostname (displayed on router client list) to match mDNS format */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "mibee_cam-%04X", ((uint16_t)mac[4] << 8) | mac[5]);
    esp_netif_set_hostname(s_netif_sta, hostname);

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
    wifi_config_t sta_config;
    fill_sta_config(&sta_config, config->wifi_ssid, config->wifi_pass);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable WiFi power save for reliable DHCP and streaming */
    esp_wifi_set_ps(WIFI_PS_NONE);


    /* Boost TX power to 20 dBm (ESP32-S3 maximum, for weak PCB antenna without external antenna) */
    int8_t power_param = (int8_t)(20 / 0.25);  // 20 dBm → 80 quarter-dBm units
    esp_err_t pwr_err = esp_wifi_set_max_tx_power(power_param);
    if (pwr_err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi TX power set to 20 dBm");
    } else {
        ESP_LOGW(TAG, "Failed to set WiFi TX power: %s", esp_err_to_name(pwr_err));
    }

    s_state = WIFI_STATE_STA_CONNECTING;
    strlcpy(s_current_ssid, config->wifi_ssid, sizeof(s_current_ssid));
    ws_broadcast("wifi_state_changed", "{\"state\":\"connecting\"}");
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

/** @brief 获取当前连接的 SSID（STA 模式），空字符串表示未连接或 AP 模式 */
const char *wifi_get_current_ssid(void)
{
    if (s_state != WIFI_STATE_STA_CONNECTED) {
        return "";
    }
    return s_current_ssid;
}

/** @brief 当前使用的配置槽位：true=备用网（wifi_ssid_2），false=主网 */
bool wifi_using_backup(void)
{
    return s_using_backup_wifi;
}

/** @brief 获取 mDNS 主机名（mibee_cam-XXXX 格式） */
const char *wifi_get_mdns_hostname(void)
{
    return s_mdns_hostname;
}
