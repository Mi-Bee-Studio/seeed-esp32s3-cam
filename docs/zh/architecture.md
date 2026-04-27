# 系统架构

## 1. 系统概览

ESP32-S3 摄像头监控系统基于 FreeRTOS 实时操作系统，在双核 ESP32-S3 上运行 14 个松耦合的 C 模块。系统采用事件驱动与轮询混合架构，摄像头采集帧数据后分两路输出：实时流和录像存储。

```
  Camera ──→ MJPEG Streamer ──→ HTTP Server ──→ Browser/Client
          └→ Video Recorder ──→ SD Card (AVI 分段)
                             └→ NAS Uploader ──→ FTP / WebDAV
  Config Manager ←→ NVS ←→ SD Card Override (wifi.txt / nas.txt)
  WiFi Manager (AP/STA) → Time Sync (SNTP)
  Status LED ← LED Controller (GPIO21, active-low)
  Watchdog (30s TWDT, panic) → Health Monitor (60s)
```

### 核心特性

| 特性 | 说明 |
|------|------|
| 双核分工 | Core 0: 录像; Core 1: 上传、SD 监控、健康检测 |
| PSRAM 依赖 | 摄像头帧缓冲分配在 PSRAM（双缓冲），无 PSRAM 无法工作 |
| 循环存储 | SD 卡剩余 < 20% 自动删除最旧录像，恢复至 30% |
| 热插拔 | SD 卡拔出自动停止录像，插入自动恢复 |
| 看门狗 | 30s TWDT，超时触发 panic 重启 |

---

## 2. 启动流程

`app_main()` 中的 19 步顺序初始化，任何关键步骤失败会记录错误日志但继续执行（部分功能降级）。

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | 初始化 NVS Flash | 损坏时自动擦除重建 |
| 2 | 配置管理器 | 从 NVS 加载配置，无配置则使用默认值 |
| 3 | 状态 LED | GPIO21 初始化，设为 `LED_STARTING`（常亮） |
| 4 | SPIFFS 挂载 | 加载 Web UI 静态资源，挂载失败自动格式化 |
| 5 | SD 卡存储 | 1-line SDMMC 模式，FAT32 文件系统 |
| 5a | 清理不完整 AVI | 删除上次断电时未正常关闭的录像文件 |
| 6 | SD 配置覆盖 | 读取 SD 卡 `/sdcard/config/wifi.txt` 和 `nas.txt`，覆盖 NVS 配置 |
| 7 | WiFi 初始化 | 有 SSID 配置→STA 模式，否则→AP 模式 |
| 8 | 摄像头初始化 | 自动检测 OV2640/OV3660，配置分辨率/FPS/质量，测试采集验证 |
| 9 | 时间同步 | 仅在 STA 已连接时启动 SNTP，阻塞等待最多 5 秒 |
| 10 | NAS 上传器 | 创建上传队列（容量 16）和上传任务 |
| 11 | 视频录像器 | 初始化 AVI 录像引擎，注册分段回调 |
| 12 | MJPEG 流服务 | 初始化实时流模块，最大 2 个并发客户端 |
| 13 | Web 服务器 | 端口 80，注册 12 个 URI 处理器（10 API + 2 通配） |
| 14 | LED 状态更新 | 根据当前 WiFi 状态更新 LED 模式 |
| 15 | 等待 STA + 开始录像 | 最多等 30 秒连接 WiFi，连接后开始录像 |
| 16 | BOOT 按钮监控 | GPIO0，按住 5 秒触发恢复出厂设置 |
| 17 | 看门狗 | 30 秒 TWDT，双核 idle 任务均纳入监控 |
| 18 | SD 监控任务 | Core 1，每 10 秒轮询 SD 卡状态，处理热插拔 |
| 19 | 健康监控任务 | Core 1，每 60 秒输出堆/栈水位信息 |

---

## 3. 模块说明

系统共 14 个模块，全部位于 `main/` 目录，每个模块一个 `.c`/`.h` 文件对。

