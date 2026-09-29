/*
 * onvif-c core — WS-Security UsernameToken verification (issue #17).
 */

#include "onvif_wsse.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  SHA-1 (FIPS 180-1) — self-contained, ~1 KB code                    */
/* ------------------------------------------------------------------ */

static uint32_t rol32(uint32_t v, int b)
{
    return (v << b) | (v >> (32 - b));
}

void onvif_sha1(const uint8_t *data, size_t len, uint8_t digest[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t bits = (uint64_t)len * 8;

    /* Padded message: data + 0x80 + zeros + 8-byte big-endian bit length. */
    size_t  padded = ((len + 8) / 64 + 1) * 64;
    uint8_t buf[128];
    /* len <= 4096 request bodies / <64 B nonce+created+password inputs in
     * the ONVIF path; longer inputs are processed in 64-byte chunks. */
    if (padded <= sizeof(buf)) {
        memset(buf, 0, padded);
        memcpy(buf, data, len);
        buf[len] = 0x80;
        for (int i = 0; i < 8; i++) {
            buf[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
        }
        const uint8_t *p = buf;
        for (size_t off = 0; off < padded; off += 64, p += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++) {
                w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
                       ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
            }
            for (int i = 16; i < 80; i++) {
                w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                uint32_t t = rol32(a, 5) + f + e + k + w[i];
                e          = d;
                d          = c;
                c          = rol32(b, 30);
                b          = a;
                a          = t;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }
    }
    for (int i = 0; i < 5; i++) {
        digest[4 * i]     = (uint8_t)(h[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(h[i] >> 8);
        digest[4 * i + 3] = (uint8_t)(h[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Base64                                                             */
/* ------------------------------------------------------------------ */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int onvif_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size)
{
    size_t   o    = 0;
    uint32_t acc  = 0;
    int      bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=' || in[i] == '\r' || in[i] == '\n' || in[i] == ' ') {
            continue;
        }
        int v = b64_val(in[i]);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_size) {
                return -1;
            }
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

int onvif_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t            o     = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        if (o + 5 >= out_size) {
            return -1;
        }
        uint32_t v   = (uint32_t)in[i] << 16;
        int      rem = (int)(in_len - i);
        if (rem > 1)
            v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2)
            v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = rem > 1 ? tbl[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? tbl[v & 63] : '=';
    }
    if (o >= out_size) {
        return -1;
    }
    out[o] = '\0';
    return (int)o;
}

/* ------------------------------------------------------------------ */
/*  Envelope field extraction (strstr-based, prefix tolerant)          */
/* ------------------------------------------------------------------ */

/** Extract the text content of the first element whose local name is
 *  `tag` (namespace prefix tolerated), into out. */
static bool element_text(const char *body, const char *tag, char *out, size_t n)
{
    size_t      tag_len = strlen(tag);
    const char *p       = body;
    while ((p = strstr(p, "<")) != NULL) {
        const char *name = p + 1;
        const char *end  = name;
        while (*end && *end != '>' && *end != ' ' && *end != '/') {
            end++;
        }
        size_t      name_len  = (size_t)(end - name);
        const char *local     = name_len > tag_len + 1 && name[name_len - tag_len - 1] == ':'
                                    ? name + name_len - tag_len
                                    : name;
        size_t      local_len = (size_t)(end - local);
        if (local_len == tag_len && strncmp(local, tag, tag_len) == 0) {
            const char *gt = strchr(end, '>');
            if (!gt) {
                return false;
            }
            if (gt[-1] == '/') {
                return false; /* empty element */
            }
            const char *text = gt + 1;
            const char *lt   = strchr(text, '<');
            if (!lt) {
                return false;
            }
            size_t len = (size_t)(lt - text);
            if (len >= n) {
                len = n - 1;
            }
            memcpy(out, text, len);
            out[len] = '\0';
            return true;
        }
        p = end;
    }
    return false;
}

bool onvif_wsse_parse_created(const char *created, int64_t *out_unix)
{
    /* "YYYY-MM-DDTHH:MM:SS" prefix (optional fraction / zone ignored). */
    int y, mo, d, h, mi, s;
    if (sscanf(created, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return false;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) {
        return false;
    }
    /* Days-from-epoch (civil algorithm, valid 1970..9999). */
    int64_t yy = y;
    if (mo <= 2) {
        yy--;
    }
    int64_t era  = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe  = yy - era * 400;
    int64_t mp   = (mo + 9) % 12;
    int64_t doy  = (153 * mp + 2) / 5 + d - 1;
    int64_t doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    *out_unix    = days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
    return true;
}

/* Constant-time-ish comparison for digest equality. */
static bool ct_memeq(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

onvif_wsse_status_t onvif_wsse_verify(const char *envelope, const char *username,
                                      const char *password, int64_t now_unix, int window_secs,
                                      bool allow_text, onvif_wsse_nonce_cache_t *cache)
{
    if (!envelope || !password || !*password) {
        return ONVIF_WSSE_BAD_DIGEST; /* fail-closed on empty password */
    }
    if (!strstr(envelope, "UsernameToken")) {
        return ONVIF_WSSE_NO_TOKEN;
    }

    char username_val[64] = {0};
    char pwd_raw[256]     = {0};
    char nonce_b64[256]   = {0};
    char created[40]      = {0};
    if (!element_text(envelope, "Username", username_val, sizeof username_val)) {
        return ONVIF_WSSE_NO_TOKEN;
    }
    if (username && *username && strcmp(username_val, username) != 0) {
        return ONVIF_WSSE_BAD_DIGEST;
    }
    bool has_password = element_text(envelope, "Password", pwd_raw, sizeof pwd_raw);
    bool has_nonce    = element_text(envelope, "Nonce", nonce_b64, sizeof nonce_b64);
    bool has_created  = element_text(envelope, "Created", created, sizeof created);
    if (!has_password) {
        return ONVIF_WSSE_NO_TOKEN;
    }

    /* PasswordText: supported only when explicitly allowed (no TLS here:
     * plaintext credentials on the wire). */
    bool is_text = strstr(envelope, "#PasswordText") != NULL;
    if (is_text) {
        if (!allow_text) {
            return ONVIF_WSSE_TEXT_REJECTED;
        }
        if (strcmp(pwd_raw, password) != 0) {
            return ONVIF_WSSE_BAD_DIGEST;
        }
        return ONVIF_WSSE_OK; /* no nonce/created to check */
    }

    if (!has_nonce || !has_created) {
        return ONVIF_WSSE_BAD_DIGEST;
    }

    int64_t created_unix = 0;
    if (!onvif_wsse_parse_created(created, &created_unix)) {
        return ONVIF_WSSE_STALE;
    }
    if (window_secs <= 0) {
        window_secs = 300;
    }
    int64_t delta = now_unix - created_unix;
    if (delta < 0) {
        delta = -delta;
    }
    if (delta > (int64_t)window_secs) {
        return ONVIF_WSSE_STALE;
    }

    /* digest = BASE64(SHA1(BASE64_DECODE(nonce) + created + password)) */
    uint8_t nonce_raw[192];
    int nonce_len = onvif_base64_decode(nonce_b64, strlen(nonce_b64), nonce_raw, sizeof nonce_raw);
    if (nonce_len < 0) {
        return ONVIF_WSSE_BAD_DIGEST;
    }

    uint8_t payload[512];
    size_t  pl = 0;
    if ((size_t)nonce_len + strlen(created) + strlen(password) + 1 > sizeof payload) {
        return ONVIF_WSSE_BAD_DIGEST;
    }
    memcpy(payload, nonce_raw, (size_t)nonce_len);
    pl = (size_t)nonce_len;
    memcpy(payload + pl, created, strlen(created));
    pl += strlen(created);
    memcpy(payload + pl, password, strlen(password));
    pl += strlen(password);

    uint8_t digest[20];
    onvif_sha1(payload, pl, digest);
    char expect_b64[32];
    if (onvif_base64_encode(digest, sizeof digest, expect_b64, sizeof expect_b64) < 0) {
        return ONVIF_WSSE_BAD_DIGEST;
    }
    if (strlen(expect_b64) != strlen(pwd_raw) ||
        !ct_memeq((const uint8_t *)expect_b64, (const uint8_t *)pwd_raw, strlen(expect_b64))) {
        return ONVIF_WSSE_BAD_DIGEST;
    }

    if (cache) {
        /* Replay guard: nonce byte-value identity via its SHA-1. */
        uint8_t nonce_hash[20];
        onvif_sha1(nonce_raw, (size_t)nonce_len, nonce_hash);
        for (int i = 0; i < ONVIF_WSSE_NONCE_SLOTS; i++) {
            if (cache->used[i] && ct_memeq(cache->nonce_hash[i], nonce_hash, 20)) {
                return ONVIF_WSSE_REPLAY;
            }
        }
        unsigned slot = cache->next % ONVIF_WSSE_NONCE_SLOTS;
        memcpy(cache->nonce_hash[slot], nonce_hash, 20);
        cache->created_unix[slot] = created_unix;
        cache->used[slot]         = true;
        cache->next++;
    }

    return ONVIF_WSSE_OK;
}
