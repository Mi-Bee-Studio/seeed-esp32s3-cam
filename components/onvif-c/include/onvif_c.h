/*
 * onvif-c — minimal ONVIF Device (server) library for ESP-IDF
 *
 * Copyright (c) 2026 MickeyZZC / Mi-Bee Studio
 *
 * SPDX-License-Identifier: MIT
 *
 * Exposes a camera to NVRs the way commercial cameras do:
 *   - SOAP Device/Media service   (POST /onvif/device_service, /onvif/media_service)
 *   - Pull-Point Events service   (POST /onvif/events_service — MotionAlarm topic)
 *   - WS-Discovery responder      (UDP 3702 multicast, Probe -> ProbeMatches + Hello)
 *   - optional mDNS               (_onvif._tcp)
 *
 * No XML parser, no dynamic state beyond per-request buffers: action
 * detection via strstr(), response generation via snprintf(). Response
 * bytes are stable and pinned by host golden tests (tests/).
 *
 * Board integration surface is ONE config struct of callbacks:
 * everything board-specific (identity, IP, stream URI, runtime gates)
 * stays in the integrator's port layer.
 */

#ifndef ONVIF_C_H
#define ONVIF_C_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on API/behavior changes: major*10000 + minor*100 + patch. */
#define ONVIF_C_VERSION 100

typedef struct {
    /* ---- identity (strings copied by reference; must outlive onvif_c_start) ---- */
    const char *manufacturer;     /* NULL -> "MiBee"                          */
    const char *model;            /* NULL -> "MiBeeCam"                       */
    const char *hardware_id;      /* NULL -> "ESP32"                          */
    const char *firmware_version; /* NULL -> "v0.1.0"                         */

    /* ---- required callbacks ---- */
    /** Stable device serial (hex string). Used by GetDeviceInformation. */
    const char *(*serial)(void);
    /** Device UUID without "urn:uuid:" prefix, e.g. "f472b01e-0000-1000-8000-aabbccddeeff". */
    const char *(*uuid)(void);
    /** Current IP, or NULL / "0.0.0.0" while not connected (services retry). */
    const char *(*ip)(void);
    /** GetStreamUri answer, e.g. "rtsp://IP:554/stream" or "http://IP:81/stream". */
    const char *(*stream_uri)(void);

    /* ---- optional callbacks ---- */
    /** GetSnapshotUri answer; NULL -> derived as http://<ip>:<http_port>/api/capture. */
    const char *(*snapshot_uri)(void);
    /** GetProfiles FrameRateLimit; NULL -> 15. */
    uint8_t (*frame_rate)(void);

    /* ---- events (Pull-Point MotionAlarm). NULL = feature absent:
     * /onvif/events_service is not registered and its XAddr is not
     * advertised in GetCapabilities.                                    */
    bool (*events_enabled)(void); /* runtime gate; false = drop motion events */

    /* ---- discovery / mDNS ---- */
    uint16_t http_port; /* 0 -> 80. Used in XAddrs/URIs.           */
    /** Subscribe the WS-Discovery task to the ESP-IDF task watchdog
     *  (esp_task_wdt; the loop paces itself with a 5 s receive timeout, so
     *  a wedged discovery task stops feeding and the TWDT fires). Requires
     *  CONFIG_ESP_TASK_WDT in the hosting project. Default false. */
    bool        wdt_watch_discovery;
    const char *mdns_hostname; /* NULL = skip mDNS entirely.              */
    const char *mdns_instance; /* NULL = model.                           */
    /** WS-Discovery Scopes body. NULL = default set (video_encoder,
     *  NetworkVideoTransmitter, hardware/<model>, name/<model>, Streaming). */
    const char *scopes;
} onvif_c_config_t;

/**
 * @brief Register SOAP handlers and start the WS-Discovery task.
 * @param httpd an esp_http_server handle (must already be started).
 * @param cfg   integration callbacks; shallow-copied, must stay valid.
 * @return ESP_OK, or first registration error (ESP_ERR_INVALID_ARG on nulls).
 *
 * Safe to call once per boot; re-registration of handlers is tolerated
 * (ESP_ERR_HTTPD_HANDLER_EXISTS is logged and ignored).
 */
esp_err_t onvif_c_start(httpd_handle_t httpd, const onvif_c_config_t *cfg);

/** Stop the discovery task and remove mDNS service. SOAP handlers stay
 *  registered (esp_http_server has no unregister API). */
esp_err_t onvif_c_stop(void);

/**
 * @brief Feed a motion state transition into the events service.
 *
 * Contract: safe to call from sensor/CSI callback context — it never
 * blocks (lock contention drops the event) and does no I/O. Events are
 * queued only while a Pull-Point subscription is alive AND the runtime
 * gate (events_enabled) returns true.
 */
void onvif_c_motion(bool active, uint8_t score);

/** True while a Pull-Point subscription is alive (diagnostic surface). */
bool onvif_c_events_subscribed(void);

int onvif_c_version(void);

#ifdef __cplusplus
}
#endif

#endif /* ONVIF_C_H */
