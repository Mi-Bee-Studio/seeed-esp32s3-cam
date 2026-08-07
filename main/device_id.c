/**
 * @file device_id.c
 * @brief Stable device identity derived from the factory eFuse MAC.
 *
 * Reads the factory MAC from eFuse exactly once and derives the serial,
 * UUID, and MAC bytes from it. See device_id.h for rationale.
 */
#include "device_id.h"
#include <string.h>
#include <stdio.h>
#include "esp_mac.h"
#include "esp_log.h"

/* Must match UUID_PREFIX in onvif_discovery.c / onvif_service.c callers. */
#define DEVICE_UUID_PREFIX "f472b01e-0000-1000-8000-"

static const char *TAG = "device_id";

static char     s_serial[13] = {0};   /* 12 hex + NUL */
static char     s_uuid[37]   = {0};   /* prefix (24) + 12 hex + NUL */
static uint8_t  s_mac[6]     = {0};
static bool     s_initialised = false;

/* Thread-safe lazy init: esp_efuse_mac_get_default() is safe to call from any
 * task and is itself idempotent. The flag write is a benign race (worst case:
 * two callers compute the same value), so no mutex is needed. */
static void ensure_initialised(void)
{
    if (s_initialised) {
        return;
    }

    /* esp_efuse_mac_get_default() reads the base MAC burned at the factory.
     * It does not depend on WiFi init state and is stable across reflashes. */
    esp_err_t ret = esp_efuse_mac_get_default(s_mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_efuse_mac_get_default failed: %s — using zero MAC",
                 esp_err_to_name(ret));
        memset(s_mac, 0, sizeof(s_mac));
    }

    snprintf(s_serial, sizeof(s_serial), "%02x%02x%02x%02x%02x%02x",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    snprintf(s_uuid, sizeof(s_uuid),
             DEVICE_UUID_PREFIX "%02x%02x%02x%02x%02x%02x",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);

    s_initialised = true;
    ESP_LOGI(TAG, "Device serial: %s", s_serial);
}

const char *device_get_serial(void)
{
    ensure_initialised();
    return s_serial;
}

const char *device_get_uuid(void)
{
    ensure_initialised();
    return s_uuid;
}

void device_get_mac(uint8_t out[6])
{
    ensure_initialised();
    memcpy(out, s_mac, 6);
}
