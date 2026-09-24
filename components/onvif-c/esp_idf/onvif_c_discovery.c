/*
 * onvif-c ESP-IDF port — WS-Discovery UDP responder + optional mDNS.
 *
 * Extracted from the MiBee Cam firmware (main/onvif_discovery.c):
 * UDP 3702, multicast 239.255.255.250; answers Probe with unicast
 * ProbeMatches and announces Hello every ~30s so NVRs can find the
 * device without probing. mDNS (_onvif._tcp) is optional via config.
 */

#include "onvif_c_port.h"
#include "../core/onvif_probe.h"
#include "esp_log.h"
#include "esp_err.h"
#if __has_include("mdns.h")
#include "mdns.h"
#define ONVIF_C_HAVE_MDNS 1
#else
#define ONVIF_C_HAVE_MDNS 0
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h" /* explicit: PSRAM recv buffer (PIT lesson) */
#if __has_include("esp_task_wdt.h")
#include "esp_task_wdt.h"
#define ONVIF_C_HAVE_WDT 1
#else
#define ONVIF_C_HAVE_WDT 0
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "onvif_c_disc";

#define ONVIF_DISCOVERY_PORT  3702
#define ONVIF_MULTICAST_GROUP "239.255.255.250"
#define PROBE_BUF_SIZE        4096
#define RESP_BUF_SIZE         2048
#define TASK_STACK_SIZE       6144
#define TASK_PRIORITY         2
#define TASK_CORE             1

static TaskHandle_t s_disc_task = NULL;

/* ------------------------------------------------------------------ */
/*  Discovery task                                                     */
/* ------------------------------------------------------------------ */

static void onvif_c_discovery_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WS-Discovery task started");

    const onvif_c_config_t *cfg         = onvif_c_cfg();
    const char             *device_uuid = cfg->uuid();
    ESP_LOGI(TAG, "Device UUID: %s", device_uuid);

#if ONVIF_C_HAVE_WDT
    bool wdt_watched = false;
    if (cfg->wdt_watch_discovery) {
        if (esp_task_wdt_add(NULL) == ESP_OK) {
            wdt_watched = true;
            ESP_LOGI(TAG, "Discovery task subscribed to the task watchdog");
        } else {
            ESP_LOGW(TAG, "Task watchdog subscribe failed (CONFIG_ESP_TASK_WDT?)");
        }
    }
