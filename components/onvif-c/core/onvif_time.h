/* onvif_time.h — SetSystemDateAndTime / SetNTP request parsing (host-testable core).
 *
 * Copyright (C) 2024 onvif-c Authors
 * SPDX-License-Identifier: MIT
 */
#ifndef ONVIF_TIME_H
#define ONVIF_TIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Longest address string parsed out of SetNTP (IPv6 textual max is 45). */
#define ONVIF_TIME_HOST_MAX 46

/** Parsed SetSystemDateAndTime request (time-configuration seam).
 *  Defined here (pure C99, stub-free) and re-exported by include/onvif_c.h
 *  for the config-callback signatures. */
typedef struct {
    bool    ntp_mode;  /* tt:DateTimeType == NTP                             */
    bool    has_utc;   /* tt:UTCDateTime present and well-formed             */
    int64_t utc_epoch; /* UTCDateTime as unix seconds (UTC)                  */
    bool    has_tz;    /* tt:TimeZone/tt:TZ present                          */
    char    tz[48];    /* POSIX TZ text, e.g. "CST-8"                        */
} onvif_c_time_req_t;

/** True when the SOAP body is a SetNTP request. */
bool onvif_time_is_set_ntp(const char *body);

/** Parse a SetSystemDateAndTime body. Returns false when a UTCDateTime is
 *  present but malformed (caller should Sender-fault); an NTP-mode request
 *  without UTCDateTime parses fine with has_utc=false. */
bool onvif_time_parse_set_request(const char *body, onvif_c_time_req_t *out);

/** Collect NTP server addresses (IPv4Address / IPv6Address leaves, in request
 *  order) into out[max]. Returns the count (0 = none — per ONVIF semantics a
 *  SetNTP with no manual servers removes the NTP configuration). */
int onvif_time_parse_set_ntp(const char *body, char out[][ONVIF_TIME_HOST_MAX], int max);

#endif /* ONVIF_TIME_H */
