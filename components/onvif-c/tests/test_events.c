/*
 * Port-layer tests — Pull-Point events service lifecycle.
 *
 * The fake clock (-Wl,--wrap=time) makes subscription expiry, idle
 * timeouts and TerminationTimes fully deterministic.
 */

#include "../include/onvif_c.h"
#include "test_util.h"
#include "onvif_fake.h"
#include <stdlib.h>

#define IDLE_LIMIT_S 120
#define LIFETIME_S   3600

static int  g_gate = 1;
static bool cb_gate(void)
{
    return g_gate != 0;
}

static const char *cb_serial(void)
{
    return T_SERIAL;
}
static const char *cb_uuid(void)
{
    return T_UUID;
}
static const char *cb_ip(void)
{
    return T_IP;
}

static const char *cb_stream_uri(void)
{
    static char uri[64];
    snprintf(uri, sizeof(uri), "rtsp://%s:554/stream", T_IP);
    return uri;
}

static onvif_c_config_t ev_cfg(uint16_t port)
{
    onvif_c_config_t c = {0};
    c.serial           = cb_serial;
    c.uuid             = cb_uuid;
    c.ip               = cb_ip;
    c.stream_uri       = cb_stream_uri;
    c.events_enabled   = cb_gate;
    c.http_port        = port;
    return c;
}

static void iso(time_t t, char *out, size_t n)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static char g_body[512];

static const char *ev_body(const char *inner)
{
    snprintf(g_body, sizeof(g_body),
             "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
             "<s:Body>%s</s:Body></s:Envelope>",
             inner);
    return g_body;
}

static const char *body_create(void)
{
    return ev_body("<tev:CreatePullPointSubscription "
                   "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\"/>");
}

static const char *body_pull(const char *limit)
{
    static char inner[160];
    if (limit) {
        snprintf(inner, sizeof(inner),
                 "<tev:PullMessages><tev:Timeout>PT1S</tev:Timeout>"
                 "<tev:MessageLimit>%s</tev:MessageLimit></tev:PullMessages>",
                 limit);
    } else {
        snprintf(inner, sizeof(inner),
                 "<tev:PullMessages><tev:Timeout>PT1S</tev:Timeout>"
                 "</tev:PullMessages>");
    }
    return ev_body(inner);
}

/* strlen() must run AFTER the body builder returns (argument evaluation
 * order is unspecified), so it lives inside this wrapper. */
static esp_err_t post_body(httpd_uri_t *u, const char *b, httpd_req_t *r)
{
    return onvif_fake_httpd_invoke(u, b, strlen(b), r);
}

static int count_events(const httpd_req_t *r)
{
    int         n = 0;
    const char *p = r->resp;
    while ((p = strstr(p, "<wsnt:NotificationMessage>")) != NULL) {
        n++;
        p++;
    }
    return n;
}

