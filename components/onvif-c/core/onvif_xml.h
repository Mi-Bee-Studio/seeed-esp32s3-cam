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
int onvif_xml_device_information(char *buf, size_t n, const char *manufacturer, const char *model,
                                 const char *firmware, const char *serial, const char *hardware_id);
/** @param events advertise the Events service XAddr (only when built with events). */
int onvif_xml_capabilities(char *buf, size_t n, const char *ip, unsigned port, bool events);

/* ---- Media service ---- */
int onvif_xml_profiles(char *buf, size_t n, int frame_rate);
int onvif_xml_stream_uri(char *buf, size_t n, const char *uri);
int onvif_xml_snapshot_uri(char *buf, size_t n, const char *uri);

/** GetServices — list the served services (Device/Media/Analytics,
 *  + Events when built with events) with Namespace + XAddr. */
int onvif_xml_services(char *buf, size_t n, const char *ip, unsigned port, bool events);
/** GetScopes — the resolved discovery scopes as Fixed/ScopeItem pairs. */
int onvif_xml_get_scopes(char *buf, size_t n, const char *scopes);
/** SystemReboot — protocol-level answer only (host decides side effects). */
int onvif_xml_system_reboot(char *buf, size_t n);
/** SetSystemDateAndTime — accept + acknowledge. */
int onvif_xml_set_system_date_and_time_ack(char *buf, size_t n);
/** Device GetServiceCapabilities — nothing optional supported. */
int onvif_xml_device_service_capabilities(char *buf, size_t n);

/* ---- Media service (issue #14) ---- */
int onvif_xml_video_sources(char *buf, size_t n, const char *token, int width, int height,
                            int frame_rate);
int onvif_xml_video_encoder_configurations(char *buf, size_t n, const char *token, int width,
                                           int height, int frame_rate, int bitrate_kbps);
int onvif_xml_video_encoder_configuration(char *buf, size_t n, const char *token, int width,
                                          int height, int frame_rate, int bitrate_kbps);
int onvif_xml_video_encoder_configuration_options(char *buf, size_t n, int width, int height,
                                                  int frame_rate);
int onvif_xml_set_video_encoder_configuration_ack(char *buf, size_t n);
int onvif_xml_guaranteed_encoder_instances(char *buf, size_t n);
int onvif_xml_set_synchronization_point_ack(char *buf, size_t n);
/** Media GetServiceCapabilities — snapshot yes, multicast explicitly off. */
int onvif_xml_media_service_capabilities(char *buf, size_t n);

/* ---- Events service statics (issue #15) ---- */
int onvif_xml_event_properties(char *buf, size_t n);
int onvif_xml_events_service_capabilities(char *buf, size_t n);
int onvif_xml_events_sync_point_ack(char *buf, size_t n);

/* ---- Media2 service (ver20, tr2 — minimal Profile-T subset, issue #18) ---- */
int onvif_xml_media2_profiles(char *buf, size_t n, int frame_rate);
int onvif_xml_media2_stream_uri(char *buf, size_t n, const char *uri);
int onvif_xml_media2_sync_point_ack(char *buf, size_t n);

/* ---- Faults ---- */
int onvif_xml_fault_action_not_supported(char *buf, size_t n);
/** WS-Security rejection fault (sent with HTTP 401). */
int onvif_xml_fault_not_authorized(char *buf, size_t n);

/* ---- Events service (Pull-Point) ----
 * Timestamps are pre-formatted ISO-8601 ("YYYY-MM-DDThh:mm:ssZ", 24 bytes). */
int         onvif_xml_create_pull_point_response(char *buf, size_t n, const char *ip, unsigned port,
                                                 const char *now, const char *termination);
int         onvif_xml_renew_response(char *buf, size_t n, const char *termination);
const char *onvif_xml_unsubscribe_response(void);
int         onvif_xml_events_fault(char *buf, size_t n, const char *subcode, const char *text);
/* PullMessages response is assembled in three stages (open / N events / close). */
int onvif_xml_pull_open(char *buf, size_t n);
int onvif_xml_pull_event(char *buf, size_t n, const char *utc, bool active, unsigned score);
int onvif_xml_pull_close(char *buf, size_t n, const char *now, const char *termination);

#endif /* ONVIF_C_XML_H */
