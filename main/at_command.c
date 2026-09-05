/*
 * at_command.c — MiBee Cam 家族 AT 控制台核心（契约 v1.1，2026-09-05）
 *
 * 四仓共享同一份核心（md5 纪律，docs/at-command.md §0）：解析器、指令表、
 * 响应框架（`+NAME:内容` 数据行 / `OK` / `ERROR: 原因`）、HELP 自动生成。
 * 一切板差异（IO 后端、能力裁剪、生效语义、§5 扩展指令）在 main/at_port.c。
 *
 * 传输约定（契约 §1）：输入大小写不敏感；非 AT 行静默忽略（与 ESP_LOG
 * 共口时不刷屏）；参数区分大小写。
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_timer.h"

#include "at_command.h"
#include "at_port.h"

static void at_status_emit_trampoline(const char *name, const char *value);
static void at_scan_emit_trampoline(const char *ssid, int32_t rssi, int32_t auth);

static const char *TAG = "at_cmd";

#define AT_LINE_MAX     160
#define AT_TASK_STACK   4096
#define AT_TASK_PRIO    5
#define AT_SETTLE_MS    3000   /* 上电静默期：让启动日志先走完 */

/* ── 输出（port 后端） ─────────────────────────────────────────── */

static void at_out(const char *s)
{
    at_port_write(s);
}

static void at_ok(void)
{
    at_out("OK\r\n");
}

static void at_error(const char *reason)
{
    char buf[96];
    if (reason && reason[0]) {
        snprintf(buf, sizeof(buf), "ERROR: %s\r\n", reason);
    } else {
        snprintf(buf, sizeof(buf), "ERROR\r\n");
    }
    at_out(buf);
}

static void at_data(const char *name, const char *fmt, ...)
{
    char body[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char line[160];
    snprintf(line, sizeof(line), "+%s: %s\r\n", name, body);
    at_out(line);
}

/* ── 家族刻度 framesize_t 值 → 短名（契约 v1.3 §5） ───────────── */

static const char *at_framesize_label(int v)
{
    switch (v) {
        case 10: return "VGA";
        case 11: return "SVGA";
        case 12: return "XGA";
        case 13: return "HD";
        case 14: return "SXGA";
        case 15: return "UXGA";
        default: return "UNKNOWN";
    }
}

/* ── 核心指令实现 ──────────────────────────────────────────────── */

static void cmd_gmr(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *chip_name = "ESP32";
    switch (chip.model) {
        case CHIP_ESP32:    chip_name = "ESP32";    break;
        case CHIP_ESP32S2:  chip_name = "ESP32-S2"; break;
        case CHIP_ESP32S3:  chip_name = "ESP32-S3"; break;
        case CHIP_ESP32C3:  chip_name = "ESP32-C3"; break;
        default: break;
    }
    at_gmr_info_t g;
    at_port_gmr_info(&g);

    /* 契约 v1.1：GMR 必须报 esp_app_desc 实际版本，禁止硬编码 */
    at_data("GMR", "fw:%s", (app && app->version[0]) ? app->version : "unknown");
    at_data("GMR", "chip:%s rev:%u", chip_name, (unsigned)chip.revision);
    at_data("GMR", "board:%s", g.board);
    at_data("GMR", "sensor:%s", g.sensor);
}

static void cmd_wifi_query(void)
{
    at_wifi_info_t w;
    at_port_wifi_info(&w);
    at_data("WIFI", "state:%s", w.state);
    at_data("WIFI", "ssid:%s", w.ssid[0] ? w.ssid : "(unset)");
    at_data("WIFI", "ip:%s", w.ip[0] ? w.ip : "0.0.0.0");
    /* 红线（契约 §3）：任何读指令不回显密码 */
}

static void cmd_wifi_set(const char *args)
{
    /* AT+WIFI=ssid,pass：首个逗号前为 ssid，其后整体为 pass（pass 可含逗号） */
    const char *comma = strchr(args, ',');
    if (!comma) {
        at_error("usage: AT+WIFI=ssid,pass");
        return;
    }
    char ssid[33];
    size_t ssid_len = (size_t)(comma - args);
    if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
        at_error("invalid ssid");
        return;
    }
    memcpy(ssid, args, ssid_len);
    ssid[ssid_len] = '\0';
    const char *pass = comma + 1;
    if (!pass[0]) {
        at_error("empty password");
        return;
    }
    esp_err_t ret = at_port_wifi_set(ssid, pass);
    if (ret != ESP_OK) {
        at_error("save failed");
        return;
    }
    at_ok();   /* 生效方式随板（§6）；port 需要时已打印 +REBOOTING 等行 */
}

static void cmd_wifi_scan(void)
{
    esp_err_t ret = at_port_wifi_scan(at_scan_emit_trampoline);
    if (ret == ESP_OK) {
        at_ok();
    } else {
        at_error("scan failed");
    }
}

