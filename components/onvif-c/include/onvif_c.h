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
#include "../core/onvif_time.h" /* onvif_c_time_req_t (time-configuration seam) */
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on API/behavior changes: major*10000 + minor*100 + patch. */
#define ONVIF_C_VERSION 200

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
    /** SetSynchronizationPoint hook (Media + Events): the encoder should
     * emit a keyframe at the next opportunity. NULL = acknowledge only
     * (issue #14).                                                          */
    void (*on_keyframe)(void);

    /* ---- authentication (WS-Security UsernameToken, issue #17) ----
     * auth_password == NULL = feature absent: every request is served
     * unauthenticated (the historical behavior). When set, every action
     * except the pre-auth set (GetSystemDateAndTime per the Core spec)
     * must carry a valid UsernameToken (PasswordDigest; replay-guarded). */
    const char *(*auth_password)(void);   /* required to enable auth       */
    const char *auth_username;            /* NULL -> "admin"               */
    uint16_t    auth_window_secs;         /* Created window; 0 -> 300      */
    bool        auth_allow_password_text; /* insecure without TLS       */

    /* ---- events (Pull-Point MotionAlarm). NULL = feature absent:
     * /onvif/events_service is not registered and its XAddr is not
     * advertised in GetCapabilities.                                    */
    bool (*events_enabled)(void); /* runtime gate; false = drop motion events */

    /* ---- time configuration (SetSystemDateAndTime / SetNTP) ----
     * All four NULL = feature absent: SetSystemDateAndTime / SetNTP answer
     * ter:ActionNotSupported, and GetSystemDateAndTime reports MANUAL with
     * TZ "UTC" (never claim NTP without a management seam — an NVR that
     * cannot correct the clock must be told the truth, issue #43-class). */
    /** True when wall time is actually NTP-maintained right now — drives the
     *  GetSystemDateAndTime DateTimeType. */
    bool (*time_ntp_active)(void);
    /** Current POSIX TZ string for GetSystemDateAndTime; NULL/"" -> "UTC". */
    const char *(*time_tz)(void);
    /** Apply SetSystemDateAndTime: ntp_mode -> prefer the NTP source;
     *  has_utc -> set the wall clock to utc_epoch; has_tz -> set timezone.
     *  Non-ESP_OK return -> Sender fault carrying esp_err_to_name text.
     *  NULL = unsupported. */
    esp_err_t (*time_apply)(const onvif_c_time_req_t *req);
    /** Apply SetNTP: replace the NTP server list with the n given address
     *  strings (request order); n == 0 -> stop using NTP. Non-ESP_OK ->
     *  Sender fault. NULL = unsupported (the historical default). */
    esp_err_t (*ntp_set)(const char *const *servers, size_t n);

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
