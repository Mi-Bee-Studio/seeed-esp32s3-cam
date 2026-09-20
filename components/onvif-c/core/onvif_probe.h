/*
 * onvif-c core — WS-Discovery message handling (pure C, host-testable).
 */

#ifndef ONVIF_C_PROBE_H
#define ONVIF_C_PROBE_H

#include <stdbool.h>
#include <stddef.h>

/** True when the datagram is a WS-Discovery Probe (not our own
 *  ProbeMatches/Hello echo). */
bool onvif_probe_is_probe(const char *body);

/** Extract the Probe's wsa:MessageID into out (RelatesTo value for
 *  the response). Returns false when absent/too large. */
bool onvif_probe_message_id(const char *body, char *out, size_t n);

/** Build a unicast ProbeMatches reply. uuid without urn:uuid: prefix;
 *  port is the HTTP port advertised in XAddrs (never hardcoded). */
int onvif_probe_build_matches(char *buf, size_t n, const char *relates_to, const char *uuid,
                              const char *ip, unsigned port, const char *scopes);

/** Build a multicast Hello announcement. */
int onvif_probe_build_hello(char *buf, size_t n, const char *uuid, const char *ip, unsigned port,
                            const char *scopes);

#endif /* ONVIF_C_PROBE_H */