| 模块 | 文件 | 职责 | 关键函数 |
|------|------|------|----------|
| 主入口 | `main.c` | 19 步启动流程，任务创建 | `app_main()`, `on_segment_complete()` |
| 摄像头驱动 | `camera_driver.c` | OV2640/OV3660 自动检测，帧采集 | `camera_init()`, `camera_capture()` |
| 视频录像器 | `video_recorder.c` | AVI MJPEG 分段录像，状态机 | `recorder_start()`, `recorder_stop()` |
| MJPEG 流服务 | `mjpeg_streamer.c` | MJPEG 实时视频流推送 | `mjpeg_streamer_init()`, `mjpeg_streamer_register()` |
| Web 服务器 | `web_server.c` | HTTP 服务器 + REST API（10 端点） | `web_server_start()`, `web_server_get_handle()` |
| WiFi 管理 | `wifi_manager.c` | AP/STA 双模式，自动选择 | `wifi_init()`, `wifi_scan()` |
| 配置管理 | `config_manager.c` | NVS 持久化，SD 卡覆盖 | `config_init()`, `config_save()`, `config_reset()` |
| 存储管理 | `storage_manager.c` | SD 卡挂载/卸载，循环清理 | `storage_init()`, `storage_cleanup()` |
| NAS 上传 | `nas_uploader.c` | 队列化上传调度，自动重试 | `nas_uploader_init()`, `nas_uploader_enqueue()` |
| FTP 客户端 | `ftp_client.c` | FTP 协议上传实现 | — |
| WebDAV 客户端 | `webdav_client.c` | WebDAV 协议上传实现 | — |
| 状态 LED | `status_led.c` | 5 种 LED 模式控制 | `led_init()`, `led_set_status()` |
| 时间同步 | `time_sync.c` | SNTP 同步，手动设置时间 | `time_sync_init()`, `time_is_synced()` |
| JSON 解析 | `cJSON.c` | 第三方 JSON 库（IDF v6.0 已移除） | — |

### 配置结构体

```c
typedef struct {
    char wifi_ssid[33];       // WiFi 名称
    char wifi_pass[64];       // WiFi 密码
    char ftp_host[64];        // FTP 服务器地址
    uint16_t ftp_port;        // FTP 端口
    char ftp_user[32];        // FTP 用户名
    char ftp_pass[32];        // FTP 密码
    char ftp_path[128];       // FTP 远程路径
    bool ftp_enabled;         // FTP 上传开关
    char webdav_url[128];     // WebDAV URL
    char webdav_user[32];     // WebDAV 用户名
    char webdav_pass[32];     // WebDAV 密码
    bool webdav_enabled;      // WebDAV 上传开关
    uint8_t resolution;       // 0=VGA, 1=SVGA, 2=XGA
    uint8_t fps;              // 1-30
    uint16_t segment_sec;     // 每段时长（秒）
    uint8_t jpeg_quality;     // 1-63
    char web_password[32];    // Web 管理密码
    char device_name[32];     // 设备名称
} cam_config_t;
```

---

## 4. 数据流

### 录像数据流

摄像头采集的 JPEG 帧经过录像器写入 AVI 文件，分段完成后触发回调链：

```
camera_capture()
    │
    ▼
Video Recorder (recording_task, Core 0)
    │  ← 帧数据写入 AVI 文件
    │  ← 按 segment_sec 时长分段
    │  ← 写入 AVI idx1 索引
    │
    ▼  分段完成
on_segment_complete(filepath, size)
    │
    ├──→ nas_uploader_enqueue(filepath)
    │       │
    │       ▼
    │    Upload Task (Core 1)
    │       ├── FTP 上传 (ftp_enabled)
    │       └── WebDAV 上传 (webdav_enabled)
    │       失败重试 3 次，连续失败 10 次暂停 5 分钟
    │
    └──→ storage_cleanup()
            │
            ▼
         检查剩余空间 < 20%？
            ├── 是 → 删除最旧录像文件，直到 ≥ 30%
            └── 否 → 无操作
```

### MJPEG 实时流数据流

```
Browser → GET /stream
    │
    ▼
HTTP Server → mjpeg_stream_handler()
    │
    ▼  循环采集（受限 2 个并发客户端）
camera_capture() → JPEG 帧
    │
    ▼
HTTP 分块传输 (multipart/x-mixed-replace)
    │  Content-Type: image/jpeg
    │  Boundary: --frame
    │
    ▼
Browser 实时渲染
```

