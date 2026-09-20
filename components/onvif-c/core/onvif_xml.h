/*
 * onvif-c core — canonical SOAP response builders (pure C, host-testable).
 *
 * All functions write into (buf, n) snprintf-style and return the would-be
 * length; negative or >= n means truncation. The exact bytes are pinned by
 * tests/golden — do not reflow "cosmetically": NVR integrators may match
 * raw substrings (byte stability guarantee).
 *
 * Namespace style follows the field-proven firmware output:
 *   Device/Media envelopes: soap:/tds:/trt:/tt: (lowercase "utf-8")
 *   Events envelopes:       s:/tev:/wsnt:      (uppercase "UTF-8")
 */

#ifndef ONVIF_C_XML_H
#define ONVIF_C_XML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- Device service ---- */
int onvif_xml_system_date_and_time(char *buf, size_t n, const struct tm *utc);
int onvif_xml_device_information(char *buf, size_t n, const char *manufacturer,
                                 const char *model, const char *firmware,
                                 const char *serial, const char *hardware_id);
/** @param events advertise the Events service XAddr (only when built with events). */
int onvif_xml_capabilities(char *buf, size_t n, const char *ip, bool events);

/* ---- Media service ---- */
int onvif_xml_profiles(char *buf, size_t n, int frame_rate);
int onvif_xml_stream_uri(char *buf, size_t n, const char *uri);
int onvif_xml_snapshot_uri(char *buf, size_t n, const char *uri);

/* ---- Faults ---- */
int onvif_xml_fault_action_not_supported(char *buf, size_t n);

/* ---- Events service (Pull-Point) ----
 * Timestamps are pre-formatted ISO-8601 ("YYYY-MM-DDThh:mm:ssZ", 24 bytes). */
int onvif_xml_create_pull_point_response(char *buf, size_t n, const char *ip,
                                         const char *now, const char *termination);
int onvif_xml_renew_response(char *buf, size_t n, const char *termination);
const char *onvif_xml_unsubscribe_response(void);
int onvif_xml_events_fault(char *buf, size_t n, const char *subcode,
                           const char *text);
/* PullMessages response is assembled in three stages (open / N events / close). */
int onvif_xml_pull_open(char *buf, size_t n);
int onvif_xml_pull_event(char *buf, size_t n, const char *utc, bool active,
                         unsigned score);
int onvif_xml_pull_close(char *buf, size_t n, const char *now,
                         const char *termination);

#endif /* ONVIF_C_XML_H */
