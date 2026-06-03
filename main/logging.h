/*
 * Copyright (C) 2024 MiBeeHomeCam Authors
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

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ── Event type string constants ──
 * Recording events
 */
#define LOG_EVENT_REC_STARTED       "recording_started"
#define LOG_EVENT_REC_STOPPED       "recording_stopped"
#define LOG_EVENT_SEGMENT_COMPLETE  "segment_complete"
#define LOG_EVENT_FRAME_DROP        "frame_drop"

/* Upload events */
#define LOG_EVENT_UPLOAD_STARTED    "upload_started"
#define LOG_EVENT_UPLOAD_SUCCESS    "upload_success"
#define LOG_EVENT_UPLOAD_FAILED     "upload_failed"
#define LOG_EVENT_QUEUE_FULL        "queue_full"

/* WiFi events */
#define LOG_EVENT_WIFI_CONNECTED    "wifi_connected"
#define LOG_EVENT_WIFI_DISCONNECTED "wifi_disconnected"
#define LOG_EVENT_WIFI_AP_STARTED   "wifi_ap_started"

/* SD card events */
#define LOG_EVENT_SD_INSERTED       "sd_inserted"
#define LOG_EVENT_SD_REMOVED        "sd_removed"
#define LOG_EVENT_SD_CLEANUP_STARTED  "sd_cleanup_started"
#define LOG_EVENT_SD_CLEANUP_DONE     "sd_cleanup_completed"

/**
 * @brief Output a structured JSON log line to serial.
 *
 * Format: {"type":"<event_type>","ts":"<ISO-8601>","msg":"<formatted message>"}
 *
 * @param type  Event type string (use one of the LOG_EVENT_* constants).
 * @param msg   printf-style format string.
 * @param ...   Optional printf arguments.
 */
void log_event_impl(const char *type, const char *msg, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @brief Convenience macro — shadow over log_event_impl().
 *
 * Example:
 *   LOG_EVENT(LOG_EVENT_REC_STARTED, "resolution=%ux%u fps=%u", 640, 480, 10);
 */
#define LOG_EVENT(type, msg, ...)  log_event_impl(type, msg, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
