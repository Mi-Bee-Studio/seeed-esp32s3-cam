# ESP32-S3 相机 Web 服务器 API 文档

## 概述

本文档详细描述了 ESP32-S3 相机 Web 服务器提供的所有 REST API 接口。所有 API 端点都支持跨域请求，并使用 JSON 格式进行数据交换。

### 认证方式

- **请求头认证**: `X-Password: <password>`
- **查询参数认证**: `?password=<password>`
- **默认密码**: `12345678`
- **认证失败**: 返回 401 Unauthorized 状态码

### 响应格式

所有 API 响应都使用统一的 JSON 格式：

```json
{
  "ok": true,
  "data": { ... }
}
```

错误响应：
```json
{
  "ok": false,
  "error": "错误信息"
}
```

### CORS 支持

所有 API 端点都支持 CORS 预检请求：
- `Access-Control-Allow-Origin: *`
- `Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS`
- `Access-Control-Allow-Headers: Content-Type, X-Password`

---

## 摄像头控制 (Camera Control)

### 获取系统状态

**端点**: `GET /api/status`

**描述**: 获取系统当前状态，包括录像状态、存储信息、WiFi 连接状态等。

**认证**: 需要密码认证

**响应**:
```json
{
  "ok": true,
  "data": {
    "recording": "idle|recording|paused|error",
    "current_file": "当前录像文件名",
    "sd_free_percent": 85.5,
    "wifi_ssid": "MyWiFi",
    "wifi_state": "STA|AP|disconnected",
    "ip": "192.168.1.100",
    "camera": "OV2640|OV3660|unknown",
    "resolution": "VGA|SVGA|XGA",
    "time_synced": true,
    "uptime": 1234,
    "last_upload": "2024-01-01 12:00:00",
    "upload_queue": 0,
    "upload_paused": false
  }
}
```

**示例请求**:
```
GET /api/status
X-Password: 12345678
```

---

## 系统配置 (System Config)

### 获取配置信息

**端点**: `GET /api/config`

**描述**: 获取当前系统配置信息，密码字段会被遮掩。

**认证**: 需要密码认证

**响应**:
```json
{
  "ok": true,
  "data": {
    "wifi_ssid": "MyWiFi",
    "wifi_pass": "****",
    "device_name": "ESP32-Camera",
    "ftp_host": "ftp.example.com",
    "ftp_port": 21,
    "ftp_user": "user",
    "ftp_path": "/recordings",
    "ftp_enabled": true,
    "webdav_url": "https://webdav.example.com",
    "webdav_user": "user",
    "webdav_enabled": false,
    "resolution": 0,
    "fps": 10,
    "segment_sec": 60,
    "jpeg_quality": 32,
    "web_password": "****"
  }
}
```

**示例请求**:
```
GET /api/config
X-Password: 12345678
```

### 更新配置信息

**端点**: `POST /api/config`

**描述**: 更新系统配置。密码字段为可选，留空保持不变。

**认证**: 需要密码认证

**请求体**: JSON 格式配置数据

```json
{
  "wifi_ssid": "NewWiFi",
  "wifi_pass": "newpassword",
  "device_name": "NewDeviceName",
  "ftp_host": "newftp.example.com",
  "ftp_port": 22,
  "ftp_user": "newuser",
  "ftp_path": "/new/path",
  "ftp_enabled": true,
  "webdav_url": "https://newwebdav.example.com",
  "webdav_user": "newwebdavuser",
  "webdav_enabled": true,
  "resolution": 1,
  "fps": 15,
  "segment_sec": 120,
  "jpeg_quality": 40,
  "web_password": "newwebpass"
}
```

**注意**:
- `wifi_pass`、`web_password` 等敏感字段如果为 `"****"` 则保持原值不变
- 其他字段为空则保持不变
- 配置修改后会立即保存到 NVS

**示例请求**:
```
POST /api/config
Content-Type: application/json
X-Password: 12345678

{
  "wifi_ssid": "MyNewNetwork",
  "device_name": "Camera_01"
}
```

---

## WiFi 管理 (WiFi Management)

### 扫描 WiFi 网络

**端点**: `GET /api/scan`

**描述**: 扫描可用的 WiFi 网络，返回网络列表和信号强度信息。

**认证**: 无需认证

**响应**:
```json
{
  "ok": true,
  "data": {
    "networks": [
      {
        "ssid": "MyWiFi",
        "rssi": -65,
        "auth": 3
      },
      {
        "ssid": "GuestWiFi",
        "rssi": -80,
        "auth": 0
      }
    ]
  }
}
```

**响应字段说明**:
- `ssid`: 网络名称
- `rssi`: 信号强度（负值，越大越好）
- `auth`: 认证方式（0=开放，3=WPA2-PSK）

**示例请求**:
```
GET /api/scan
```

---

## 存储管理 (Storage Management)

### 获取文件列表

**端点**: `GET /api/files`

**描述**: 获取 SD 卡中录制的文件列表。

**认证**: 无需认证

**响应**:
```json
{
  "ok": true,
  "data": {
    "files": [
      {
        "name": "video_001.avi",
        "size": 10485760,
        "date": "2024-01-01 12:00:00"
      },
      {
        "name": "video_002.avi",
        "size": 5242880,
        "date": "2024-01-01 12:30:00"
      }
    ]
  }
}
```

**响应字段说明**:
- `name`: 文件名
- `size`: 文件大小（字节）
- `date`: 文件创建时间

**示例请求**:
```
GET /api/files
```

### 删除文件

**端点**: `DELETE /api/files?name=文件名`

**描述**: 删除指定的视频文件。

**认证**: 需要密码认证

