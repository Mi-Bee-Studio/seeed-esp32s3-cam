# onvif-c

[![CI](https://github.com/mickeyzzc/onvif-c/actions/workflows/ci.yml/badge.svg)](https://github.com/mickeyzzc/onvif-c/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Coverage](https://img.shields.io/badge/line%20coverage-95%25-brightgreen.svg)](tests/coverage.sh)

[English](README.md) | 中文

**纯 C 的 ESP-IDF ONVIF 设备端（服务端）库** —— 用 SOAP + WS-Discovery +
Pull-Point 事件把相机交给 NVR，零第三方依赖，代码占用 ~10 KB。抽取自
[MiBee Cam 固件](https://github.com/Mi-Bee-Studio)（ESP32 家族、4 个板型）
的生产实现，其响应 XML 与 **MiBee NVR 字节稳定**，并经海康类客户端实测。

## 特性

- **SOAP 设备/媒体服务** —— NVR 发现并添加相机所需的精确动作集：
  `GetSystemDateAndTime`、`GetDeviceInformation`、`GetCapabilities`、
  `GetProfiles`、`GetStreamUri`、`GetSnapshotUri`。
- **Pull-Point 事件服务** —— `tns1:VideoSource/MotionAlarm` 主题
  （`Source=CSI`、`State`、`Score 0-100`）；单订阅（新顶旧）、授予 1h
  TerminationTime、120s 空闲过期、**无长轮询**（PullMessages 立即返回，
  esp_http_server worker 永不阻塞）。
- **WS-Discovery 应答器** —— UDP 3702 / 组播 239.255.255.250；对 Probe 单播
  回 ProbeMatches，每 ~30s 周期性 Hello 广播；socket/bind/组播加入失败自动
  重试直至 WiFi 就绪。
- **可选 mDNS** —— `_onvif._tcp` 广告；构建里没有 `espressif/mdns` 时干净
  编译出局。
- **无 XML 解析器、无动态状态** —— 动作识别 `strstr()`、响应生成
  `snprintf()`；仅每请求缓冲；运动事件生产者钩子非阻塞，传感器回调语境安全。
- **单一配置接缝** —— 板级差异（身份、IP、流地址、运行时开关、HTTP 端口）
  全部收敛到 `onvif_c_config_t` 回调，零硬编码。

## 用法

以 vendored 形式放入（如 `components/onvif-c`），在 main 的 `REQUIRES` 加
`onvif-c`：

```c
#include "onvif_c.h"

static const char *my_stream_uri(void) {
    return "rtsp://192.0.2.134:554/stream";   /* 或 http://ip:81/stream */
}

void app_onvif_start(httpd_handle_t httpd) {
    onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "ESP32-S3-N16R8",
        .firmware_version = "v0.1.0",
        .serial           = my_serial,        /* 稳定十六进制序列号          */
        .uuid             = my_uuid,          /* 不带 urn:uuid: 前缀         */
        .ip               = my_ip,            /* NULL/"0.0.0.0" = 未就绪     */
        .stream_uri       = my_stream_uri,
        .frame_rate       = my_fps,           /* NULL -> 15                  */
        .events_enabled   = my_events_gate,   /* NULL = 不提供事件服务       */
        .http_port        = 80,               /* 0 -> 80；进所有广告 URI     */
        .mdns_hostname    = "mibeecam-a1b2",  /* NULL = 跳过 mDNS            */
    };
    onvif_c_start(httpd, &cfg);
}

/* 由运动检测器调用（如 WiFi-CSI 回调）—— 永不阻塞： */
onvif_c_motion(true, 87);   /* MotionAlarm State=true  Score=87 */
onvif_c_motion(false, 4);
```

`tools/onvif_probe.py <ip>` 是免硬件冒烟测试：跑遍所有已服务动作与完整
Pull-Point 订阅周期，全过退出 0。

## API 参考

完整契约内联在 [`include/onvif_c.h`](include/onvif_c.h)，速查：

| 函数 | 契约 |
| --- | --- |
| `onvif_c_start(httpd, cfg)` | 注册 `/onvif/device_service` + `/onvif/media_service`（带 `events_enabled` 时加 `/onvif/events_service`），启动 WS-Discovery（+ 可选 mDNS）。缺必填回调返回 `ESP_ERR_INVALID_ARG`，否则返回首个注册错误；重启后的重复注册被容忍（`ESP_ERR_HTTPD_HANDLER_EXISTS` 记 WARN 忽略）。 |
| `onvif_c_stop()` | 停发现任务、摘 mDNS 服务。SOAP handler 留在 httpd（esp_http_server 无注销 API）。 |
| `onvif_c_motion(active, score)` | 喂入运动状态迁移。永不阻塞（锁竞争即丢弃）、无 I/O，传感器/CSI 回调语境安全。仅订阅存活且 `events_enabled()` 为真时入队。 |
| `onvif_c_events_subscribed()` | 订阅存活期间为真（诊断面）。 |
| `onvif_c_version()` | 返回 `ONVIF_C_VERSION`（`主*10000+次*100+修订`，v0.1.0 → 100）。 |

`onvif_c_config_t` 字段（字符串按引用持有，须比服务活得久）：

| 字段 | 必填 | 默认 | 说明 |
| --- | --- | --- | --- |
| `serial`、`uuid`、`ip`、`stream_uri` | **是**（启动校验） | — | `ip` 连网中可答 `NULL`/`"0.0.0.0"`，发现任务会等；`uuid` 不带 `urn:uuid:` 前缀。 |
| `manufacturer`、`model`、`hardware_id`、`firmware_version` | 否 | `"MiBee"`、`"MiBeeCam"`、`"ESP32"`、`"v0.1.0"` | GetDeviceInformation 身份串。 |
| `frame_rate` | 否 | 15 | GetProfiles `FrameRateLimit`。 |
| `snapshot_uri` | 否 | 派生 `http://<ip>:<http_port>/api/capture` | GetSnapshotUri 应答。 |
| `events_enabled` | 否 | NULL = 无此能力 | 运行时门；NULL 时事件服务不注册也不广播。 |
| `http_port` | 否 | 80 | 流入**所有**广告 URI。 |
| `wdt_watch_discovery` | 否 | false | 把 WS-Discovery 任务挂上 ESP-IDF 任务看门狗（`CONFIG_ESP_TASK_WDT`）；任务卡死即停喂、TWDT 触发。 |
| `mdns_hostname`、`mdns_instance` | 否 | NULL = 跳过 mDNS | instance 默认取 `model`。 |
| `scopes` | 否 | 由 `model` 生成 | WS-Discovery Scopes 正文；启动时一次性解析。 |

## 集成指南

1. 把本树 vendored 到 `components/onvif-c`（`tests/`、`examples/`、`docs/`、
   `.github/` 可删），main 的 `REQUIRES` 加 `onvif-c`。`espressif/mdns`
   可选：声明即启用 mDNS。
2. 写你的板级适配层——所有板级事实只住这里。完整真实范例见 MiBee Cam 固件
   的 [`main/onvif_port.c`](https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam/blob/main/main/onvif_port.c)
   （约 100 行：身份取 MAC/efuse、IP 取 wifi_manager、流地址取 RTSP 服务、
   事件门接配置）。
3. httpd 起来后调一次 `onvif_port_start()`；检测器里喂
   `onvif_c_motion()`。
4. 用 `tools/onvif_probe.py <ip>` 验收（退出 0 = 全表面 OK）。

四个 MiBee Cam 仓（ESP32 + ESP32-S3，IDF v5.5/v6.0）锁步携带本组件，
是上游的生产用户。

## 安全性

**onvif-c 不实现任何 ONVIF 认证。** 没有 WS-UsernameToken / `wsse` 头解析、
没有 HTTP basic/digest、没有凭据回调、没有 401 路径——所有已服务动作、
Pull-Point 订阅、WS-Discovery 应答对网络上任何可达本机的主机开放。仅部署
在所有主机都可信的网络（或放在强制认证的反向代理之后）；另外，配置了
凭据的 ONVIF 客户端可能拒绝添加本设备。这是极简设备库的有意取舍而非
疏漏；需要认证请作为后续小版本的功能需求提出。

## 字节稳定保证

响应的元素名、前缀、属性顺序与命名空间风格都是**承重的**——NVR 集成方可能
做裸子串匹配。设备/媒体信封用 `soap:`/`tds:`/`trt:`/`tt:`（小写 `utf-8`
声明），事件信封用 `s:`/`tev:`/`wsnt:`（大写 `UTF-8`），与生产固件字节一致。
精确输出由宿主机金样本测试（`tests/`）钉死；那里的 diff 是行为变更，不是
外观调整。

两个服务族的前缀风格刻意不同——各自都是真实 NVR 在生产中对话过的字节，
不要"统一"。

## 质量门禁（TDD）

库内一切变更先写测试、后改代码，并由 CI 门禁焊死：

| 门禁 | 命令 | 强制内容 |
| --- | --- | --- |
| 宿主机测试 | `tests/run.sh` | 213 项检查：core 金样本字节 + 经桩件驱动的完整 esp_idf 移植层 |
| 覆盖率 | `tests/coverage.sh` | `core/` + `esp_idf/` 行覆盖 ≥80%（当前 95%） |
| 代码风格 | `tools/check_style.sh` | clang-format 干净（锁定 `clang-format==22.1.8`，见 `.clang-format`） |
| 仓库卫生 | `tools/check-repo-hygiene.sh` | 不跟踪垃圾/涉密文件 |

宿主机测试设施（`tests/`）只需一个 C 编译器 + pthreads：

- **core 金样本**（`test_core.c`）—— 纯 C，钉死每个响应字节。
- **移植层**（`test_service.c` / `test_events.c` / `test_discovery.c`）——
  用 ESP-IDF 桩件（`tests/host_stubs/`）驱动真实 `esp_idf/` handler：
  请求/响应捕获型假 httpd、假时钟（`-Wl,--wrap=time`，订阅过期完全可
  复现）、pthread 任务、虚拟 UDP 网络（注入 WS-Discovery Probe、捕获
  ProbeMatches/Hello，含 socket/bind/组播加入失败的重试路径）。

克隆后装一次 pre-commit 钩子：`tools/setup-hooks.sh`。

## 库卫生

- core（`core/`）纯 C、不含 ESP-IDF 头——宿主机系统 `cc` 即可测试；
  ESP-IDF 面（`esp_idf/`）是薄传输层。
- 同时支持 ESP-IDF v5.5.x 与 v6.0.x（双双在 CI）。
- 库代码路径无 `ESP_LOGx` 之外日志、无 `printf`、生产者语境 API 不阻塞。
- **零硬编码端点**：`cfg->http_port` 流入所有广告 URI（capabilities
  XAddr、WS-Discovery XAddr、订阅地址）；板级细节绝不漏进库内。

## 版本策略

语义化版本；`ONVIF_C_VERSION` 编码为 `主*10000+次*100+修订`
（运行期 `onvif_c_version()` 可读）。兼容规则：

- **字节钉死**：同一大版本内，未改语义的动作响应字节永不变化——金样本
  diff 是发版阻断事件。
- core 构造器签名（`core/*.h`）属内部稳定面：破坏性变更升次版本；公共
  `onvif_c.h` 面目标是大版本内永不破坏。
- 发版由 tag（`v*`）触发；发版工作流会先跑全套宿主机测试再发布。

## 状态

v0.1.0 —— 首个发版；API 接缝稳定；在 [Mi-Bee Studio](https://github.com/Mi-Bee-Studio)
四块 ESP32/ESP32-S3 相机板上对 MiBee NVR 每日生产验证。客户端对应（Go）：
[onvif-go](https://github.com/mickeyzzc/onvif-go)；兄弟设备端库（Rust）：
[onvif-rs](https://github.com/mickeyzzc/onvif-rs)。

## 文档

主题手册在文档站：<https://www.mlsbs.top/docs/mibeelibs>（仓内 `docs/`
仅为重定向）。

## 许可

MIT —— 见 [LICENSE](LICENSE)。抽取自 MiBee Cam 固件（Mi-Bee Studio）；
固件仓继续以 GPL-3.0-or-later 发布，本组件在位双许可。
