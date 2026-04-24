# ParrotCam v0.1

基于 [Seeed Studio XIAO ESP32-S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) 开发板的智能监控摄像头固件。支持 MJPEG 实时视频流、AVI 分段录像、FTP/WebDAV 自动上传、Web 管理界面，专为宠物监控等长时间录制场景设计。

固件使用 ESP-IDF 框架开发，针对 ESP32-S3 的 8MB Octal PSRAM 进行了优化，可在资源受限的嵌入式环境下稳定运行实时视频采集、编码和传输。

---

## 目录

- [功能特性](#功能特性)
- [硬件要求](#硬件要求)
- [硬件规格](#硬件规格)
- [快速开始](#快速开始)
- [项目结构](#项目结构)
- [API 接口总览](#api-接口总览)
- [配置参数说明](#配置参数说明)
- [LED 指示灯](#led-指示灯)
- [TF 卡配置覆盖](#tf-卡配置覆盖)
- [存储管理策略](#存储管理策略)
- [启动流程](#启动流程)
- [恢复出厂设置](#恢复出厂设置)
- [分区表](#分区表)
- [故障排除](#故障排除)
- [许可证](#许可证)

---

## 功能特性

### 视频流

- **MJPEG 实时流**：基于 HTTP `multipart/x-mixed-replace` 协议，boundary 为 `frame`，浏览器可直接查看
- **多客户端支持**：最多允许 2 个客户端同时拉流（`MAX_STREAM_CLIENTS = 2`），超出时返回 503
- **三档分辨率**：VGA (640×480)、SVGA (800×600)、XGA (1024×768)，运行时可切换
- **可调帧率**：1-30 FPS，通过配置参数控制帧间延时

### 录像

- **AVI 格式录制**：每帧 JPEG 直接封装为 AVI Motion JPEG 格式，无需转码
- **自动分段**：按可配置的时间段（默认 300 秒）自动分割为独立 AVI 文件
- **录像路径**：`/sdcard/recordings/`，文件名按时间戳命名
- **循环存储**：存储空间低于 20% 时自动删除最旧录像，直至恢复至 30%
- **启动清理**：开机时自动清理上次异常断电留下的不完整 AVI 文件

### NAS 上传

- **双协议支持**：FTP 和 WebDAV，可同时启用
- **后台队列**：NAS 上传器运行在独立后台任务中，带上传队列，不阻塞主流程
- **WebDAV**：基于 HTTP PUT 上传文件，支持 MKCOL 递归创建远程目录，带指数退避重试（3 次）
- **FTP**：标准 FTP 协议，支持 connect/upload/mkdir_recursive 操作

### WiFi

- **AP/STA 双模式**：未配置 WiFi 时自动进入 AP 模式，配置后切换为 STA 模式连接路由器
- **AP 模式**：SSID 格式为 `ParrotCam-XXXX`（XXXX 为 MAC 地址后两字节的十六进制），密码 `12345678`，IP 固定 `192.168.4.1`
- **自动重连**：STA 模式下断开后自动尝试重连
- **WiFi 扫描**：通过 API 可扫描周围可用网络

### Web 管理

- **HTTP 服务器**：端口 80，提供 REST API 和静态文件服务
- **10 个 API 端点**：覆盖状态查询、配置管理、文件操作、录像控制等
- **静态文件服务**：Web UI 资源存储在 SPIFFS 分区，由 HTTP 服务器直接分发
- **CORS 支持**：所有 API 端点响应 OPTIONS 预检请求
- **密码认证**：写操作需要通过 `X-Password` 请求头或 `password` 查询参数验证

### 配置管理

- **NVS 持久化**：所有配置项存储在 NVS 分区，掉电不丢失
- **SD 卡覆盖**：支持通过 TF 卡上的配置文件覆盖 NVS 中的设置
- **优先级**：SD 卡配置 > NVS 存储 > 编译时默认值

### 系统可靠性

- **TF 卡热插拔**：10 秒轮询检测 TF 卡状态，自动恢复录像
- **硬件看门狗**：30 秒超时，任务卡死时触发 panic 重启
- **健康监控**：60 秒周期输出 heap/PSRAM/stack 使用情况，堆内存低于 20KB 报 CRITICAL，PSRAM 低于 500KB 报 WARNING
- **安全防护**：API 写操作需密码认证，防止未授权操作

---

## 硬件要求

### 引脚定义

以下引脚定义针对 XIAO ESP32-S3 Sense 开发板，直接从源码验证：

| 功能 | GPIO | 说明 |
|------|------|------|
| Camera XCLK | 10 | 相机主时钟输出 |
| Camera SIOD | 40 | SCCB 数据线 (SDA) |
| Camera SIOC | 39 | SCCB 时钟线 (SCL) |
| Camera D0 | 18 | 数据线 D0 |
| Camera D1 | 17 | 数据线 D1 |
| Camera D2 | 16 | 数据线 D2 |
| Camera D3 | 15 | 数据线 D3 |
| Camera D4 | 14 | 数据线 D4 |
| Camera D5 | 12 | 数据线 D5 |
| Camera D6 | 11 | 数据线 D6 |
| Camera D7 | 48 | 数据线 D7 |
| Camera VSYNC | 38 | 垂直同步信号 |
| Camera HREF | 47 | 水平参考信号 |
| Camera PCLK | 13 | 像素时钟 |
| SD CLK | 7 | SD 卡时钟 |
| SD CMD | 10 | SD 卡命令（与 Camera XCLK 复用） |
| SD D0 | 8 | SD 卡数据线 |
| LED | 21 | 状态指示灯（低电平有效，active low） |
| BOOT | 0 | 启动按钮（长按 5 秒恢复出厂设置） |

> **说明**：SD 卡使用 1-line SDMMC 模式（仅 D0 数据线），以释放 GPIO10 给相机 XCLK 使用。SD CMD 与 Camera XCLK 共享 GPIO10，通过 SDMMC 驱动的 1-bit 模式实现引脚复用。

### 外设要求

- **TF 卡**：FAT32 格式，建议 Class 10 及以上，容量 32GB 以内
- **摄像头模块**：板载 OV2640 或 OV3660，固件启动时自动检测传感器型号

---

## 硬件规格

| 项目 | 规格 |
|------|------|
| 主控芯片 | ESP32-S3-WROOM-1-N8R8 |
| CPU | 双核 Xtensa LX7，最高 240MHz |
| Flash | 8MB |
| PSRAM | 8MB Octal (SPIRAM_MODE_OCT) |
| 摄像头传感器 | OV2640 / OV3660（自动检测） |
| 相机缓冲 | 双帧缓冲 (fb_count = 2)，位于 PSRAM |
| 无线 | WiFi 2.4GHz (802.11 b/g/n) |
| 分区 | factory app 3.5MB + SPIFFS 256KB + NVS 24KB |

---

## 快速开始

### 环境准备

安装 ESP-IDF v5.x（也兼容 v6.0）开发环境：

```bash
# 克隆 ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf

# 安装工具链（Linux/macOS）
./install.sh
# Windows 请使用 install.bat

# 激活环境变量
. ./export.sh    # Linux/macOS
# export.bat     # Windows
```

### 编译与烧录

```bash
# 克隆项目
git clone https://github.com/<your-repo>/esp32s3-camera.git
cd esp32s3-camera

# 编译固件
idf.py build

# 烧录并打开串口监视器（替换为实际串口号）
idf.py -p COM3 flash monitor
# Linux 下串口通常为 /dev/ttyUSB0 或 /dev/ttyACM0
```

### 首次配置

1. 烧录完成后设备自动启动，LED 常亮（启动中）
2. 未配置 WiFi 时自动进入 AP 模式，LED 慢闪（1 秒周期）
3. 用手机或电脑连接 WiFi：`ParrotCam-XXXX`（密码 `12345678`）
4. 浏览器访问 `http://192.168.4.1`
5. 默认管理密码：`admin`
6. 在 Web 界面中配置家庭 WiFi 的 SSID 和密码
7. 保存配置后设备自动切换为 STA 模式并连接路由器
8. 连接成功后 LED 熄灭，录像自动开始

---

## 项目结构

```
main/
├── main.c              # 主入口，19 步启动流程，看门狗，SD 监控，健康监控
├── config_manager.c    # NVS 配置持久化，SD 卡覆盖解析，默认值定义
├── config_manager.h    # cam_config_t 结构体，配置 API 声明
├── wifi_manager.c      # WiFi AP/STA 管理，AP SSID 生成，扫描，自动重连
├── wifi_manager.h      # WiFi 状态枚举，管理 API 声明
├── web_server.c        # HTTP 服务器，12 个 URI 处理器，CORS，密码认证
├── web_server.h        # Web 服务器 API 声明
├── camera_driver.c     # OV2640/OV3660 驱动，分辨率切换，帧捕获与释放
├── camera_driver.h     # camera_frame_t 结构体，分辨率枚举，驱动 API
├── storage_manager.c   # SD 卡挂载，文件列表排序，循环清理策略（20%/30%）
├── storage_manager.h   # file_info_t 结构体，存储 API 声明
├── video_recorder.c    # AVI 录像引擎，AVI 文件头构建，分段录制，状态回调
├── video_recorder.h    # recorder_state_t 枚举，录像控制 API
├── mjpeg_streamer.c    # MJPEG 实时流服务，多客户端管理（最大 2 个）
├── mjpeg_streamer.h    # MJPEG 流注册/注销 API
├── nas_uploader.c      # NAS 上传队列调度器，后台工作任务，FTP/WebDAV 分发
├── nas_uploader.h      # 上传队列 API 声明
├── ftp_client.c        # FTP 协议客户端，socket 级实现
├── ftp_client.h        # FTP 操作 API 声明
├── webdav_client.c     # WebDAV 协议客户端，HTTP PUT/MKCOL/HEAD，递归目录创建
├── webdav_client.h     # WebDAV 操作 API 声明
├── time_sync.c         # SNTP 时间同步 + 手动时间设置
├── time_sync.h         # 时间同步 API 声明
├── status_led.c        # LED 状态机（5 种模式），FreeRTOS 软件定时器驱动
├── status_led.h        # led_status_t 枚举，LED 控制 API
├── cJSON.c             # JSON 解析/生成库（第三方单文件）
├── cJSON.h             # cJSON API 声明
├── CMakeLists.txt      # 组件注册，源文件列表，依赖声明
└── web_ui/             # Web 管理界面静态资源（SPIFFS 打包）
    ├── index.html      # 主页面（状态概览）
    ├── config.html     # 配置管理页面
    ├── preview.html    # 实时预览页面
    └── files.html      # 录像文件管理页面
```

顶层文件：

```
├── CMakeLists.txt      # 顶层 CMake 构建配置
├── partitions.csv      # 分区表定义
├── sdkconfig.defaults  # ESP-IDF 默认编译配置
└── README.md           # 本文档
```

---

## API 接口总览

所有 API 均工作在 HTTP 端口 80。写操作（POST/DELETE）需要在请求头 `X-Password` 或查询参数 `password` 中提供管理密码。

### 端点列表

| 方法 | 路径 | 认证 | 描述 |
|------|------|------|------|
| GET | `/api/status` | 否 | 获取系统状态（录像状态、SD 容量、WiFi 信息、摄像头型号等） |
| GET | `/api/config` | 否 | 获取当前完整配置（密码字段以 `****` 掩码显示） |
| POST | `/api/config` | 是 | 更新配置项，保存到 NVS 并立即生效 |
| GET | `/api/files` | 否 | 获取 TF 卡录像文件列表（按文件名排序） |
| DELETE | `/api/files?name=<文件名>` | 是 | 删除指定的录像文件 |
| GET | `/api/download?name=<文件名>` | 否 | 下载指定录像文件，Content-Type 为 video/x-msvideo |
| GET | `/api/scan` | 否 | 扫描周围 WiFi 网络，返回 SSID、信号强度、加密方式 |
| POST | `/api/time` | 是 | 手动设置系统时间（JSON body 含时间参数） |
| POST | `/api/record?action=start` | 是 | 开始录像 |
| POST | `/api/record?action=stop` | 是 | 停止录像 |
| POST | `/api/reset` | 是 | 恢复出厂设置并重启设备 |
| GET | `/stream` | 否 | MJPEG 实时视频流（multipart/x-mixed-replace，boundary=frame） |
| OPTIONS | `/*` | 否 | CORS 预检请求处理 |
| GET | `/*` | 否 | 静态文件服务（从 SPIFFS 分区读取 Web UI 资源） |

### 认证方式

需要认证的接口支持两种方式传递密码：

**方式一：HTTP 请求头**
```bash
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"resolution": 2}'
```

**方式二：URL 查询参数**
```bash
curl -X POST "http://192.168.4.1/api/record?action=start&password=admin"
```

认证失败时返回 HTTP 401 和 JSON 错误信息。

### 响应格式

所有 API 返回 JSON 格式，统一结构：

```json
{
  "ok": true,
  "data": { ... }
}
```

错误时：

```json
{
  "ok": false,
  "error": "错误描述"
}
```

---

## 配置参数说明

配置项定义在 `cam_config_t` 结构体中，通过 NVS 持久化存储。以下为完整参数列表（从 `config_manager.c` 中 `s_defaults` 验证）：

### WiFi 配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `wifi_ssid` | string[32] | `""` | WiFi 名称。为空时设备以 AP 模式启动 |
| `wifi_pass` | string[63] | `""` | WiFi 密码 |

### 设备配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `device_name` | string[31] | `"ParrotCam"` | 设备名称，用于 AP 模式 SSID 前缀 |
| `web_password` | string[31] | `"admin"` | Web 管理界面密码，用于 API 认证 |

### FTP 上传配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ftp_host` | string[63] | `""` | FTP 服务器地址 |
| `ftp_port` | uint16 | `21` | FTP 服务器端口 |
| `ftp_user` | string[31] | `""` | FTP 登录用户名 |
| `ftp_pass` | string[31] | `""` | FTP 登录密码 |
| `ftp_path` | string[127] | `"/ParrotCam"` | FTP 上传目标路径 |
| `ftp_enabled` | bool | `false` | 是否启用 FTP 自动上传 |

### WebDAV 上传配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `webdav_url` | string[127] | `""` | WebDAV 服务器 URL |
| `webdav_user` | string[31] | `""` | WebDAV 用户名 |
| `webdav_pass` | string[31] | `""` | WebDAV 密码 |
| `webdav_enabled` | bool | `false` | 是否启用 WebDAV 自动上传 |

### 视频配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `resolution` | uint8 | `1` | 分辨率：0 = VGA (640×480)，1 = SVGA (800×600)，2 = XGA (1024×768) |
| `fps` | uint8 | `10` | 帧率，范围 1-30 |
| `segment_sec` | uint16 | `300` | 录像分段时长（秒），默认 5 分钟一段 |
| `jpeg_quality` | uint8 | `12` | JPEG 压缩质量，范围 1-63，数值越低画质越好 |

---

## LED 指示灯

状态 LED 连接在 GPIO21，低电平有效（active low）。使用 FreeRTOS 软件定时器驱动，共 5 种状态模式：

| 状态 | LED 表现 | 定时器周期 | 触发场景 |
|------|---------|-----------|---------|
| `LED_STARTING` | 常亮 | 无（定时器停止） | 系统启动初始化阶段 |
| `LED_AP_MODE` | 慢闪 | 1000ms | 未配置 WiFi，以 AP 模式运行 |
| `LED_WIFI_CONNECTING` | 快闪 | 200ms | STA 模式下正在连接路由器 |
| `LED_RUNNING` | 熄灭 | 无（定时器停止） | 正常运行中（已连接 WiFi，录像进行中） |
| `LED_ERROR` | 双闪 | 200ms（基础节拍） | 错误状态（如 TF 卡故障、初始化失败等） |

**双闪时序详解**（`LED_ERROR`）：
1. 点亮 200ms（第一闪）
2. 熄灭 200ms
3. 点亮 200ms（第二闪）
4. 熄灭约 1000ms（5 个 200ms 节拍）
5. 循环重复

---

## TF 卡配置覆盖

设备支持通过 TF 卡上的配置文件覆盖 NVS 中的设置，适合批量部署时预先写入配置。

### WiFi 配置文件

路径：`/sdcard/config/wifi.txt`

```
SSID=YourWiFiNetwork
PASS=YourWiFiPassword
```

### NAS 配置文件

路径：`/sdcard/config/nas.txt`

```
FTP_HOST=192.168.1.200
FTP_PORT=21
FTP_USER=ftpuser
FTP_PASS=ftppassword
FTP_PATH=/ParrotCam
FTP_ENABLED=true
WEBDAV_URL=http://192.168.1.200/webdav
WEBDAV_USER=webdavuser
WEBDAV_PASS=webdavpass
WEBDAV_ENABLED=false
```

### 优先级

```
SD 卡配置文件  >  NVS 持久化配置  >  编译时默认值
```

设备每次启动时会检测 SD 卡上的配置文件，如果存在则覆盖 NVS 中对应的配置项。NVS 中的配置通过 Web API 修改时立即生效并持久化。

---

## 存储管理策略

录像文件存储在 TF 卡的 `/sdcard/recordings/` 目录下，文件扩展名为 `.avi`。

### 循环存储

当 TF 卡可用空间低于 **20%** 时，存储管理器自动触发清理：

1. 按文件名升序排序（时间戳命名，最旧的排在前面）
2. 逐一删除最旧的录像文件
3. 每删除一个文件后检查可用空间
4. 当可用空间恢复至 **30%** 时停止清理

相关宏定义（`storage_manager.c`）：
- `CLEANUP_LOW_PCT = 20.0f` — 触发清理阈值
- `CLEANUP_HIGH_PCT = 30.0f` — 停止清理阈值

### 启动清理

设备启动时会执行一次不完整文件清理（`recorder_cleanup_incomplete`），递归扫描 `/sdcard/recordings/` 目录，删除上次异常断电留下的不完整 AVI 录像文件。

### SD 卡热插拔

独立的 SD 监控任务以 10 秒为周期轮询检测 TF 卡状态：
- **拔出**：暂停录像，标记 SD 卡不可用
- **插入**：重新挂载 TF 卡，恢复录像

---

## 启动流程

设备上电后按以下 19 步严格顺序初始化（源自 `main.c` 的 `app_main` 函数）：

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | 初始化 NVS Flash | `nvs_flash_init()`，非易失性存储基础 |
| 2 | 初始化配置管理器 | 从 NVS 加载配置，不存在则使用默认值 |
| 3 | 初始化状态 LED | GPIO21 配置为输出，LED 常亮（`LED_STARTING`） |
| 4 | 挂载 SPIFFS | 加载 Web UI 静态资源文件系统 |
| 5 | 挂载 SD 卡 | 1-line SDMMC 模式初始化 TF 卡 |
| 5a | 清理不完整录像 | 递归删除上次异常断电留下的不完整 AVI 文件 |
| 6 | 加载 SD 卡配置覆盖 | 读取 `/sdcard/config/wifi.txt` 和 `nas.txt` |
| 7 | 初始化 WiFi | 根据 wifi_ssid 是否为空选择 AP 或 STA 模式 |
| 8 | 初始化摄像头 | 配置引脚、分辨率、JPEG 质量，检测 OV2640/OV3660 |
| 9 | 时间同步 | STA 模式下启动 SNTP 同步，AP 模式跳过 |
| 10 | 初始化 NAS 上传器 | 创建上传队列和后台工作任务 |
| 11 | 初始化视频录像器 | 配置 AVI 录像引擎和状态回调 |
| 12 | 初始化 MJPEG 流服务 | 准备实时流服务器 |
| 13 | 启动 Web 服务器 | 端口 80，注册所有 API 处理器和流端点 |
| 14 | 更新 LED 状态 | 根据当前 WiFi 状态切换 LED 模式 |
| 15 | 等待 STA 连接 | STA 模式下等待 WiFi 连接成功后自动开始录像 |
| 16 | 启动 BOOT 按钮监控 | 检测 GPIO0 长按 5 秒触发恢复出厂设置 |
| 17 | 配置看门狗 | 30 秒超时，任务卡死时触发 panic 重启 |
| 18 | 启动 SD 监控任务 | 10 秒周期轮询 SD 卡热插拔（运行在 Core 1） |
| 19 | 启动健康监控任务 | 60 秒周期输出 heap/PSRAM/stack 信息（运行在 Core 1） |

---

## 恢复出厂设置

提供三种恢复方式，均会清除 NVS 配置并重启设备：

### 方法一：BOOT 按钮长按

1. 设备运行状态下，按住 BOOT 按钮（GPIO0）
2. 持续按住 **5 秒**
3. 设备自动执行恢复出厂设置并重启
4. 重启后所有配置恢复为默认值，以 AP 模式启动

### 方法二：Web 管理界面

1. 浏览器访问设备 Web 管理界面
2. 在设置页面点击「恢复出厂设置」
3. 确认操作后设备自动重启

### 方法三：API 调用

```bash
curl -X POST http://设备IP/api/reset \
  -H "Content-Type: application/json" \
  -H "X-Password: admin"
```

---

## 分区表

分区定义在 `partitions.csv` 中，针对 8MB Flash 空间划分：

| 分区名 | 类型 | 偏移地址 | 大小 | 说明 |
|--------|------|---------|------|------|
| nvs | data/nvs | 0x9000 | 24KB (0x6000) | 非易失性配置存储 |
| phy_init | data/phy | 0xf000 | 4KB (0x1000) | PHY 校准数据 |
| factory | app/factory | 0x10000 | 3.5MB (0x380000) | 固件应用程序 |
| storage | data/spiffs | 自动 | 256KB (0x40000) | Web UI 静态资源 |

---

## 故障排除

### 日志与健康监控

通过串口监视器查看运行日志：

```bash
idf.py -p COM3 monitor
# 退出监视器: Ctrl+]
```

健康监控每 60 秒输出一次系统资源状态：

```
I (60000) main: HEALTH: heap=85432 PSRAM=3145728 rec_hwm=2048 nas_hwm=1024
```

**关键阈值**（定义在 `health_monitor_task`）：

| 指标 | 阈值 | 级别 | 日志示例 |
|------|------|------|---------|
| 可用堆内存 | < 20KB | CRITICAL | `CRITICAL: Free heap below 20KB (xxxxx bytes)` |
| 可用 PSRAM | < 500KB | WARNING | `WARNING: Free PSRAM below 500KB (xxxxx bytes)` |

### 常见问题

**1. 设备始终以 AP 模式启动，无法连接路由器**

- 检查 `wifi_ssid` 和 `wifi_pass` 配置是否正确
- 确认路由器使用 2.4GHz 频段（ESP32-S3 不支持 5GHz）
- 通过 Web 界面或 API 重新配置 WiFi 参数

**2. TF 卡无法识别**

- 确认 TF 卡已格式化为 FAT32 文件系统
- 尝试更换 TF 卡，建议使用 Class 10 及以上
- 检查串口日志中 SD 卡初始化是否报错

**3. 录像中断或文件损坏**

- 检查 TF 卡可用空间（可通过 `/api/status` 查看 `sd_free_percent`）
- 确认 TF 卡写入速度是否达标
- 设备异常断电后重启会自动清理不完整的 AVI 文件

**4. Web 界面无法访问**

- 确认设备 IP 地址（AP 模式下为 192.168.4.1）
- 检查客户端是否与设备在同一网络
- AP 模式下确保已连接到 `ParrotCam-XXXX` WiFi

**5. NAS 上传失败**

- 检查 FTP/WebDAV 服务器地址、端口、用户名和密码
- 确认设备与 NAS 在同一网络
- 查看串口日志中 FTP/WebDAV 连接错误信息

**6. 视频流画面卡顿**

- 降低分辨率或帧率（通过 `/api/config` 修改）
- 检查 WiFi 信号强度
- 确认同时拉流的客户端数量不超过 2 个

**7. 设备频繁重启**

- 检查串口日志中是否有看门狗超时（Watchdog timeout）
- 查看健康监控日志中堆内存是否低于 20KB（CRITICAL）
- 确认电源供电是否稳定（USB 供电建议 5V/1A 以上）

---

## 许可证

本项目基于 [MIT License](https://opensource.org/licenses/MIT) 开源。