**查询参数**:
- `name`: 要删除的文件名

**示例请求**:
```
DELETE /api/files?name=video_001.avi
X-Password: 12345678
```

### 下载文件

**端点**: `GET /api/download?name=文件名`

**描述**: 下载指定的视频文件。

**认证**: 无需认证

**查询参数**:
- `name`: 要下载的文件名

**响应**: 二进制数据，内容类型为 `video/avi`

**示例请求**:
```
GET /api/download?name=video_001.avi
```

---

## 录像控制 (Video Recording)

### 控制录像

**端点**: `POST /api/record?action=start|stop`

**描述**: 开始或停止录像功能。

**认证**: 需要密码认证

**查询参数**:
- `action`: 操作类型，`start` 或 `stop`

**响应**:
```json
{
  "ok": true,
  "data": {
    "action": "start",
    "status": "recording|error"
  }
}
```

**示例请求**:
```
POST /api/record?action=start
X-Password: 12345678
```

**示例响应**:
```json
{
  "ok": true,
  "data": {
    "action": "start",
    "status": "recording"
  }
}
```

---

## 文件管理 (File Management)

*请参考存储管理部分的相关端点*

---

## 固件更新 (Firmware Update)

### 重置设备

**端点**: `POST /api/reset`

**描述**: 执行工厂重置，恢复出厂设置并重启设备。

**认证**: 需要密码认证

**响应**:
```json
{
  "ok": true,
  "data": {
    "message": "Rebooting..."
  }
}
```

**示例请求**:
```
POST /api/reset
X-Password: 12345678
```

---

## 实时流 (Live Streaming)

### MJPEG 实时视频流

**端点**: `GET /stream`

**描述**: 实时 MJPEG 视频流，返回连续的 JPEG 图像帧。

**认证**: 无需认证

**响应**: `multipart/x-mixed-replace` 流式数据

**响应头**:
```
Content-Type: multipart/x-mixed-replace; boundary=frame
Access-Control-Allow-Origin: *
```

**数据格式**:
```
--frame
Content-Type: image/jpeg
Content-Length: <帧大小>

<JPEG图像数据>
--frame
```

**注意**:
- 同时最多支持 2 个客户端连接
- 帧率由配置中的 `fps` 参数控制
- 客户端断开连接时自动清理资源

**示例请求**:
```
GET /stream
```

**客户端示例**:
```html
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Camera Stream</title>
</head>
<body>
    <img id="stream" src="/stream" style="max-width: 100%;">
</body>
</html>
```

---

## 时间同步 (Time Sync)

### 设置手动时间

**端点**: `POST /api/time`

**描述**: 手动设置系统时间，用于录像文件的时间戳。

**认证**: 需要密码认证

**请求体**:
```json
{
  "year": 2024,
  "month": 1,
  "day": 1,
  "hour": 12,
  "min": 0,
  "sec": 0
}
```

**响应**:
```json
{
  "ok": true,
  "data": {}
}
```

**示例请求**:
```
POST /api/time
Content-Type: application/json
X-Password: 12345678

{
  "year": 2024,
  "month": 1,
  "day": 1,
  "hour": 12,
  "min": 30,
  "sec": 0
}
```

---

## 错误响应

所有 API 在出错时都会返回统一的 JSON 格式：

```json
{
  "ok": false,
  "error": "错误描述"
}
```

常见错误状态码：
- `400 Bad Request`: 请求格式错误或参数缺失
- `401 Unauthorized`: 认证失败或密码错误
- `404 Not Found`: 资源不存在
- `500 Internal Server Error`: 服务器内部错误

---

## 静态文件服务

系统还提供静态文件服务，支持以下文件类型：
- `index.html`: 主页面
- `.css`: 样式文件
- `.js`: JavaScript 文件
- `.png`: 图片文件
- `.ico`: 图标文件

**示例**:
```
GET /index.html
GET /style.css
GET /script.js
```

---

## 使用示例

### JavaScript 客户端示例

```javascript
// 获取系统状态
async function getStatus() {
    const response = await fetch('/api/status', {
        headers: {
            'X-Password': '12345678'
        }
    });
    const data = await response.json();
    return data;
}

// 更新配置
async function updateConfig(config) {
    const response = await fetch('/api/config', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'X-Password': '12345678'
        },
        body: JSON.stringify(config)
    });
    return await response.json();
}

// 开始录像
async function startRecording() {
    const response = await fetch('/api/record?action=start', {
        headers: {
            'X-Password': '12345678'
        }
    });
    return await response.json();
}

// 获取文件列表
async function getFiles() {
    const response = await fetch('/api/files');
    return await response.json();
}

// 下载文件
async function downloadFile(filename) {
    const response = await fetch(`/api/download?name=${encodeURIComponent(filename)}`);
    return response.blob();
}
```

### cURL 示例

```bash
# 获取状态
curl -X GET "http://192.168.1.100/api/status" \
     -H "X-Password: 12345678"

# 更新配置
curl -X POST "http://192.168.1.100/api/config" \
     -H "Content-Type: application/json" \
     -H "X-Password: 12345678" \
     -d '{"wifi_ssid": "MyNetwork", "device_name": "Camera01"}'

# 开始录像
curl -X POST "http://192.168.1.100/api/record?action=start" \
     -H "X-Password: 12345678"

# 获取文件列表
curl -X GET "http://192.168.1.100/api/files"

# 扫描 WiFi
curl -X GET "http://192.168.1.100/api/scan"
```

---

## 版本信息

- **文档版本**: 1.0
- **固件版本**: [请参考设备版本]
- **更新日期**: 2024-12-24