/* port 扫描回抛 → +AP: 行（核心统一格式） */
static void at_scan_emit_trampoline(const char *ssid, int32_t rssi, int32_t auth)
{
    at_data("AP", "%s,%ld,%ld", ssid, (long)rssi, (long)auth);
}

static void cmd_ip_query(void)
{
    at_wifi_info_t w;
    at_port_wifi_info(&w);
    at_data("IP", "ip:%s", w.ip[0] ? w.ip : "0.0.0.0");
    if (w.mask[0]) at_data("IP", "mask:%s", w.mask);
    if (w.gw[0])   at_data("IP", "gw:%s", w.gw);
}

static void cmd_status(void)
{
    char buf[32];
    uint64_t us = esp_timer_get_time();
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)(us / 1000000ULL));
    at_data("STATUS", "uptime:%s", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)(esp_get_free_heap_size() / 1024));
    at_data("STATUS", "heap:%sKB", buf);

    at_wifi_info_t w;
    at_port_wifi_info(&w);
    at_data("STATUS", "wifi:%s", w.state);
    at_data("STATUS", "ssid:%s", w.ssid[0] ? w.ssid : "(unset)");
    at_data("STATUS", "ip:%s", w.ip[0] ? w.ip : "0.0.0.0");

    at_gmr_info_t g;
    at_port_gmr_info(&g);
    at_data("STATUS", "sensor:%s", g.sensor);

    at_cam_res_info_t r;
    if (at_port_cam_res_info(&r)) {
        at_data("STATUS", "res:%s", at_framesize_label(r.cur));
    }
    int qmin = 0, qmax = 0;
    int q = at_port_cam_qual_get(&qmin, &qmax);
    at_data("STATUS", "qual:%d [%d-%d]", q, qmin, qmax);

    /* 板级增量行（+TEMP: / +STREAM: 等） */
    at_port_status_extra(at_status_emit_trampoline);
}

static void at_status_emit_trampoline(const char *name, const char *value)
{
    at_data("STATUS", "%s:%s", name, value);
}

static void cmd_camres_query(void)
{
    at_cam_res_info_t r;
    if (!at_port_cam_res_info(&r)) {
        at_error("camera not ready");
        return;
    }
    at_data("CAMRES", "%d %s", r.cur, at_framesize_label(r.cur));
    /* 列表与 GET /api/camera supported_resolutions 同源 */
    char list[160] = {0};
    size_t used = 0;
    for (int i = 0; i < r.opt_count && used + 24 < sizeof(list); i++) {
        used += snprintf(list + used, sizeof(list) - used, "%s%d=%s",
                         i ? "," : "", r.opts[i].value, at_framesize_label(r.opts[i].value));
    }
    at_data("CAMLIST", "%s", list);
    at_data("CAMCAP", "%d %s (source: %s)", r.cap, at_framesize_label(r.cap), r.cap_source);
}

static void cmd_camres_set(const char *args)
{
    char *end = NULL;
    long v = strtol(args, &end, 10);
    if (end == args || *end != '\0') {
        at_error("usage: AT+CAMRES=n");
        return;
    }
    esp_err_t ret = at_port_cam_res_set((int)v);
    if (ret == ESP_ERR_INVALID_ARG) {
        at_cam_res_info_t r;
        int cap = 0;
        const char *src = "board";
        if (at_port_cam_res_info(&r)) {
            cap = r.cap;
            src = r.cap_source;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "unsupported (max %d %s, source: %s)",
                 cap, at_framesize_label(cap), src);
        at_error(msg);
        return;
    }
    if (ret != ESP_OK) {
        at_error("apply failed");
        return;
    }
    at_ok();   /* 生效语义随板（§6）；port 已打印 +APPLY/+REBOOTING 行 */
}

static void cmd_camqual_query(void)
{
    int qmin = 0, qmax = 0;
    int q = at_port_cam_qual_get(&qmin, &qmax);
    at_data("CAMQUAL", "%d [%d-%d]", q, qmin, qmax);
}

static void cmd_camqual_set(const char *args)
{
    char *end = NULL;
    long v = strtol(args, &end, 10);
    if (end == args || *end != '\0') {
        at_error("usage: AT+CAMQUAL=n");
        return;
    }
    esp_err_t ret = at_port_cam_qual_set((int)v);
    if (ret == ESP_ERR_INVALID_ARG) {
        int qmin = 0, qmax = 0;
        at_port_cam_qual_get(&qmin, &qmax);
        char msg[48];
        snprintf(msg, sizeof(msg), "out of range [%d-%d]", qmin, qmax);
        at_error(msg);
        return;
    }
    if (ret != ESP_OK) {
        at_error("apply failed");
        return;
    }
    at_ok();
}

