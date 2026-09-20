# onvif-c

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
  回 ProbeMatches，每 ~30s 周期性 Hello 广播。
- **可选 mDNS** —— `_onvif._tcp` 广告。
- **无 XML 解析器、无动态状态** —— 动作识别 `strstr()`、响应生成
  `snprintf()`；仅每请求缓冲；运动事件生产者钩子非阻塞，传感器回调语境安全。
- **单一配置接缝** —— 板级差异（身份、IP、流地址、运行时开关）全部收敛到
  `onvif_c_config_t` 回调。

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
        .firmware_version = "v0.2.0",
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
| 宿主机测试 | `tests/run.sh` | 212 项检查：core 金样本字节 + 经桩件驱动的完整 esp_idf 移植层 |
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
- 同时支持 ESP-IDF v5.5.x 与 v6.0.x。
- 库代码路径无 `ESP_LOGx` 之外日志、无 `printf`、生产者语境 API 不阻塞。
- **零硬编码端点**：`cfg->http_port` 流入所有广告 URI（capabilities
  XAddr、WS-Discovery XAddr、订阅地址）；板级细节绝不漏进库内。

## 状态

v0.2.0 —— API 接缝稳定；在 [Mi-Bee Studio](https://github.com/Mi-Bee-Studio)
四块 ESP32/ESP32-S3 相机板上对 MiBee NVR 每日生产验证。客户端对应（Go）：
[onvif-go](https://github.com/mickeyzzc/onvif-go)；兄弟设备端库（Rust）：
[onvif-rs](https://github.com/mickeyzzc/onvif-rs)。

## 文档

主题手册在文档站：<https://www.mlsbs.top/docs/mibeelibs>（仓内 `docs/`
仅为重定向）。

## 许可

MIT —— 见 [LICENSE](LICENSE)。抽取自 MiBee Cam 固件（Mi-Bee Studio）；
固件仓继续以 GPL-3.0-or-later 发布，本组件在位双许可。
