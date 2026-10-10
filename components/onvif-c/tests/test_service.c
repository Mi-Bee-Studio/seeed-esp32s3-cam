/*
 * Port-layer tests — SOAP Device/Media dispatch, config seam, lifecycle.
 *
 * Drives the real handlers registered by onvif_c_start() through the fake
 * httpd: request bodies go in via httpd_req_recv, responses are captured
 * per request. The fake clock makes GetSystemDateAndTime deterministic.
 *
 * Discipline: every phase that starts the library ends with stop_env(),
 * because a successful onvif_c_start() leaves the discovery task running.
 */

#include "../include/onvif_c.h"
#include "../esp_idf/onvif_c_port.h"
#include "test_util.h"
#include "../core/onvif_wsse.h"
#include "../core/onvif_time.h"
#include "onvif_fake.h"
#include <stdlib.h>

/* ---- controllable callbacks ---- */

static const char *cb_auth_password(void)
{
    return "s3cret";
}
static int  g_keyframe_calls;
static void cb_keyframe(void)
{
    g_keyframe_calls++;
}
/* time-configuration seam state */
static int                g_time_apply_calls;
static onvif_c_time_req_t g_time_req;
static esp_err_t          g_time_apply_err = ESP_OK;
static int                g_ntp_calls;
static size_t             g_ntp_n;
static char               g_ntp_last[ONVIF_TIME_HOST_MAX];
static esp_err_t          g_ntp_err = ESP_OK;
static int                g_ntp_active; /* 0 -> MANUAL (cb present) */
static const char        *g_tz_cb;      /* NULL -> cb absent */

static bool cb_time_ntp_active(void)
{
    return g_ntp_active == 1;
}
static const char *cb_time_tz(void)
{
    return g_tz_cb;
}
static esp_err_t cb_time_apply(const onvif_c_time_req_t *r)
{
    g_time_req = *r;
    g_time_apply_calls++;
    return g_time_apply_err;
}
static esp_err_t cb_ntp_set(const char *const *servers, size_t n)
{
    g_ntp_calls++;
    g_ntp_n = n;
    if (n > 0)
        snprintf(g_ntp_last, sizeof(g_ntp_last), "%s", servers[0]);
    return g_ntp_err;
}

