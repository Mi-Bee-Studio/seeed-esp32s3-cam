# ESP32-S3 鹦鹉监控摄像头固件

基于 ESP32-S3 的智能监控摄像头固件，专为鹦鹉等宠物监控而设计。支持WiFi AP/STA模式、MJPEG实时流、视频分段录制、NAS自动上传等功能。

## 功能特性

- 📹 **高清视频流** - 支持VGA(640x480)、SVGA(800x600)、XGA(1024x768)分辨率
- 🎥 **MJPEG实时预览** - 低延迟实时视频流，支持多客户端连接
- 📁 **视频分段录制** - 自动分段录制到TF卡，支持循环录制
- ☁️ **NAS自动上传** - 支持FTP和WebDAV协议自动上传视频文件
- 🌐 **WiFi双模式** - 支持AP模式和STA模式（自动连接WiFi）
- 📱 **Web管理界面** - 现代化Web界面，支持配置管理和文件操作
- 🔧 **RESTful API** - 完整的API接口，支持远程控制和配置
- 🔄 **OTA升级** - 支持在线固件更新
- 💾 **TF卡监控** - 自动检测TF卡插入/拔出，支持循环存储管理
- 🚨 **状态指示灯** - LED显示设备运行状态和网络状态
- ⏰ **时间同步** - 自动NTP时间同步或手动设置时间

## 硬件要求

- **主控芯片**: ESP32-S3-WROOM-1-N8R8
- **摄像头模块**: ESP32-Camera (支持OV2640/OV3660传感器)
- **存储**: TF卡槽（建议Class 10，32GB以上）
- **LED**: 状态指示灯（GPIO2）
- **复位**: BOOT按钮（GPIO0，长按5秒恢复出厂设置）

## 快速开始

### 1. 环境准备

```bash
# 安装ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh

# 设置环境变量
. ./export.sh
```

### 2. 编译固件

```bash
# 克隆项目
git clone <repository-url>
cd esp32s3-camera

# 配置默认设置
cp sdkconfig.defaults sdkconfig

# 编译
idf.py build
```

### 3. 烧录固件

```bash
# 连接ESP32-S3到USB
# 烧录固件
idf.py -p /dev/ttyUSB0 flash

# 监视串口输出
idf.py -p /dev/ttyUSB0 monitor
```

### 4. 首次配置

1. 烧录后设备启动，默认进入AP模式
2. 连接WiFi网络：`ParrotCam_AP`
3. 访问Web界面：`http://192.168.4.1`
4. 使用默认密码登录：`admin` / `12345678`
5. 配置WiFi网络和NAS设置

## 项目结构

```
esp32s3-camera/
├── main/
│   ├── main.c              # 主程序入口，系统初始化
│   ├── config_manager.h    # 配置管理器头文件
│   ├── config_manager.c    # 配置管理器实现
│   ├── wifi_manager.h      # WiFi管理器头文件
│   ├── wifi_manager.c      # WiFi管理器实现
│   ├── web_server.h        # Web服务器头文件
│   ├── web_server.c        # Web服务器实现
│   ├── camera_driver.h     # 摄像头驱动头文件
│   ├── camera_driver.c     # 摄像头驱动实现
│   ├── storage_manager.h   # 存储管理器头文件
│   ├── storage_manager.c   # 存储管理器实现
│   ├── status_led.h        # 状态LED头文件
│   ├── status_led.c        # 状态LED实现
│   ├── video_recorder.h    # 视频录制器头文件
│   ├── video_recorder.c    # 视频录制器实现
│   ├── mjpeg_streamer.h    # MJPEG流媒体头文件
│   ├── mjpeg_streamer.c    # MJPEG流媒体实现
│   ├── nas_uploader.h      # NAS上传器头文件
│   ├── nas_uploader.c      # NAS上传器实现
│   └── time_sync.h         # 时间同步头文件
├── CMakeLists.txt          # 项目配置文件
└── sdkconfig.defaults      # 默认配置文件
```

## API 接口

### 基础信息

| 方法 | 路径 | 描述 |
|------|------|------|
| GET | `/api/status` | 获取设备状态信息 |
| GET | `/api/config` | 获取当前配置 |
| POST | `/api/config` | 更新配置 |
| GET | `/api/files` | 获取录制文件列表 |
| DELETE | `/api/files?name=xxx` | 删除指定文件 |
| GET | `/api/download?name=xxx` | 下载指定文件 |
| GET | `/api/scan` | 扫描可用WiFi网络 |
| POST | `/api/time` | 手动设置时间 |
| POST | `/api/record?action=start|stop` | 控制录制状态 |
| POST | `/api/reset` | 恢复出厂设置 |

