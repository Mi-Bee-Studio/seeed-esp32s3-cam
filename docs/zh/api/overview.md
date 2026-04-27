# API 概述

> ESP32-S3 摄像头监控固件 REST API 文档

[状态查询](status.md) | [配置接口](config.md) | [文件管理](files.md) | [设备控制](control.md) | [视频流](stream.md) | [WiFi 扫描](wifi.md) | [完整示例](examples.md)

---

本固件基于 `web_server.c`、`mjpeg_streamer.c`、`config_manager.c` 源码编写，最后更新：2026-04-24。

## 基础地址

| 模式 | 地址 |
|------|------|
| AP 模式（默认） | `http://192.168.4.1` |
| STA 模式 | `http://<设备IP>` |

## 认证机制

部分接口需要密码认证，支持两种方式传递密码：

| 方式 | 格式 | 示例 |
|------|------|------|
| 请求头 | `X-Password: <密码>` | `X-Password: admin` |
| 查询参数 | `?password=<密码>` | `?password=admin` |

- **默认密码**：`admin`（可通过 `POST /api/config` 修改 `web_password` 字段）
- 认证逻辑优先检查 `X-Password` 请求头，其次检查 `password` 查询参数
- 认证失败返回 `401 Unauthorized`，响应体：`{"ok": false, "error": "Unauthorized"}`

## 统一响应格式

所有 API 接口返回 `Content-Type: application/json`，格式统一为：

**成功响应**：
```json
{
  "ok": true,
  "data": { ... }
}
```

**错误响应**：
```json
{
  "ok": false,
  "error": "错误描述信息"
}
```

> 注意：`POST /api/config` 成功时返回 `{"ok": true}`，不含 `data` 字段。

## CORS 跨域支持