static void cmd_cfgget(const char *args)
{
    int count = 0;
    const at_cfg_field_t *fields = at_port_cfg_fields(&count);
    for (int i = 0; i < count; i++) {
        if (strcasecmp(fields[i].name, args) == 0) {
            if (fields[i].secret) {
                at_error("write-only field");
                return;
            }
            char val[96];
            val[0] = '\0';
            if (fields[i].get) {
                fields[i].get(val, sizeof(val));
            }
            at_data("CFGGET", "%s=%s", fields[i].name, val);
            at_ok();
            return;
        }
    }
    at_error("unknown field");
}

static void cmd_cfgset(const char *args)
{
    const char *comma = strchr(args, ',');
    if (!comma) {
        at_error("usage: AT+CFGSET=field,value");
        return;
    }
    char name[48];
    size_t nlen = (size_t)(comma - args);
    if (nlen == 0 || nlen >= sizeof(name)) {
        at_error("invalid field name");
        return;
    }
    memcpy(name, args, nlen);
    name[nlen] = '\0';

    int count = 0;
    const at_cfg_field_t *fields = at_port_cfg_fields(&count);
    for (int i = 0; i < count; i++) {
        if (strcasecmp(fields[i].name, name) == 0) {
            esp_err_t ret = fields[i].set ? fields[i].set(comma + 1) : ESP_ERR_NOT_SUPPORTED;
            if (ret == ESP_ERR_INVALID_ARG) {
                at_error("invalid value");
            } else if (ret != ESP_OK) {
                at_error("set failed");
            } else {
                at_ok();
            }
            return;
        }
    }
    at_error("unknown field");
}

static void cmd_help(void);

/* ── 核心指令表 ────────────────────────────────────────────────── */

typedef enum { CAP_NONE = 0, CAP_WIFI_SCAN = 1, CAP_CFG = 2 } at_cap_bit_t;

typedef struct {
    const char *name;              /* 不含 AT+ 前缀，不含 ?/= */
    at_cap_bit_t cap;              /* 能力门控 */
    bool has_query;                /* 支持 NAME? 形式 */
    bool has_set;                  /* 支持 NAME= 形式 */
    const char *help;
    void (*query)(void);
    void (*set)(const char *args);
} at_cmd_t;

static const at_cmd_t s_core_cmds[] = {
    { "HELP",     CAP_NONE, false, false, "list commands",            cmd_help,     NULL         },
    { "GMR",      CAP_NONE, false, false, "fw / chip / board / sensor", cmd_gmr,   NULL         },
    { "WIFI",     CAP_NONE, true,  true,  "WIFI? | WIFI=ssid,pass",   cmd_wifi_query, cmd_wifi_set },
    { "WIFISCAN", CAP_WIFI_SCAN, false, false, "scan APs",            cmd_wifi_scan, NULL        },
    { "IP",       CAP_NONE, true,  false, "IP/mask/gateway",          cmd_ip_query, NULL         },
    { "STATUS",   CAP_NONE, false, false, "uptime/heap/wifi/camera",  cmd_status,   NULL         },
    { "CAMRES",   CAP_NONE, true,  true,  "CAMRES? | CAMRES=n",       cmd_camres_query, cmd_camres_set },
    { "CAMQUAL",  CAP_NONE, true,  true,  "CAMQUAL? | CAMQUAL=n",     cmd_camqual_query, cmd_camqual_set },
    { "REBOOT",   CAP_NONE, false, false, "restart device",           NULL, NULL                  },
    { "RESTORE",  CAP_NONE, false, false, "factory reset + reboot",   NULL, NULL                  },
    { "CFGGET",   CAP_CFG,  false, true,  "CFGGET=field",             NULL, NULL                  },
    { "CFGSET",   CAP_CFG,  false, true,  "CFGSET=field,value",       NULL, NULL                  },
    { "SAVE",     CAP_CFG,  false, false, "persist config",           NULL, NULL                  },
};

static void cmd_help(void)
{
    const at_caps_t *caps = at_port_caps();
    at_out("+HELP: core:\r\n");
    for (size_t i = 0; i < sizeof(s_core_cmds) / sizeof(s_core_cmds[0]); i++) {
        const at_cmd_t *c = &s_core_cmds[i];
        bool allowed = (c->cap == CAP_NONE) ||
                       (c->cap == CAP_WIFI_SCAN && caps->wifi_scan) ||
                       (c->cap == CAP_CFG && caps->cfg);
        if (!allowed) continue;
        at_data("HELP", "%-22s %s", c->name, c->help);
    }
    int ext_count = 0;
    const at_ext_cmd_t *exts = at_port_ext_cmds(&ext_count);
    if (ext_count > 0) {
        at_out("+HELP: board:\r\n");
        for (int i = 0; i < ext_count; i++) {
            at_data("HELP", "%-22s %s", exts[i].name, exts[i].help);
        }
    }
    at_ok();
}

