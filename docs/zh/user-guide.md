# 使用手册

> ESP32-S3 摄像头监控固件的日常使用指南

## 概述

本文档介绍 ESP32-S3 摄像头监控固件的日常使用方法，包括 Web 管理界面操作、配置参数说明、录像与存储管理、NAS 上传等功能。

## Web 管理界面

设备内置 HTTP 服务器，提供 Web 管理界面和 REST API。

### 访问地址

| 模式 | 地址 |
|------|------|
| AP 模式 | `http://192.168.4.1` |
| STA 模式 | `http://<设备IP>` |

- AP 模式：连接 WiFi 热点 `MiBeeHomeCam-XXXX`（密码 `12345678`），然后访问 `http://192.168.4.1`
- STA 模式：设备连接路由器后，通过路由器分配的 IP 访问

### 登录密码

默认管理密码：`admin`。涉及写操作的 API 请求需要通过 `X-Password` 请求头或 `?password=xxx` 查询参数传递密码。

### 页面说明

| 页面 | 功能 |
|------|------|
| index | 状态总览（录像状态、WiFi、存储、摄像头型号、运行时间） |
| config | 配置管理（WiFi、录像、NAS 上传等全部参数） |
| preview | 实时视频预览（浏览器内 MJPEG 流） |
| files | 录像文件管理（浏览、下载、删除） |

### 仪表盘页面

![仪表盘](images/index-dashboard.png)

仪表盘显示实时设备状态：录像状态、当前文件、WiFi 连接、SD 卡用量、摄像头型号、芯片温度和运行时间。

### 配置页面

![配置页面](images/config-page.png)

所有设备参数均可在此修改：WiFi 凭据、视频分辨率/帧率/画质、录像分段时长、WebDAV/HTTP(S) 上传设置、摄像头翻转和系统密码。

### 文件管理页面

![文件管理](images/files-page.png)

按日期组织的录像文件浏览。支持折叠展开日期分组、批量选择、文件夹级下载/删除和持久化登录认证。

### 视频预览页面

![视频预览](images/preview-page.png)

浏览器内直接查看实时 MJPEG 视频流，最多支持 2 个同时观看的客户端。

## 配置管理

### 通过 Web 界面

打开 `config` 页面，直接修改各项参数并保存。修改后立即生效，同时持久化到 NVS 闪存。

### 通过 API

使用 `POST /api/config` 接口修改配置，需附带 `X-Password` 认证：

```bash
# 修改 WiFi
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' -H 'X-Password: admin' \
  -d '{"wifi_ssid":"MyWiFi","wifi_pass":"mypassword"}'

# 修改视频参数
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' -H 'X-Password: admin' \
  -d '{"resolution":1,"fps":10,"jpeg_quality":12,"segment_sec":300}'
```

> 密码字段传 `"****"` 表示不修改，传实际值则更新。

### 完整配置参数表

#### WiFi 配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `wifi_ssid` | string | `""` | WiFi 名称，为空则进入 AP 模式 |
| `wifi_pass` | string | `""` | WiFi 密码 |

#### 设备配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `device_name` | string | `"MiBeeHomeCam"` | 设备名称 |
| `web_password` | string | `"admin"` | Web 管理密码 |

#### 上传方式配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `upload_method` | uint8 | `0` | 上传方式：0=禁用, 1=WebDAV, 2=HTTP(S) |
| `upload_base_path` | string | `"/MiBeeHomeCam"` | 上传基础路径 |
| `webdav_url` | string | `""` | WebDAV 服务器 URL |
| `webdav_user` | string | `""` | WebDAV 用户名 |
| `webdav_pass` | string | `""` | WebDAV 密码（GET 请求返回 `****`） |
| `webdav_enabled` | bool | `false` | 是否启用 WebDAV 上传 |
| `http_upload_url` | string | `""` | HTTP(S) 上传完整 URL |
| `http_upload_user` | string | `""` | HTTP(S) 用户名 |
| `http_upload_pass` | string | `""` | HTTP(S) 密码（GET 请求返回 `****`） |
| `http_upload_skip_cert_verify` | bool | `false` | 跳过 HTTPS 证书验证 |

#### 摄像头配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `vflip` | bool | `false` | 垂直翻转 |
| `hmirror` | bool | `false` | 水平镜像 |
#### 视频配置

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `resolution` | uint8 | `1` | 分辨率：0=VGA(640×480), 1=SVGA(800×600), 2=XGA(1024×768) |
| `fps` | uint8 | `10` | 帧率，范围 1-30 |
| `segment_sec` | uint16 | `300` | 录像分段时长（秒） |
| `jpeg_quality` | uint8 | `12` | JPEG 画质，1-63，数值越低画质越好 |

## LED 指示灯

板载 LED（GPIO21，低电平有效）通过不同闪烁模式反映设备当前状态：