---

## 5. FreeRTOS 任务表

| 任务名 | 函数 | 优先级 | 核心 | 栈大小 | 周期/触发 | 文件 |
|--------|------|--------|------|--------|-----------|------|
| `recorder` | `recording_task` | 5 (configMAX-2) | Core 0 | 4096 B | 持续循环（帧采集） | video_recorder.c |
| `upload` | `upload_task` | 3 | Core 1 | 6144 B | 队列阻塞等待 | nas_uploader.c |
| `sd_monitor` | `sd_monitor_task` | 2 | Core 1 | 3072 B | 10 秒轮询 | main.c |
| `boot_btn` | `boot_button_monitor` | 1 | 任意 | 2048 B | 200ms 轮询 | main.c |
| `health_mon` | `health_monitor_task` | 1 | Core 1 | 3072 B | 60 秒轮询 | main.c |
| `httpd` | ESP-IDF 内置 | 默认 | — | 8192 B | 事件驱动 | web_server.c |
| `main` | `app_main` | 1 | Core 0 | 8192 B | 初始化后返回 | main.c |

### 任务调度策略

- **Core 0**：运行录像任务（高优先级），确保帧采集不被抢占导致丢帧
- **Core 1**：运行上传、SD 监控、健康监控等非实时任务
- **BOOT 按钮监控**：不绑定核心，低优先级轮询
- **Web 服务器**：由 ESP-IDF httpd 库管理，栈 8192 字节

---

## 6. 分区表

来自 `partitions.csv`，使用自定义分区表：

| 名称 | 类型 | 子类型 | 偏移 | 大小 | 说明 |
|------|------|--------|------|------|------|
| `nvs` | data | nvs | 0x9000 | 24 KB (0x6000) | 非易失性存储（配置持久化） |
| `phy_init` | data | phy | 0xF000 | 4 KB (0x1000) | PHY 校准数据 |
| `factory` | app | factory | 0x10000 | 3584 KB (0x380000) | 固件应用程序 |
| `storage` | data | spiffs | 自动 | 256 KB (0x40000) | Web UI 静态资源 |

**总 Flash 大小**：8 MB

> **注意**：当前无 OTA 分区，仅 factory 应用。如需 OTA 支持，需修改 `partitions.csv` 将 factory 拆分为 `ota_0` + `ota_1` + `ota_data`。

---

## 7. Web API 端点

服务器运行在端口 80，共 12 个 URI 处理器：

| 方法 | 路径 | 认证 | 说明 |
|------|------|------|------|
| GET | `/api/status` | 否 | 设备状态（录像、WiFi、存储、摄像头） |
| GET | `/api/config` | 否 | 当前配置（密码字段返回 `****`） |
| POST | `/api/config` | 是 | 修改配置 |
| GET | `/api/files` | 否 | 录像文件列表 |
| DELETE | `/api/files` | 是 | 删除指定文件 |
| GET | `/api/download?name=xxx` | 否 | 下载录像文件 |
| POST | `/api/files/batch` | 是 | 批量删除文件 |
| GET | `/api/scan` | 否 | WiFi AP 扫描 |
| POST | `/api/time` | 是 | 手动设置时间 |
| POST | `/api/record?action=start\|stop` | 是 | 录像控制 |
| POST | `/api/reset` | 是 | 恢复出厂设置 |
| OPTIONS | `/*` | 否 | CORS 预检 |
| GET | `/*` | 否 | 静态文件（Web UI） |

| POST | `/api/files/batch` | 是 | 批量删除文件 |
| GET | `/metrics` | 否 | Prometheus 监控指标 |

认证方式：通过 `X-Password` 请求头或 `?password=xxx` 查询参数传递管理密码。

---

## 8. Web UI

4 个 HTML 页面，烧录至 SPIFFS 分区，由通配 GET 处理器提供服务：

| 页面 | 文件 | 功能 |
|------|------|------|
| 首页 | `index.html` | 状态总览 |
| 配置 | `config.html` | WiFi / NAS / 摄像头参数配置 |
| 文件 | `files.html` | 录像文件浏览、下载、删除 |
| 预览 | `preview.html` | MJPEG 实时流预览 |