/*
 * at_port.c — seeed XIAO ESP32-S3 Sense 板级 port 层（家族 AT 核心
 * at_command.c 的钩子，契约 docs/at-command.md v1.1）
 *
 * IO：USB-JTAG CDC（/dev/ttyACM0，open 不复位）。本板 sdkconfig 主控制台是
 * UART0、stdin 不通 CDC，故不走 VFS/stdin，用 usb_serial_jtag 驱动直收
 * 直发（IDF v6 驱动安装收配置结构体）+ 逐字节手工组行。上电 USB 枚举等待
 * 由核心任务的 3s 静默期覆盖（AT_SETTLE_MS），port 层不重复睡。
 *
 * 生效语义（契约 §6，对齐本板 HTTP 面）：
 *   WiFi      保存 + 1s 重启（凭据经 config 落盘）
 *   分辨率    保存 + 1s 重启（OV5640 运行时 set_framesize 无效，PIT-019）
 *   画质      热应用（camera_set_quality 单寄存器写）
 *   翻转/镜像/日夜 热应用（同 POST /api/config 路径）
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "driver/usb_serial_jtag.h"

#include "at_port.h"
#include "config_manager.h"
#include "camera_driver.h"
#include "wifi_manager.h"
#include "storage_manager.h"
#include "video_recorder.h"

static const char *TAG = "at_port";

/* ── 能力裁剪（契约 §2；本板：扫描 + CFG 面全开） ───────────────── */

static const at_caps_t s_caps = {
    .wifi_scan = true,
    .cfg       = true,   /* 配置契约 v1.0 迁移完成后本板具备类型化配置面 */
};

const at_caps_t *at_port_caps(void)
{
    return &s_caps;
}

/* ── IO：USB-JTAG CDC 直收直发 ─────────────────────────────────── */

#define AT_PORT_LINE_MAX 128

static char s_line[AT_PORT_LINE_MAX];
static int  s_pos = 0;

esp_err_t at_port_init(void)
{
    usb_serial_jtag_driver_config_t jtag_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };
    if (usb_serial_jtag_driver_install(&jtag_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "USB_SERIAL_JTAG driver install failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

int at_port_getline(char *buf, int len)
{
    /* 逐字节手工组行（stdin 不通 CDC，不能 fgets）。部分行状态保存在
     * 静态缓冲里跨调用保持；100ms 无数据返回 -1 让核心任务喘口气。 */
    uint8_t ch;
    while (1) {
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            return -1;
        }
        if (ch == '\r' || ch == '\n') {
            int out = (s_pos < len - 1) ? s_pos : (len - 1);
            memcpy(buf, s_line, out);
            buf[out] = '\0';
            s_pos = 0;
            return out;   /* 0 = 空行，核心自行跳过 */
        }
        if (s_pos < AT_PORT_LINE_MAX - 1) {
            s_line[s_pos++] = (char)ch;
        }
        /* 超长行：丢弃溢出字节，等 EOL 截断返回 */
    }
}

void at_port_write(const char *s)
{
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(100));
}

/* ── GMR / 传感器（信设备不信文档：实戴 OV5640 由驱动实检出） ──── */

void at_port_gmr_info(at_gmr_info_t *out)
{
    strlcpy(out->board, "seeed-xiao-s3", sizeof(out->board));
    switch (camera_get_sensor()) {
        case CAMERA_SENSOR_OV2640: strlcpy(out->sensor, "OV2640", sizeof(out->sensor)); break;
        case CAMERA_SENSOR_OV3660: strlcpy(out->sensor, "OV3660", sizeof(out->sensor)); break;
        case CAMERA_SENSOR_OV5640: strlcpy(out->sensor, "OV5640", sizeof(out->sensor)); break;
        default:                   strlcpy(out->sensor, "unknown", sizeof(out->sensor)); break;
    }
}

/* ── WiFi ──────────────────────────────────────────────────────── */

