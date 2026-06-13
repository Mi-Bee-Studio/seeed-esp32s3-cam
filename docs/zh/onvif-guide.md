# ONVIF 协议指南

## 概述

ONVIF（开放网络视频接口论坛）是用于基于 IP 的物理安全产品的开放标准。此固件实现了最基础的 ONVIF 流媒体配置文件，允许网络录像机（NVR）自动发现并连接到 MiBee Cam 作为 IP 摄像头。

**合规等级**: 仅基础流媒体配置文件（无 Profile S，无 PTZ）  
**目的**: 允许 NVR 自动发现并添加此摄像头  
**流格式**: HTTP MJPEG（非 H.264）

该实现专注于 NVR 集成所需的核心流媒体功能，通过标准的 ONVIF SOAP 消息提供设备识别、服务发现和流媒体访问。

## 发现流程 ASCII 流程图

```
NVR                          多播网络                      MiBee Cam
 │                                │                                  │
 │──── 探测（UDP 多播） ────>│──── 239.255.255.250:3702 ──────>│
 │                                │                                  │
 │                                │    onvif_discovery_task()        │
 │                                │    ├─ is_probe_message() ✓       │
 │                                │    ├─ extract_message_id()       │
 │                                │    └─ build_probe_matches()      │
 │                                │                                  │
 │<── 探测匹配（单播） ────│<─── UDP 单播响应 ────────│
 │                                │                                  │
 │  NVR 现在知道设备 IP           │                                  │
 │                                │                                  │
 │──── POST /onvif/device_service (SOAP) ──────────────────────────>│
 │    Action: GetDeviceInformation│                                  │
 │<─── Response: 制造商, 型号, 固件版本, 序列号 ──────────────────│
 │                                │                                  │
 │──── POST /onvif/device_service (SOAP) ──────────────────────────>│
 │    Action: GetCapabilities     │                                  │
 │<─── Response: 设备 + 媒体服务 URL ──────────────────────│
 │                                │                                  │
 │──── POST /onvif/media_service (SOAP) ───────────────────────────>│
 │    Action: GetProfiles         │                                  │
 │<─── Response: 视频配置文件（分辨率, JPEG 编码） ────────│
 │                                │                                  │
 │──── POST /onvif/media_service (SOAP) ───────────────────────────>│
 │    Action: GetStreamUri        │                                  │
 │<─── Response: http://<IP>/stream ───────────────────────────────│
 │                                │                                  │
 │  NVR 连接到 MJPEG 流         │                                  │
 │──── GET http://<IP>:81/stream ─────────────────────────────────>│
```

周期性 Hello 公告：
```
MiBee Cam ──── Hello（多播） ────> 239.255.255.250:3702
              （每 ~30 秒）
```

## 实现的 SOAP 操作

| 服务 | 操作 | 目的 | 关键响应字段 |
|---------|--------|---------|-------------------|
| Device | GetDeviceInformation | 设备身份 | Manufacturer=MiBee, Model=MiBee Cam, 固件版本, Serial=MAC |
| Device | GetCapabilities | 服务端点 | Device XAddr, Media XAddr |
| Device | GetServices | 可用服务 | Device + Media 命名空间 URL |
| Device | GetSystemDateAndTime | 当前时间 | UTC 日期/时间 |
| Media | GetProfiles | 视频配置 | 分辨率, JPEG 编码, FPS |
| Media | GetStreamUri | 流 URL | http://<IP>/stream |

## 技术架构

- **发现**: `onvif_discovery.c` — Core 1 上的 UDP 多播监听器，优先级 2，6144B 栈
  - 多播组: 239.255.255.250:3702
  - UUID: 基于 MAC（f472b01e-0000-1000-8000-{MAC}）
  - Hello 周期: ~30 秒
  - 范围: video_encoder, NetworkVideoTransmitter, Profile/Streaming
  
