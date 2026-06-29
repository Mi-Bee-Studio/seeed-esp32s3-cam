# 系统架构

## 1. 系统概览

ESP32-S3 摄像头监控系统基于 FreeRTOS 实时操作系统，在双核 ESP32-S3 上运行 27 个松耦合的 C 模块。系统采用事件驱动与轮询混合架构，摄像头采集帧数据后分两路输出：实时 HTTP MJPEG 流和录像存储。

```
  Camera ──→ Frame Broadcaster ──→ MJPEG Streamer (端口 81) ──→ 浏览器/客户端
          └→ Video Recorder ──→ SD Card (AVI 分段)
                             └→ NAS Uploader ──→ WebDAV / HTTP(S) (互斥)
  Config Manager ←→ NVS ←→ SD Card Override (wifi.txt / nas.txt)
  WiFi Manager (AP/STA) → Time Sync (SNTP)
  Status LED ← LED Controller (GPIO21, 低电平有效)
  Watchdog (30s TWDT, panic) → Health Monitor (60s)
  ONVIF Discovery (UDP 组播) → NVR 自动发现
  Audio Driver (I2S PDM) → Audio Broadcaster → RTSP Server (端口 554)
```

### 核心特性

| 特性 | 说明 |
|------|------|
| 双核分工 | Core 0: 录像；Core 1: 上传、SD 监控、健康检测、ONVIF |
| PSRAM 依赖 | 摄像头帧缓冲分配在 PSRAM（双缓冲），无 PSRAM 无法工作 |
| 循环存储 | SD 卡剩余空间 < 20% 自动删除最旧录像，恢复到 30% |
| 热插拔 | SD 卡拔出自动停止录像，插入自动恢复 |
| 看门狗 | 30s TWDT，超时触发 panic 重启 |
| ONVIF 发现 | WS-Discovery 组播，NVR 可自动发现 |

---

## 2. 启动流程

`app_main()` 中 21 步顺序初始化，任何关键步骤失败仅记录错误但继续执行（部分功能降级）。

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | 初始化 NVS Flash | 损坏时自动擦除重建 |
| 2 | 配置管理器 | 从 NVS 加载配置，无配置则使用默认值 |
| 3 | 状态 LED | GPIO21 初始化，设为 `LED_STARTING`（常亮） |
| 4 | SPIFFS 挂载 | 加载 Web UI 静态资源，挂载失败自动格式化 |
| 5 | SD 卡存储 | SPI 模式，FAT32 文件系统 |
| 5a | 清理不完整 AVI | 删除上次异常断电时未正确关闭的录像文件 |
| 6 | SD 卡配置覆盖 | 读取 SD 卡 `/sdcard/config/wifi.txt` 和 `config.txt`，覆盖 NVS 配置 |
| 7 | WiFi 初始化 | 已配置 SSID → STA 模式，否则 → AP 模式 |
| 8 | 摄像头初始化 | 自动检测 OV2640/OV3660，配置分辨率/帧率/质量，测试采集验证 |
| 9 | 时间同步 | STA 连接后启动 SNTP，阻塞等待最多 5 秒 |
| 10 | NAS 上传器 | 创建上传队列（容量 16）和上传任务 |
| 11 | 帧广播器 | 初始化帧发布/订阅中心，供 MJPEG 订阅者使用 |
| 12 | 视频录像器 | 初始化 AVI 录像引擎，注册分段回调 |
| 13 | MJPEG 流服务 | 初始化独立 TCP 服务器（端口 81），最大 2 个并发客户端 |
| 14 | Web 服务器 | 端口 80，注册 URI 处理程序 + ONVIF 端点 |
| 14a | OTA 更新器 | 初始化固件 OTA 更新能力 |
| 15 | LED 状态更新 | 根据当前 WiFi 状态更新 LED 模式 |
| 16 | ONVIF 发现 | 启动 WS-Discovery UDP 组播监听（WiFi 连接后） |
| 17 | 等待 STA + 开始录像 | 最多等待 30 秒 WiFi 连接，连接后开始录像 |
| 18 | BOOT 按钮监控 | GPIO0，长按 5 秒触发恢复出厂设置 |
| 19 | 看门狗 | 30s TWDT，双核 idle 任务均纳入监控 |
| 20 | SD 监控任务 | Core 1，每 10 秒轮询 SD 卡状态，处理热插拔 |
| 21 | 健康监控任务 | Core 1，每 60 秒输出堆/栈水位信息 |