| 模式 | LED 表现 | 含义 |
|------|----------|------|
| LED_STARTING | 常亮 | 系统启动中 |
| LED_AP_MODE | 慢闪（1 秒周期） | AP 热点模式，等待配置 |
| LED_WIFI_CONNECTING | 快闪（200ms 周期） | 正在连接 WiFi |
| LED_RUNNING | 熄灭 | 正常运行，录像中 |
| LED_ERROR | 双闪 | 错误状态（SD 卡故障等） |

**双闪时序详解**（LED_ERROR）：

```
ON 200ms → OFF 200ms → ON 200ms → OFF 1000ms → 循环
|← 第1次闪烁 →|← 第2次闪烁 →|←  长间隔   →|
```

## WiFi 配置

### AP 模式

设备首次启动或未配置 WiFi 时自动进入 AP 模式：

- SSID：`MiBeeHomeCam-XXXX`（XXXX 为 MAC 地址后 4 位十六进制）
- 密码：`12345678`
- IP 地址：`192.168.4.1`
- 加密方式：WPA2-PSK
- 最大连接数：4

### 切换到 STA 模式

配置 `wifi_ssid` 和 `wifi_pass` 后重启，设备自动连接路由器。设置后需重启切换模式。

### 自动重连

STA 模式下断开后，设备每 60 秒自动尝试重连，无需手动干预。

## 录像管理

### 自动录像

设备启动后，如果 SD 卡就绪，会自动开始录像。

### 手动控制

通过 API 手动开始或停止录像：

```bash
# 开始录像
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# 停止录像
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

### 录像格式

- **格式**：AVI（MJPEG 编码）
- **分段**：按 `segment_sec`（默认 300 秒 = 5 分钟）自动分段
- **文件命名**：`REC_YYYYMMDD_HHMMSS.avi`（如 `REC_20260424_143000.avi`）
- **存储路径**：`/sdcard/recordings/YYYY-MM/DD/`（按日期分目录）

每个分段完成后自动触发：NAS 上传 → 存储清理 → 开启下一段。

## 存储管理

### 录像路径

```
/sdcard/recordings/
├── 2026-04/
│   ├── 24/
│   │   ├── REC_20260424_080000.avi
│   │   ├── REC_20260424_080500.avi
│   │   └── ...
│   └── 25/
│       └── ...
```

### 循环存储

当 SD 卡剩余空间不足时自动清理：

- **低于 20%**：开始删除最早的录像文件
- **持续清理直到 30%**：每次分段录像完成后检查
- 安全限制：单次清理最多删除 100 个文件

### 启动清理

设备启动时会自动扫描 `/sdcard/recordings/` 目录，删除 RIFF 头大小为 0 的不完整 AVI 文件（通常是异常断电导致）。

### SD 卡热插拔

设备每 10 秒检测一次 SD 卡状态：

| 事件 | 行为 |
|------|------|
| SD 卡拔出 | 停止当前录像，LED 显示错误状态 |
| SD 卡插入 | 重新挂载，自动恢复录像，LED 恢复正常 |

## NAS 上传

### 上传方式配置

通过 `upload_method` 字段选择上传方式（互斥，只能选一种）：

| 值 | 方式 | 说明 |
|----|------|------|
| `0` | 禁用 | 不上传，仅本地 SD 卡存储 |
| `1` | WebDAV | 使用 WebDAV 协议上传到 NAS |
| `2` | HTTP(S) | 使用 HTTP PUT 上传，支持 HTTPS |
#### WebDAV 上传配置

| 参数 | 说明 |
|------|------|
| `webdav_url` | WebDAV 服务器完整 URL |
| `webdav_user` | WebDAV 用户名 |
| `webdav_pass` | WebDAV 密码 |
| `webdav_enabled` | 设为 `true` 启用（需配合 `upload_method=1`） |

#### HTTP(S) 上传配置

| 参数 | 说明 |
|------|------|
| `http_upload_url` | HTTP(S) 上传完整 URL |
| `http_upload_user` | HTTP(S) 用户名 |
| `http_upload_pass` | HTTP(S) 密码 |
| `http_upload_skip_cert_verify` | 跳过 HTTPS 证书验证（自签名证书时设为 `true`） |
| `upload_base_path` | 上传基础路径（默认 `/MiBeeHomeCam`） |

**互斥说明**：上传方式只能选一种，不能同时启用。切换方式后，之前的上传配置仍然保留在 NVS 中，但不会被使用。

### 上传队列

- 队列容量：16 个文件
- 每个文件最多重试 3 次
- 连续失败 10 次后暂停上传 5 分钟
- 后台任务异步处理，不影响录像

### 查看上传状态

```bash
curl http://192.168.4.1/api/status
```

返回 `upload_queue`（待上传数）和 `last_upload`（上次上传时间）。

## TF 卡配置覆盖

设备启动时会读取 SD 卡上的配置文件，优先级高于 NVS 中保存的配置。

### wifi.txt

路径：`/sdcard/config/wifi.txt`

```
SSID=你的WiFi名称
PASS=你的WiFi密码
```

### nas.txt

路径：`/sdcard/config/nas.txt`

UPLOAD_METHOD=webdav
WEBDAV_URL=https://dav.example.com/path
WEBDAV_USER=user
WEBDAV_PASS=pass
WEBDAV_ENABLED=false
HTTP_UPLOAD_URL=
HTTP_UPLOAD_USER=
HTTP_UPLOAD_PASS=
HTTP_UPLOAD_SKIP_CERT=true
UPLOAD_BASE_PATH=/MiBeeHomeCam
```