所有 HTTP 响应（包括错误响应和静态文件）均包含以下 CORS 头：

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Password
```

`OPTIONS` 请求（预检请求）返回上述 CORS 头和空响应体，状态码 200。

## 接口总览

| # | 方法 | 路径 | 认证 | 描述 |
|---|------|------|------|------|
| 1 | GET | `/api/status` | 否 | 获取设备状态 |
| 2 | GET | `/api/config` | 否 | 获取当前配置 |
| 3 | POST | `/api/config` | **是** | 更新配置 |
| 4 | GET | `/api/files` | 否 | 获取录制文件列表 |
| 5 | DELETE | `/api/files?name=xxx` | **是** | 删除指定文件 |
| 6 | GET | `/api/download?name=xxx` | 否 | 下载指定文件 |
| 7 | GET | `/api/scan` | 否 | 扫描 WiFi 网络 |
| 8 | POST | `/api/time` | **是** | 手动设置系统时间 |
| 9 | POST | `/api/record?action=start\|stop` | **是** | 控制录像 |
| 10 | POST | `/api/reset` | **是** | 恢复出厂设置 |
| 11 | GET | `/stream` | 否 | MJPEG 实时视频流 |
| 12 | OPTIONS | `/*` | 否 | CORS 预检请求 |
| 13 | GET | `/*` | 否 | 静态文件（SPIFFS） |

## HTTP 状态码

| 状态码 | 含义 | 触发场景 |
|--------|------|----------|
| 200 | 成功 | 请求处理成功 |
| 400 | 请求错误 | 参数缺失、JSON 格式错误、路径遍历检测 |
| 401 | 认证失败 | 密码错误或未提供密码（需要认证的接口） |
| 404 | 资源不存在 | 文件不存在、静态文件未找到 |
| 500 | 服务器内部错误 | WiFi 扫描失败、时间设置失败 |
| 503 | 服务不可用 | MJPEG 流客户端连接数已达上限 |

## 附录

### SD 卡路径

| 用途 | 路径 |
|------|------|
| 录制文件 | `/sdcard/recordings/` |
| WiFi 配置覆盖 | `/sdcard/config/wifi.txt` |
| NAS 配置覆盖 | `/sdcard/config/nas.txt` |

### 配置优先级

```
SD 卡配置文件 > NVS 存储 > 编译时默认值
```

### SPIFFS 分区

Web 界面静态文件存储在 SPIFFS 分区（约 256KB），路径前缀 `/spiffs/`。
Web 界面静态文件存储在 SPIFFS 分区（约 256KB），路径前缀 `/spiffs/`。

### 服务器配置

| 参数 | 值 |
|------|-----|
| 端口 | 80 |
| 最大 URI 处理器 | 20 |
| 任务栈大小 | 8192 字节 |
| 接收超时 | 30 秒 |
| 发送超时 | 30 秒 |
| URI 匹配模式 | 通配符（wildcard） |

## `/api/status` 响应字段

`/api/status` 返回包含以下字段的 JSON：

```json
{
  "ok": true,
  "data": {
    "recording": false,
    "wifi_state": "STA_CONNECTED",
    "sd_available": 75.5,
    "sd_total": 29.7,
    "sd_free_percent": 75.5,
    "camera_sensor": "OV2640",
    "camera_res": "SVGA",
    "camera_quality": 12,
    "chip_temp": 42.5,
    "free_heap": 185432,
    "free_psram": 4194304
  }
}
```

### 新增字段：`chip_temp`

- **类型**：`number`（温度，摄氏度）
- **范围**：20-80°C（ESP32-S3 典型工作范围）
- **说明**：ESP32-S3 内核温度
- **新增版本**：支持温度监控功能的版本

### `/api/files/batch` - 批量文件操作

批量删除录制文件，单次请求可删除多个文件。

**请求**：
```json
{
  "names": ["file1.avi", "file2.avi", "file3.avi"]
}
```

**响应**：
```json
{
  "ok": true,
  "data": {
    "deleted": 2,
    "failed": 1
  }
}
```

**认证**：需要（X-Password 请求头或 ?password= 查询参数）

---

### `/metrics` - Prometheus 监控指标

以 Prometheus text exposition 格式暴露设备指标，供外部监控系统使用。

**请求**：`GET /metrics`

**响应**：`text/plain`（无需认证）

**可用指标**：

```text
# HELP esp_free_heap_bytes Free heap memory bytes
# TYPE esp_free_heap_bytes gauge
esp_free_heap_bytes 185432

# HELP esp_free_psram_bytes Free PSRAM memory bytes
# TYPE esp_free_psram_bytes gauge
esp_free_psram_bytes 4194304

# HELP esp_chip_temp_celsius ESP32-S3 chip temperature
# TYPE esp_chip_temp_celsius gauge
esp_chip_temp_celsius 42.5

# HELP esp_recording_state Recording state (0=IDLE, 1=RECORDING, 2=PAUSED, 3=ERROR)
# TYPE esp_recording_state gauge
esp_recording_state 0

# HELP sd_free_bytes SD card free bytes
# TYPE sd_free_bytes gauge
sd_free_bytes 22385920

# HELP sd_total_bytes SD card total bytes
# TYPE sd_total_bytes gauge
sd_total_bytes 31457280

# HELP sd_free_percent SD card free percentage
# TYPE sd_free_percent gauge
sd_free_percent 71.1

# HELP wifi_state WiFi state (0=AP, 1=STA_CONNECTING, 2=STA_CONNECTED, 3=STA_DISCONNECTED)
# TYPE wifi_state gauge
wifi_state 2

# HELP upload_queue_size NAS upload queue size
# TYPE upload_queue_size gauge
upload_queue_size 0
```

**Prometheus 抓取配置**：

```yaml
scrape_configs:
  - job_name: 'esp32-cam'
    scrape_interval: 15s
    static_configs:
      - targets: ['192.168.1.100:80']
```

## 附录
### 服务器配置

| 参数 | 值 |
|------|-----|
| 端口 | 80 |
| 最大 URI 处理器 | 20 |
| 任务栈大小 | 8192 字节 |
| 接收超时 | 30 秒 |
| 发送超时 | 30 秒 |
| URI 匹配模式 | 通配符（wildcard） |