/*
 * at_command.c — 家族统一 AT 指令面（seeed 板）
 *
 * 契约：docs/at-command.md v1.0（2026-09-04）。本板通道为 USB-JTAG CDC
 * （/dev/ttyACM*）——注意 sdkconfig 主控制台是 UART0，stdin 不通 CDC，
 * 故用 USB_SERIAL_JTAG 驱动直收（usb_serial_jtag_read_bytes）直发。
 * 输出与 ESP_LOG 同流（副控制台）。
 *
 * 板级语义（与 api-contract §5 一致）：
 *   分辨率变更 → 保存+重启（OV5640 运行时 set_framesize 无效，PIT-019）
 *   画质变更   → 热应用（camera_set_quality 单寄存器写）
 */

#include "at_command.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config_manager.h"
#include "camera_driver.h"
#include "wifi_manager.h"

static const char *TAG = "at_cmd";

#define AT_LINE_MAX 128

/* ── 低层 IO：USB-JTAG CDC 直收直发 ─────────────────────────────── */

static void at_out(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        usb_serial_jtag_write_bytes((const uint8_t *)buf, (size_t)n, pdMS_TO_TICKS(100));
    }
}

#define AT_OK()          at_out("OK\r\n")
#define AT_ERR(msg)      at_out("ERROR: %s\r\n", (msg))

/* ── 指令实现（家族核心） ───────────────────────────────────────── */

static void cmd_at(const char *p)      { (void)p; AT_OK(); }

static void cmd_help(const char *p)
{
    (void)p;
    at_out("\r\nMiBeeCam AT commands (family contract v1.0):\r\n"
           "  AT                        handshake\r\n"
           "  AT+HELP                   this message\r\n"
           "  AT+GMR                    firmware/board version\r\n"
           "  AT+STATUS                 consolidated status\r\n"
           "  AT+WIFI?                  WiFi state (no password)\r\n"
           "  AT+WIFI=ssid,password     set WiFi, save, reboot\r\n"
           "  AT+IP?                    IP address\r\n"
           "  AT+CAMRES?                resolution + supported range\r\n"
           "  AT+CAMRES=n               set resolution (reboot to apply)\r\n"
           "  AT+CAMQUAL?               quality + range\r\n"
           "  AT+CAMQUAL=n              set quality 10-63 (hot)\r\n"
           "  AT+REBOOT                 reboot\r\n"
           "  AT+RESTORE                factory reset + reboot\r\n"
           "OK\r\n");
}

static void cmd_gmr(const char *p)
{
    (void)p;
    const esp_app_desc_t *app = esp_app_get_description();
    at_out("MiBee Cam (seeed XIAO ESP32-S3 Sense, OV5640 detected at runtime)\r\n"
           "Firmware: %s %s\r\nESP-IDF: %s\r\n",
           app->project_name, app->version, app->idf_ver);
    AT_OK();
}