---

## 3. 模块说明

系统共 27 个模块，全部位于 `main/` 目录，每个模块一个 `.c`/`.h` 文件对。

| 模块 | 文件 | 职责 | 关键函数 |
|------|------|------|----------|
| 主入口 | `main.c` | 启动流程，任务创建 | `app_main()`, `on_segment_complete()` |
| 摄像头驱动 | `camera_driver.c` | OV2640/OV3660 自动检测，帧采集 | `camera_init()`, `camera_capture()` |
| 视频录像器 | `video_recorder.c` | AVI MJPEG 分段录像，状态机，延时摄影模式 | `recorder_start()`, `recorder_stop()` |
| 帧广播器 | `frame_broadcaster.c` | 帧发布/订阅中心，向 MJPEG 订阅者分发 JPEG 帧 | `fbroadcast_publish()`, `fbroadcast_subscribe()` |
| MJPEG 流服务 | `mjpeg_streamer.c` | MJPEG 实时流，独立 TCP 服务器（端口 81） | `mjpeg_streamer_init()` |
| Web 服务器 | `web_server.c` | HTTP 服务器 + REST API + ONVIF 端点 | `web_server_start()`, `web_server_get_handle()` |
| WiFi 管理 | `wifi_manager.c` | AP/STA 双模式，自动选择 | `wifi_init()`, `wifi_scan()` |
| 配置管理 | `config_manager.c` | NVS 持久化，SD 卡覆盖 | `config_init()`, `config_save()`, `config_reset()` |
| 存储管理 | `storage_manager.c` | SD 卡挂载/卸载，循环清理 | `storage_init()`, `storage_cleanup()` |
| NAS 上传 | `nas_uploader.c` | 互斥上传调度（WebDAV 或 HTTP/HTTPS） | `nas_uploader_init()`, `nas_uploader_enqueue()` |
| HTTP 上传客户端 | `http_upload_client.c` | HTTP/HTTPS PUT 流式上传 | — |
| WebDAV 客户端 | `webdav_client.c` | WebDAV 协议上传实现 | — |
| ONVIF 发现 | `onvif_discovery.c` | WS-Discovery UDP 组播，NVR 自动发现 | `onvif_discovery_init()` |
| ONVIF 服务 | `onvif_service.c` | SOAP 处理：GetDeviceInformation, GetCapabilities, GetProfiles, GetStreamUri | `onvif_register_handlers()` |
| 状态 LED | `status_led.c` | 5 种 LED 模式控制 | `led_init()`, `led_set_status()` |
| 时间同步 | `time_sync.c` | SNTP 同步，手动设置时间 | `time_sync_init()`, `time_is_synced()` |
| 运动检测 | `motion_detector.c` | 帧差分析，运动评分，告警触发 | `motion_detector_init()`, `motion_detector_process()` |
| OTA 更新器 | `ota_updater.c` | 固件升级，版本管理 | `ota_updater_init()`, `ota_update_start()` |
| Webhook | `webhook.c` | HTTP 事件通知 | `webhook_send()`, `webhook_init()` |
| WebSocket 服务器 | `ws_server.c` | 实时推送更新到 WebUI 客户端 | `ws_server_init()`, `ws_broadcast()` |
| SHA-256 哈希 | `sha256.c` | 文件完整性校验 | `sha256_hash()`, `sha256_verify()` |
| JSON 解析器 | `cJSON.c` | 第三方 JSON 库（IDF v6.0 内置） | — |
| 音频驱动 | `audio_driver.c` | I2S PDM 麦克风采集，16→8kHz 降采样 | `audio_init()`, `audio_capture()` |
| 音频广播器 | `audio_broadcaster.c` | 音频帧发布/订阅中心（镜像帧广播器） | `abroadcast_publish()`, `abroadcast_subscribe()` |
| G.711 编解码 | `g711_codec.c` | G.711 μ-law 编解码器（ITU-T 标准） | `g711_encode()`, `g711_decode()` |
| RTSP 服务器 | `rtsp_server.cpp` | RTSP 服务器（MJPEG+G.711 双轨道，摘要认证，端口 554） | `rtsp_server_init()`, `rtsp_server_start()` |
| SD 日志 | `sd_log.c` | SD 卡结构化事件日志，自动轮转 | `sd_log_init()`, `sd_log_write()` |