void test_events(void)
{
    httpd_req_t      r;
    httpd_uri_t     *u;
    char             now_s[24], term_s[24];
    onvif_c_config_t cfg = ev_cfg(80);
    httpd_handle_t   hd  = onvif_fake_httpd_handle();

    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_time_set(T_EPOCH);
    g_gate = 1;

    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "events start ok");
    u = onvif_fake_httpd_find("/onvif/events_service");
    CHECK(u != NULL, "events handler registered");

    /* pull before any subscription -> dereferenced fault */
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "pull without subscription handled");
    CHECK_SUB(r.resp, "ter:SubscriptionReferenceDereferenced", "pull without subscription faults");

    /* create -> address, CurrentTime/TerminationTime from the fake clock */
    CHECK(post_body(u, body_create(), &r) == ESP_OK, "create handled");
    CHECK_SUB(r.resp, "CreatePullPointSubscriptionResponse", "create response");
    CHECK_SUB(r.resp, "http://" T_IP ":80/onvif/events_service",
              "subscription address with default port");
    iso(T_EPOCH + LIFETIME_S, term_s, sizeof(term_s));
    CHECK_SUB(r.resp, term_s, "granted termination is now+1h");
    CHECK(onvif_c_events_subscribed(), "subscribed after create");

    /* second create while alive replaces the previous one */
    CHECK(post_body(u, body_create(), &r) == ESP_OK, "re-create handled");

    /* closed runtime gate drops motion events at the producer */
    g_gate = 0;
    onvif_c_motion(true, 90);
    g_gate = 1;
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "pull after gated motion handled");
    CHECK(count_events(&r) == 0, "gated motion not queued");
    CHECK_SUB(r.resp, "PullMessagesResponse", "pull response shape");

    /* motion -> pull delivers FIFO with state/score/timestamp */
    onvif_c_motion(true, 87);
    onvif_c_motion(false, 3);
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "pull with events handled");
    CHECK(count_events(&r) == 2, "both events delivered");
    iso(T_EPOCH, now_s, sizeof(now_s));
    {
        char        exp[64];
        const char *first = strstr(r.resp, "<wsnt:NotificationMessage>");
        CHECK(first != NULL, "event xml present");
        snprintf(exp, sizeof(exp), "UtcTime=\"%s\"", now_s);
        CHECK_SUB(first, exp, "event utc from fake clock");
        CHECK_SUB(first, "Name=\"State\" Value=\"true\"", "first event active");
        CHECK_SUB(first, "Name=\"Score\" Value=\"87\"", "first event score");
        CHECK_SUB(r.resp, "Name=\"State\" Value=\"false\"", "second event clear");
    }

    /* MessageLimit=1 splits delivery across pulls */
    onvif_c_motion(true, 10);
    onvif_c_motion(true, 11);
    onvif_c_motion(true, 12);
    CHECK(post_body(u, body_pull("1"), &r) == ESP_OK, "limited pull handled");
    CHECK(count_events(&r) == 1, "MessageLimit=1 delivers one");
    CHECK_SUB(r.resp, "Name=\"Score\" Value=\"10\"", "FIFO head delivered");
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "follow-up pull handled");
    CHECK(count_events(&r) == 2, "remainder delivered");

    /* invalid limits fall back to the cap of 6 */
    for (int i = 0; i < 8; i++) {
        onvif_c_motion(true, (uint8_t)(20 + i));
    }
    CHECK(post_body(u, body_pull("0"), &r) == ESP_OK, "limit=0 pull handled");
    CHECK(count_events(&r) == 6, "limit 0 falls back to 6");
    CHECK(post_body(u, body_pull("999"), &r) == ESP_OK, "limit=999 pull handled");
    CHECK(count_events(&r) == 2, "remaining events drain");

    /* ring overflow: 15 pushes keep the last 12 */
    for (int i = 0; i < 15; i++) {
        onvif_c_motion(true, (uint8_t)i);
    }
    CHECK(post_body(u, body_pull("abc"), &r) == ESP_OK, "garbage limit pull handled");
    CHECK(count_events(&r) == 6, "garbage limit falls back to 6");
    {
        char exp[48];
        snprintf(exp, sizeof(exp), "Name=\"Score\" Value=\"%d\"", 3);
        CHECK_SUB(r.resp, exp, "overflow dropped the three oldest");
    }
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "drain pull handled");
    CHECK(count_events(&r) == 6, "second overflow batch delivered");

    /* renew extends termination */
    onvif_fake_time_advance(60);
    CHECK(post_body(u, ev_body("<wsnt:Renew/>"), &r) == ESP_OK, "renew handled");
    CHECK_SUB(r.resp, "RenewResponse", "renew response");
    iso(T_EPOCH + 60 + LIFETIME_S, term_s, sizeof(term_s));
    CHECK_SUB(r.resp, term_s, "renew termination reflects the fake clock");

    /* idle expiry: >120s without a pull */
    onvif_fake_time_advance(IDLE_LIMIT_S + 1);
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "idle-expired pull handled");
    CHECK_SUB(r.resp, "ter:SubscriptionReferenceDereferenced", "idle expiry faults the pull");
    CHECK(!onvif_c_events_subscribed(), "subscribed false after idle expiry");
    CHECK(post_body(u, ev_body("<wsnt:Renew/>"), &r) == ESP_OK, "renew after expiry handled");
    CHECK_SUB(r.resp, "ter:SubscriptionReferenceDereferenced", "renew after expiry faults");

    /* termination expiry: granted lifetime elapses */
    CHECK(post_body(u, body_create(), &r) == ESP_OK, "recreate handled");
    onvif_fake_time_advance(LIFETIME_S + 1);
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "termination-expired pull handled");
    CHECK_SUB(r.resp, "ter:SubscriptionReferenceDereferenced",
              "termination expiry faults the pull");

    /* unsubscribe closes cleanly; motion afterwards is dropped silently */
    CHECK(post_body(u, body_create(), &r) == ESP_OK, "create before unsubscribe handled");
    onvif_c_motion(true, 55);
    CHECK(post_body(u, ev_body("<wsnt:Unsubscribe/>"), &r) == ESP_OK, "unsubscribe handled");
    CHECK_SUB(r.resp, "UnsubscribeResponse", "unsubscribe response");
    CHECK(!onvif_c_events_subscribed(), "subscribed false after unsubscribe");
    onvif_c_motion(true, 56);
    CHECK(post_body(u, body_pull(NULL), &r) == ESP_OK, "pull after unsubscribe handled");
    CHECK_SUB(r.resp, "ter:SubscriptionReferenceDereferenced", "pull after unsubscribe faults");
    CHECK(count_events(&r) == 0, "post-unsubscribe motion not delivered");

    /* events statics (issue #15) */
    CHECK(post_body(u, ev_body("<tev:GetEventProperties/>"), &r) == ESP_OK,
          "event properties handled");
    CHECK_SUB(r.resp, "<wsnt:FixedTopicSet>true</wsnt:FixedTopicSet>", "fixed topic set");
    CHECK_SUB(r.resp, "MessageContentFilterDialect", "message filter dialect");

    CHECK(post_body(u, ev_body("<tev:GetServiceCapabilities/>"), &r) == ESP_OK,
          "events caps handled");
    CHECK_SUB(r.resp, "WSPullPointSupport=\"true\"", "pull point supported");

    CHECK(post_body(u, ev_body("<tev:SetSynchronizationPoint/>"), &r) == ESP_OK,
          "events sync point handled");
    CHECK_SUB(r.resp, "SetSynchronizationPointResponse", "events sync point ack");

    CHECK(post_body(u, ev_body("<tev:GetNotARealAction/>"), &r) == ESP_OK,
          "unsupported events action handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "unsupported action fault");
    CHECK(onvif_fake_httpd_invoke(u, NULL, 0, &r) == ESP_OK, "empty events body handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "empty events body fault");
    CHECK(onvif_fake_httpd_invoke(u, "x", 5000, &r) == ESP_OK, "oversize events body handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "oversize events body fault");
    onvif_fake_httpd_recv_fail_once();
    CHECK(post_body(u, body_create(), &r) == ESP_OK, "events recv failure handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "events recv failure fault");

    /* partial first recv must be reassembled across recv() calls (#7) */
    onvif_fake_httpd_recv_short_once(20);
    CHECK(post_body(u, body_create(), &r) == ESP_OK, "events partial read handled");
    CHECK_SUB(r.resp, "CreatePullPointSubscriptionResponse", "events partial body reassembled");

    /* non-default http_port lands in the subscription address */
    {
        onvif_c_config_t cfg8 = ev_cfg(8080);
        CHECK(onvif_c_start(hd, &cfg8) == ESP_OK, "restart with port 8080");
        CHECK(post_body(u, body_create(), &r) == ESP_OK, "create on 8080 handled");
        CHECK_SUB(r.resp, "http://" T_IP ":8080/onvif/events_service",
                  "subscription address honors http_port");
    }

    onvif_c_stop();
    onvif_fake_task_drain();
}