以 `#` 开头的行会被忽略。

### 配置优先级

```
TF 卡配置文件 > NVS 闪存 > 默认值
```

从 TF 卡读取的配置会同步写回 NVS，确保一致性。

## 视频流

### 浏览器查看

直接在浏览器中打开：

```
http://<设备IP>/stream
```

### 客户端限制

最多支持 **2 个**同时连接的流客户端。超过限制时返回 503 错误。

### 调整画质

通过 `POST /api/config` 修改视频参数：

| 参数 | 可选值 | 说明 |
|------|--------|------|
| `resolution` | 0, 1, 2 | VGA(640×480)、SVGA(800×600)、XGA(1024×768) |
| `fps` | 1-30 | 帧率，越高越流畅但占用更多带宽 |
| `jpeg_quality` | 1-63 | 画质，数值越低画质越好（建议 10-20） |

### RTSP 实时流

除了 HTTP MJPEG 流外，设备还提供 RTSP 实时流服务：

```
rtsp://<设备IP>:554/stream
```

**VLC 播放**：打开 VLC → 媒体 → 打开网络串流 → 输入 `rtsp://<设备IP>:554/stream`

**最大并发客户端**：2 个

### 摄像头翻转设置

通过 `POST /api/config` 修改摄像头翻转方向：

```bash
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' -H 'X-Password: admin' \
  -d '{"vflip":true,"hmirror":true}'
```

| 参数 | 说明 |
|------|------|
| `vflip` | 垂直翻转（上下颠倒） |
| `hmirror` | 水平镜像（左右翻转） |

> ⚠️ 翻转设置对**新采集的帧**立即生效。已录制的 AVI 文件不受影响。建议在停止录像后修改翻转设置。
## 时间管理

### 自动同步

STA 模式下设备启动后会自动通过 NTP 同步时间：

- 服务器：`cn.pool.ntp.org`、`ntp.aliyun.com`
- 同步超时：5 秒（超时后异步继续）

### 手动设置

```bash
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year":2026,"month":4,"day":24,"hour":14,"min":30,"sec":0}'
```

## 恢复出厂设置

### 方法一：BOOT 按钮

按住 BOOT 按钮（GPIO0）**5 秒**，设备会自动恢复出厂设置并重启。

### 方法二：Web 界面

在 Web 配置页面点击"恢复出厂设置"按钮。

### 方法三：API

```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

### 恢复结果

所有配置参数恢复为默认值（WiFi 清空、密码重置为 `admin`），设备重启后进入 AP 模式。TF 卡中的录像文件不会被删除。

---

## 功能互斥与资源限制

### ⚠️ 上传方式互斥
上传方式（upload_method）只能选择一种：禁用、WebDAV 或 HTTP(S)。切换方式后，之前的上传配置仍然保留在 NVS 中，但不会被使用。

### ⚠️ 视频流并发限制
系统共有 3 个视频流消费者共享摄像头帧缓冲：
1. **录像任务** (recording_task) — Core 0，最高优先级
2. **MJPEG HTTP 流** (/stream) — 最多 2 个客户端
3. **RTSP 流** (rtsp://<IP>:554/stream) — 最多 2 个客户端

摄像头帧缓冲池大小为 2（双缓冲），当 3 个消费者同时活跃时，可能出现帧竞争。建议：
- 正常使用：录像 + 1 路 HTTP 流 或 录像 + 1 路 RTSP 流
- 不建议同时开启 HTTP 流和 RTSP 流给多个客户端观看
- 录像任务优先级最高，不会因流客户端增加而丢帧

### ⚠️ RTSP 与 HTTP 流的关系
- RTSP 和 HTTP MJPEG 流是**独立的**，可以同时运行
- 两者共享摄像头帧缓冲，但不共享网络连接
- RTSP 使用 TCP-interleaved 模式（端口 554），不使用 UDP
- RTSP 不支持认证（v1 版本）

### ⚠️ 翻转设置不影响已录制的视频
vflip/hmirror 设置对**新采集的帧**立即生效，但：
- 已录制的 AVI 文件不受影响
- 录像过程中可以切换翻转，但同一段视频内可能出现方向不一致的帧
- 建议在停止录像后修改翻转设置

### ⚠️ 上传方式从旧版本升级
如果从旧版本（支持 FTP）升级，设备会自动迁移 NVS 配置：
- 旧 `ftp_enabled=true` → `upload_method=1`（WebDAV）
- 旧 `webdav_enabled=true` → `upload_method=1`（WebDAV）
- 旧 FTP 相关 NVS 键值会被自动删除