---

## 4. 数据流

### 录像数据流

摄像头采集的 JPEG 帧由录像器写入 AVI 文件，分段完成后触发回调链：

```
camera_capture()
    │
    ▼
    视频录像器 (recording_task, Core 0)
    │  ← 帧数据写入 AVI 文件
    │  ← 按 segment_sec 时长分段（连续 vs 延时模式）
    │  ← 写入 AVI idx1 索引
    │
    ▼  分段完成
    on_segment_complete(filepath, size)
    │
    ├──→ nas_uploader_enqueue(filepath)
    │       │
    │       ▼
    │    上传任务 (Core 1)
    │       ├── upload_method=1 → WebDAV 上传
    │       ├── upload_method=2 → HTTP(S) 上传
    │       └── upload_method=0 → 跳过上传
    │       失败重试 3 次，连续失败 10 次后暂停 5 分钟
    │
    └──→ storage_cleanup()
            │
            ▼
         检查剩余空间 < cleanup_low_pct?（默认 20%）
            ├── 是 → 删除最旧录像文件，直到 ≥ cleanup_high_pct（默认 30%）
            │        写入失败时也尝试清理+重试
            └── 否 → 无操作
```

**延时摄影模式**：当 `timelapse_interval_sec > 0` 时，按指定间隔（1-300秒）采集帧而非连续采集。文件使用 `TLM_` 前缀，以 15fps 播放。

### MJPEG 实时流数据流

MJPEG 流服务运行在**端口 81** 作为独立 TCP 服务器，与端口 80 的主 httpd 完全分离。详见[端口分离设计](port-separation-design.md)。

```
浏览器 → GET /stream (端口 81)
    │
    ▼
mjpeg_listen_task (Core 1, 优先级 3)
    └── accept() → 为每个客户端生成 mjpeg_client_task
         │
         ▼
frame_broadcaster → fbroadcast_receive()
         │
         ▼
video_recorder（帧生产者）→ fbroadcast_publish()
         │  ← recording_task 中的 camera_capture()
         │  ← PSRAM 副本 → 与摄像头缓冲区解耦
         │
         ▼
HTTP 分块传输 (multipart/x-mixed-replace)
    │  Content-Type: image/jpeg
    │  Boundary: --frame
    │
    ▼
浏览器实时渲染
```

### RTSP 服务器数据流

RTSP Server (端口 554) ← frame_broadcaster + audio_broadcaster
  → MJPEG 视频轨道（RTP payload type 26）
  → G.711 μ-law 音频轨道（RTP payload type 0, PCMU）
  → 摘要认证

---

## 5. FreeRTOS 任务表

| 任务名 | 函数 | 优先级 | 核心 | 栈大小 | 周期/触发 | 文件 |
|--------|------|--------|------|--------|-----------|------|
| `recorder` | `recording_task` | 5 (configMAX-2) | Core 0 | 4096 B | 连续循环（帧采集） | video_recorder.c |
| `upload` | `upload_task` | 3 | Core 1 | 6144 B | 队列阻塞等待 | nas_uploader.c |
| `mjpeg_listen` | `mjpeg_listen_task` | 3 | Core 1 | 4096 B | 连接接受循环 | mjpeg_streamer.c |
| `mjpeg_cli_N` | `mjpeg_client_task` | 2 | Core 1 | 4096 B | 帧接收+发送循环 | mjpeg_streamer.c |
| `onvif_disc` | `onvif_discovery_task` | 2 | Core 1 | 6144 B | UDP 组播监听 | onvif_discovery.c |
| `sd_monitor` | `sd_monitor_task` | 2 | Core 1 | 3072 B | 10 秒轮询 | main.c |
| `boot_btn` | `boot_button_monitor` | 1 | 任意 | 2048 B | 200ms 轮询 | main.c |
| `health_mon` | `health_monitor_task` | 1 | Core 1 | 3072 B | 60 秒轮询 | main.c |
| `ws_server` | `websocket_server_task` | 3 | Core 1 | 2048 B | 客户端事件驱动 | ws_server.c |
| `httpd` | ESP-IDF 内置 | 默认 | — | 16384 B | 事件驱动 | web_server.c |
| `main` | `app_main` | 1 | Core 0 | 8192 B | 初始化后返回 | main.c |

