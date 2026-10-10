/*
 * Host test harness implementation: fake clock, httpd, tasks, UDP network.
 * See onvif_fake.h for the control API. Compiled into the port-layer test
 * binary only — never into the library or firmware.
 */

#include "onvif_fake.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "freertos/semphr.h"
#include "esp_task_wdt.h"

/* ------------------------------------------------------------------ */
/*  esp_err_to_name                                                    */
/* ------------------------------------------------------------------ */

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_HTTPD_HANDLER_EXISTS:
        return "ESP_ERR_HTTPD_HANDLER_EXISTS";
    default:
        return "UNKNOWN";
    }
}

void *heap_caps_malloc(size_t size, unsigned int caps)
{
    (void)caps;
    return malloc(size);
}

/* ------------------------------------------------------------------ */
/*  Fake clock (linked with -Wl,--wrap=time)                           */
/* ------------------------------------------------------------------ */

static time_t g_now = 1750000000;

void onvif_fake_time_set(time_t t)
{
    g_now = t;
}
void onvif_fake_time_advance(time_t s)
{
    g_now += s;
}
time_t onvif_fake_time_get(void)
{
    return g_now;
}

time_t __wrap_time(void *t)
{
    if (t) {
        *(time_t *)t = g_now;
    }
    return g_now;
}

/* ------------------------------------------------------------------ */
/*  Fake httpd                                                         */
/* ------------------------------------------------------------------ */

struct onvif_fake_httpd {
    int dummy;
};

static struct onvif_fake_httpd g_httpd;

#define FAKE_HTTPD_MAX_URIS 8

static struct {
    httpd_uri_t uri;
    int         used;
} g_uris[FAKE_HTTPD_MAX_URIS];

static int g_fail_register_once; /* error to return once, 0 = off */
static int g_fail_register_nth;  /* ordinal of call to fail, 0 = off */
static int g_fail_register_err;
static int g_register_calls;
static int g_recv_short_once; /* cap next recv, 0 = off        */
static int g_recv_fail_once;  /* fail next recv, 0 = off       */

void onvif_fake_httpd_reset(void)
{
    memset(g_uris, 0, sizeof(g_uris));
    g_fail_register_once = 0;
    g_fail_register_nth  = 0;
    g_fail_register_err  = 0;
    g_register_calls     = 0;
    g_recv_short_once    = 0;
    g_recv_fail_once     = 0;
}

httpd_handle_t onvif_fake_httpd_handle(void)
{
    return &g_httpd;
}

void onvif_fake_httpd_fail_register_once(int err)
{
    g_fail_register_once = err;
}

void onvif_fake_httpd_fail_register_nth(int nth, int err)
{
    g_fail_register_nth = nth;
    g_fail_register_err = err;
}

int onvif_fake_httpd_register_count(void)
{
    int n = 0;
    for (int i = 0; i < FAKE_HTTPD_MAX_URIS; i++) {
        n += g_uris[i].used;
    }
    return n;
}