### 状态接口示例

```json
GET /api/status
{
  "ok": true,
  "data": {
    "recording": "recording",
    "current_file": "20240424_120000_001.avi",
    "sd_free_percent": 75.5,
    "wifi_ssid": "MyWiFi",
    "wifi_state": "STA",
    "ip": "192.168.1.100",
    "camera": "OV2640",
    "resolution": "SVGA (800x600)",
    "time_synced": true,
    "uptime": 3600,
    "last_upload": "20240424_115959_001.avi",
    "upload_queue": 2
  }
}
```

### 配置接口示例

```json
GET /api/config
{
  "ok": true,
  "data": {
    "wifi_ssid": "MyWiFi",
    "wifi_pass": "****",
    "device_name": "ParrotCam",
    "ftp_host": "192.168.1.200",
    "ftp_port": 21,
    "ftp_user": "user",
    "ftp_path": "/ParrotCam",
    "ftp_enabled": true,
    "webdav_url": "http://192.168.1.200/webdav",
    "webdav_user": "user",
    "webdav_enabled": false,
    "resolution": 1,
    "fps": 10,
    "segment_sec": 300,
    "jpeg_quality": 12,
    "web_password": "****"
  }
}
```

## LED 指示灯

| 状态 | LED模式 | 描述 |
|------|---------|------|
| LED_STARTING | 常亮 | 系统启动中 |
| LED_AP_MODE | 慢闪(1秒) | AP模式运行中 |
| LED_WIFI_CONNECTING | 快闪(200ms) | WiFi连接中 |
| LED_RUNNING | 熄灭 | 正常运行(录制中) |
| LED_ERROR | 双闪 | 错误状态(如TF卡故障) |

## TF 卡配置覆盖

设备支持通过TF卡覆盖配置，文件格式：

### config/wifi.txt
```
SSID=YourWiFiNetwork
PASS=YourWiFiPassword
```

### config/nas.txt
```
FTP_HOST=192.168.1.200
FTP_PORT=21
FTP_USER=ftpuser
FTP_PASS=ftppassword
FTP_PATH=/recordings
FTP_ENABLED=true
WEBDAV_URL=http://192.168.1.200/webdav
WEBDAV_USER=webdavuser
WEBDAV_PASS=webdavpass
WEBDAV_ENABLED=false
```

**配置优先级**: TF卡配置 > NVS存储配置 > 默认配置

## 恢复出厂设置

### 方法一：按钮复位
1. 开机状态下长按BOOT按钮5秒
2. LED会闪烁红色表示正在执行恢复
3. 设备自动重启并恢复默认配置

### 方法二：Web界面
1. 登录Web管理界面
2. 进入设置页面
3. 点击"恢复出厂设置"按钮
4. 设备会自动重启并恢复默认配置

### 方法三：API调用
```bash
curl -X POST http://设备IP/api/reset \
  -H "Content-Type: application/json" \
  -d '{"password": "当前密码"}'
```

## 默认配置

- **设备名称**: ParrotCam
- **Web登录密码**: admin
- **AP模式SSID**: ParrotCam_AP
- **AP模式IP**: 192.168.4.1
- **视频格式**: AVI (MJPEG编码)
- **默认分辨率**: SVGA (800x600)
- **默认帧率**: 10 FPS
- **分段时长**: 300秒(5分钟)
- **JPEG质量**: 12 (数值越小质量越好)

## 故障排除

### 常见问题

1. **无法连接WiFi**
   - 检查WiFi密码是否正确
   - 确认设备是否在AP模式
   - 尝试重启设备

2. **TF卡无法识别**
   - 检查TF卡格式化为FAT32
   - 尝试更换TF卡
   - 确认TF卡容量在32GB以下

3. **视频录制失败**
   - 检查TF卡空间是否充足
   - 确认TF卡读写速度符合要求
   - 检查摄像头模块是否正确安装

4. **Web界面无法访问**
   - 确认设备IP地址
   - 检查网络连接
   - 尝试清除浏览器缓存

### 调试信息

通过串口监视器可以查看详细日志：
```bash
idf.py -p /dev/ttyUSB0 monitor
```

## 许可证

MIT License

## 作者

ParrotCam Team

## 贡献

欢迎提交Issue和Pull Request来改进这个项目。