- **SOAP 服务**: `onvif_service.c` — 在端口 80 httpd 上注册的 HTTP 处理程序
  - 两个端点: `/onvif/device_service` 和 `/onvif/media_service`
  - 无 XML 解析器 — 使用 strstr() 进行操作检测
  - 通过 snprintf() 生成响应
  - 最大请求体: 4096 字节

- **流 URI**: 返回 `http://<IP>/stream` — 端口 81 上的 MJPEG 流

## NVR 兼容性

以下 NVR 已通过 MiBee Cam ONVIF 实现测试：

| NVR | 发现 | 视频播放 | 备注 |
|-----|-----------|----------------|-------|
| **MiBeeNVR** 🏆 | ✅ | ✅ | **原生配套 NVR** — 基于 Go 语言，单二进制文件，ONVIF 自动发现，完整 MJPEG 支持。[github.com/Mi-Bee-Studio/MiBeeNvr](https://github.com/Mi-Bee-Studio/MiBeeNvr) |
| 通用 ONVIF | ✅ | ✅ (MJPEG) | 与标准兼容 NVR 的最佳兼容性 |
| 海康威视 | ✅ | ⚠️ | 可能期望 H.264，回退到 MJPEG |
| 大华 | ✅ | ⚠️ | 类似海康威视 |
| Blue Iris | ✅ | ✅ | 完全 MJPEG 支持 |
| Shinobi | ✅ | ✅ | 完全 MJPEG 支持 |
| Frigate | ✅ | ✅ | 完全 MJPEG 支持 |

**MiBeeNVR** 是 MiBee Cam 推荐的配套 NVR，提供最佳集成体验：无缝 ONVIF 自动发现、实时预览、录像管理以及现代化 Web UI——所有这些都包含在单个二进制文件中，可在从 Raspberry Pi 3B 到服务器的任何设备上运行。

重要说明：
- 此摄像头仅输出 **MJPEG**（非 H.264）。某些 NVR 期望 H.264，可能无法正常工作。
- `GetStreamUri` 返回 `http://<IP>/stream`，这是 HTTP MJPEG 流。
- 无 ONVIF 身份验证 — 摄像头不实现 `wsse:Security` 头。

## 常见问题 / 故障排除

问：我的 NVR 无法发现摄像头
答：检查清单：
1. 摄像头必须处于 STA 模式（连接到路由器，非 AP 模式）
2. NVR 和摄像头必须在同一子网（多播不跨子网）
3. 检查串口日志中的 "Received Probe from..." — 如果看到这个，发现功能正常
4. 某些 NVR 需要定向发现 — 直接 POST 到 /onvif/device_service
5. 路由器多播设置 — 某些路由器过滤多播流量

问：NVR 发现了摄像头但无法播放视频
答： 
1. 流 URI 是 http://<IP>/stream，这是 HTTP MJPEG
2. NVR 必须支持 MJPEG — 许多期望 H.264 并且不会播放 MJPEG
3. 尝试在 NVR 中手动添加流 URL
4. 首先检查 MJPEG 流是否在浏览器中工作

问：为什么 ONVIF 发现功能在 AP 模式下不工作？
答：ONVIF 发现任务仅在 WiFi STA 连接后启动。在 AP 模式下，没有多播路由。通过 STA 模式将摄像头连接到您的路由器。

问：是否支持 RTSP？
答：不支持。RTSP 服务器已被移除。请使用端口 81 上的 HTTP MJPEG 流。

问：我可以设置 ONVIF 身份验证吗？
答：目前不支持。实现是最小化的，不支持用户名/密码身份验证。

问：支持哪些 ONVIF 配置文件？
答：流媒体配置文件（基础）。无 PTZ，无分析，无事件服务。

## 配置参考
- ONVIF 始终启用 — 无需配置
- ONVIF 中显示的设备名称: "MiBeeCam"（硬编码）
- 制造商: "MiBee"（硬编码）
- 流 URI 解析和 FPS 遵循摄像头配置设置