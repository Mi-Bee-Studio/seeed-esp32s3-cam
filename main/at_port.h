/*
 * at_port.h — MiBee Cam 家族 AT 核心的板级 port 层契约（v1.1，2026-09-05）
 *
 * 家族纪律（docs/at-command.md §0）：main/at_command.c 与本文件在四仓
 * md5 一致；一切板差异（IO 后端、能力裁剪、生效语义、§5 扩展指令）只允许
 * 进入各仓的 main/at_port.c 实现层。
 */
#ifndef AT_PORT_H
#define AT_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 能力裁剪（契约 §2 ⬜ 项） ──────────────────────────────────── */
typedef struct {
    bool wifi_scan;   /* AT+WIFISCAN */
    bool cfg;         /* AT+CFGGET / AT+CFGSET / AT+SAVE */
} at_caps_t;

const at_caps_t *at_port_caps(void);

/* ── IO 后端（UART0-stdin / USB-JTAG CDC，各板自选） ───────────── */
esp_err_t at_port_init(void);
/* 阻塞读一行（已去 \r\n；返回长度，<0 出错） */
int at_port_getline(char *buf, int len);
/* 写一行（不含行尾；核心已在文本中带 \r\n） */
void at_port_write(const char *s);

/* ── 板名 / 实测传感器（AT+GMR / AT+STATUS 共用） ─────────────── */
typedef struct {
    char board[24];    /* "ai-thinker" / "esp32s3-n16r8" / "luatos-a10" / "seeed-xiao-s3" */
    char sensor[16];   /* 实测型号（信设备不信文档），如 "OV2640" */
} at_gmr_info_t;

void at_port_gmr_info(at_gmr_info_t *out);

/* ── WiFi ──────────────────────────────────────────────────────── */
typedef struct {
    char state[16];    /* "ap" / "connecting" / "connected" / "disconnected" */
    char ssid[33];     /* 当前实际连接的 SSID（区别于配置值；未连接为空） */
    char ip[16];
    char gw[16];
    char mask[16];
} at_wifi_info_t;

void at_port_wifi_info(at_wifi_info_t *out);

/* 写入凭据：port 负责保存 + 生效（生效方式随板，契约 §6：
 * 默认保存+1s 重启；luatos 热连）。返回 ESP_OK 后核心回 OK；
 * port 需要时自行打印 +REBOOTING: 之类的补充行。 */
esp_err_t at_port_wifi_set(const char *ssid, const char *pass);

/* 扫描：结果逐条经 emit 上抛（顺序由 port 决定，家族约定 RSSI 降序） */
typedef void (*at_scan_emit_fn)(const char *ssid, int32_t rssi, int32_t auth);
esp_err_t at_port_wifi_scan(at_scan_emit_fn emit);

/* ── 摄像头 ────────────────────────────────────────────────────── */
typedef struct {
    int value;         /* framesize_t 家族刻度（10-15） */
    const char *label; /* "VGA (640x480)" 长标签 */
} at_res_opt_t;

typedef struct {
    int cur;                      /* 当前值 */
    const char *cur_label;        /* 当前短名（"VGA"） */
    const at_res_opt_t *opts;     /* 支持列表（与 GET /api/camera 同源） */
    int opt_count;
    int cap;                      /* 三层上限值 */
    const char *cap_source;       /* "sensor" / "board" / "memory" */
} at_cam_res_info_t;

bool at_port_cam_res_info(at_cam_res_info_t *out);
/* 设置分辨率：越界 ESP_ERR_INVALID_ARG；生效语义随板（§6），port 自行
 * 打印 +APPLY:hot / +REBOOTING: 补充行 */
esp_err_t at_port_cam_res_set(int value);

/* 画质：返回当前值；qmin/qmax 出参为板级边界（家族 10-63） */
int at_port_cam_qual_get(int *qmin, int *qmax);
/* 越界 ESP_ERR_INVALID_ARG；生效语义随板（§6） */
esp_err_t at_port_cam_qual_set(int value);

/* ── AT+STATUS 的板级增量行（+TEMP: / +STREAM: 等），核心最后调用 ── */
void at_port_status_extra(void (*emit)(const char *name, const char *value));

/* ── 系统动作 ──────────────────────────────────────────────────── */
void at_port_reboot(void);    /* 默认语义：短延时后 esp_restart() */
void at_port_restore(void);   /* 恢复出厂配置 + 重启 */

/* ── CFGGET/CFGSET 白名单字段表（契约 §2；secret 字段只写不读） ── */
typedef enum { AT_CFG_U8, AT_CFG_U16, AT_CFG_I8, AT_CFG_STR } at_cfg_type_t;

typedef struct {
    const char *name;   /* 契约 JSON 字段名 */
    at_cfg_type_t type;
    bool secret;        /* true: CFGGET 返回 ERROR: write-only */
    void (*get)(char *buf, size_t len);      /* 非 secret 必填 */
    esp_err_t (*set)(const char *val);       /* 必填；返回 ESP_OK/ESP_ERR_INVALID_ARG */
} at_cfg_field_t;

const at_cfg_field_t *at_port_cfg_fields(int *count);
void at_port_save(void);      /* 落盘（各仓写路径自带落盘时为幂等） */

/* ── 历史别名解析（契约 §4；返回核心归一名或 NULL） ─────────────── */
/* 例：luatos 把 "CWJAP?" 映射为 "WIFI?"。入参/出参均不含 "AT+" 前缀。 */
const char *at_port_alias(const char *name);

/* ── 板级扩展指令（契约 §5 登记制） ───────────────────────────── */
typedef struct {
    const char *name;   /* 不含 AT+ 前缀，如 "WIFI2"、"STREAM?" 按 port 习惯拆分 */
    const char *help;
    /* 入参为去掉 "AT+" 前缀后的完整命令串（含 ?/= 与参数）。
     * 返回 ESP_OK = 已完整应答；ESP_ERR_NOT_SUPPORTED = 交还核心报 ERROR。 */
    esp_err_t (*handler)(const char *cmd);
} at_ext_cmd_t;

const at_ext_cmd_t *at_port_ext_cmds(int *count);

#ifdef __cplusplus
}
#endif

#endif /* AT_PORT_H */
