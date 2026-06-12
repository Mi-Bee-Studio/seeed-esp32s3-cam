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
 * @file onvif_discovery.c
 * @brief WS-Discovery UDP multicast listener for ONVIF NVR discovery.
 *
 * Listens on UDP port 3702, multicast group 239.255.255.250.
 * When a WS-Discovery Probe message arrives, responds with a ProbeMatches
 * message containing the device service URL.
 *
 * References:
 *   - ONVIF Network Interface Specification
 *   - WS-Discovery 1.1 (http://schemas.xmlsoap.org/ws/2005/04/discovery)
 */

#include "onvif_discovery.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "wifi_manager.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "onvif_disc";

#define ONVIF_DISCOVERY_PORT 3702
#define ONVIF_MULTICAST_GROUP "239.255.255.250"
#define PROBE_BUF_SIZE 4096
#define RESP_BUF_SIZE 2048
#define TASK_STACK_SIZE 6144
#define TASK_PRIORITY 2
#define TASK_CORE 1

/* Fixed UUID prefix for MAC-based device UUID generation.
 * Format: f472b01e-0000-1000-8000-{MAC6bytes} */
#define UUID_PREFIX "f472b01e-0000-1000-8000-"

/**
 * @brief Generate a device UUID from the STA MAC address.
 * Output format: f472b01e-0000-1000-8000-XXXXXXXXXXXX
 * where XXXXXXXXXXXX is the 6-byte MAC in hex.
 */
static void generate_device_uuid(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size,
             UUID_PREFIX "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/**
 * @brief Extract the MessageID value from a WS-Discovery Probe message.
 * Looks for a "MessageID" tag and extracts the content between > and <.
 * @param body The received XML buffer
 * @param body_len Length of the buffer
 * @param out Output buffer for the MessageID value
 * @param out_size Size of output buffer
 * @return true if MessageID was found and extracted
 */
static bool extract_message_id(const char *body, size_t body_len,
                                char *out, size_t out_size)
{
    /* Search for "MessageID" anywhere in the message */
    const char *tag = strstr(body, "MessageID");
    if (!tag) {
        return false;
    }

    /* Find the closing > of the opening tag */
    const char *gt = strchr(tag, '>');
    if (!gt || (size_t)(gt - body) >= body_len) {
        return false;
    }
    gt++; /* skip past > */

    /* Find the opening < of the closing tag */
    const char *lt = strchr(gt, '<');
    if (!lt || (size_t)(lt - body) > body_len) {
        return false;
    }

    size_t len = lt - gt;
    if (len == 0 || len >= out_size) {
        return false;
    }

    memcpy(out, gt, len);
    out[len] = '\0';
    return true;
}

/**
 * @brief Check if the received message is a WS-Discovery Probe.
 */
static bool is_probe_message(const char *body, size_t body_len)
{
    (void)body_len;
    /* Check for "wsdiscovery:Probe" or "ws-discovery:Probe" or just "Probe" */
    if (strstr(body, "wsdiscovery:Probe") ||
        strstr(body, "ws-discovery:Probe") ||
        strstr(body, ":Probe")) {
        /* Also verify it's not a ProbeMatches response */
        if (!strstr(body, "ProbeMatches")) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Build a ProbeMatches response XML.
 * @param relates_to The MessageID from the incoming Probe (to populate RelatesTo)
 * @param device_uuid The device's own UUID
 * @param ip_str Current IP address string
 * @param out Output buffer for the response
 * @param out_size Size of output buffer
 */
static void build_probe_matches(const char *relates_to,
                                 const char *device_uuid,
                                 const char *ip_str,
                                 char *out, size_t out_size)
{
    snprintf(out, out_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope "
        "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:tns=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<soap:Header>"
        "<wsa:Action>"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches"
        "</wsa:Action>"
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
        relates_to ? relates_to : "",
        device_uuid,
        ip_str);
}

/**
 * @brief Build a WS-Discovery Hello message for multicast announcement.
 * Sent once on startup so NVRs can discover the device without waiting for Probe.
 */
static void build_hello_message(const char *device_uuid,
                                const char *ip_str,
                                char *out, size_t out_size)
{
    snprintf(out, out_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soap:Envelope "
        "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:tns=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<soap:Header>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
        "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
        "</soap:Header>"
        "<soap:Body>"
        "<wsd:Hello>"
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
        "</wsd:Hello>"
        "</soap:Body>"
        "</soap:Envelope>",
        device_uuid,
        device_uuid,
        ip_str);
}

/**
 * @brief FreeRTOS task that listens for WS-Discovery Probe messages
 * and responds with ProbeMatches.
 */
static void onvif_discovery_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ONVIF discovery task started");

    /* Device UUID only needs to be generated once */
    char device_uuid[64];
    generate_device_uuid(device_uuid, sizeof(device_uuid));
    ESP_LOGI(TAG, "Device UUID: %s", device_uuid);

    /* Allocate PSRAM buffer for receiving Probe messages */
    char *recv_buf = (char *)heap_caps_malloc(PROBE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM receive buffer");
        vTaskDelete(NULL);
        return;
    }

    int sock = -1;

    while (1) {
        /* Close previous socket if reconnecting */
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }

        /* Create UDP socket */
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            ESP_LOGW(TAG, "Failed to create socket, retrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Allow address reuse */
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        /* Bind to port 3702 on all interfaces */
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(ONVIF_DISCOVERY_PORT);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind port %d, retrying in 10s",
                     ONVIF_DISCOVERY_PORT);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* Join multicast group — use actual STA IP (INADDR_ANY fails on ESP32/LWIP) */
        char *local_ip = wifi_get_ip_str();
        if (!local_ip || strcmp(local_ip, "0.0.0.0") == 0) {
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
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &imreq, sizeof(imreq)) < 0) {
            ESP_LOGW(TAG, "Failed to join multicast group on %s, retrying in 10s", local_ip);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        ESP_LOGI(TAG, "Joined multicast %s on interface %s", ONVIF_MULTICAST_GROUP, local_ip);

        /* Set receive timeout so we can periodically check WiFi state */
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ESP_LOGI(TAG, "Listening for ONVIF Probe on UDP %s:%d",
                 ONVIF_MULTICAST_GROUP, ONVIF_DISCOVERY_PORT);

        /* Send multicast Hello to announce ourselves to NVRs */
        {
            char *ip_str = wifi_get_ip_str();
            if (ip_str && strcmp(ip_str, "0.0.0.0") != 0) {
                /* Set outgoing multicast interface */
                struct in_addr local_addr;
                local_addr.s_addr = inet_addr(ip_str);
                setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                           &local_addr, sizeof(local_addr));

                /* Set multicast TTL to 2 (reach local subnet + one hop) */
                int ttl = 2;
                setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

                char hello_buf[RESP_BUF_SIZE];
                build_hello_message(device_uuid, ip_str,
                                    hello_buf, sizeof(hello_buf));
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                dest.sin_port = htons(ONVIF_DISCOVERY_PORT);
                dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
                sendto(sock, hello_buf, strlen(hello_buf), 0,
                       (struct sockaddr *)&dest, sizeof(dest));
                ESP_LOGI(TAG, "Sent WS-Discovery Hello to %s:%d",
                         ONVIF_MULTICAST_GROUP, ONVIF_DISCOVERY_PORT);
            }
        }
        int hello_counter = 0;
        /* Main receive loop */
        while (1) {
            struct sockaddr_in sender_addr;
            socklen_t addr_len = sizeof(sender_addr);
            memset(recv_buf, 0, PROBE_BUF_SIZE);

            int recv_len = recvfrom(sock, recv_buf, PROBE_BUF_SIZE - 1, 0,
                                    (struct sockaddr *)&sender_addr, &addr_len);
            if (recv_len <= 0) {
                /* Timeout (~5s) — periodically resend Hello for NVR discovery */
                hello_counter++;
                if (hello_counter >= 6) {
                    /* Every ~30s resend multicast Hello */
                    hello_counter = 0;
                    char *ip_str = wifi_get_ip_str();
                    if (ip_str && strcmp(ip_str, "0.0.0.0") != 0) {
                        char hello_buf[RESP_BUF_SIZE];
                        build_hello_message(device_uuid, ip_str,
                                            hello_buf, sizeof(hello_buf));
                        struct sockaddr_in dest;
                        memset(&dest, 0, sizeof(dest));
                        dest.sin_family = AF_INET;
                        dest.sin_port = htons(ONVIF_DISCOVERY_PORT);
                        dest.sin_addr.s_addr = inet_addr(ONVIF_MULTICAST_GROUP);
                        struct in_addr local_addr;
                        local_addr.s_addr = inet_addr(ip_str);
                        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                                   &local_addr, sizeof(local_addr));
                        sendto(sock, hello_buf, strlen(hello_buf), 0,
                               (struct sockaddr *)&dest, sizeof(dest));
                        ESP_LOGI(TAG, "Resent Hello (periodic)");
                    }
                }
                continue;
            }

            recv_buf[recv_len] = '\0';

            /* Check if this is a Probe message */
            if (!is_probe_message(recv_buf, recv_len)) {
                continue;
            }

            /* Get current IP — skip if WiFi not connected */
            char *ip_str = wifi_get_ip_str();
            if (!ip_str || strcmp(ip_str, "0.0.0.0") == 0) {
                ESP_LOGW(TAG, "WiFi not connected, skipping Probe response");
                continue;
            }

            /* Extract MessageID from the Probe */
            char relates_to[128] = {0};
            if (!extract_message_id(recv_buf, recv_len,
                                     relates_to, sizeof(relates_to))) {
                /* If we can't extract a MessageID, use a fallback */
                strncpy(relates_to, "urn:uuid:unknown", sizeof(relates_to) - 1);
            }

            ESP_LOGI(TAG, "Received Probe from %s, sending ProbeMatches",
                     inet_ntoa(sender_addr.sin_addr));

            /* Build and send ProbeMatches response */
            char resp_buf[RESP_BUF_SIZE];
            build_probe_matches(relates_to, device_uuid, ip_str,
                                resp_buf, sizeof(resp_buf));

            /* Send unicast response using the same socket */
            /* Reset IP_MULTICAST_IF to ensure unicast routing works */
            struct in_addr if_addr;
            if_addr.s_addr = inet_addr(ip_str);
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF,
                       &if_addr, sizeof(if_addr));
            int sent = sendto(sock, resp_buf, strlen(resp_buf), 0,
                               (struct sockaddr *)&sender_addr, sizeof(sender_addr));
            ESP_LOGI(TAG, "ProbeMatches sent to %s:%d, result=%d, errno=%d",
                     inet_ntoa(sender_addr.sin_addr),
                     ntohs(sender_addr.sin_port),
                     sent, errno);
        }
    }

    /* Should never reach here */
    if (recv_buf) free(recv_buf);
    if (sock >= 0) close(sock);
    vTaskDelete(NULL);
}

esp_err_t onvif_discovery_init(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        onvif_discovery_task,
        "onvif_disc",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        NULL,
        TASK_CORE
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ONVIF discovery task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ONVIF discovery task created");
    return ESP_OK;
}
