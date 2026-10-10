/*
 * Host test harness control API — the knobs tests use to drive the fakes
 * declared by the stub headers in this directory (httpd / tasks / clock /
 * virtual UDP network). Implementation: onvif_fake.c.
 *
 * The clock is link-time wrapped: build with -Wl,--wrap=time so every
 * time() call inside the library reads the controllable fake clock.
 */

#ifndef ONVIF_FAKE_H
#define ONVIF_FAKE_H

#include <stddef.h>
#include <time.h>
#include "esp_http_server.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

/* ---- fake clock ---- */
void   onvif_fake_time_set(time_t t);
void   onvif_fake_time_advance(time_t seconds);
time_t onvif_fake_time_get(void);

/* ---- fake httpd ---- */
void           onvif_fake_httpd_reset(void);
httpd_handle_t onvif_fake_httpd_handle(void);
/* Next httpd_register_uri_handler returns this error once (e.g. to test
 * ESP_ERR_HTTPD_HANDLER_EXISTS tolerance or hard-failure propagation). */
void onvif_fake_httpd_fail_register_once(int err);
/* The nth httpd_register_uri_handler call (1-based, since last reset)
 * returns this error. */
void         onvif_fake_httpd_fail_register_nth(int nth, int err);
int          onvif_fake_httpd_register_count(void);
httpd_uri_t *onvif_fake_httpd_find(const char *uri); /* NULL if absent */
/* Next httpd_req_recv returns at most n bytes (short read), once. */
void onvif_fake_httpd_recv_short_once(int n);
/* Next httpd_req_recv returns -1 (transport error), once. */
void onvif_fake_httpd_recv_fail_once(void);
/* Run a registered handler with body as the request body; content_len is
 * declared explicitly so oversize/empty cases can be simulated. The
 * captured response lands in out_req. */
esp_err_t onvif_fake_httpd_invoke(httpd_uri_t *u, const char *body, size_t content_len,
                                  httpd_req_t *out_req);

/* ---- fake tasks ---- */
void         onvif_fake_task_fail_create_once(void);
TaskHandle_t onvif_fake_task_last(void);
/* Block until the fake task's thread has exited, then free the handle. */
void onvif_fake_task_join(TaskHandle_t t);
/* Join and release every live fake task (call after onvif_c_stop()). */
void onvif_fake_task_drain(void);

/* ---- virtual UDP network ---- */
void onvif_fake_net_reset(void);
/* Largest tick count ever passed to vTaskDelay since the last reset.
 * The wdt-watched discovery task must never sleep >1s in one call. */
extern int onvif_fake_delay_max_ticks;
void       onvif_fake_net_fail_socket_once(void);
void       onvif_fake_net_fail_bind_once(void);
void       onvif_fake_net_fail_membership_once(void);
/* Queue an inbound datagram on every open fake socket (sender as given). */
void onvif_fake_net_inject(const char *data, const char *from_ip, unsigned from_port);
int  onvif_fake_net_send_count(void);
/* Copy send idx (oldest first) into buf; -1 when idx out of range. */
int onvif_fake_net_send_get(int idx, char *buf, size_t n, char *dest_ip, size_t ipn,
                            unsigned *dest_port);
/* Wait up to timeout_ms for an outbound send containing `needle`;
 * returns its index or -1. */
int onvif_fake_net_wait_send(const char *needle, int timeout_ms);

#endif /* ONVIF_FAKE_H */