void at_port_wifi_info(at_wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (wifi_get_state()) {
        case WIFI_STATE_AP:              strlcpy(out->state, "ap", sizeof(out->state)); break;
        case WIFI_STATE_STA_CONNECTING:  strlcpy(out->state, "connecting", sizeof(out->state)); break;
        case WIFI_STATE_STA_CONNECTED:   strlcpy(out->state, "connected", sizeof(out->state)); break;
        default:                         strlcpy(out->state, "disconnected", sizeof(out->state)); break;
    }
    /* 当前实际连接的 SSID（区别于配置值；AP/未连接为空） */
    strlcpy(out->ssid, wifi_get_current_ssid(), sizeof(out->ssid));
    strlcpy(out->ip, wifi_get_ip_str(), sizeof(out->ip));

    esp_netif_t *netif = wifi_get_sta_netif();
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(out->gw, sizeof(out->gw), IPSTR, IP2STR(&ip.gw));
            snprintf(out->mask, sizeof(out->mask), IPSTR, IP2STR(&ip.netmask));
        }
    }
}

esp_err_t at_port_wifi_set(const char *ssid, const char *pass)
{
    /* 本板配置面无独立 wifi setter：锁 + 直写 + 落盘（与 POST /api/config
     * 同一路径），随后保存+1s 重启生效（契约 §6 默认语义） */
    config_lock();
    strlcpy(config_get()->wifi_ssid, ssid, sizeof(config_get()->wifi_ssid));
    strlcpy(config_get()->wifi_pass, pass, sizeof(config_get()->wifi_pass));
    esp_err_t ret = config_save();
    config_unlock();
    if (ret != ESP_OK) {
        return ret;
    }
    at_port_write("+REBOOTING: wifi credentials saved\r\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

esp_err_t at_port_wifi_scan(at_scan_emit_fn emit)
{
    /* 与 GET /api/scan 同源：wifi_manager 扫描（内部已处理 STA 流量中断） */
    wifi_ap_info_t aps[20];
    int count = wifi_scan(aps, 20);
    if (count < 0) {
        return ESP_FAIL;
    }
    /* RSSI 降序（契约 §2 家族约定） */
    for (int i = 1; i < count; i++) {
        wifi_ap_info_t key = aps[i];
        int j = i - 1;
        while (j >= 0 && aps[j].rssi < key.rssi) {
            aps[j + 1] = aps[j];
            j--;
        }
        aps[j + 1] = key;
    }
    for (int i = 0; i < count; i++) {
        emit(aps[i].ssid, aps[i].rssi, aps[i].auth_mode);
    }
    return ESP_OK;
}

/* ── 摄像头 ────────────────────────────────────────────────────── */

/* 家族刻度可选表（与 GET /api/camera supported_resolutions 同源：
 * 值 = framesize_t 10-15；三层上限过滤在 at_port_cam_res_info 内做） */
static const at_res_opt_t k_res_table[] = {
    { 10, "VGA (640x480)"   },
    { 11, "SVGA (800x600)"  },
    { 12, "XGA (1024x768)"  },
    { 13, "HD (1280x720)"   },
    { 14, "SXGA (1280x1024)"},
    { 15, "UXGA (1600x1200)"},
};

bool at_port_cam_res_info(at_cam_res_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cur = config_get()->cam_framesize;
    out->cur_label = camera_res_to_str((camera_res_t)out->cur);

    /* 静态表 → 家族 AT 表类型桥接（按三层上限过滤，同 /api/camera） */
    static at_res_opt_t s_opts[sizeof(k_res_table) / sizeof(k_res_table[0])];
    camera_res_t eff_max = camera_get_effective_max_res();
    int n = 0;
    for (size_t i = 0; i < sizeof(k_res_table) / sizeof(k_res_table[0]); i++) {
        if (k_res_table[i].value > (int)eff_max) {
            break;
        }
        s_opts[n].value = k_res_table[i].value;
        s_opts[n].label = k_res_table[i].label;
        n++;
    }
    out->opts = s_opts;
    out->opt_count = n;

    out->cap = (int)camera_get_effective_max_res();
    out->cap_source = camera_res_cap_source();
    return true;
}

esp_err_t at_port_cam_res_set(int value)
{
    /* 合法性 = 在板级可选表内 且 ≤ 三层上限（与 POST /api/camera 同源） */
    at_cam_res_info_t r;
    if (!at_port_cam_res_info(&r)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool ok = value <= r.cap;
    if (ok) {
        ok = false;
        for (int i = 0; i < r.opt_count; i++) {
            if (r.opts[i].value == value) { ok = true; break; }
        }
    }
    if (!ok) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value == (int)config_get()->cam_framesize) {
        return ESP_OK;   /* 无变化：免重启 */
    }
    config_lock();
    config_get()->cam_framesize = (uint8_t)value;
    esp_err_t ret = config_save();
    config_unlock();
    if (ret != ESP_OK) {
        return ret;
    }
    /* 本板生效语义：保存+重启（OV5640 运行时 set_framesize 实测无效，
     * PIT-019；应答 OK 后 1s 重启，同 HTTP 面行为） */
    at_port_write("+REBOOTING: OV5640 framesize needs reboot\r\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

int at_port_cam_qual_get(int *qmin, int *qmax)
{
    if (qmin) *qmin = CAMERA_QUALITY_MIN;
    if (qmax) *qmax = CAMERA_QUALITY_MAX;
    return config_get()->cam_quality;
}

esp_err_t at_port_cam_qual_set(int value)
{
    if (value < CAMERA_QUALITY_MIN || value > CAMERA_QUALITY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    config_lock();
    config_get()->cam_quality = (uint8_t)value;
    esp_err_t ret = config_save();
    config_unlock();
    if (ret != ESP_OK) {
        return ret;
    }
    /* 本板生效语义：热应用（OV5640 set_quality 单寄存器写，实测安全） */
    ret = camera_set_quality((uint8_t)value);
    if (ret != ESP_OK) {
        return ret;   /* 已保存；相机未就绪时下次启动生效 */
    }
    at_port_write("+APPLY: hot\r\n");
    return ESP_OK;
}

/* ── STATUS 板级增量行 ─────────────────────────────────────────── */

void at_port_status_extra(void (*emit)(const char *name, const char *value))
{
    /* 录像状态（与 GET /api/record 同源映射） */
    const char *rec;
    switch (recorder_get_state()) {
        case RECORDER_RECORDING: rec = "recording"; break;
        case RECORDER_PAUSED:    rec = "paused";    break;
        case RECORDER_ERROR:     rec = "error";     break;
        default:                 rec = "stopped";   break;
    }
    emit("recording", rec);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", storage_is_available() ? 1 : 0);
    emit("sd", buf);
}

/* ── 系统动作 ──────────────────────────────────────────────────── */

void at_port_reboot(void)
{
    at_port_write("+REBOOTING\r\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void at_port_restore(void)
{
    at_port_write("+RESTORING: factory reset\r\n");
    config_reset();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── CFGGET/CFGSET 白名单（契约 §2；secret 字段只写不读） ──────── */

/* 泛型读取：按 offset/type 从快照取值（单一事实源 = struct 布局；
 * bool 字段按 1 字节 0/1 读，与 U8 同构） */
typedef struct {
    const char *name;
    at_cfg_type_t type;
    bool secret;
    size_t off;
} port_field_t;

static const port_field_t s_fields[] = {
    { "device_name",              AT_CFG_STR, false, offsetof(cam_config_t, device_name) },
    { "wifi_ssid",                AT_CFG_STR, false, offsetof(cam_config_t, wifi_ssid) },
    { "wifi_pass",                AT_CFG_STR, true,  offsetof(cam_config_t, wifi_pass) },
    { "wifi_ssid_2",              AT_CFG_STR, false, offsetof(cam_config_t, wifi_ssid_2) },
    { "wifi_pass_2",              AT_CFG_STR, true,  offsetof(cam_config_t, wifi_pass_2) },
    { "web_password",             AT_CFG_STR, true,  offsetof(cam_config_t, web_password) },
    { "timezone",                 AT_CFG_STR, false, offsetof(cam_config_t, timezone) },
    { "cam_framesize",            AT_CFG_U8,  false, offsetof(cam_config_t, cam_framesize) },
    { "cam_fps",                  AT_CFG_U8,  false, offsetof(cam_config_t, cam_fps) },
    { "cam_quality",              AT_CFG_U8,  false, offsetof(cam_config_t, cam_quality) },
    { "cam_vflip",                AT_CFG_U8,  false, offsetof(cam_config_t, cam_vflip) },
    { "cam_hmirror",              AT_CFG_U8,  false, offsetof(cam_config_t, cam_hmirror) },
    { "xclk_freq_mhz",            AT_CFG_U8,  false, offsetof(cam_config_t, xclk_freq_mhz) },
    { "onvif_enable",             AT_CFG_U8,  false, offsetof(cam_config_t, onvif_enable) },
    { "day_night_mode",           AT_CFG_U8,  false, offsetof(cam_config_t, day_night_mode) },
    { "rtsp_user",                AT_CFG_STR, false, offsetof(cam_config_t, rtsp_user) },
    { "rtsp_pass",                AT_CFG_STR, true,  offsetof(cam_config_t, rtsp_pass) },
    { "motion_enabled",           AT_CFG_U8,  false, offsetof(cam_config_t, motion_enabled) },
    { "motion_sensitivity",       AT_CFG_U8,  false, offsetof(cam_config_t, motion_sensitivity) },
    { "motion_cooldown_s",        AT_CFG_U16, false, offsetof(cam_config_t, motion_cooldown_s) },
    { "motion_active_interval_s", AT_CFG_U8,  false, offsetof(cam_config_t, motion_active_interval_s) },
    { "segment_sec",              AT_CFG_U16, false, offsetof(cam_config_t, segment_sec) },
    { "record_on_boot",           AT_CFG_U8,  false, offsetof(cam_config_t, record_on_boot) },
    { "video_record_to_sd",       AT_CFG_U8,  false, offsetof(cam_config_t, video_record_to_sd) },
    { "audio_record_to_sd",       AT_CFG_U8,  false, offsetof(cam_config_t, audio_record_to_sd) },
    { "timelapse_enabled",        AT_CFG_U8,  false, offsetof(cam_config_t, timelapse_enabled) },
    { "timelapse_mode",           AT_CFG_U8,  false, offsetof(cam_config_t, timelapse_mode) },
    { "sd_log_enabled",           AT_CFG_U8,  false, offsetof(cam_config_t, sd_log_enabled) },
    { "cleanup_low_pct",          AT_CFG_U8,  false, offsetof(cam_config_t, cleanup_low_pct) },
    { "cleanup_high_pct",         AT_CFG_U8,  false, offsetof(cam_config_t, cleanup_high_pct) },
};

enum {
    F_DEVICE_NAME = 0, F_WIFI_SSID, F_WIFI_PASS, F_WIFI_SSID_2, F_WIFI_PASS_2,
    F_WEB_PASSWORD, F_TIMEZONE, F_CAM_FRAMESIZE, F_CAM_FPS, F_CAM_QUALITY,
    F_CAM_VFLIP, F_CAM_HMIRROR, F_XCLK, F_ONVIF, F_DAY_NIGHT, F_RTSP_USER,
    F_RTSP_PASS, F_MOTION_EN, F_MOTION_SENS, F_MOTION_COOL, F_MOTION_ACT,
    F_SEGMENT, F_RECORD_ON_BOOT, F_VIDEO_TO_SD, F_AUDIO_TO_SD, F_TL_EN,
    F_TL_MODE, F_SD_LOG, F_CLEANUP_LOW, F_CLEANUP_HIGH,
};

static void field_get_generic(const port_field_t *f, char *buf, size_t len)
{
    cam_config_t snap;
    config_get_copy(&snap);
    const void *p = (const uint8_t *)&snap + f->off;
    switch (f->type) {
        case AT_CFG_STR: snprintf(buf, len, "%s", (const char *)p); break;
        case AT_CFG_U8:  snprintf(buf, len, "%u", (unsigned)*(const uint8_t *)p); break;
        case AT_CFG_U16: snprintf(buf, len, "%u", (unsigned)*(const uint16_t *)p); break;
        case AT_CFG_I8:  snprintf(buf, len, "%d", (int)*(const int8_t *)p); break;
    }
}

#define CFG_GET_FN(fname, idx) \
    static void fname(char *buf, size_t len) { field_get_generic(&s_fields[idx], buf, len); }

CFG_GET_FN(cfg_get_device_name,   F_DEVICE_NAME)
CFG_GET_FN(cfg_get_wifi_ssid,     F_WIFI_SSID)
CFG_GET_FN(cfg_get_wifi_ssid_2,   F_WIFI_SSID_2)
CFG_GET_FN(cfg_get_timezone,      F_TIMEZONE)
CFG_GET_FN(cfg_get_cam_framesize, F_CAM_FRAMESIZE)
CFG_GET_FN(cfg_get_cam_fps,       F_CAM_FPS)
CFG_GET_FN(cfg_get_cam_quality,   F_CAM_QUALITY)
CFG_GET_FN(cfg_get_cam_vflip,     F_CAM_VFLIP)
CFG_GET_FN(cfg_get_cam_hmirror,   F_CAM_HMIRROR)
CFG_GET_FN(cfg_get_xclk,          F_XCLK)
CFG_GET_FN(cfg_get_onvif,         F_ONVIF)
CFG_GET_FN(cfg_get_day_night,     F_DAY_NIGHT)
CFG_GET_FN(cfg_get_rtsp_user,     F_RTSP_USER)
CFG_GET_FN(cfg_get_motion_en,     F_MOTION_EN)
CFG_GET_FN(cfg_get_motion_sens,   F_MOTION_SENS)
CFG_GET_FN(cfg_get_motion_cool,   F_MOTION_COOL)
CFG_GET_FN(cfg_get_motion_act,    F_MOTION_ACT)
CFG_GET_FN(cfg_get_segment,       F_SEGMENT)
CFG_GET_FN(cfg_get_record_on_boot, F_RECORD_ON_BOOT)
CFG_GET_FN(cfg_get_video_to_sd,   F_VIDEO_TO_SD)
CFG_GET_FN(cfg_get_audio_to_sd,   F_AUDIO_TO_SD)
CFG_GET_FN(cfg_get_tl_en,         F_TL_EN)
CFG_GET_FN(cfg_get_tl_mode,       F_TL_MODE)
CFG_GET_FN(cfg_get_sd_log,        F_SD_LOG)
CFG_GET_FN(cfg_get_cleanup_low,   F_CLEANUP_LOW)
CFG_GET_FN(cfg_get_cleanup_high,  F_CLEANUP_HIGH)

/* ── 写侧：锁 + 直写 + config_save（整表校验+落盘，与 POST /api/config
 *    同路径）。本板无组 setter，"快照代入组参数"的家族模式在此等价为：
 *    单字段校验先行，config_save 内 config_validate 兜底整表域。 ── */

static bool parse_num(const char *v, long *out)
{
    char *end = NULL;
    *out = strtol(v, &end, 10);
    return (end != v && *end == '\0');
}

/* 契约 §4：字符串先验长度，越界拒绝（不静默截断凭据） */
static esp_err_t set_str_field(char *dst, size_t dstsz, const char *v, bool allow_empty)
{
    if (!v[0] && !allow_empty) return ESP_ERR_INVALID_ARG;
    if (strlen(v) >= dstsz) return ESP_ERR_INVALID_ARG;
    config_lock();
    strlcpy(dst, v, dstsz);
    esp_err_t ret = config_save();
    config_unlock();
    return ret;
}

static esp_err_t set_bool_field(bool *dst, const char *v)
{
    long val;
    if (!parse_num(v, &val) || (val != 0 && val != 1)) return ESP_ERR_INVALID_ARG;
    config_lock();
    *dst = val != 0;
    esp_err_t ret = config_save();
    config_unlock();
    return ret;
}

static esp_err_t set_u8_field(uint8_t *dst, const char *v, long lo, long hi)
{
    long val;
    if (!parse_num(v, &val) || val < lo || val > hi) return ESP_ERR_INVALID_ARG;
    config_lock();
    *dst = (uint8_t)val;
    esp_err_t ret = config_save();
    config_unlock();
    return ret;
}

static esp_err_t set_u16_field(uint16_t *dst, const char *v, long lo, long hi)
{
    long val;
    if (!parse_num(v, &val) || val < lo || val > hi) return ESP_ERR_INVALID_ARG;
    config_lock();
    *dst = (uint16_t)val;
    esp_err_t ret = config_save();
    config_unlock();
    return ret;
}

static esp_err_t cfg_set_device_name(const char *v)
{
    return set_str_field(config_get()->device_name, sizeof(config_get()->device_name), v, false);
}
static esp_err_t cfg_set_wifi_ssid(const char *v)
{
    /* 只换 ssid 不动 pass（凭据对完整写入走 AT+WIFI=，含重启生效） */
    return set_str_field(config_get()->wifi_ssid, sizeof(config_get()->wifi_ssid), v, true);
}
static esp_err_t cfg_set_wifi_pass(const char *v)
{
    return set_str_field(config_get()->wifi_pass, sizeof(config_get()->wifi_pass), v, true);
}
static esp_err_t cfg_set_wifi_ssid_2(const char *v)
{
    return set_str_field(config_get()->wifi_ssid_2, sizeof(config_get()->wifi_ssid_2), v, true);
}
static esp_err_t cfg_set_wifi_pass_2(const char *v)
{
    return set_str_field(config_get()->wifi_pass_2, sizeof(config_get()->wifi_pass_2), v, true);
}
static esp_err_t cfg_set_web_password(const char *v)
{
    /* 契约 v1.1：≥6 位（与 POST /api/config 同源） */
    if (strlen(v) < 6) return ESP_ERR_INVALID_ARG;
    return set_str_field(config_get()->web_password, sizeof(config_get()->web_password), v, false);
}
static esp_err_t cfg_set_timezone(const char *v)
{
    return set_str_field(config_get()->timezone, sizeof(config_get()->timezone), v, false);
}
static esp_err_t cfg_set_rtsp_user(const char *v)
{
    return set_str_field(config_get()->rtsp_user, sizeof(config_get()->rtsp_user), v, false);
}
static esp_err_t cfg_set_rtsp_pass(const char *v)
{
    return set_str_field(config_get()->rtsp_pass, sizeof(config_get()->rtsp_pass), v, true);
}

static esp_err_t cfg_set_cam_framesize(const char *v)
{
    long val;
    if (!parse_num(v, &val)) return ESP_ERR_INVALID_ARG;
    return at_port_cam_res_set((int)val) == ESP_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}
static esp_err_t cfg_set_cam_fps(const char *v)
{
    return set_u8_field(&config_get()->cam_fps, v, 1, 30);
}
static esp_err_t cfg_set_cam_quality(const char *v)
{
    esp_err_t ret = set_u8_field(&config_get()->cam_quality, v,
                                 CAMERA_QUALITY_MIN, CAMERA_QUALITY_MAX);
    if (ret != ESP_OK) {
        return ret;
    }
    /* 保存成功即 OK；热应用 best-effort（相机未就绪则下次启动生效） */
    camera_set_quality(config_get()->cam_quality);
    return ESP_OK;
}
static esp_err_t cfg_set_cam_vflip(const char *v)
{
    esp_err_t ret = set_bool_field(&config_get()->cam_vflip, v);
    if (ret == ESP_OK) {
        camera_set_flip(config_get()->cam_vflip, config_get()->cam_hmirror);
    }
    return ret;
}
static esp_err_t cfg_set_cam_hmirror(const char *v)
{
    esp_err_t ret = set_bool_field(&config_get()->cam_hmirror, v);
    if (ret == ESP_OK) {
        camera_set_flip(config_get()->cam_vflip, config_get()->cam_hmirror);
    }
    return ret;
}
static esp_err_t cfg_set_xclk(const char *v)
{
    long val;
    if (!parse_num(v, &val) || (val != 10 && val != 16 && val != 20)) {
        return ESP_ERR_INVALID_ARG;
    }
    return set_u8_field(&config_get()->xclk_freq_mhz, v, 10, 20);
}
static esp_err_t cfg_set_onvif(const char *v)
{
    /* 变更重启后生效（SOAP 注册/WS-Discovery 为启动期动作） */
    return set_u8_field(&config_get()->onvif_enable, v, 0, 1);
}
static esp_err_t cfg_set_day_night(const char *v)
{
    esp_err_t ret = set_u8_field(&config_get()->day_night_mode, v, 0, 2);
    if (ret == ESP_OK) {
        camera_set_day_night(config_get()->day_night_mode);
    }
    return ret;
}
static esp_err_t cfg_set_motion_en(const char *v)
{
    return set_bool_field(&config_get()->motion_enabled, v);
}
static esp_err_t cfg_set_motion_sens(const char *v)
{
    return set_u8_field(&config_get()->motion_sensitivity, v, 0, 100);
}
static esp_err_t cfg_set_motion_cool(const char *v)
{
    return set_u16_field(&config_get()->motion_cooldown_s, v, 1, 300);
}
static esp_err_t cfg_set_motion_act(const char *v)
{
    return set_u8_field(&config_get()->motion_active_interval_s, v, 1, 30);
}
static esp_err_t cfg_set_segment(const char *v)
{
    return set_u16_field(&config_get()->segment_sec, v, 5, 3600);
}
static esp_err_t cfg_set_record_on_boot(const char *v)
{
    return set_bool_field(&config_get()->record_on_boot, v);
}
static esp_err_t cfg_set_video_to_sd(const char *v)
{
    return set_bool_field(&config_get()->video_record_to_sd, v);
}
static esp_err_t cfg_set_audio_to_sd(const char *v)
{
    return set_bool_field(&config_get()->audio_record_to_sd, v);
}
static esp_err_t cfg_set_tl_en(const char *v)
{
    return set_bool_field(&config_get()->timelapse_enabled, v);
}
static esp_err_t cfg_set_tl_mode(const char *v)
{
    return set_u8_field(&config_get()->timelapse_mode, v, 0, 1);
}
static esp_err_t cfg_set_sd_log(const char *v)
{
    return set_bool_field(&config_get()->sd_log_enabled, v);
}
static esp_err_t cfg_set_cleanup_low(const char *v)
{
    return set_u8_field(&config_get()->cleanup_low_pct, v, 1, 99);
}
static esp_err_t cfg_set_cleanup_high(const char *v)
{
    long val;
    if (!parse_num(v, &val) || val < 1 || val > 80) return ESP_ERR_INVALID_ARG;
    /* 契约 §4 组合校验：high ≥ low+5（config_validate 兜底，这里先验给出
     * 精确的 invalid value 而非 set failed） */
    if (val < (long)config_get()->cleanup_low_pct + 5) return ESP_ERR_INVALID_ARG;
    return set_u8_field(&config_get()->cleanup_high_pct, v, 1, 80);
}

/* 白名单表（get 仅非 secret 字段；set 含 secret 写入） */
static const at_cfg_field_t s_cfg_fields[] = {
    { "device_name",              AT_CFG_STR, false, cfg_get_device_name,    cfg_set_device_name },
    { "wifi_ssid",                AT_CFG_STR, false, cfg_get_wifi_ssid,      cfg_set_wifi_ssid },
    { "wifi_pass",                AT_CFG_STR, true,  NULL,                   cfg_set_wifi_pass },
    { "wifi_ssid_2",              AT_CFG_STR, false, cfg_get_wifi_ssid_2,    cfg_set_wifi_ssid_2 },
    { "wifi_pass_2",              AT_CFG_STR, true,  NULL,                   cfg_set_wifi_pass_2 },
    { "web_password",             AT_CFG_STR, true,  NULL,                   cfg_set_web_password },
    { "timezone",                 AT_CFG_STR, false, cfg_get_timezone,       cfg_set_timezone },
    { "cam_framesize",            AT_CFG_U8,  false, cfg_get_cam_framesize,  cfg_set_cam_framesize },
    { "cam_fps",                  AT_CFG_U8,  false, cfg_get_cam_fps,        cfg_set_cam_fps },
    { "cam_quality",              AT_CFG_U8,  false, cfg_get_cam_quality,    cfg_set_cam_quality },
    { "cam_vflip",                AT_CFG_U8,  false, cfg_get_cam_vflip,      cfg_set_cam_vflip },
    { "cam_hmirror",              AT_CFG_U8,  false, cfg_get_cam_hmirror,    cfg_set_cam_hmirror },
    { "xclk_freq_mhz",            AT_CFG_U8,  false, cfg_get_xclk,           cfg_set_xclk },
    { "onvif_enable",             AT_CFG_U8,  false, cfg_get_onvif,          cfg_set_onvif },
    { "day_night_mode",           AT_CFG_U8,  false, cfg_get_day_night,      cfg_set_day_night },
    { "rtsp_user",                AT_CFG_STR, false, cfg_get_rtsp_user,      cfg_set_rtsp_user },
    { "rtsp_pass",                AT_CFG_STR, true,  NULL,                   cfg_set_rtsp_pass },
    { "motion_enabled",           AT_CFG_U8,  false, cfg_get_motion_en,      cfg_set_motion_en },
    { "motion_sensitivity",       AT_CFG_U8,  false, cfg_get_motion_sens,    cfg_set_motion_sens },
    { "motion_cooldown_s",        AT_CFG_U16, false, cfg_get_motion_cool,    cfg_set_motion_cool },
    { "motion_active_interval_s", AT_CFG_U8,  false, cfg_get_motion_act,     cfg_set_motion_act },
    { "segment_sec",              AT_CFG_U16, false, cfg_get_segment,        cfg_set_segment },
    { "record_on_boot",           AT_CFG_U8,  false, cfg_get_record_on_boot, cfg_set_record_on_boot },
    { "video_record_to_sd",       AT_CFG_U8,  false, cfg_get_video_to_sd,    cfg_set_video_to_sd },
    { "audio_record_to_sd",       AT_CFG_U8,  false, cfg_get_audio_to_sd,    cfg_set_audio_to_sd },
    { "timelapse_enabled",        AT_CFG_U8,  false, cfg_get_tl_en,          cfg_set_tl_en },
    { "timelapse_mode",           AT_CFG_U8,  false, cfg_get_tl_mode,        cfg_set_tl_mode },
    { "sd_log_enabled",           AT_CFG_U8,  false, cfg_get_sd_log,         cfg_set_sd_log },
    { "cleanup_low_pct",          AT_CFG_U8,  false, cfg_get_cleanup_low,    cfg_set_cleanup_low },
    { "cleanup_high_pct",         AT_CFG_U8,  false, cfg_get_cleanup_high,   cfg_set_cleanup_high },
};

const at_cfg_field_t *at_port_cfg_fields(int *count)
{
    if (count) {
        *count = (int)(sizeof(s_cfg_fields) / sizeof(s_cfg_fields[0]));
    }
    return s_cfg_fields;
}

void at_port_save(void)
{
    /* 本仓所有写路径自带落盘（config_save）→ 幂等空操作（契约 §2 AT+SAVE） */
}

/* ── 历史别名（契约 §4；seeed 无历史别名） ─────────────────────── */

const char *at_port_alias(const char *name)
{
    (void)name;
    return NULL;
}

/* ── 板级扩展指令（契约 §5 登记制；seeed 无扩展） ──────────────── */

const at_ext_cmd_t *at_port_ext_cmds(int *count)
{
    if (count) *count = 0;
    return NULL;
}
