/* onvif_time.c — SetSystemDateAndTime / SetNTP request parsing.
 *
 * Element lookup matches local names (namespace-prefix agnostic — the
 * WS-Discovery parser's discipline); values are leaf text only.
 *
 * Copyright (C) 2024 onvif-c Authors
 * SPDX-License-Identifier: MIT
 */
#include "onvif_time.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool onvif_time_is_set_ntp(const char *body)
{
    return body && strstr(body, "SetNTP") != NULL;
}

/** Advance past an identifier ([A-Za-z0-9_.-]+). */
static const char *scan_ident(const char *q)
{
    while (isalnum((unsigned char)*q) || *q == '_' || *q == '.' || *q == '-')
        q++;
    return q;
}

/** Advance over [prefix ':'] and return the start of the local name;
 *  sets *name_end to the char after the name. */
static const char *split_name(const char *tag, const char **name_end)
{
    const char *name = tag;
    const char *q    = scan_ident(tag);
    if (*q == ':') {
        name = q + 1;
        q    = scan_ident(name);
    }
    *name_end = q;
    return name;
}

/** Find the text start of the first open tag whose LOCAL name matches;
 *  sets *end to the '<' of the matching close tag (or string end when
 *  unterminated). Returns NULL when the tag is absent. */
static const char *elem_scope(const char *hay, const char *local, const char **end)
{
    const size_t ll = strlen(local);
    for (const char *p = strchr(hay, '<'); p; p = strchr(p + 1, '<')) {
        const char *q = p + 1;
        if (!isalpha((unsigned char)*q) && *q != '_')
            continue; /* not a tag: comment, PI, close, CDATA */
        const char *name;
        const char *name_end;
        name         = split_name(q, &name_end);
        ptrdiff_t nl = name_end - name;
        if ((size_t)nl != ll || strncmp(name, local, ll) != 0 || *name_end != '>')
            continue;
        const char *text = name_end + 1;
        for (const char *c = strchr(text, '<'); c; c = strchr(c + 1, '<')) {
            if (c[1] != '/')
                continue;
            const char *cn, *cne;
            cn = split_name(c + 2, &cne);
            if ((size_t)(cne - cn) == ll && strncmp(cn, local, ll) == 0 && *cne == '>') {
                *end = c;
                return text;
            }
        }
        *end = text + strlen(text); /* unterminated: tolerate */
        return text;
    }
    return NULL;
}

/** Leaf text inside [scope, end): first open tag matching local, up to the
 *  next '<'. Returns NULL when absent; sets *len (0 when empty). */
static const char *leaf_text(const char *scope, const char *end, const char *local, size_t *len)
{
    const size_t ll = strlen(local);
    for (const char *p = memchr(scope, '<', (size_t)(end - scope)); p && p < end;
         p             = strchr(p + 1, '<')) {
        const char *q = p + 1;
        if (!isalpha((unsigned char)*q) && *q != '_')
            continue;
        const char *name;
        const char *name_end;
        name         = split_name(q, &name_end);
        ptrdiff_t nl = name_end - name;
        if ((size_t)nl != ll || strncmp(name, local, ll) != 0 || *name_end != '>')
            continue;
        const char *text = name_end + 1;
        const char *stop = memchr(text, '<', (size_t)(end - text));
        *len             = stop ? (size_t)(stop - text) : (size_t)(end - text);
        return text;
    }
    return NULL;
}

static bool parse_num(const char *s, size_t len, long lo, long hi, long *out)
{
    if (len == 0 || len > 5)
        return false;
    char  tmp[8];
    char *endp = NULL;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    long v   = strtol(tmp, &endp, 10);
    if (endp != tmp + len || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

/** Howard Hinnant's days_from_civil — days since 1970-01-01, proleptic
 *  Gregorian. Deterministic and TZ-independent (host timegm is not). */
static int64_t days_from_civil(int64_t y, long m, long d)
{
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

bool onvif_time_parse_set_request(const char *body, onvif_c_time_req_t *out)
{
    if (!body || !out)
        return false;
    memset(out, 0, sizeof(*out));

    size_t      len = 0;
    const char *dt  = leaf_text(body, body + strlen(body), "DateTimeType", &len);
    if (dt && len == 3 && strncmp(dt, "NTP", 3) == 0)
        out->ntp_mode = true;

    {
        const char *tze   = NULL;
        const char *scope = elem_scope(body, "TimeZone", &tze);
        if (scope) {
            size_t      tl = 0;
            const char *tz = leaf_text(scope, tze, "TZ", &tl);
            if (!tz) { /* tolerate clients omitting the TimeZone wrapper */
                tz = leaf_text(body, body + strlen(body), "TZ", &tl);
                if (!tz)
                    return false; /* TimeZone present but no TZ child */
            }
            if (tl >= sizeof(out->tz))
                tl = sizeof(out->tz) - 1;
            memcpy(out->tz, tz, tl);
            out->tz[tl] = '\0';
            out->has_tz = true;
        }
    }

    const char *ue = NULL;
    const char *us = elem_scope(body, "UTCDateTime", &ue);
    if (!us)
        return true; /* NTP-mode (or bare mode switch) without UTC time */

    long        y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    size_t      l = 0;
    const char *t;
    bool        ok = true;
    if (!(t = leaf_text(us, ue, "Year", &l)) || !parse_num(t, l, 1970, 9999, &y))
        ok = false;
    if (ok && (!(t = leaf_text(us, ue, "Month", &l)) || !parse_num(t, l, 1, 12, &mo)))
        ok = false;
    if (ok && (!(t = leaf_text(us, ue, "Day", &l)) || !parse_num(t, l, 1, 31, &d)))
        ok = false;
    if (ok && (!(t = leaf_text(us, ue, "Hour", &l)) || !parse_num(t, l, 0, 23, &h)))
        ok = false;
    if (ok && (!(t = leaf_text(us, ue, "Minute", &l)) || !parse_num(t, l, 0, 59, &mi)))
        ok = false;
    if (ok && (!(t = leaf_text(us, ue, "Second", &l)) || !parse_num(t, l, 0, 60, &s)))
        ok = false;
    if (!ok)
        return false;

    out->has_utc   = true;
    out->utc_epoch = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}

int onvif_time_parse_set_ntp(const char *body, char out[][ONVIF_TIME_HOST_MAX], int max)
{
    if (!body || !out || max <= 0)
        return 0;
    const size_t blen = strlen(body);
    int          n    = 0;
    for (int pass = 0; pass < 2 && n < max; pass++) {
        const char *local = pass == 0 ? "IPv4Address" : "IPv6Address";
        for (const char *p = body; (p = strstr(p, local)) != NULL && n < max; p += 1) {
            /* local-name match: the char before must not continue an
             * identifier and the tag must close right after the name */
            if (p > body && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '-'))
                continue;
            const char *q = p + strlen(local);
            if (*q != '>')
                continue;
            const char *text = q + 1;
            const char *stop = memchr(text, '<', blen - (size_t)(text - body));
            size_t      len  = stop ? (size_t)(stop - text) : strlen(text);
            if (len == 0 || len >= ONVIF_TIME_HOST_MAX)
                continue;
            memcpy(out[n], text, len);
            out[n][len] = '\0';
            n++;
        }
    }
    return n;
}