httpd_uri_t *onvif_fake_httpd_find(const char *uri)
{
    for (int i = 0; i < FAKE_HTTPD_MAX_URIS; i++) {
        if (g_uris[i].used && strcmp(g_uris[i].uri.uri, uri) == 0) {
            return &g_uris[i].uri;
        }
    }
    return NULL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t hd, const httpd_uri_t *uri)
{
    (void)hd;
    g_register_calls++;
    if (g_fail_register_once) {
        int err              = g_fail_register_once;
        g_fail_register_once = 0;
        return (esp_err_t)err;
    }
    if (g_fail_register_nth == g_register_calls) {
        g_fail_register_nth = 0;
        return (esp_err_t)g_fail_register_err;
    }
    /* Like the real httpd: same URI + method twice is HANDLER_EXISTS. */
    for (int i = 0; i < FAKE_HTTPD_MAX_URIS; i++) {
        if (g_uris[i].used && strcmp(g_uris[i].uri.uri, uri->uri) == 0 &&
            g_uris[i].uri.method == uri->method) {
            return ESP_ERR_HTTPD_HANDLER_EXISTS;
        }
    }
    for (int i = 0; i < FAKE_HTTPD_MAX_URIS; i++) {
        if (!g_uris[i].used) {
            g_uris[i].uri  = *uri;
            g_uris[i].used = 1;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    r->recv_calls++;
    if (g_recv_fail_once) {
        g_recv_fail_once = 0;
        return -1;
    }
    /* Serve the injected stream from where the last call left off, so a
     * handler that loops over recv() (as required by #7) reassembles the
     * body; a one-shot cap simulates a short TCP segment. */
    size_t avail = r->inject_len - r->inject_off;
    size_t n     = avail < buf_len ? avail : buf_len;
    if (g_recv_short_once > 0) {
        if ((size_t)g_recv_short_once < n) {
            n = (size_t)g_recv_short_once;
        }
        g_recv_short_once = 0;
    }
    memcpy(buf, r->inject_body + r->inject_off, n);
    r->inject_off += n;
    return (int)n;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type)
{
    snprintf(r->resp_type, sizeof(r->resp_type), "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status)
{
    snprintf(r->status, sizeof(r->status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, size_t len)
{
    if (r->status[0] == '\0') {
        snprintf(r->status, sizeof(r->status), "200 OK");
    }
    if (len > FAKE_HTTPD_RESP_MAX - 1 - r->resp_len) {
        len = FAKE_HTTPD_RESP_MAX - 1 - r->resp_len;
    }
    memcpy(r->resp + r->resp_len, buf, len);
    r->resp_len += len;
    r->resp[r->resp_len] = '\0';
    r->resp_calls++;
    return ESP_OK;
}

void onvif_fake_httpd_recv_short_once(int n)
{
    g_recv_short_once = n;
}

void onvif_fake_httpd_recv_fail_once(void)
{
    g_recv_fail_once = 1;
}

esp_err_t onvif_fake_httpd_invoke(httpd_uri_t *u, const char *body, size_t content_len,
                                  httpd_req_t *out_req)
{
    memset(out_req, 0, sizeof(*out_req));
    out_req->content_len = content_len;
    out_req->inject_body = body ? body : "";
    out_req->inject_len  = body ? strlen(body) : 0;
    return u->handler(out_req);
}

/* ------------------------------------------------------------------ */
/*  Fake tasks (pthreads)                                              */
/* ------------------------------------------------------------------ */

struct onvif_fake_task {
    pthread_t tid;
    int       alive;  /* thread created, may still be running */
    int       joined; /* pthread_join completed */
};

struct task_spawn {
    TaskFunction_t fn;
    void          *arg;
};

/* Registry of allocated handles so drain() can join/free every task
 * exactly once, regardless of start/stop ordering. */
#define FAKE_TASK_MAX 16
static struct onvif_fake_task *g_tasks[FAKE_TASK_MAX];
static TaskHandle_t            g_last_task;
static int                     g_fail_create_once;

static void *task_trampoline(void *p)
{
    struct task_spawn s = *(struct task_spawn *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core)
{
    (void)name;
    (void)stack;
    (void)prio;
    (void)core;
    if (g_fail_create_once) {
        g_fail_create_once = 0;
        return pdFAIL;
    }
    struct onvif_fake_task *t = calloc(1, sizeof(*t));
    struct task_spawn      *s = malloc(sizeof(*s));
    if (!t || !s) {
        free(t);
        free(s);
        return pdFAIL;
    }
    /* Find a registry slot BEFORE publishing the handle. */
    int slot = -1;
    for (int i = 0; i < FAKE_TASK_MAX; i++) {
        if (!g_tasks[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        free(t);
        free(s);
        return pdFAIL;
    }
    s->fn    = fn;
    s->arg   = arg;
    t->alive = 1;
    /* Real FreeRTOS publishes the handle before the task can run; doing
     * it after pthread_create lets the new thread observe a NULL/cleared
     * handle and exit instantly (a classic fake-induced race). */
    g_tasks[slot] = t;
    g_last_task   = t;
    if (handle) {
        *handle = t;
    }
    if (pthread_create(&t->tid, NULL, task_trampoline, s) != 0) {
        g_tasks[slot] = NULL;
        if (handle) {
            *handle = NULL;
        }
        free(t);
        free(s);
        return pdFAIL;
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    /* Cannot kill a pthread. The discovery loop self-exits once its handle
     * is cleared; vTaskDelete(NULL) from the task itself is a no-op. */
    (void)task;
}

int onvif_fake_delay_max_ticks = 0;

void vTaskDelay(UBaseType_t ticks)
{
    /* Retry paths pass huge tick counts; cap the wall-clock so tests with
     * deliberate failure injection still run in milliseconds. */
    unsigned ms = ticks > 10 ? 10 : ticks;
    usleep(ms * 1000);
    if ((int)ticks > onvif_fake_delay_max_ticks) {
        onvif_fake_delay_max_ticks = (int)ticks;
    }
}

void onvif_fake_task_fail_create_once(void)
{
    g_fail_create_once = 1;
}

TaskHandle_t onvif_fake_task_last(void)
{
    return g_last_task;
}

void onvif_fake_task_join(TaskHandle_t t)
{
    if (t && t->alive) {
        pthread_join(t->tid, NULL);
        t->alive = 0;
    }
}

void onvif_fake_task_drain(void)
{
    for (int i = 0; i < FAKE_TASK_MAX; i++) {
        if (g_tasks[i]) {
            onvif_fake_task_join(g_tasks[i]);
            free(g_tasks[i]);
            g_tasks[i] = NULL;
        }
    }
    g_last_task = NULL;
}

/* ------------------------------------------------------------------ */
/*  Fake semaphores                                                    */
/* ------------------------------------------------------------------ */

struct onvif_fake_mutex {
    pthread_mutex_t m;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct onvif_fake_mutex *mtx = malloc(sizeof(*mtx));
    if (!mtx) {
        return NULL;
    }
    pthread_mutex_init(&mtx->m, NULL);
    return mtx;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mtx, UBaseType_t ticks)
{
    if (!mtx) {
        return pdFALSE;
    }
    if (ticks == 0) {
        return pthread_mutex_trylock(&mtx->m) == 0 ? pdTRUE : pdFALSE;
    }
    return pthread_mutex_lock(&mtx->m) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mtx)
{
    if (!mtx) {
        return pdFALSE;
    }
    return pthread_mutex_unlock(&mtx->m) == 0 ? pdTRUE : pdFALSE;
}

/* ------------------------------------------------------------------ */
/*  Virtual UDP network                                                */
/* ------------------------------------------------------------------ */

#define FAKE_NET_MAX_FDS   4
#define FAKE_NET_RX_QUEUE  8
#define FAKE_NET_MAX_SENDS 256
#define FAKE_NET_DGRAM_MAX 2200

struct fake_dgram {
    int      len;
    char     data[FAKE_NET_DGRAM_MAX];
    uint32_t from_raw;
    uint16_t from_port;
};

struct fake_tx {
    int      len;
    char     data[FAKE_NET_DGRAM_MAX];
    uint32_t dest_raw;
    uint16_t dest_port;
};

struct fake_fd {
    int               used;
    int               bound;
    struct fake_dgram rx[FAKE_NET_RX_QUEUE];
    int               rx_head, rx_count;
};

static struct fake_fd g_fds[FAKE_NET_MAX_FDS];
static struct fake_tx g_sends[FAKE_NET_MAX_SENDS];
static int            g_send_count;

static int g_fail_socket_once;
static int g_fail_bind_once;
static int g_fail_membership_once;

void onvif_fake_net_reset(void)
{
    memset(g_fds, 0, sizeof(g_fds));
    memset(g_sends, 0, sizeof(g_sends));
    g_send_count           = 0;
    g_fail_socket_once     = 0;
    g_fail_bind_once       = 0;
    g_fail_membership_once = 0;
}

void onvif_fake_net_fail_socket_once(void)
{
    g_fail_socket_once = 1;
}
void onvif_fake_net_fail_bind_once(void)
{
    g_fail_bind_once = 1;
}
void onvif_fake_net_fail_membership_once(void)
{
    g_fail_membership_once = 1;
}

int onvif_fake_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    if (g_fail_socket_once) {
        g_fail_socket_once = 0;
        return -1;
    }
    for (int i = 0; i < FAKE_NET_MAX_FDS; i++) {
        if (!g_fds[i].used) {
            memset(&g_fds[i], 0, sizeof(g_fds[i]));
            g_fds[i].used = 1;
            return i;
        }
    }
    return -1;
}

int onvif_fake_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    (void)len;
    if (fd < 0 || fd >= FAKE_NET_MAX_FDS || !g_fds[fd].used) {
        return -1;
    }
    if (g_fail_bind_once) {
        g_fail_bind_once = 0;
        return -1;
    }
    (void)addr;
    g_fds[fd].bound = 1;
    return 0;
}

int onvif_fake_setsockopt(int fd, int level, int optname, const void *optval, socklen_t len)
{
    (void)optval;
    (void)len;
    if (fd < 0 || fd >= FAKE_NET_MAX_FDS || !g_fds[fd].used) {
        return -1;
    }
    if (level == IPPROTO_IP && optname == IP_ADD_MEMBERSHIP && g_fail_membership_once) {
        g_fail_membership_once = 0;
        return -1;
    }
    return 0;
}

int onvif_fake_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from,
                        socklen_t *fromlen)
{
    (void)flags;
    if (fd < 0 || fd >= FAKE_NET_MAX_FDS || !g_fds[fd].used) {
        return -1;
    }
    struct fake_fd *f = &g_fds[fd];
    if (f->rx_count == 0) {
        usleep(2000); /* emulate the 5 s SO_RCVTIMEO at test pace */
        return 0;
    }
    struct fake_dgram *d = &f->rx[f->rx_head];
    size_t             n = (size_t)d->len < len ? (size_t)d->len : len;
    memcpy(buf, d->data, n);
    f->rx_head = (f->rx_head + 1) % FAKE_NET_RX_QUEUE;
    f->rx_count--;
    if (from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)(void *)from;
        sin->sin_family         = AF_INET;
        sin->sin_port           = htons(d->from_port);
        sin->sin_addr.s_addr    = d->from_raw;
        *fromlen                = sizeof(struct sockaddr_in);
    }
    return (int)n;
}

int onvif_fake_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to,
                      socklen_t tolen)
{
    (void)flags;
    (void)tolen;
    if (fd < 0 || fd >= FAKE_NET_MAX_FDS || !g_fds[fd].used) {
        return -1;
    }
    if (g_send_count >= FAKE_NET_MAX_SENDS || len > FAKE_NET_DGRAM_MAX) {
        return -1;
    }
    struct fake_tx *s = &g_sends[g_send_count];
    memcpy(s->data, buf, len);
    s->len = (int)len;
    if (to && tolen >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)to;
        s->dest_port                  = ntohs(sin->sin_port);
        s->dest_raw                   = sin->sin_addr.s_addr;
    } else {
        s->dest_port = 0;
        s->dest_raw  = 0;
    }
    g_send_count++;
    return (int)len;
}

int onvif_fake_close(int fd)
{
    if (fd < 0 || fd >= FAKE_NET_MAX_FDS) {
        return -1;
    }
    g_fds[fd].used = 0;
    return 0;
}

void onvif_fake_net_inject(const char *data, const char *from_ip, unsigned from_port)
{
    struct fake_dgram d;
    memset(&d, 0, sizeof(d));
    d.len = (int)strlen(data);
    if (d.len > FAKE_NET_DGRAM_MAX) {
        d.len = FAKE_NET_DGRAM_MAX;
    }
    memcpy(d.data, data, (size_t)d.len);
    d.from_raw  = inet_addr(from_ip);
    d.from_port = (uint16_t)from_port;

    for (int i = 0; i < FAKE_NET_MAX_FDS; i++) {
        struct fake_fd *f = &g_fds[i];
        if (!f->used || f->rx_count == FAKE_NET_RX_QUEUE) {
            continue;
        }
        int tail    = (f->rx_head + f->rx_count) % FAKE_NET_RX_QUEUE;
        f->rx[tail] = d;
        f->rx_count++;
    }
}

int onvif_fake_net_send_count(void)
{
    return g_send_count;
}

static void raw_to_ip(uint32_t raw, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (raw >> 24) & 0xFFu, (raw >> 16) & 0xFFu, (raw >> 8) & 0xFFu,
             raw & 0xFFu);
}

int onvif_fake_net_send_get(int idx, char *buf, size_t n, char *dest_ip, size_t ipn,
                            unsigned *dest_port)
{
    if (idx < 0 || idx >= g_send_count) {
        return -1;
    }
    size_t len = (size_t)g_sends[idx].len;
    if (len >= n) {
        len = n - 1;
    }
    memcpy(buf, g_sends[idx].data, len);
    buf[len] = '\0';
    if (dest_ip && ipn) {
        raw_to_ip(g_sends[idx].dest_raw, dest_ip, ipn);
    }
    if (dest_port) {
        *dest_port = g_sends[idx].dest_port;
    }
    return g_sends[idx].len;
}

int onvif_fake_net_wait_send(const char *needle, int timeout_ms)
{
    char scratch[FAKE_NET_DGRAM_MAX];
    for (int waited = 0; waited <= timeout_ms; waited += 10) {
        for (int i = 0; i < g_send_count; i++) {
            onvif_fake_net_send_get(i, scratch, sizeof(scratch), NULL, 0, NULL);
            if (strstr(scratch, needle)) {
                return i;
            }
        }
        usleep(10000);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  inet_addr / inet_ntoa (virtual network byte order = first octet    */
/*  in the most significant byte of the raw u32)                       */
/* ------------------------------------------------------------------ */

in_addr_t inet_addr(const char *cp)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 ||
        d > 255) {
        return htonl(INADDR_ANY);
    }
    return (in_addr_t)((a << 24) | (b << 16) | (c << 8) | d);
}

char *inet_ntoa(struct in_addr in)
{
    static char bufs[4][16];
    static int  rot;
    char       *buf = bufs[rot++ & 3];
    raw_to_ip(in.s_addr, buf, sizeof(bufs[0]));
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Fake task watchdog (counters only)                                 */
/* ------------------------------------------------------------------ */

int onvif_fake_wdt_added;
int onvif_fake_wdt_feeds;
int onvif_fake_wdt_deleted;

esp_err_t esp_task_wdt_add(void *task_handle)
{
    (void)task_handle;
    onvif_fake_wdt_added++;
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void)
{
    onvif_fake_wdt_feeds++;
    return ESP_OK;
}

esp_err_t esp_task_wdt_delete(void *task_handle)
{
    (void)task_handle;
    onvif_fake_wdt_deleted++;
    return ESP_OK;
}