static void cmd_status(const char *p)
{
    (void)p;
    cam_config_t *cfg = config_get();
    bool conn = (wifi_get_state() == WIFI_STATE_STA_CONNECTED);
    at_out("Board:      XIAO ESP32-S3 Sense\r\n"
           "Uptime:     %lld s\r\n"
           "Free heap:  %lu bytes\r\n"
           "WiFi:       %s\r\n"
           "SSID:       %s\r\n"
           "IP:         %s\r\n"
           "Camera:     res=%s quality=%u [%d-%d]\r\n",
           (long long)(esp_timer_get_time() / 1000000),
           (unsigned long)esp_get_free_heap_size(),
           conn ? "STA connected" : "disconnected",
           cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(none)",
           conn ? wifi_get_ip_str() : "-",
           camera_res_to_str(camera_get_resolution()),
           cfg->cam_quality, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
    AT_OK();
}

static void cmd_wifi(const char *p)
{
    if (!p || p[0] == '?' || p[0] == '\0') {
        bool conn = (wifi_get_state() == WIFI_STATE_STA_CONNECTED);
        cam_config_t *cfg = config_get();
        at_out("State: %s\r\nSSID: %s\r\nIP: %s\r\n",
               conn ? "connected" : "disconnected",
               cfg->wifi_ssid[0] ? cfg->wifi_ssid : "(not set)",
               conn ? wifi_get_ip_str() : "-");
        AT_OK();
        return;
    }
    /* AT+WIFI=ssid,pass */
    char ssid[33] = {0};
    char pass[65] = {0};
    char *comma = strchr(p, ',');
    if (!comma) {
        AT_ERR("Usage: AT+WIFI=ssid,password");
        return;
    }
    size_t slen = (size_t)(comma - p);
    if (slen == 0 || slen >= sizeof(ssid)) {
        AT_ERR("bad ssid");
        return;
    }
    memcpy(ssid, p, slen);
    strlcpy(pass, comma + 1, sizeof(pass));

    config_lock();
    strlcpy(config_get()->wifi_ssid, ssid, sizeof(config_get()->wifi_ssid));
    strlcpy(config_get()->wifi_pass, pass, sizeof(config_get()->wifi_pass));
    config_save();
    config_unlock();

    at_out("OK — WiFi set: SSID='%s', rebooting...\r\n", ssid);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void cmd_ip(const char *p)
{
    (void)p;
    bool conn = (wifi_get_state() == WIFI_STATE_STA_CONNECTED);
    at_out("IP: %s\r\n", conn ? wifi_get_ip_str() : "-");
    AT_OK();
}

static void cmd_camres(const char *p)
{
    cam_config_t *cfg = config_get();
    camera_res_t eff_max = camera_get_effective_max_res();
    if (!p || p[0] == '?' || p[0] == '\0') {
        at_out("Resolution: %s  supported: 0-%d (board-tested max: %s)\r\n",
               camera_res_to_str(camera_get_resolution()), (int)eff_max,
               camera_res_to_str(eff_max));
        AT_OK();
        return;
    }
    int n = atoi(p);
    if (n < 0 || n > (int)eff_max) {
        at_out("ERROR: resolution must be 0-%d (board-tested max: %s)\r\n",
               (int)eff_max, camera_res_to_str(eff_max));
        return;
    }
    if (n == (int)cfg->cam_framesize) {
        AT_OK();
        return;
    }
    config_lock();
    config_get()->cam_framesize = (uint8_t)n;
    config_save();
    config_unlock();
    /* PIT-019：本板分辨率变更必须重启应用 */
    at_out("OK — resolution=%d saved, rebooting to apply\r\n", n);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void cmd_camqual(const char *p)
{
    cam_config_t *cfg = config_get();
    if (!p || p[0] == '?' || p[0] == '\0') {
        at_out("Quality: %u  range: [%d-%d] (hot)\r\n",
               cfg->cam_quality, CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
        AT_OK();
        return;
    }
    int n = atoi(p);
    if (n < CAMERA_QUALITY_MIN || n > CAMERA_QUALITY_MAX) {
        at_out("ERROR: quality must be %d-%d\r\n", CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
        return;
    }
    if (n == (int)cfg->cam_quality) {
        AT_OK();
        return;
    }
    config_lock();
    config_get()->cam_quality = (uint8_t)n;
    config_save();
    config_unlock();
    camera_set_quality((uint8_t)n);   /* 单寄存器写，热应用 */
    at_out("OK — quality=%d (applied)\r\n", n);
}

static void cmd_reboot(const char *p)
{
    (void)p;
    at_out("OK — rebooting...\r\n");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void cmd_restore(const char *p)
{
    (void)p;
    at_out("OK — factory reset, rebooting...\r\n");
    config_reset();
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

/* ── 分发（前缀精确匹配，'='/“?”切参数） ────────────────────────── */

typedef struct {
    const char *name;                 /* 不含 AT 前缀，如 "+WIFI" */
    void (*handler)(const char *params);
} at_entry_t;

static const at_entry_t s_cmds[] = {
    { "",        cmd_at      },
    { "+HELP",   cmd_help    },
    { "+GMR",    cmd_gmr     },
    { "+STATUS", cmd_status  },
    { "+WIFI",   cmd_wifi    },
    { "+IP",     cmd_ip      },
    { "+CAMRES", cmd_camres  },
    { "+CAMQUAL",cmd_camqual },
    { "+REBOOT", cmd_reboot  },
    { "+RESTORE",cmd_restore },
};
#define N_CMDS ((int)(sizeof(s_cmds) / sizeof(s_cmds[0])))

static void process_line(char *line)
{
    /* strip \r\n */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return;
    if (!(len >= 2 && (line[0]=='A'||line[0]=='a') && (line[1]=='T'||line[1]=='t'))) {
        return;   /* 非 AT 行（日志噪声）静默忽略 */
    }
    const char *suffix = line + 2;
    const char *params = "";
    char buf[AT_LINE_MAX];
    strlcpy(buf, suffix, sizeof(buf));
    char *eq = strchr(buf, '=');
    char *q  = strchr(buf, '?');
    if (eq) { *eq = '\0'; params = eq + 1; }
    else if (q) { *q = '\0'; params = "?"; }

    for (int i = 0; i < N_CMDS; i++) {
        if (strcmp(buf, s_cmds[i].name) == 0) {
            s_cmds[i].handler(params);
            return;
        }
    }
    AT_ERR("unknown command. Type AT+HELP");
}

/* ── 任务与初始化 ──────────────────────────────────────────────── */

static void at_task(void *arg)
{
    (void)arg;
    char line[AT_LINE_MAX];
    int pos = 0;
    uint8_t ch;

    /* 等 USB 枚举 + 系统稳定 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (1) {
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                process_line(line);
                pos = 0;
            }
        } else if (pos < AT_LINE_MAX - 1) {
            line[pos++] = (char)ch;
        }
    }
}

esp_err_t at_command_init(void)
{
    usb_serial_jtag_driver_config_t jtag_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    if (usb_serial_jtag_driver_install(&jtag_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "USB_SERIAL_JTAG driver install failed");
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(at_task, "at_cmd", 4096, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "AT task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "AT command interface ready on USB-JTAG CDC (family contract v1.0)");
    return ESP_OK;
}
