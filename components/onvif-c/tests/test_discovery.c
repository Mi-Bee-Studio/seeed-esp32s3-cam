/*
 * Port-layer tests — WS-Discovery responder over the virtual UDP network.
 *
 * The discovery task runs as a real thread against the fake sockets in
 * tests/host_stubs: injected Probes come back as unicast ProbeMatches,
 * Hello announcements appear in the captured outbound log, and failure
 * injection (socket/bind/membership) exercises the retry paths.
 */

#include "../include/onvif_c.h"
#include "test_util.h"
#include "onvif_fake.h"
#include "esp_task_wdt.h"
#include <stdlib.h>
#include <unistd.h>

static int g_ip_mode = 0; /* 0 good, 1 zero-always, 2 zero-first-2 */
static int g_ip_zero_calls;
static int g_ip_zero_once; /* next call returns 0.0.0.0, then re-arms */

static const char *cb_serial(void)
{
    return T_SERIAL;
}
static const char *cb_uuid(void)
{
    return T_UUID;
}

static const char *cb_ip_none(void)
{
    return NULL; /* simulates "no IP yet" (netif down / DHCP pending) */
}

static const char *cb_ip(void)
{
    if (g_ip_mode == 1) {
        return "0.0.0.0";
    }
    if (g_ip_mode == 2 && g_ip_zero_calls++ < 2) {
        return "0.0.0.0";
    }
    if (g_ip_zero_once) {
        g_ip_zero_once = 0;
        return "0.0.0.0";
    }
    return T_IP;
}

static const char *cb_stream_uri(void)
{
    static char uri[64];
    snprintf(uri, sizeof(uri), "rtsp://%s:554/stream", T_IP);
    return uri;
}

static onvif_c_config_t disco_cfg(void)
{
    onvif_c_config_t c = {0};
    c.serial           = cb_serial;
    c.uuid             = cb_uuid;
    c.ip               = cb_ip;
    c.stream_uri       = cb_stream_uri;
    c.http_port        = 8080;
    c.scopes           = "SCOPEX";
    return c;
}

static char g_probe[512];

static const char *probe_body(const char *message_id)
{
    if (message_id) {
        snprintf(g_probe, sizeof(g_probe),
                 "<?xml version=\"1.0\"?>"
                 "<soap:Envelope"
                 " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
                 " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
                 "<soap:Header><wsa:MessageID>%s</wsa:MessageID></soap:Header>"
                 "<soap:Body><wsdiscovery:Probe/></soap:Body>"
                 "</soap:Envelope>",
                 message_id);
    } else {
        snprintf(g_probe, sizeof(g_probe),
                 "<?xml version=\"1.0\"?>"
                 "<soap:Envelope"
                 " xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">"
                 "<soap:Body><wsdiscovery:Probe/></soap:Body>"
                 "</soap:Envelope>");
    }
    return g_probe;
}

static int count_sends_with(const char *needle)
{
    char buf[2304];
    int  n = 0;
    for (int i = 0; i < onvif_fake_net_send_count(); i++) {
        onvif_fake_net_send_get(i, buf, sizeof(buf), NULL, 0, NULL);
        if (strstr(buf, needle)) {
            n++;
        }
    }
    return n;
}