### 任务调度策略

- **Core 0**：运行录像任务（高优先级），确保帧采集不被抢占导致丢帧
- **Core 1**：运行上传、MJPEG 流、ONVIF 发现、SD 监控、健康监控、WebSocket 服务器等非实时任务
- **BOOT 按钮监控**：不绑定核心，低优先级轮询
- **Web 服务器**：由 ESP-IDF httpd 库管理，16384 字节栈
- **WebSocket 服务器**：运行在 Core 1，向连接的客户端广播状态更新

---

## 6. 分区表

来自 `partitions.csv`，使用自定义双 OTA 分区表：

| 名称 | 类型 | 子类型 | 偏移 | 大小 | 说明 |
|------|------|--------|------|------|------|
| `nvs` | data | nvs | 0x9000 | 24 KB (0x6000) | 非易失性存储（配置持久化） |
| `phy_init` | data | phy | 0xF000 | 4 KB (0x1000) | PHY 校准数据 |
| `ota_0` | app | ota_0 | 0x10000 | 1792 KB (0x1D0000) | OTA 插槽 A |
| `ota_1` | app | ota_1 | 0x1E0000 | 1792 KB (0x1D0000) | OTA 插槽 B |
| `otadata` | data | ota | 0x3B0000 | 8 KB (0x2000) | OTA 选择数据 |
| `spiffs` | data | spiffs | 0x3B2000 | 256 KB (0x40000) | Web UI 静态资源 |

**Flash 总大小**：8 MB

> **注意**：双 OTA 插槽支持无缝固件更新。`esp_https_ota` 自动处理插槽切换。

---

## 7. Web API 端点

服务器运行在端口 80，另含 ONVIF SOAP 端点：

| 方法 | 路径 | 认证 | 说明 |
|------|------|------|------|
| GET | `/api/status` | 否 | 设备状态（录像、WiFi、存储、摄像头、温度） |
| GET | `/api/config` | 否 | 当前配置（密码字段返回 `****`） |
| POST | `/api/config` | 是 | 修改配置 |
| GET | `/api/files` | 否 | 录像文件列表 |
| POST | `/api/files/batch` | 是 | 批量删除文件 |
| DELETE | `/api/files` | 是 | 删除指定文件 |
| GET | `/api/download?name=xxx` | 否 | 下载录像文件 |
| GET | `/api/scan` | 否 | 扫描 WiFi AP |
| POST | `/api/time` | 是 | 手动设置时间 |
| POST | `/api/record?action=start\|stop` | 是 | 录像控制 |
| POST | `/api/ota` | 是 | 通过 URL 触发 OTA 固件更新 |
| POST | `/api/format` | 是 | 格式化 SD 卡（需确认） |
| POST | `/api/reset` | 是 | 恢复出厂设置 |
| GET | `/metrics` | 否 | Prometheus 指标（文本格式） |
| POST | `/onvif/device_service` | 否 | ONVIF 设备服务（SOAP） |
| POST | `/onvif/media_service` | 否 | ONVIF 媒体服务（SOAP） |
| OPTIONS | `/*` | 否 | CORS 预检请求 |
| GET | `/*` | 否 | 静态文件（Web UI） |

认证：通过 `X-Password` 请求头或 `?password=xxx` 查询参数传递管理密码。

---

## 8. Web UI

6 个 HTML 页面，烧录到 SPIFFS 分区，由通配符 GET 处理程序服务：

| 页面 | 文件 | 功能 |
|------|------|------|
| 首页 | `index.html` | 状态概览 |
| 配置 | `config.html` | WiFi / NAS / 摄像头参数配置（手风琴布局） |
| 文件 | `files.html` | 录像文件浏览、下载、批量操作，可折叠日期分组 |
| 预览 | `preview.html` | MJPEG 实时流预览，支持全屏和截图 |
| OTA | `ota.html` | 固件更新界面，URL 输入和进度显示 |
| 设置 | `setup.html` | 首次 WiFi 配置向导 |

**Prometheus 集成**：`/metrics` 端点以文本格式提供 15+ 项系统指标。

**温度监控**：ESP32-S3 芯片温度通过 `/api/status` 获取，仪表盘颜色编码指示。