#endif

    char *recv_buf = (char *)heap_caps_malloc(PROBE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!recv_buf) {
        recv_buf = malloc(PROBE_BUF_SIZE); /* PSRAM-less boards */
    }
    if (!recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        vTaskDelete(NULL);
        return;
    }

    int sock = -1;

    while (1) {
        if (s_disc_task == NULL) {
            break;
        }

#if ONVIF_C_HAVE_WDT
        if (wdt_watched) {
            esp_task_wdt_reset();
        }
#endif

        if (sock >= 0) {
            close(sock);
            sock = -1;
        }

        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            ESP_LOGW(TAG, "Failed to create socket, retrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family      = AF_INET;
        bind_addr.sin_port        = htons(ONVIF_DISCOVERY_PORT);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind port %d, retrying in 10s", ONVIF_DISCOVERY_PORT);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Join multicast group on the current interface IP. */
        const char *local_ip = onvif_c_cfg_ip();
        if (strcmp(local_ip, "0.0.0.0") == 0) {
            ESP_LOGW(TAG, "No IP yet, retrying in 10s");
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        struct ip_mreq imreq;
        memset(&imreq, 0, sizeof(imreq));
        imreq.imr_interface.s_addr = inet_addr(local_ip);
        imreq.imr_multiaddr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imreq, sizeof(imreq)) < 0) {
            ESP_LOGW(TAG, "Failed to join multicast on %s, retrying in 10s", local_ip);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        ESP_LOGI(TAG, "Joined multicast %s on %s", ONVIF_MULTICAST_GROUP, local_ip);

        struct timeval tv;
        tv.tv_sec  = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ESP_LOGI(TAG, "Listening for ONVIF Probe on UDP %s:%d", ONVIF_MULTICAST_GROUP,
                 ONVIF_DISCOVERY_PORT);

        /* Send initial multicast Hello. */
        {
            struct in_addr local_addr;
            local_addr.s_addr = inet_addr(local_ip);
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &local_addr, sizeof(local_addr));

            int ttl = 2;
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

            char hello_buf[RESP_BUF_SIZE];
            int  hl = onvif_probe_build_hello(hello_buf, sizeof(hello_buf), device_uuid, local_ip,
                                              cfg->http_port, onvif_c_cfg_scopes());
            if (hl <= 0 || (size_t)hl >= sizeof(hello_buf)) {
                /* Would-be length is not a byte count: sending it would
                 * read past this stack buffer. Skip, keep serving. */
                ESP_LOGW(TAG, "Hello truncated - check uuid/scopes length");
            } else {
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family      = AF_INET;
                dest.sin_port        = htons(ONVIF_DISCOVERY_PORT);
                dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
                sendto(sock, hello_buf, hl > 0 ? hl : 0, 0, (struct sockaddr *)&dest, sizeof(dest));
                ESP_LOGI(TAG, "Sent initial Hello to %s:%d", ONVIF_MULTICAST_GROUP,
                         ONVIF_DISCOVERY_PORT);
            }
        }

        int hello_counter = 0;

        while (1) {
            if (s_disc_task == NULL) {
                break;
            }

#if ONVIF_C_HAVE_WDT
            if (wdt_watched) {
                esp_task_wdt_reset();
            }
#endif

            struct sockaddr_in sender_addr;
            socklen_t          addr_len = sizeof(sender_addr);
            memset(recv_buf, 0, PROBE_BUF_SIZE);

            int recv_len = recvfrom(sock, recv_buf, PROBE_BUF_SIZE - 1, 0,
                                    (struct sockaddr *)&sender_addr, &addr_len);
            if (recv_len <= 0) {
                /* Timeout — periodically resend Hello (~30s). */
                hello_counter++;
                if (hello_counter >= 6) {
                    hello_counter      = 0;
                    const char *ip_str = onvif_c_cfg_ip();
                    if (strcmp(ip_str, "0.0.0.0") != 0) {
                        char hello_buf[RESP_BUF_SIZE];
                        int  hl =
                            onvif_probe_build_hello(hello_buf, sizeof(hello_buf), device_uuid,
                                                    ip_str, cfg->http_port, onvif_c_cfg_scopes());
                        struct sockaddr_in dest;
                        memset(&dest, 0, sizeof(dest));
                        dest.sin_family      = AF_INET;
                        dest.sin_port        = htons(ONVIF_DISCOVERY_PORT);
                        dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
                        struct in_addr local_addr;
                        local_addr.s_addr = inet_addr(ip_str);
                        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &local_addr,
                                   sizeof(local_addr));
                        if (hl > 0 && (size_t)hl < sizeof(hello_buf)) {
                            sendto(sock, hello_buf, (size_t)hl, 0, (struct sockaddr *)&dest,
                                   sizeof(dest));
                            ESP_LOGI(TAG, "Resent Hello (periodic)");
                        }
                    }
                }
                continue;
            }

            recv_buf[recv_len] = '\0';

            if (!onvif_probe_is_probe(recv_buf)) {
                continue;
            }

            const char *ip_str = onvif_c_cfg_ip();
            if (strcmp(ip_str, "0.0.0.0") == 0) {
                ESP_LOGW(TAG, "WiFi not connected, skipping Probe response");
                continue;
            }

            char relates_to[128] = {0};
            if (!onvif_probe_message_id(recv_buf, relates_to, sizeof(relates_to))) {
                strncpy(relates_to, "urn:uuid:unknown", sizeof(relates_to) - 1);
            }

            ESP_LOGI(TAG, "Received Probe from %s, sending ProbeMatches",
                     inet_ntoa(sender_addr.sin_addr));

            char resp_buf[RESP_BUF_SIZE];
            int  rl = onvif_probe_build_matches(resp_buf, sizeof(resp_buf), relates_to, device_uuid,
                                                ip_str, cfg->http_port, onvif_c_cfg_scopes());

            if (rl <= 0 || (size_t)rl >= sizeof(resp_buf)) {
                ESP_LOGW(TAG, "ProbeMatches truncated - check uuid/scopes "
                              "length, dropping probe");
                continue;
            }

            struct in_addr if_addr;
            if_addr.s_addr = inet_addr(ip_str);
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &if_addr, sizeof(if_addr));
            int sent = sendto(sock, resp_buf, (size_t)rl, 0, (struct sockaddr *)&sender_addr,
                              sizeof(sender_addr));
            ESP_LOGI(TAG, "ProbeMatches sent to %s:%d, result=%d", inet_ntoa(sender_addr.sin_addr),
                     ntohs(sender_addr.sin_port), sent);
        }
    }

    free(recv_buf);
    if (sock >= 0)
        close(sock);
#if ONVIF_C_HAVE_WDT
    if (wdt_watched) {
        esp_task_wdt_delete(NULL);
    }
#endif
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  mDNS (optional)                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t init_mdns(void)
{
    const onvif_c_config_t *cfg = onvif_c_cfg();
    if (!cfg->mdns_hostname) {
        return ESP_OK;
    }
#if ONVIF_C_HAVE_MDNS
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mdns_hostname_set(cfg->mdns_hostname);
    mdns_instance_name_set(cfg->mdns_instance ? cfg->mdns_instance : cfg->model);

    mdns_service_add(NULL, "_onvif", "_tcp", cfg->http_port, NULL, 0);
    mdns_service_txt_item_set("_onvif", "_tcp", "txtvers", "1");

    ESP_LOGI(TAG, "mDNS initialized: %s.local (_onvif._tcp port %u)", cfg->mdns_hostname,
             (unsigned)cfg->http_port);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "mdns requested (%s) but espressif/mdns not in build", cfg->mdns_hostname);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* ------------------------------------------------------------------ */
/*  Lifecycle (internal; called by onvif_c_start/stop)                 */
/* ------------------------------------------------------------------ */

esp_err_t onvif_c_discovery_start(void)
{
    /* mDNS first — non-fatal on failure. */
    esp_err_t ret = init_mdns();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable, WS-Discovery continues");
    }

    if (s_disc_task != NULL) {
        ESP_LOGW(TAG, "Discovery task already running");
        return ESP_OK;
    }

    BaseType_t created =
        xTaskCreatePinnedToCore(onvif_c_discovery_task, "onvif_disc", TASK_STACK_SIZE, NULL,
                                TASK_PRIORITY, &s_disc_task, TASK_CORE);

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WS-Discovery task");
        s_disc_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WS-Discovery task created");
    return ESP_OK;
}

esp_err_t onvif_c_discovery_stop(void)
{
    TaskHandle_t task = s_disc_task;
    s_disc_task       = NULL;
    if (task) {
        vTaskDelete(task);
    }

#if ONVIF_C_HAVE_MDNS
    mdns_service_remove("_onvif", "_tcp");
    mdns_free();
#endif

    ESP_LOGI(TAG, "WS-Discovery stopped");
    return ESP_OK;
}
