# 安装指南

## 1. 环境准备

### 必要工具

| 工具 | 版本 | 说明 |
|------|------|------|
| ESP-IDF | v5.x 或 v6.0 | 乐鑫官方开发框架 |
| Python | 3.8+ | ESP-IDF 依赖 |
| Git | 最新 | 源码管理 |
| USB 驱动 | — | CH340/CP2102 或板载 USB-CDC |

### 安装 ESP-IDF

**Windows：**

1. 下载 [ESP-IDF Tools Installer](https://dl.espressif.com/dl/esp-idf/)
2. 运行安装程序，选择下载 ESP-IDF v5.4 或更高版本
3. 安装完成后，从开始菜单打开 "ESP-IDF 5.x CMD" 终端

**Linux / macOS：**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
. ./export.sh
```

> 建议将 `alias get_idf='. ~/esp/esp-idf/export.sh'` 加入 shell 配置文件。

---

## 2. 获取源码

```bash
git clone https://github.com/Mi-Bee-Studio/esp32s3-cam.git
cd esp32s3-camera
```

项目结构：

```
esp32s3-camera/
├── main/                 # 固件源码（14 个 C 模块）
│   ├── main.c            # 入口，19 步启动流程
│   ├── camera_driver.c   # 摄像头驱动
│   ├── video_recorder.c  # AVI 录像引擎
│   ├── mjpeg_streamer.c  # MJPEG 实时流
│   ├── web_server.c      # HTTP 服务器 + REST API
│   ├── nas_uploader.c    # NAS 上传调度
│   ├── wifi_manager.c    # WiFi AP/STA 管理
│   ├── config_manager.c  # NVS 配置持久化
│   ├── storage_manager.c # SD 卡 + 循环清理
│   ├── status_led.c      # LED 状态机
│   ├── time_sync.c       # SNTP 时间同步
│   ├── ftp_client.c      # FTP 客户端
│   ├── webdav_client.c   # WebDAV 客户端
│   ├── cJSON.c/h         # JSON 解析库
│   └── web_ui/           # Web 管理界面（4 个 HTML 页面）
├── docs/                 # 项目文档
├── partitions.csv        # 分区表
└── sdkconfig.defaults    # 硬件默认配置
```

---

## 3. 编译

确保 ESP-IDF 环境已加载（终端中可执行 `idf.py`），然后：

```bash
idf.py set-target esp32s3
idf.py build
```

首次编译需要下载依赖组件，耗时 3-5 分钟。后续增量编译约 30 秒。

### 编译输出

编译成功后固件位于：
```
build/parrot_cam.bin
```

---

## 4. 烧录

### 连接设备

1. 用 USB Type-C 线连接 XIAO ESP32-S3 Sense 与电脑
2. 确认设备管理器中出现 COM 端口

### 烧录并监控

```bash
idf.py -p COM3 flash monitor
```

将 `COM3` 替换为实际端口号。

- **Windows**: `COM3`, `COM4` 等
- **Linux**: `/dev/ttyUSB0` 或 `/dev/ttyACM0`
- **macOS**: `/dev/cu.usbmodem*`

### 仅烧录（不监控）

```bash
idf.py -p COM3 flash
```

### 退出监控

按 `Ctrl+]` 退出串口监控。

---

## 5. 首次配置

### 步骤概览

```
上电 → LED 常亮（启动中） → LED 慢闪（AP 模式）
    → 连接 WiFi "ParrotCam-XXXX"
    → 浏览器打开 192.168.4.1
    → 使用密码 admin 登录
    → 配置页填写 WiFi 信息
    → 设备自动切换 STA 模式
    → LED 熄灭（正常运行，开始录像）
```

### 详细步骤

#### 1. 上电启动

烧录完成后设备自动重启，串口日志显示：

```
I (xxx) main: ParrotCam v0.1 starting...
I (xxx) main: Free heap: XXXXX  Free PSRAM: XXXXXXX
```

#### 2. 确认 AP 模式

LED 开始慢闪（1 秒周期），表示进入 AP 模式。

#### 3. 连接 WiFi

在手机或电脑上搜索 WiFi 名称：`ParrotCam-XXXX`（XXXX 为 MAC 地址后 4 位十六进制），连接该网络。

#### 4. 打开管理页面

浏览器访问 `http://192.168.4.1`，进入配置页面。

#### 5. 配置 WiFi

在配置页面填写：
- **WiFi SSID**：你的家庭/办公 WiFi 名称
- **WiFi 密码**：WiFi 密码
- 点击保存

#### 6. 等待连接

设备自动切换到 STA 模式并连接 WiFi。LED 变为快闪（200ms），连接成功后 LED 熄灭。

#### 7. 获取 IP 地址

在 AP 模式期间可通过串口日志查看分配到的 IP 地址：

```
I (xxx) wifi: STA connected, IP: 192.168.1.xxx
```

#### 8. 后续访问

通过 STA 模式 IP 地址访问管理页面：`http://192.168.1.xxx`

### SD 卡配置覆盖（可选）

如果无法通过 Web 界面配置 WiFi，可在 TF 卡根目录创建配置文件：

**`/sdcard/config/wifi.txt`**：
```
WIFI_SSID=你的WiFi名称
WIFI_PASS=你的WiFi密码
```

**`/sdcard/config/nas.txt`**：
```
FTP_HOST=192.168.1.100
FTP_PORT=21
FTP_USER=username
FTP_PASS=password
FTP_PATH=/recordings
FTP_ENABLED=true
```

将 TF 卡插入设备后重启，系统自动读取并覆盖 NVS 配置。

---

## 6. 验证

### 串口日志检查

正常启动后应看到以下关键日志：

```
I (xxx) storage: SD card mounted OK
I (xxx) camera: Sensor: OV2640, Resolution: SVGA, Quality: 12
I (xxx) web: Web server started on port 80
I (xxx) main: Recording started
I (xxx) main: ParrotCam v0.1 initialized successfully
I (xxx) main: Camera: OV2640 @ SVGA
I (xxx) main: WiFi: STA, IP: 192.168.1.xxx
```

### Web 界面检查

1. 浏览器访问 `http://<设备IP>`
2. 首页显示设备状态（WiFi 已连接、录像中、存储空间等）
3. 进入 "预览" 页面，确认能看到实时画面

### 实时流测试

直接在浏览器访问视频流地址：

```
http://<设备IP>/stream
```

应看到连续的 MJPEG 实时画面。最大支持 2 个并发客户端。

### API 状态检查

```bash
curl http://<设备IP>/api/status
```

返回 JSON 包含 `recording`、`wifi_state`、`sd_available`、`camera_sensor` 等字段，全部正常即表示系统运行正常。
