/**
 * @file device_id.h
 * @brief Stable device identity derived from the factory eFuse MAC.
 *
 * All device identifiers (ONVIF SerialNumber, WS-Discovery UUID, metrics MAC)
 * MUST go through this module. It reads the MAC once from eFuse at first use
 * and caches it, so the identity is:
 *   - independent of WiFi state (safe to call before WiFi init / in AP mode),
 *   - stable across reboots and reflashes (factory eFuse never changes),
 *   - consistent across every consumer (ONVIF discovery == ONVIF service).
 *
 * Previously these sites called esp_read_mac(ESP_MAC_WIFI_STA) at request time,
 * which returns all-zeros before WiFi is ready and pollutes NVR stable_id.
 * See GitHub issues #2 / #3.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stable 12-hex-char device serial (e.g. "a4cf12345678").
 * @return Pointer to a static NUL-terminated buffer (12 hex chars). Cached.
 */
const char *device_get_serial(void);

/**
 * @brief ONVIF/WS-Discovery device UUID.
 * Format: "f472b01e-0000-1000-8000-a4cf12345678" (24-char prefix + 12 hex).
 * @return Pointer to a static NUL-terminated buffer. Cached.
 */
const char *device_get_uuid(void);

/**
 * @brief Fill out[6] with the cached factory MAC bytes.
 * @param out 6-byte output buffer (caller-allocated).
 */
void device_get_mac(uint8_t out[6]);

#ifdef __cplusplus
}
#endif
