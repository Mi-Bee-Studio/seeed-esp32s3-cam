# MiBee Cam — ESP32-S3 智能摄像头固件

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x%2Fv6.0-1C7C3D?logo=espressif)](https://idf.espressif.com/)
[![Platform](https://img.shields.io/badge/Platform-XIAO%20ESP32--S3%20Sense-EA4C89)](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
[![CI](https://img.shields.io/github/actions/workflow/status/Mi-Bee-Studio/seeed-esp32s3-cam/release.yml?logo=github&label=CI)](https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam/actions)
[![Release](https://img.shields.io/github/v/release/Mi-Bee-Studio/seeed-esp32s3-cam?logo=github)](https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam/releases)
[![Firmware](https://img.shields.io/badge/%E5%9B%BA%E4%BB%B6-v0.3.0-7C3AED)](https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam/releases)

> **家用级 AI 增强监控摄像头固件** — 巴掌大的身躯，企业级的能耐。
> 一块 15 美元的 XIAO ESP32-S3 Sense 开发板，变身全功能网络监控摄像头。

[English](README.md) | 中文文档

---

## 为什么选择 MiBee Cam？

大多数 ESP32 摄像头项目止步于「能看视频流」。MiBee Cam 一路走到底——分段录像、智能延时摄影（运动检测自适应）、双协议 NAS 上传（WebDAV + HTTPS）、ONVIF 自动发现（兼容 Synology/Milestone 等 NVR）、OTA 远程升级、紫色主题 Web 管理界面——全部跑在一个双核 FreeRTOS 上。无需 Linux、无需云服务。**你的摄像头、你的数据、你的网络。**

---

## 功能亮点

- **智能录像** — 连续录像、普通延时、**动态延时**（运动自适应捕获，无活动时节省 90%+ 存储）
- **实时视频** — HTTP MJPEG（端口 81），最多 2 路并发
- **NAS 上传** — WebDAV 或 HTTP(S) PUT，队列调度，3 次重试退避，互斥选择
- **运动检测** — 帧差分析，0–100 评分，触发动态延时和 Webhook 告警
- **Web 仪表盘** — 6 页响应式界面：状态总览、配置管理、文件管理、实时预览、OTA 升级、首次配置向导
- **OTA 远程升级** — 网页上传固件或从 URL 触发更新，SHA‑256 校验
- **WebSocket 实时推送** — 状态更新、运动事件实时推送到仪表盘
- **Webhook 通知** — 运动检测、录像异常、系统事件 HTTP 回调
- **Prometheus `/metrics`** — 15+ 项系统指标，接入 Grafana / 家庭实验室监控
- **循环存储** — SD 卡剩余 < 20% 自动清理，恢复至 30%，开机崩溃恢复清理
- **TF 卡热插拔** — 拔出自动暂停录像，插入自动恢复（10 秒轮询）
- **双模 WiFi** — AP 模式用于配置，STA 模式用于生产，支持备用 SSID + 自动回退
- **SNTP 时间同步** — 双 NTP 服务器、手动设置、时区配置
- **健康看门狗** — 30s TWDT + 60s 健康监控（堆 / PSRAM / 栈 / 芯片温度）
- **紫色主题** — Mi&Bee 品牌视觉，全站统一
- **音频采集 + RTSP 推流** — 板载 MEMS 麦克风、G.711 μ-law 编解码、双轨道 RTSP（MJPEG+音频）供 NVR 集成，支持摘要认证
- **TF 卡事件日志** — 结构化 JSON 事件日志，按日期轮转，保留 7 天
- **智能录制控制** — 独立切换视频/音频录制到 SD 卡；流媒体无需本地存储

---

## Web 管理界面

![仪表盘](docs/images/index-dashboard.png)
*仪表盘 — 实时状态、录像控制、系统健康状况一目了然*

| 页面 | 功能 |
|------|------|
| **仪表盘** (`/`) | 录像状态、WiFi 信息、存储用量、芯片温度、固件版本 |
| **配置** (`/config.html`) | 全部设置：WiFi、视频、录像模式、NAS、Webhook、运动检测 |
| **文件** (`/files.html`) | 按日期分组浏览、下载、批量删除录像文件 |
| **预览** (`/preview.html`) | 实时 MJPEG 视频流，支持全屏和截图 |
| **OTA** (`/ota.html`) | 固件远程升级，支持 URL 和文件上传 |
| **首次配置** (`/setup.html`) | WiFi 配置向导，首次开机自动引导 |

![文件管理](docs/images/files-page.png)
![配置页面](docs/images/config-page.png)

---

## 快速开始

```bash
git clone --recursive https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam.git
cd seeed-esp32s3-cam
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

> 需要 ESP-IDF v5.x 或 v6.0。👉 [详细安装指南](docs/zh/getting-started.md)

### 硬件要求

- [XIAO ESP32-S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)（任意带 PSRAM 的 ESP32-S3 均可）
- TF 卡（FAT32，Class 10+）
- USB-C 数据线（供电 + 烧录）

👉 [引脚定义与硬件详情](docs/zh/hardware.md)

---

## 录像模式

| 模式 | 说明 | 存储消耗 |
|------|------|----------|
| **连续录像**（默认） | 标准 AVI 分段录像 | 最高（全帧率） |
| **普通延时** | 固定间隔拍摄（5–300s），15fps AVI | 低（约 1/间隔倍数） |
| **动态延时** | 运动自适应：有动作时快速拍摄，无动作时慢速 | 最低（安静时段节省 90%+） |

动态延时是核心亮点——设置 `timelapse_mode=2`，调节灵敏度（0–100），运动检测自动切换拍摄间隔。适用于停车场、仓库、3D 打印机监控等场景。

---

## API 速览

| 方法 | 路径 | 认证 | 说明 |
|------|------|------|------|
| GET | `/api/status` | 否 | 设备状态（录像、WiFi、存储、温度、运动、固件） |
| GET | `/api/config` | 否 | 当前配置（密码字段返回 `****`） |
| POST | `/api/config` | 是 | 修改配置 |
| POST | `/api/record?action=start\|stop` | 是 | 录像控制 |
| POST | `/api/ota` | 是 | OTA 固件升级（从 URL） |
| POST | `/api/format` | 是 | 格式化 SD 卡 |
| POST | `/api/reset` | 是 | 恢复出厂设置 |
| POST | `/api/time` | 是 | 手动设置系统时间 |
| GET | `/api/files` | 否 | 录像文件列表 |
| POST | `/api/files/batch` | 是 | 批量删除文件 |
| DELETE | `/api/files?name=xxx` | 是 | 删除单个文件 |
| GET | `/api/download?name=xxx` | 否 | 下载录像文件 |
| GET | `/api/scan` | 否 | 扫描 WiFi 网络 |
| GET | `/stream` | 否 | MJPEG 实时视频流 |
| GET | `/metrics` | 否 | Prometheus 监控指标（text 格式） |
| GET | `/onvif/*` | 否 | ONVIF WS-Discovery + SOAP 服务 |
| WS | `ws://<IP>/` | 否 | WebSocket 实时推送 |
| RTSP | `rtsp://<IP>:554/stream` | 摘要认证 | MJPEG+G.711 双轨道流，供 NVR 使用 |

默认密码：`admin` 👉 [完整 API 文档](docs/zh/api/overview.md)

---

## 项目结构

```
main/  —  27 个 C 模块 + main.c + cJSON（平面布局）
├── main.c                 # 入口，20 步启动流程，看门狗
├── camera_driver.c/h      # OV2640/OV3660 摄像头驱动
├── video_recorder.c/h     # AVI MJPEG 分段录像（3 种模式）
├── motion_detector.c/h    # 帧差运动检测分析
├── mjpeg_streamer.c/h     # HTTP MJPEG 实时推流
├── frame_broadcaster.c/h  # 帧缓冲分发器（解耦 FB 消费者）
├── onvif_service.c/h      # ONVIF SOAP 服务处理
├── onvif_discovery.c/h    # ONVIF WS-Discovery 多播
├── web_server.c/h         # HTTP 服务器 + REST API（16 个端点）
├── ws_server.c/h          # WebSocket 实时推送
├── ota_updater.c/h        # OTA 远程固件升级
├── sha256.c/h             # SHA-256 哈希校验
├── webhook.c/h            # HTTP 事件通知
├── nas_uploader.c/h       # 上传调度（WebDAV / HTTPS 互斥）
├── webdav_client.c/h      # WebDAV 协议客户端
├── http_upload_client.c/h # HTTP/HTTPS 分块上传
├── audio_driver.c/h       # I2S PDM 麦克风采集，16→8kHz 降采样
├── audio_broadcaster.c/h  # 音频帧发布/订阅中心（镜像帧广播器）
├── g711_codec.c/h         # G.711 μ-law 编解码器（ITU-T 标准）
├── rtsp_server.cpp/h      # RTSP 服务器（MJPEG+G.711 双轨道，摘要认证，端口 554）
└── sd_log.c/h             # SD 卡结构化事件日志，自动轮转
├── wifi_manager.c/h       # WiFi AP/STA 双模管理
├── config_manager.c/h     # NVS 配置持久化 + SD 卡覆盖
├── storage_manager.c/h    # SD 卡 FAT32 + 循环清理
├── time_sync.c/h          # SNTP 双服务器时间同步
├── status_led.c/h         # GPIO21 LED 状态机（5 种模式）
├── logging.c/h            # 日志工具
└── cJSON.c/h              # 内嵌 JSON 解析库（IDF v6.0 已移除）
```

👉 [系统架构详解](docs/zh/architecture.md)

---

## 文档

| 指南 | 内容 |
|------|------|
| [安装指南](docs/zh/getting-started.md) | 环境搭建、编译烧录、首次配置 |
| [硬件手册](docs/zh/hardware.md) | 引脚定义、硬件规格、接线说明 |
| [使用手册](docs/zh/user-guide.md) | 配置管理、录像模式、NAS、OTA、运动检测 |
| [系统架构](docs/zh/architecture.md) | 启动流程、模块设计、数据流 |
| [API 参考](docs/zh/api/overview.md) | 完整 REST API + WebSocket + ONVIF |
| [故障排除](docs/zh/troubleshooting.md) | 常见问题排查与修复 |
| [LED 状态说明](docs/zh/led-status.md) | LED 闪烁模式解码 |

---

## 许可证

[GNU General Public License v3.0](LICENSE) — 自由使用、修改和分发。