static int g_gate    = 1;
static int g_fps     = 12;
static int g_ip_mode = 0;  /* 0 good, 1 zero-always */
static int g_ip_zero_once; /* next call returns 0.0.0.0, then re-arms */

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
    if (g_ip_mode == 1) {
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

static const char *cb_snapshot_uri(void)
{
    return "http://" T_IP "/cap";
}

/* #6 fixtures: integrator strings long enough to overflow a 4096-byte
 * response buffer (would-be length >> actual bytes). */
static const char *cb_stream_uri_huge(void)
{
    static char uri[5000];
    memset(uri, 'u', sizeof(uri) - 1);
    uri[sizeof(uri) - 1] = '\0';
    return uri;
}

static const char *cb_snapshot_uri_huge(void)
{
    return cb_stream_uri_huge();
}
static uint8_t cb_frame_rate(void)
{
    return (uint8_t)g_fps;
}
static bool cb_gate(void)
{
    return g_gate != 0;
}

static onvif_c_config_t base_cfg(void)
{
    onvif_c_config_t c = {0};
    c.serial           = cb_serial;
    c.uuid             = cb_uuid;
    c.ip               = cb_ip;
    c.stream_uri       = cb_stream_uri;
    c.frame_rate       = cb_frame_rate;
    c.on_keyframe      = cb_keyframe;
    c.time_ntp_active  = cb_time_ntp_active;
    c.time_tz          = cb_time_tz;
    c.time_apply       = cb_time_apply;
    c.ntp_set          = cb_ntp_set;
    c.http_port        = 80;
    return c;
}

static void reset_env(void)
{
    onvif_fake_httpd_reset();
    onvif_fake_net_reset();
    onvif_fake_time_set(T_EPOCH);
    g_gate         = 1;
    g_fps          = 12;
    g_ip_mode      = 0;
    g_ip_zero_once = 0;

    g_time_apply_calls = 0;
    g_time_apply_err   = ESP_OK;
    memset(&g_time_req, 0, sizeof(g_time_req));
    g_ntp_calls   = 0;
    g_ntp_n       = 0;
    g_ntp_err     = ESP_OK;
    g_ntp_last[0] = '\0';
    g_ntp_active  = 0;
    g_tz_cb       = NULL;
}

static void stop_env(void)
{
    onvif_c_stop();
    onvif_fake_task_drain();
}

static char g_body[512];

/* POST one SOAP action; body and length are built in a fixed order. */
static esp_err_t post(httpd_uri_t *u, const char *action, httpd_req_t *r)
{
    snprintf(g_body, sizeof(g_body),
             "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
             "<s:Body>%s</s:Body></s:Envelope>",
             action);
    return onvif_fake_httpd_invoke(u, g_body, strlen(g_body), r);
}

void test_service(void)
{
    httpd_req_t      r;
    httpd_uri_t     *u;
    onvif_c_config_t cfg;
    httpd_handle_t   hd = onvif_fake_httpd_handle();

    /* ---- onvif_c_start argument validation ---- */
    reset_env();
    cfg = base_cfg();
    CHECK(onvif_c_start(NULL, &cfg) == ESP_ERR_INVALID_ARG, "NULL httpd rejected");
    CHECK(onvif_c_start(hd, NULL) == ESP_ERR_INVALID_ARG, "NULL cfg rejected");
    cfg        = base_cfg();
    cfg.serial = NULL;
    CHECK(onvif_c_start(hd, &cfg) == ESP_ERR_INVALID_ARG, "NULL serial rejected");
    cfg      = base_cfg();
    cfg.uuid = NULL;
    CHECK(onvif_c_start(hd, &cfg) == ESP_ERR_INVALID_ARG, "NULL uuid rejected");
    cfg    = base_cfg();
    cfg.ip = NULL;
    CHECK(onvif_c_start(hd, &cfg) == ESP_ERR_INVALID_ARG, "NULL ip rejected");
    cfg            = base_cfg();
    cfg.stream_uri = NULL;
    CHECK(onvif_c_start(hd, &cfg) == ESP_ERR_INVALID_ARG, "NULL stream_uri rejected");
    CHECK(onvif_fake_httpd_register_count() == 0, "nothing registered on bad args");

    /* ---- happy start, no events ---- */
    reset_env();
    cfg = base_cfg(); /* events_enabled == NULL */
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start OK");
    CHECK(onvif_fake_httpd_register_count() == 3, "device+media+media2 registered");
    CHECK(onvif_fake_httpd_find("/onvif/events_service") == NULL,
          "no events handler without events_enabled");
    CHECK(onvif_c_version() == ONVIF_C_VERSION, "version macro exported");
    u = onvif_fake_httpd_find("/onvif/device_service");
    CHECK(u != NULL, "device handler found");

    /* GetSystemDateAndTime — deterministic via the fake clock */
    onvif_fake_time_set(T_EPOCH);
    CHECK(post(u, "<tds:GetSystemDateAndTime/>", &r) == ESP_OK, "date handler ok");
    CHECK_SUB(r.resp, "<tds:GetSystemDateAndTimeResponse>", "date response tag");
    CHECK_SUB(r.resp, "<tt:TZ>UTC</tt:TZ>", "date timezone");
    {
        time_t    t = T_EPOCH;
        struct tm tm;
        char      exp[40];
        gmtime_r(&t, &tm);
        snprintf(exp, sizeof(exp), "<tt:Hour>%d</tt:Hour>", tm.tm_hour);
        CHECK_SUB(r.resp, exp, "date hour from fake clock");
    }

    /* GetDeviceInformation — defaults resolve for every optional field */
    cfg = base_cfg(); /* manufacturer/model/hw/fw all NULL */
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "restart with new cfg ok");
    CHECK(post(u, "<tds:GetDeviceInformation/>", &r) == ESP_OK, "info handler ok");
    CHECK_SUB(r.resp, "<tds:Manufacturer>MiBee</tds:Manufacturer>", "default manufacturer");
    CHECK_SUB(r.resp, "<tds:Model>MiBeeCam</tds:Model>", "default model");
    CHECK_SUB(r.resp, "<tds:HardwareId>ESP32</tds:HardwareId>", "default hardware id");
    CHECK_SUB(r.resp, "<tds:FirmwareVersion>v0.1.0</tds:FirmwareVersion>",
              "default firmware version");
    CHECK_SUB(r.resp, "<tds:SerialNumber>" T_SERIAL "</tds:SerialNumber>", "serial from callback");
    CHECK_SUB(r.resp, "GetDeviceInformationResponse", "info response tag");

    /* default WS-Discovery scopes built from the model */
    CHECK_SUB(onvif_c_cfg_scopes(), "onvif://www.onvif.org/name/MiBeeCam",
              "default scopes include model name");
    CHECK(!onvif_c_cfg_has_events(), "has_events false without callback");

    /* ---- capabilities variants (http_port plumbing end to end) ---- */
    cfg           = base_cfg();
    cfg.http_port = 8080;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with port 8080");
    CHECK(post(u, "<tds:GetCapabilities/>", &r) == ESP_OK, "caps handler ok");
    CHECK_SUB(r.resp, "http://" T_IP ":8080/onvif/device_service",
              "caps device XAddr honors http_port");
    CHECK_SUB(r.resp, "http://" T_IP ":8080/onvif/media_service",
              "caps media XAddr honors http_port");
    CHECK(strstr(r.resp, "events_service") == NULL, "no events XAddr without events_enabled");

    cfg                = base_cfg();
    cfg.http_port      = 8080;
    cfg.events_enabled = cb_gate;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with events");
    CHECK(onvif_fake_httpd_find("/onvif/events_service") != NULL, "events handler registered");
    CHECK(onvif_c_cfg_has_events(), "has_events true with callback");
    CHECK(post(u, "<tds:GetCapabilities/>", &r) == ESP_OK, "caps handler ok");
    CHECK_SUB(r.resp, "http://" T_IP ":8080/onvif/events_service", "events XAddr when enabled");

    cfg                = base_cfg();
    cfg.http_port      = 8080;
    cfg.events_enabled = cb_gate;
    cfg.scopes         = "CUSTOM-SCOPES";
    g_ip_mode          = 1; /* ip callback answers 0.0.0.0 */
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with placeholder ip");
    CHECK(post(u, "<tds:GetCapabilities/>", &r) == ESP_OK, "caps handler ok");
    CHECK_SUB(r.resp, "http://0.0.0.0:8080/onvif/device_service", "placeholder ip propagated");
    CHECK(strcmp(onvif_c_cfg_scopes(), "CUSTOM-SCOPES") == 0, "custom scopes override default");
    g_ip_mode = 0;
    stop_env();

    /* ---- media service ---- */
    reset_env();
    cfg            = base_cfg();
    cfg.frame_rate = NULL; /* exercise the documented default (15) */
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "media start ok");
    u = onvif_fake_httpd_find("/onvif/media_service");
    CHECK(u != NULL, "media handler found");

    CHECK(post(u, "<trt:GetProfiles/>", &r) == ESP_OK, "profiles handler ok (default fps)");
    CHECK_SUB(r.resp, "<tt:FrameRateLimit>15</tt:FrameRateLimit>", "default frame rate 15");
    cfg   = base_cfg(); /* frame_rate callback present */
    g_fps = 12;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "restart with frame_rate cb");
    CHECK(post(u, "<trt:GetProfiles/>", &r) == ESP_OK, "profiles handler ok (cb fps)");
    CHECK_SUB(r.resp, "<tt:FrameRateLimit>12</tt:FrameRateLimit>", "frame rate from callback");

    CHECK(post(u, "<trt:GetStreamUri/>", &r) == ESP_OK, "stream uri handler ok");
    CHECK_SUB(r.resp, "<tt:Uri>rtsp://" T_IP ":554/stream</tt:Uri>", "stream uri from callback");

    /* GetSnapshotUri: derived URI follows http_port */
    cfg           = base_cfg();
    cfg.http_port = 8080; /* snapshot_uri NULL -> derived */
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start for derived snapshot");
    CHECK(post(u, "<trt:GetSnapshotUri/>", &r) == ESP_OK, "snapshot handler ok");
    CHECK_SUB(r.resp, "http://" T_IP ":8080/api/capture", "derived snapshot uri honors http_port");
    CHECK(post(u, "<trt:GetSnapshot/>", &r) == ESP_OK, "bare GetSnapshot ok");
    CHECK_SUB(r.resp, "http://" T_IP ":8080/api/capture", "GetSnapshot alias dispatches");

    cfg              = base_cfg();
    cfg.snapshot_uri = cb_snapshot_uri;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with snapshot callback");
    CHECK(post(u, "<trt:GetSnapshotUri/>", &r) == ESP_OK, "snapshot handler ok");
    CHECK_SUB(r.resp, "<tt:Uri>http://" T_IP "/cap</tt:Uri>", "explicit snapshot uri wins");

    /* ---- media completion (issue #14) ---- */
    CHECK(post(u, "<trt:GetVideoEncoderConfigurations/>", &r) == ESP_OK,
          "encoder configurations handled");
    CHECK_SUB(r.resp, "<trt:GetVideoEncoderConfigurationsResponse>", "encoder configs envelope");
    CHECK_SUB(r.resp, "<tt:Encoding>JPEG</tt:Encoding>", "encoder configs encoding");

    CHECK(post(u,
               "<trt:GetVideoEncoderConfiguration><trt:ConfigurationToken>t</"
               "trt:ConfigurationToken></trt:GetVideoEncoderConfiguration>",
               &r) == ESP_OK,
          "single encoder configuration handled");
    CHECK_SUB(r.resp, "GetVideoEncoderConfigurationResponse", "single encoder config envelope");

    CHECK(post(u, "<trt:GetVideoEncoderConfigurationOptions/>", &r) == ESP_OK,
          "encoder options handled");
    CHECK_SUB(r.resp,
              "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>12</tt:Max></tt:FrameRateRange>",
              "encoder options ranges");

    CHECK(post(u, "<trt:SetVideoEncoderConfiguration/>", &r) == ESP_OK,
          "set encoder configuration acknowledged");
    CHECK_SUB(r.resp, "SetVideoEncoderConfigurationResponse", "set encoder ack");

    CHECK(post(u, "<trt:GetGuaranteedNumberOfVideoEncoderInstances/>", &r) == ESP_OK,
          "guaranteed instances handled");
    CHECK_SUB(r.resp, "<trt:TotalInstances>1</trt:TotalInstances>", "one guaranteed instance");

    g_keyframe_calls = 0;
    CHECK(post(u, "<trt:SetSynchronizationPoint/>", &r) == ESP_OK, "sync point handled");
    CHECK_SUB(r.resp, "SetSynchronizationPointResponse", "sync point ack");
    CHECK(g_keyframe_calls == 1, "on_keyframe seam fired");

    CHECK(post(u, "<trt:GetServiceCapabilities/>", &r) == ESP_OK, "media caps handled");
    CHECK_SUB(r.resp, "RTP_Multicast=\"false\"", "multicast explicitly off");

    CHECK(post(u, "<trt:GetVideoSources/>", &r) == ESP_OK, "video sources handled");
    CHECK_SUB(r.resp, "GetVideoSourcesResponse", "video sources envelope");

    /* ---- device completion (issue #13) ---- */
    u = onvif_fake_httpd_find("/onvif/device_service");
    CHECK(u != NULL, "device handler still registered");
    CHECK(post(u, "<tds:GetServices/>", &r) == ESP_OK, "get services handled");
    CHECK_SUB(r.resp, "http://www.onvif.org/ver10/media/wsdl</tds:Namespace>", "media listed");
    CHECK_SUB(r.resp, "/onvif/media_service</tds:XAddr>", "media xaddr");

    CHECK(post(u, "<tds:GetScopes/>", &r) == ESP_OK, "get scopes handled");
    CHECK_SUB(r.resp, "<tt:ScopeDef>Fixed</tt:ScopeDef>", "scopes element form");
    CHECK_SUB(r.resp, "onvif://www.onvif.org/name/", "scopes content");

    CHECK(post(u, "<tds:SystemReboot/>", &r) == ESP_OK, "reboot handled");
    CHECK_SUB(r.resp, "<tds:Message>Device rebooting</tds:Message>", "reboot message");

    CHECK(post(u, "<tds:SetSystemDateAndTime/>", &r) == ESP_OK, "set date handled");
    CHECK_SUB(r.resp, "SetSystemDateAndTimeResponse", "set date ack");

    CHECK(post(u, "<tds:GetServiceCapabilities/>", &r) == ESP_OK, "device caps handled");
    CHECK_SUB(r.resp, "Network=\"false\"", "device caps network off");

    /* ---- faults and body edge cases ---- */
    /* ---- Media2 minimal face (issue #18) ---- */
    u = onvif_fake_httpd_find("/onvif/media2_service");
    CHECK(u != NULL, "media2 handler registered");
    CHECK(post(u, "<tr2:GetProfiles/>", &r) == ESP_OK, "media2 profiles handled");
    CHECK_SUB(r.resp, "<tr2:GetProfilesResponse>", "media2 profiles envelope");
    CHECK_SUB(r.resp, "VideoEncoder_1", "media2 configuration set");
    g_keyframe_calls = 0;
    CHECK(post(u, "<tr2:SetSynchronizationPoint/>", &r) == ESP_OK, "media2 sync handled");
    CHECK_SUB(r.resp, "tr2:SetSynchronizationPointResponse", "media2 sync ack");
    CHECK(g_keyframe_calls == 1, "media2 on_keyframe seam fired");
    CHECK(post(u, "<tr2:GetStreamUri/>", &r) == ESP_OK, "media2 stream uri handled");
    CHECK_SUB(r.resp, "<tr2:Uri>", "media2 plain uri flavor");
    u = onvif_fake_httpd_find("/onvif/device_service");
    CHECK(post(u, "<tds:GetServices/>", &r) == ESP_OK, "get services again");
    CHECK_SUB(r.resp, "http://www.onvif.org/ver20/media/wsdl", "media2 advertised");

    CHECK(post(u, "<trt:GetNotARealAction/>", &r) == ESP_OK, "unknown media action handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "unknown media action fault");

    CHECK(onvif_fake_httpd_invoke(u, NULL, 0, &r) == ESP_OK, "empty body handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "empty body fault");

    CHECK(onvif_fake_httpd_invoke(u, "x", 5000, &r) == ESP_OK, "oversize body handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "oversize body fault");

    onvif_fake_httpd_recv_fail_once();
    CHECK(post(u, "<trt:GetProfiles/>", &r) == ESP_OK, "recv failure handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "recv failure fault");

    /* a short first recv (one TCP segment) must be reassembled across
     * recv() calls, not degrade to a fault (#7) */
    u = onvif_fake_httpd_find("/onvif/device_service");
    onvif_fake_httpd_recv_short_once(8);
    CHECK(post(u, "<tds:GetCapabilities/>", &r) == ESP_OK, "partial read handled");
    CHECK_SUB(r.resp, "GetCapabilitiesResponse", "partial body reassembled");

    CHECK(onvif_fake_httpd_invoke(u, "zzz", 3, &r) == ESP_OK, "garbage body handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "garbage body fault");

    CHECK(post(u, "<tds:GetUsers/>", &r) == ESP_OK, "unknown device action handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "unknown device action fault");

    /* oversized integrator strings would overflow the response buffer: the
     * builder's would-be length is NOT a byte count — must fault, never
     * transmit past the buffer (#6; would be an ASan heap over-read) */
    {
        static char big[5000];
        memset(big, 'M', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        cfg                  = base_cfg();
        cfg.model            = big;
        CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with oversized model");
        CHECK(post(u, "<tds:GetDeviceInformation/>", &r) == ESP_OK, "oversized model handled");
        CHECK_SUB(r.resp, "ter:ActionNotSupported",
                  "oversized model faults instead of over-reading");
        CHECK(r.resp_len < FAKE_HTTPD_RESP_MAX, "response stays bounded");
    }
    {
        cfg              = base_cfg();
        cfg.stream_uri   = cb_stream_uri_huge;
        cfg.snapshot_uri = cb_snapshot_uri_huge;
        CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with oversized uris");
        u = onvif_fake_httpd_find("/onvif/media_service");
        CHECK(post(u, "<trt:GetStreamUri/>", &r) == ESP_OK, "oversized stream uri handled");
        CHECK_SUB(r.resp, "ter:ActionNotSupported",
                  "oversized stream uri faults instead of over-reading");
        CHECK(post(u, "<trt:GetSnapshotUri/>", &r) == ESP_OK, "oversized snapshot uri handled");
        CHECK_SUB(r.resp, "ter:ActionNotSupported",
                  "oversized snapshot uri faults instead of over-reading");
    }
    stop_env();

    /* ---- registration failure paths ---- */
    reset_env();
    cfg = base_cfg();
    onvif_fake_httpd_fail_register_once(ESP_ERR_HTTPD_HANDLER_EXISTS);
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "HANDLER_EXISTS on first uri tolerated");
    stop_env();

    reset_env();
    cfg = base_cfg();
    onvif_fake_httpd_fail_register_once(ESP_ERR_NO_MEM);
    CHECK(onvif_c_start(hd, &cfg) == ESP_ERR_NO_MEM, "hard registration failure propagates");
    CHECK(onvif_fake_httpd_register_count() == 0, "failed register stored nothing");

    reset_env();
    cfg                = base_cfg();
    cfg.events_enabled = cb_gate;
    onvif_fake_httpd_fail_register_nth(3, ESP_ERR_HTTPD_HANDLER_EXISTS);
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "events HANDLER_EXISTS tolerated");
    stop_env();

    reset_env();
    cfg = base_cfg();
    onvif_fake_task_fail_create_once();
    CHECK(onvif_c_start(hd, &cfg) == ESP_FAIL, "discovery task creation failure propagates");
    CHECK(onvif_fake_httpd_register_count() == 3, "handlers stay registered when discovery fails");

    /* ---- authentication gate (issue #17) ---- */
    onvif_fake_httpd_reset();
    reset_env();
    cfg               = base_cfg();
    cfg.auth_password = cb_auth_password;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start with auth ok");
    u = onvif_fake_httpd_find("/onvif/device_service");

    /* No token -> 401 + NotAuthorized fault. */
    CHECK(post(u, "<tds:GetDeviceInformation/>", &r) == ESP_OK, "no-token handled");
    CHECK_SUB(r.status, "401", "no-token gets 401");
    CHECK_SUB(r.resp, "NotAuthorized", "no-token fault body");

    /* Pre-auth action open per the Core spec. */
    CHECK(post(u, "<tds:GetSystemDateAndTime/>", &r) == ESP_OK, "pre-auth handled");
    CHECK_SUB(r.status, "200", "GetSystemDateAndTime open");
    CHECK_SUB(r.resp, "GetSystemDateAndTimeResponse", "pre-auth response");

    /* Valid digest accepted (built with the wsse primitives). */
    {
        uint8_t     d[20];
        char        digest_b64[32];
        char        env[1024];
        const char *created = "2026-09-29T10:00:00Z";
        int64_t     now_s   = 0;
        onvif_wsse_parse_created(created, &now_s);
        onvif_fake_time_set((time_t)now_s);
        uint8_t payload[128];
        size_t  pl = 0;
        memcpy(payload, "12345678", 8);
        pl = 8;
        memcpy(payload + pl, created, strlen(created));
        pl += strlen(created);
        memcpy(payload + pl, "s3cret", 6);
        pl += 6;
        onvif_sha1(payload, pl, d);
        onvif_base64_encode(d, 20, digest_b64, sizeof digest_b64);
        snprintf(env, sizeof env,
                 "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
                 " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
                 "oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
                 "<s:Header><wsse:Security><wsse:UsernameToken>"
                 "<wsse:Username>admin</wsse:Username>"
                 "<wsse:Password Type=\"#PasswordDigest\">%s</wsse:Password>"
                 "<wsse:Nonce>MTIzNDU2Nzg=</wsse:Nonce>"
                 "<wsu:Created>%s</wsu:Created>"
                 "</wsse:UsernameToken></wsse:Security></s:Header>"
                 "<s:Body><tds:GetDeviceInformation/></s:Body></s:Envelope>",
                 digest_b64, created);
        CHECK(onvif_fake_httpd_invoke(u, env, strlen(env), &r) == ESP_OK, "digest request handled");
        CHECK_SUB(r.status, "200", "valid digest gets 200");
        CHECK_SUB(r.resp, "GetDeviceInformationResponse", "valid digest served");

        /* Same envelope again -> replay rejected. */
        CHECK(onvif_fake_httpd_invoke(u, env, strlen(env), &r) == ESP_OK, "replay handled");
        CHECK_SUB(r.status, "401", "replayed nonce gets 401");
    }

    /* Media service behind the same gate. */
    u = onvif_fake_httpd_find("/onvif/media_service");
    CHECK(u != NULL, "media registered");
    CHECK(post(u, "<trt:GetProfiles/>", &r) == ESP_OK, "media no-token handled");
    CHECK_SUB(r.status, "401", "media no-token 401");

    onvif_c_stop();
    onvif_fake_task_drain();
}

void test_time_config_service(void)
{
    onvif_c_config_t cfg;
    httpd_uri_t     *u;
    httpd_req_t      r;
    httpd_handle_t   hd = onvif_fake_httpd_handle();

    static const char *SET_FULL =
        "<tds:SetSystemDateAndTime><tds:SystemDateAndTime>"
        "<tt:DateTimeType>MANUAL</tt:DateTimeType>"
        "<tt:TimeZone><tt:TZ>CST-8</tt:TZ></tt:TimeZone>"
        "<tt:UTCDateTime>"
        "<tt:Time><tt:Hour>12</tt:Hour><tt:Minute>34</tt:Minute><tt:Second>56</tt:Second></tt:Time>"
        "<tt:Date><tt:Year>2026</tt:Year><tt:Month>10</tt:Month><tt:Day>9</tt:Day></tt:Date>"
        "</tt:UTCDateTime></tds:SystemDateAndTime></tds:SetSystemDateAndTime>";
    static const char *SET_NTP =
        "<tds:SetNTP><tt:NTPFromDHCP>false</tt:NTPFromDHCP>"
        "<tt:NTPManual>"
        "<tt:NetworkHost><tt:Type>IPv4</tt:Type><tt:IPv4Address>192.168.1.10</tt:IPv4Address></"
        "tt:NetworkHost>"
        "<tt:NetworkHost><tt:Type>IPv4</tt:Type><tt:IPv4Address>192.168.1.11</tt:IPv4Address></"
        "tt:NetworkHost>"
        "</tt:NTPManual></tds:SetNTP>";

    /* -- applied path: ack + seam received parsed values -- */
    reset_env();
    cfg          = base_cfg();
    g_ntp_active = 1;
    g_tz_cb      = "CST-8";
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start OK");
    u = onvif_fake_httpd_find("/onvif/device_service");
    CHECK(u != NULL, "device handler found");

    CHECK(post(u, SET_FULL, &r) == ESP_OK, "set time handled");
    CHECK_SUB(r.resp, "<tds:SetSystemDateAndTimeResponse/>", "applied -> ack");
    CHECK(g_time_apply_calls == 1, "seam called once");
    CHECK(g_time_req.has_utc && !g_time_req.ntp_mode, "seam sees manual+utc");
    {
        struct tm t = {
            .tm_sec = 56, .tm_min = 34, .tm_hour = 12, .tm_mday = 9, .tm_mon = 9, .tm_year = 126};
        CHECK(g_time_req.utc_epoch == (int64_t)timegm(&t), "seam sees parsed epoch");
    }
    CHECK(g_time_req.has_tz && strcmp(g_time_req.tz, "CST-8") == 0, "seam sees tz");

    CHECK(post(u, SET_NTP, &r) == ESP_OK, "set ntp handled");
    CHECK_SUB(r.resp, "<tds:SetNTPResponse/>", "ntp applied -> ack");
    CHECK(g_ntp_calls == 1 && g_ntp_n == 2, "ntp seam called with 2 servers");
    CHECK(strcmp(g_ntp_last, "192.168.1.10") == 0, "first server in order");

    /* -- honesty: Get reflects the seam -- */
    CHECK(post(u, "<tds:GetSystemDateAndTime/>", &r) == ESP_OK, "get handled");
    CHECK_SUB(r.resp, "<tt:DateTimeType>NTP</tt:DateTimeType>", "mode NTP when active");
    CHECK_SUB(r.resp, "<tt:TZ>CST-8</tt:TZ>", "tz from seam");
    g_ntp_active = 0;
    CHECK(post(u, "<tds:GetSystemDateAndTime/>", &r) == ESP_OK, "get handled 2");
    CHECK_SUB(r.resp, "<tt:DateTimeType>MANUAL</tt:DateTimeType>", "mode MANUAL when inactive");

    /* -- seam error -> Sender fault, never a silent ack -- */
    g_time_apply_err = ESP_ERR_INVALID_STATE;
    CHECK(post(u, SET_FULL, &r) == ESP_OK, "failing apply handled");
    CHECK_SUB(r.resp, "<soap:Fault>", "apply failure -> fault");
    CHECK_SUB(r.resp, "ESP_ERR_INVALID_STATE", "fault carries err name");
    g_ntp_err = ESP_ERR_INVALID_STATE;
    CHECK(post(u, SET_NTP, &r) == ESP_OK, "failing ntp handled");
    CHECK_SUB(r.resp, "<soap:Fault>", "ntp failure -> fault");
    g_time_apply_err = ESP_OK;
    g_ntp_err        = ESP_OK;

    /* -- malformed request -> Sender fault -- */
    CHECK(post(u,
               "<tds:SetSystemDateAndTime><tt:UTCDateTime>"
               "<tt:Date><tt:Year>2026</tt:Year><tt:Month>99</tt:Month>"
               "<tt:Day>1</tt:Day></tt:Date></tt:UTCDateTime>"
               "</tds:SetSystemDateAndTime>",
               &r) == ESP_OK,
          "malformed handled");
    CHECK_SUB(r.resp, "ter:InvalidArgVal", "malformed -> InvalidArgVal");

    stop_env();

    /* -- feature absent: ActionNotSupported on both Set ops, MANUAL on Get -- */
    reset_env();
    cfg                 = base_cfg();
    cfg.time_ntp_active = NULL;
    cfg.time_tz         = NULL;
    cfg.time_apply      = NULL;
    cfg.ntp_set         = NULL;
    CHECK(onvif_c_start(hd, &cfg) == ESP_OK, "start OK (no seam)");
    u = onvif_fake_httpd_find("/onvif/device_service");
    CHECK(post(u, SET_FULL, &r) == ESP_OK, "absent set time handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "absent seam -> ActionNotSupported");
    CHECK(post(u, SET_NTP, &r) == ESP_OK, "absent set ntp handled");
    CHECK_SUB(r.resp, "ter:ActionNotSupported", "absent ntp seam -> ActionNotSupported");
    CHECK(g_time_apply_calls == 0 && g_ntp_calls == 0, "no calls when absent");
    CHECK(post(u, "<tds:GetSystemDateAndTime/>", &r) == ESP_OK, "get handled (absent)");
    CHECK_SUB(r.resp, "<tt:DateTimeType>MANUAL</tt:DateTimeType>", "no seam -> never claims NTP");

    stop_env();
}