void test_discovery(void)
{
    char             buf[2304], ip[24];
    unsigned         port;
    int              idx;
    onvif_c_config_t cfg = disco_cfg();
    httpd_handle_t   hd  = onvif_fake_httpd_handle();

    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_time_set(T_EPOCH);
    g_ip_mode       = 0;
    g_ip_zero_calls = 0;
    g_ip_zero_once  = 0;

    /* happy path: initial Hello with port, scopes and uuid */
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "discovery start ok");
    idx = onvif_fake_net_wait_send("discovery/Hello", 2000);
    CHECK(idx >= 0, "initial Hello sent");
    if (idx >= 0) {
        onvif_fake_net_send_get(idx, buf, sizeof(buf), ip, sizeof(ip), &port);
        CHECK_SUB(buf, "http://" T_IP ":8080/onvif/device_service", "Hello XAddrs honor http_port");
        CHECK_SUB(buf, "urn:uuid:" T_UUID, "Hello endpoint uuid");
        CHECK_SUB(buf, ">SCOPEX<", "Hello carries configured scopes");
    }

    /* Probe -> unicast ProbeMatches echoing MessageID */
    onvif_fake_net_inject(probe_body("urn:uuid:probe-9"), "192.0.2.99", 5000);
    idx = onvif_fake_net_wait_send("ProbeMatches", 8000);
    CHECK(idx >= 0, "ProbeMatches sent");
    if (idx >= 0) {
        onvif_fake_net_send_get(idx, buf, sizeof(buf), ip, sizeof(ip), &port);
        CHECK(strcmp(ip, "192.0.2.99") == 0, "ProbeMatches unicast to prober");
        CHECK(port == 5000, "ProbeMatches to prober port");
        CHECK_SUB(buf, "<wsa:RelatesTo>urn:uuid:probe-9</wsa:RelatesTo>",
                  "RelatesTo echoes Probe MessageID");
        CHECK_SUB(buf, "http://" T_IP ":8080/onvif/device_service",
                  "ProbeMatches XAddrs honor http_port");
    }

    /* Probe without MessageID falls back to the placeholder RelatesTo */
    onvif_fake_net_inject(probe_body(NULL), "192.0.2.99", 5001);
    idx = onvif_fake_net_wait_send("urn:uuid:unknown", 8000);
    CHECK(idx >= 0, "missing MessageID uses placeholder RelatesTo");

    /* non-Probe traffic is ignored (no new ProbeMatches) */
    int matches_before = count_sends_with("ProbeMatches");
    onvif_fake_net_inject("<soap:Body><svc:Hello/></soap:Body>", "192.0.2.99", 5002);
    usleep(100000); /* give the task a chance to (wrongly) answer */
    CHECK(count_sends_with("ProbeMatches") == matches_before, "non-Probe datagram ignored");

    /* Probe while IP placeholder: answered once IP is back */
    g_ip_zero_once = 1; /* exactly one cfg_ip call reports 0.0.0.0 */
    onvif_fake_net_inject(probe_body("urn:uuid:probe-blind"), "192.0.2.99", 5003);
    usleep(50000);
    onvif_fake_net_inject(probe_body("urn:uuid:probe-ok"), "192.0.2.99", 5004);
    idx = onvif_fake_net_wait_send("urn:uuid:probe-ok", 8000);
    CHECK(idx >= 0, "probe answered after placeholder-ip window");

    /* periodic Hello: recv timeouts eventually resend (~6 polls here) */
    {
        int hellos0 = count_sends_with("discovery/Hello");
        int spins   = 0;
        while (count_sends_with("discovery/Hello") <= hellos0 && spins < 200) {
            usleep(10000);
            spins++;
        }
        CHECK(count_sends_with("discovery/Hello") > hellos0, "periodic Hello resent after idle");
    }

    CHECK(onvif_c_stop() == ESP_OK, "discovery stop ok");
    onvif_fake_task_drain();

    /* retry path: socket creation fails once, then succeeds */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_net_fail_socket_once();
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with socket failure ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "Hello after socket retry");
    onvif_c_stop();
    onvif_fake_task_drain();

    /* retry path: bind fails once, then succeeds */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_net_fail_bind_once();
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with bind failure ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "Hello after bind retry");
    onvif_c_stop();
    onvif_fake_task_drain();

    /* retry path: multicast membership fails once, then succeeds */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_net_fail_membership_once();
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with membership failure ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "Hello after membership retry");
    onvif_c_stop();
    onvif_fake_task_drain();

    /* "no IP yet" at boot: retries until the ip callback reports an address */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    g_ip_mode       = 2; /* first two ip reads are 0.0.0.0 */
    g_ip_zero_calls = 0;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start without ip ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "Hello once ip is available");
    onvif_c_stop();
    onvif_fake_task_drain();
    g_ip_mode = 0;

    /* Bye on stop (issue #16): the departure announcement is captured by
     * the fake net even though the task's socket dies with the task. */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start for bye test ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "hello before bye");
    onvif_fake_net_reset();
    CHECK(onvif_c_stop() == ESP_OK, "stop announces departure");
    onvif_fake_task_drain();
    CHECK(onvif_fake_net_wait_send("discovery/Bye", 2000) >= 0, "Bye sent on stop");

    /* Resolve -> ResolveMatches unicast (issue #16) */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start for resolve test ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "hello before resolve");
    {
        char resolve_body[512];
        snprintf(resolve_body, sizeof(resolve_body),
                 "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
                 " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
                 " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
                 "<s:Header><wsa:MessageID>urn:uuid:resolve-1</wsa:MessageID></s:Header>"
                 "<s:Body><wsd:Resolve>"
                 "<wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address>"
                 "</wsa:EndpointReference></wsd:Resolve>"
                 "</s:Body></s:Envelope>",
                 T_UUID);
        onvif_fake_net_inject(resolve_body, "192.168.7.9", 37020);
        CHECK(onvif_fake_net_wait_send("ResolveMatches", 8000) >= 0,
              "ResolveMatches sent for our address");
    }
    onvif_c_stop();
    onvif_fake_task_drain();

    /* mDNS requested but espressif/mdns absent: warning, discovery lives */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    {
        onvif_c_config_t cfg_mdns = disco_cfg();
        cfg_mdns.mdns_hostname    = "mibeecam-demo";
        CHECK(onvif_c_start(hd, &cfg_mdns) == ESP_OK, "start with unavailable mDNS still ok");
        CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0,
              "Hello despite mDNS being unavailable");
        onvif_c_stop();
        onvif_fake_task_drain();
    }

    /* oversized scopes would overflow the 2048-byte stack buffer: no
     * Hello and no ProbeMatches may leave the board (#6 stack path) */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    {
        static char big_scopes[4000];
        memset(big_scopes, 'S', sizeof(big_scopes) - 1);
        big_scopes[sizeof(big_scopes) - 1] = '\0';
        onvif_c_config_t cfg_big           = disco_cfg();
        cfg_big.scopes                     = big_scopes;
        CHECK(onvif_c_start(hd, &cfg_big) == ESP_OK, "start with huge scopes");
        usleep(150000); /* initial + periodic Hello windows */
        onvif_fake_net_inject(probe_body("urn:uuid:probe-big"), "192.0.2.99", 5010);
        usleep(150000);
        CHECK(onvif_fake_net_send_count() == 0, "truncated discovery frames are never sent");
        onvif_c_stop();
        onvif_fake_task_drain();
    }

    /* watchdog: discovery task feeds only when wdt_watch_discovery set */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_wdt_added   = 0;
    onvif_fake_wdt_feeds   = 0;
    onvif_fake_wdt_deleted = 0;
    {
        onvif_c_config_t cfg_w    = disco_cfg();
        cfg_w.wdt_watch_discovery = true;
        CHECK(onvif_c_start(hd, &cfg_w) == ESP_OK, "wdt start ok");
        int spins = 0;
        while (onvif_fake_wdt_added == 0 && spins < 200) {
            usleep(10000);
            spins++;
        }
        CHECK(onvif_fake_wdt_added == 1, "discovery task subscribed to wdt");
        int f0 = onvif_fake_wdt_feeds;
        CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0,
              "Hello still flows while wdt-watched");
        usleep(100000); /* several 2 ms loop iterations */
        CHECK(onvif_fake_wdt_feeds > f0, "watched discovery task feeds");
        onvif_c_stop();
        onvif_fake_task_drain();
        CHECK(onvif_fake_wdt_deleted == 1, "unsubscribe on task exit");
    }

    /* wdt-watched discovery must never sleep >1s in one vTaskDelay:
     * the no-IP retry loop is sliced into 1s chunks with a feed between
     * (regression: unit-2 crash loop 2026-09-28, offender onvif_disc). */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_wdt_added       = 0;
    onvif_fake_wdt_feeds       = 0;
    onvif_fake_delay_max_ticks = 0;
    {
        onvif_c_config_t cfg_n    = disco_cfg();
        cfg_n.ip                  = cb_ip_none; /* no IP -> retry loop */
        cfg_n.wdt_watch_discovery = true;
        CHECK(onvif_c_start(hd, &cfg_n) == ESP_OK, "no-ip wdt start ok");
        int spins = 0;
        while (onvif_fake_wdt_added == 0 && spins < 200) {
            usleep(10000);
            spins++;
        }
        CHECK(onvif_fake_wdt_added == 1, "no-ip discovery subscribed to wdt");
        usleep(300000); /* several sliced retry cycles */
        CHECK(onvif_fake_wdt_feeds > 1, "no-ip retry path keeps feeding");
        CHECK(onvif_fake_delay_max_ticks <= 1000, /* stub pdMS_TO_TICKS identity */
              "wdt-watched task never sleeps >1s per delay");
        onvif_c_stop();
        onvif_fake_task_drain();
    }
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_wdt_added = 0;
    onvif_fake_wdt_feeds = 0;
    {
        onvif_c_config_t cfg_w = disco_cfg(); /* flag default false */
        CHECK(onvif_c_start(hd, &cfg_w) == ESP_OK, "non-wdt start ok");
        usleep(50000);
        CHECK(onvif_fake_wdt_added == 0, "no subscribe without the flag");
        CHECK(onvif_fake_wdt_feeds == 0, "no feeds without the flag");
        onvif_c_stop();
        onvif_fake_task_drain();
    }

    /* stop -> start cycle creates a fresh task */
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "restart after stop ok");
    CHECK(onvif_fake_net_wait_send("discovery/Hello", 8000) >= 0, "Hello after restart");
    onvif_c_stop();
    onvif_fake_task_drain();
}
