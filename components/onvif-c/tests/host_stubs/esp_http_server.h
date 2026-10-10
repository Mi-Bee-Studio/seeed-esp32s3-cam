/*
 * Host test stub: esp_http_server.h — request/response capture, no server.
 *
 * The library only reads req->content_len and pulls the body with
 * httpd_req_recv(); responses are captured per-request. Harness-only fields
 * live in the same struct so tests can drive real handlers directly.
 */

#ifndef STUB_ESP_HTTP_SERVER_H
#define STUB_ESP_HTTP_SERVER_H

#include <stddef.h>
#include "esp_err.h"

#define FAKE_HTTPD_RESP_MAX 8192
#define HTTP_POST           3

typedef struct httpd_req {
    /* ---- library-visible ---- */
    size_t content_len;
    /* ---- harness fields ---- */
    const char *inject_body; /* stream served by httpd_req_recv        */
    size_t      inject_len;
    size_t      inject_off;      /* consumed so far (partial-read loops)   */
    int         recv_short_once; /* >0: next recv returns at most this     */
    int         recv_fail_once;  /* 1: next recv returns -1                */
    int         recv_calls;
    char        resp[FAKE_HTTPD_RESP_MAX];
    size_t      resp_len;
    char        resp_type[64];
    char        status[32];
    int         resp_calls;
} httpd_req_t;

typedef struct onvif_fake_httpd *httpd_handle_t;

typedef struct {
    const char *uri;
    int         method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

esp_err_t httpd_register_uri_handler(httpd_handle_t hd, const httpd_uri_t *uri);
int       httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, size_t len);

#endif /* STUB_ESP_HTTP_SERVER_H */
