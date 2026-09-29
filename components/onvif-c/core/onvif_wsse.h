/*
 * onvif-c core — WS-Security UsernameToken verification (PasswordDigest).
 *
 * Pure C99, zero third-party dependencies, host-testable: SHA-1 and
 * Base64 are implemented here so the ESP-IDF build needs no mbedtls.
 * Digest is the ONVIF Core formula:
 *   BASE64(SHA1(BASE64_DECODE(nonce) + created + password))
 * Constant-time comparisons; replay protection is the caller-owned
 * bounded nonce cache (onvif_wsse_nonce_cache_t).
 */

#ifndef ONVIF_C_WSSE_H
#define ONVIF_C_WSSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SHA-1 / Base64 primitives (also used by tests) ---- */
void onvif_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
int  onvif_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_size);
int  onvif_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size);

/* Bounded replay cache: fixed slots, oldest-created evicted. */
#define ONVIF_WSSE_NONCE_SLOTS 16

typedef struct {
    uint8_t  nonce_hash[ONVIF_WSSE_NONCE_SLOTS][20];
    int64_t  created_unix[ONVIF_WSSE_NONCE_SLOTS];
    bool     used[ONVIF_WSSE_NONCE_SLOTS];
    unsigned next; /* round-robin eviction slot */
} onvif_wsse_nonce_cache_t;

typedef enum {
    ONVIF_WSSE_OK = 0,
    ONVIF_WSSE_NO_TOKEN,     /* request carries no UsernameToken            */
    ONVIF_WSSE_BAD_DIGEST,   /* password mismatch (or malformed fields)    */
    ONVIF_WSSE_STALE,        /* Created outside the freshness window       */
    ONVIF_WSSE_REPLAY,       /* nonce seen before                          */
    ONVIF_WSSE_TEXT_REJECTED /* PasswordText presented while disallowed    */
} onvif_wsse_status_t;

/** Verify the UsernameToken inside a raw SOAP envelope (strstr-based,
 *  namespace-prefix tolerant — no XML parser).
 *
 *  username      expected username (NULL = accept any).
 *  password      expected password (fail-closed on NULL/empty).
 *  window_secs   Created freshness window (<= 0 -> 300 s default).
 *  allow_text    also accept PasswordText (insecure without TLS).
 *  cache         nonce replay cache (NULL skips replay protection).
 */
onvif_wsse_status_t onvif_wsse_verify(const char *envelope, const char *username,
                                      const char *password, int64_t now_unix, int window_secs,
                                      bool allow_text, onvif_wsse_nonce_cache_t *cache);

/** Parse "YYYY-MM-DDTHH:MM:SS[.frac]Z" into unix seconds; false on
 *  malformed input (the caller treats that as a stale token). */
bool onvif_wsse_parse_created(const char *created, int64_t *out_unix);

#endif /* ONVIF_C_WSSE_H */