/* ── 分发 ──────────────────────────────────────────────────────── */

static bool cap_allows(at_cap_bit_t cap)
{
    const at_caps_t *caps = at_port_caps();
    if (cap == CAP_NONE) return true;
    if (cap == CAP_WIFI_SCAN) return caps->wifi_scan;
    if (cap == CAP_CFG) return caps->cfg;
    return false;
}

static void dispatch(const char *line)
{
    /* 大小写不敏感的 AT 前缀（契约 §1） */
    if (strncasecmp(line, "AT", 2) != 0) {
        return;   /* 非 AT 行静默忽略 */
    }
    if (line[2] == '\0') {
        at_ok();
        return;
    }
    if (line[2] != '+') {
        return;   /* AT<其他> 视为噪声，忽略 */
    }

    const char *cmd = line + 3;
    if (!cmd[0]) {
        at_error("empty command");
        return;
    }

    /* 在首个 '=' 或 '?' 处切分：name / args */
    char name[32];
    char args[AT_LINE_MAX];
    bool is_query = false;
    const char *eq = strpbrk(cmd, "=?");
    if (eq) {
        is_query = (*eq == '?');
        size_t nlen = (size_t)(eq - cmd);
        if (nlen >= sizeof(name)) {
            at_error("command too long");
            return;
        }
        memcpy(name, cmd, nlen);
        name[nlen] = '\0';
        strlcpy(args, is_query ? "" : eq + 1, sizeof(args));
    } else {
        strlcpy(name, cmd, sizeof(name));
        args[0] = '\0';
    }

    /* 历史别名（契约 §4；板级映射在 port） */
    const char *canonical = at_port_alias(name);
    if (canonical) {
        strlcpy(name, canonical, sizeof(name));
    }

    for (size_t i = 0; i < sizeof(s_core_cmds) / sizeof(s_core_cmds[0]); i++) {
        const at_cmd_t *c = &s_core_cmds[i];
        if (strcasecmp(c->name, name) != 0) continue;

        if (!cap_allows(c->cap)) {
            at_error("not supported on this board");
            return;
        }
        if (is_query && !c->has_query) {
            at_error("query not supported");
            return;
        }
        if (!is_query && args[0] != '\0' && !c->has_set) {
            at_error("assignment not supported");
            return;
        }
        if (!is_query && args[0] == '\0' && c->has_set) {
            at_error("missing argument");
            return;
        }

        /* 无参动作型指令 */
        if (strcmp(c->name, "REBOOT") == 0) { at_ok(); at_port_reboot(); return; }
        if (strcmp(c->name, "RESTORE") == 0) { at_ok(); at_port_restore(); return; }
        if (strcmp(c->name, "SAVE") == 0) { at_port_save(); at_ok(); return; }
        if (strcmp(c->name, "CFGGET") == 0) { cmd_cfgget(args); return; }
        if (strcmp(c->name, "CFGSET") == 0) { cmd_cfgset(args); return; }

        if (is_query) c->query ? c->query() : (void)0;
        else if (args[0] != '\0') c->set ? c->set(args) : (void)0;
        else c->query ? c->query() : (void)0;   /* 裸名视同查询 */
        return;
    }

    /* 板级扩展（§5）：前缀匹配整串 */
    int ext_count = 0;
    const at_ext_cmd_t *exts = at_port_ext_cmds(&ext_count);
    for (int i = 0; i < ext_count; i++) {
        size_t nlen = strlen(exts[i].name);
        if (strncasecmp(cmd, exts[i].name, nlen) == 0 &&
            (cmd[nlen] == '\0' || cmd[nlen] == '=' || cmd[nlen] == '?')) {
            esp_err_t ret = exts[i].handler ? exts[i].handler(cmd) : ESP_ERR_NOT_SUPPORTED;
            if (ret != ESP_OK) {
                at_error("extension failed");
            }
            return;
        }
    }

    at_error("unknown command (AT+HELP for list)");
}

/* ── 任务 ──────────────────────────────────────────────────────── */

static void at_task(void *arg)
{
    char line[AT_LINE_MAX];

    /* 上电静默期：启动日志密集期不抢口 */
    vTaskDelay(pdMS_TO_TICKS(AT_SETTLE_MS));

    at_out("\r\n+READY: MiBee Cam AT console (AT+HELP)\r\n");

    while (1) {
        int n = at_port_getline(line, sizeof(line));
        if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (n == 0) {
            continue;
        }
        dispatch(line);
    }
}

esp_err_t at_command_init(void)
{
    esp_err_t ret = at_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "port init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (xTaskCreate(at_task, "at_cmd", AT_TASK_STACK, NULL, AT_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AT console core ready (contract v1.1)");
    return ESP_OK;
}
