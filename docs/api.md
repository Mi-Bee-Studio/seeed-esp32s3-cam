# ParrotCam REST API 文档

> 本文档基于 `web_server.c`、`mjpeg_streamer.c`、`config_manager.c` 源码逐一验证编写。
> 最后更新：2026-04-24

---

## 1. 概述

ParrotCam 固件内置 HTTP Web 服务器（端口 80），提供 RESTful API 用于设备状态查询、配置管理、文件操作和录像控制。同时提供 MJPEG 实时视频流和静态文件服务。

### 1.1 基础地址

| 模式 | 地址 |
|------|------|
| AP 模式（默认） | `http://192.168.4.1` |
| STA 模式 | `http://<设备IP>` |

### 1.2 认证机制

部分接口需要密码认证，支持两种方式传递密码：

| 方式 | 格式 | 示例 |
|------|------|------|
| 请求头 | `X-Password: <密码>` | `X-Password: admin` |
| 查询参数 | `?password=<密码>` | `?password=admin` |

- **默认密码**：`admin`（可通过 `POST /api/config` 修改 `web_password` 字段）
- 认证逻辑优先检查 `X-Password` 请求头，其次检查 `password` 查询参数
- 认证失败返回 `401 Unauthorized`，响应体：`{"ok": false, "error": "Unauthorized"}`

### 1.3 统一响应格式

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

### 1.4 CORS 跨域支持

所有 HTTP 响应（包括错误响应和静态文件）均包含以下 CORS 头：

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Password
```

`OPTIONS` 请求（预检请求）返回上述 CORS 头和空响应体，状态码 200。

---

## 2. 接口总览

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

---

## 3. 接口详细说明

---

### 3.1 GET /api/status — 获取设备状态

获取设备当前运行状态，包括录像、存储、WiFi、摄像头、时间同步和上传队列信息。

**认证**：无需认证

**源码**：`api_status_handler`（web_server.c）

**响应示例**：
```json
{
  "ok": true,
  "data": {
    "recording": "idle",
    "current_file": "",
    "sd_free_percent": 85.5,
    "wifi_ssid": "MyWiFi",
    "wifi_state": "STA",
    "ip": "192.168.1.100",
    "camera": "OV2640",
    "resolution": "SVGA",
    "time_synced": true,
    "uptime": 3600,
    "last_upload": "20260424_120000.avi",
    "upload_queue": 2
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `recording` | string | 录像状态：`"idle"` 空闲、`"recording"` 录制中、`"paused"` 已暂停、`"error"` 错误 |
| `current_file` | string | 当前录像文件名（AVI 格式），空闲时为空字符串 `""` |
| `sd_free_percent` | number | SD 卡剩余空间百分比（如 `85.5` 表示 85.5%） |
| `wifi_ssid` | string | 已配置的 WiFi SSID，空字符串表示 AP 模式 |
| `wifi_state` | string | WiFi 状态：`"AP"` 热点模式、`"STA"` 已连接路由器、`"disconnected"` 未连接 |
| `ip` | string | 当前 IP 地址字符串 |
| `camera` | string | 摄像头传感器型号：`"OV2640"`、`"OV3660"`、`"unknown"` |
| `resolution` | string | 当前分辨率名称：`"VGA"`、`"SVGA"`、`"XGA"` |
| `time_synced` | bool | NTP 时间是否已同步 |
| `uptime` | number | 自开机以来的运行秒数 |
| `last_upload` | string | 最近一次上传的文件名，无上传记录时为空字符串 |
| `upload_queue` | number | 等待上传的文件数量 |

**cURL 示例**：
```bash
curl http://192.168.4.1/api/status
```

**JavaScript 示例**：
```javascript
const resp = await fetch('/api/status');
const { data } = await resp.json();
console.log(`录像状态: ${data.recording}, 剩余空间: ${data.sd_free_percent}%`);
```

---

### 3.2 GET /api/config — 获取当前配置

获取设备当前全部配置项。密码类字段会被遮掩显示。

**认证**：无需认证

**源码**：`api_config_get_handler`（web_server.c）

**响应示例**：
```json
{
  "ok": true,
  "data": {
    "wifi_ssid": "MyWiFi",
    "wifi_pass": "****",
    "device_name": "ParrotCam",
    "ftp_host": "192.168.1.200",
    "ftp_port": 21,
    "ftp_user": "ftpuser",
    "ftp_path": "/ParrotCam",
    "ftp_enabled": true,
    "webdav_url": "",
    "webdav_user": "",
    "webdav_enabled": false,
    "resolution": 1,
    "fps": 10,
    "segment_sec": 300,
    "jpeg_quality": 12,
    "web_password": "****"
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `wifi_ssid` | string | WiFi 网络名称，空字符串表示 AP 模式 |
| `wifi_pass` | string | WiFi 密码，已设置时显示 `"****"`，未设置时显示 `""` |
| `device_name` | string | 设备名称（默认 `"ParrotCam"`） |
| `ftp_host` | string | FTP 服务器地址 |
| `ftp_port` | number | FTP 端口（默认 `21`） |
| `ftp_user` | string | FTP 用户名 |
| `ftp_path` | string | FTP 上传路径（默认 `"/ParrotCam"`） |
| `ftp_enabled` | bool | 是否启用 FTP 上传 |
| `webdav_url` | string | WebDAV 服务器地址 |
| `webdav_user` | string | WebDAV 用户名 |
| `webdav_enabled` | bool | 是否启用 WebDAV 上传 |
| `resolution` | number | 分辨率编号：`0`=VGA(640×480)、`1`=SVGA(800×600)、`2`=XGA(1024×768) |
| `fps` | number | 录像帧率（默认 `10`） |
| `segment_sec` | number | 视频分段时长，单位秒（默认 `300`，即 5 分钟） |
| `jpeg_quality` | number | JPEG 图像质量（默认 `12`，数值越小质量越好） |
| `web_password` | string | Web 管理密码，已设置时显示 `"****"`，未设置时显示 `""` |

> **重要**：`ftp_pass` 和 `webdav_pass` **不会**在此接口中返回。这是设计上的安全考量。
> 只有 `wifi_pass` 和 `web_password` 会以遮掩形式（`"****"`）返回。

**cURL 示例**：
```bash
curl http://192.168.4.1/api/config
```

**JavaScript 示例**：
```javascript
const resp = await fetch('/api/config');
const { data } = await resp.json();
console.log(`设备名: ${data.device_name}, 分辨率: ${data.resolution}`);
```

---

### 3.3 POST /api/config — 更新配置

更新设备配置。请求体为 JSON 格式，只需包含要修改的字段。配置修改后立即保存到 NVS 非易失性存储。

**认证**：需要密码认证

**源码**：`api_config_post_handler`（web_server.c）

**请求体**：
```json
{
  "wifi_ssid": "NewWiFi",
  "wifi_pass": "newpassword",
  "device_name": "MyCamera",
  "ftp_host": "192.168.1.200",
  "ftp_port": 21,
  "ftp_user": "user",
  "ftp_pass": "ftppassword",
  "ftp_path": "/recordings",
  "ftp_enabled": true,
  "webdav_url": "https://dav.example.com",
  "webdav_user": "davuser",
  "webdav_pass": "davpassword",
  "webdav_enabled": false,
  "resolution": 2,
  "fps": 15,
  "segment_sec": 600,
  "jpeg_quality": 8,
  "web_password": "newpass"
}
```

**可修改字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `wifi_ssid` | string | WiFi 网络名称 |
| `wifi_pass` | string | WiFi 密码 |
| `device_name` | string | 设备名称 |
| `ftp_host` | string | FTP 服务器地址 |
| `ftp_port` | number | FTP 端口号 |
| `ftp_user` | string | FTP 用户名 |
| `ftp_pass` | string | FTP 密码 |
| `ftp_path` | string | FTP 上传路径 |
| `ftp_enabled` | bool | 启用/禁用 FTP 上传 |
| `webdav_url` | string | WebDAV 服务器地址 |
| `webdav_user` | string | WebDAV 用户名 |
| `webdav_pass` | string | WebDAV 密码 |
| `webdav_enabled` | bool | 启用/禁用 WebDAV 上传 |
| `resolution` | number | 分辨率：`0`=VGA、`1`=SVGA、`2`=XGA |
| `fps` | number | 录像帧率 |
| `segment_sec` | number | 分段时长（秒） |
| `jpeg_quality` | number | JPEG 质量 |
| `web_password` | string | Web 管理密码 |

**密码字段特殊行为**：

四个密码字段（`wifi_pass`、`ftp_pass`、`webdav_pass`、`web_password`）具有特殊处理逻辑：
- 如果值为 `"****"`（四个星号），则**忽略该字段**，保留当前密码不变
- 如果值为其他字符串，则更新为新的密码值
- 此设计允许客户端在 `GET /api/config` 获取配置后回传数据而不泄露密码

**成功响应**：
```json
{
  "ok": true
}
```

> 注意：成功时无 `data` 字段。

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |
| 400 | 请求体为空或超过 2048 字节 | `"Empty or too large body"` |
| 400 | JSON 解析失败 | `"Invalid JSON"` |

**cURL 示例**：
```bash
# 修改 WiFi 配置
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "HomeWiFi", "wifi_pass": "mypassword"}'

# 只修改分辨率和帧率
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"resolution": 2, "fps": 15}'

# 保留原密码不变（传入 ****）
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "NewNet", "wifi_pass": "****", "web_password": "****"}'
```

**JavaScript 示例**：
```javascript
// 更新 FTP 配置
async function updateFtp() {
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Password': 'admin'
    },
    body: JSON.stringify({
      ftp_host: '192.168.1.200',
      ftp_user: 'user',
      ftp_pass: 'secret',
      ftp_path: '/cam',
      ftp_enabled: true
    })
  });
  return await resp.json();
}
```

---

### 3.4 GET /api/files — 获取录制文件列表

获取 SD 卡 `/sdcard/recordings/` 目录下的录制文件列表。

**认证**：无需认证

**源码**：`api_files_get_handler`（web_server.c）

**响应示例**：
```json
{
  "ok": true,
  "data": {
    "files": [
      {
        "name": "20260424_120000.avi",
        "size": 10485760,
        "date": "2026-04-24 12:00:00"
      },
      {
        "name": "20260424_130000.avi",
        "size": 5242880,
        "date": "2026-04-24 13:00:00"
      }
    ]
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `files` | array | 文件信息数组，最多返回 64 个文件 |
| `files[].name` | string | 文件名 |
| `files[].size` | number | 文件大小（字节） |
| `files[].date` | string | 文件日期/时间字符串 |

> 无文件或 SD 卡未插入时返回空数组：`{"ok": true, "data": {"files": []}}`

**cURL 示例**：
```bash
curl http://192.168.4.1/api/files
```

**JavaScript 示例**：
```javascript
const resp = await fetch('/api/files');
const { data } = await resp.json();
data.files.forEach(f => {
  console.log(`${f.name} — ${(f.size / 1048576).toFixed(1)} MB — ${f.date}`);
});
```

---

### 3.5 DELETE /api/files?name=xxx — 删除文件

删除 SD 卡上指定的录制文件。

**认证**：需要密码认证

**源码**：`api_files_delete_handler`（web_server.c）

**查询参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 要删除的文件名（不含路径前缀） |

**安全机制**：
- 文件名中包含 `..` 的请求将被拒绝（防止路径遍历攻击）
- 实际删除路径为 `/sdcard/recordings/<name>`

**成功响应**：
```json
{
  "ok": true
}
```

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |
| 400 | 缺少查询参数 | `"Missing query"` |
| 400 | 缺少 name 参数 | `"Missing name parameter"` |
| 400 | 文件名包含 `..` | `"Invalid name"` |
| 404 | 文件不存在或删除失败 | `"File not found or delete failed"` |

**cURL 示例**：
```bash
curl -X DELETE "http://192.168.4.1/api/files?name=20260424_120000.avi" \
  -H "X-Password: admin"
```

**JavaScript 示例**：
```javascript
async function deleteFile(filename) {
  const resp = await fetch(
    `/api/files?name=${encodeURIComponent(filename)}`,
    {
      method: 'DELETE',
      headers: { 'X-Password': 'admin' }
    }
  );
  return await resp.json();
}
```

---

### 3.6 GET /api/download?name=xxx — 下载文件

下载 SD 卡上指定的录制文件，返回 AVI 格式的二进制数据。

**认证**：无需认证

**源码**：`api_download_handler`（web_server.c）

**查询参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 要下载的文件名（不含路径前缀） |

**安全机制**：
- 文件名中包含 `..` 的请求将被拒绝（防止路径遍历攻击）
- 实际下载路径为 `/sdcard/recordings/<name>`

**响应头**：
```
Content-Type: video/avi
Content-Length: <文件大小>
Content-Disposition: attachment; filename="<文件名>"
Access-Control-Allow-Origin: *
```

**响应体**：二进制 AVI 数据，以 1024 字节分块传输。

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 400 | 缺少查询参数 | `"Missing query"` |
| 400 | 缺少 name 参数 | `"Missing name parameter"` |
| 400 | 文件名包含 `..` | `"Invalid name"` |
| 404 | 文件不存在 | `"File not found"` |

**cURL 示例**：
```bash
# 下载并保存为本地文件
curl -o video.avi "http://192.168.4.1/api/download?name=20260424_120000.avi"
```

**JavaScript 示例**：
```javascript
async function downloadFile(filename) {
  const resp = await fetch(
    `/api/download?name=${encodeURIComponent(filename)}`
  );
  if (!resp.ok) throw new Error(`下载失败: ${resp.status}`);
  const blob = await resp.blob();
  // 创建下载链接
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}
```

---

### 3.7 GET /api/scan — 扫描 WiFi 网络

扫描周围的 WiFi 接入点，返回可用网络列表。

**认证**：无需认证

**源码**：`api_scan_handler`（web_server.c）

**响应示例**：
```json
{
  "ok": true,
  "data": {
    "networks": [
      {
        "ssid": "HomeWiFi",
        "rssi": -45,
        "auth": 3
      },
      {
        "ssid": "GuestNet",
        "rssi": -72,
        "auth": 3
      },
      {
        "ssid": "OpenCafe",
        "rssi": -85,
        "auth": 0
      }
    ]
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `networks` | array | 扫描到的网络数组，最多返回 20 个 |
| `networks[].ssid` | string | 网络名称（SSID） |
| `networks[].rssi` | number | 信号强度（负值，越接近 0 信号越强，如 `-45` 强于 `-85`） |
| `networks[].auth` | number | 认证模式（`wifi_auth_mode_t` 枚举值） |

**常见 auth 值**：

| 值 | 含义 |
|----|------|
| 0 | 开放网络（无密码） |
| 1 | WEP |
| 2 | WPA-PSK |
| 3 | WPA2-PSK |
| 4 | WPA/WPA2-PSK |
| 5 | WPA2-Enterprise |

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 500 | 扫描失败 | `"Scan failed"` |

**cURL 示例**：
```bash
curl http://192.168.4.1/api/scan
```

**JavaScript 示例**：
```javascript
async function scanWifi() {
  const resp = await fetch('/api/scan');
  const { data } = await resp.json();
  data.networks
    .sort((a, b) => b.rssi - a.rssi)
    .forEach(n => {
      console.log(`${n.ssid} — RSSI: ${n.rssi} — 认证: ${n.auth}`);
    });
}
```

---

### 3.8 POST /api/time — 手动设置时间

手动设置设备系统时间。在无法通过 NTP 自动同步时（如 AP 模式），可通过此接口手动校时。

**认证**：需要密码认证

**源码**：`api_time_handler`（web_server.c）

**请求体**（6 个字段全部必填）：
```json
{
  "year": 2026,
  "month": 4,
  "day": 24,
  "hour": 14,
  "min": 30,
  "sec": 0
}
```

**请求字段说明**：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `year` | number | 是 | 年份（如 2026） |
| `month` | number | 是 | 月份（1-12） |
| `day` | number | 是 | 日期（1-31） |
| `hour` | number | 是 | 小时（0-23） |
| `min` | number | 是 | 分钟（0-59） |
| `sec` | number | 是 | 秒（0-59） |

> 六个字段缺一不可，缺少任意字段将返回 400 错误。

**成功响应**：
```json
{
  "ok": true
}
```

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |
| 400 | 请求体为空（超过 512 字节限制） | `"Empty body"` |
| 400 | JSON 解析失败 | `"Invalid JSON"` |
| 400 | 缺少时间字段 | `"Missing time fields"` |
| 500 | 设置时间失败 | `"Failed to set time"` |

**cURL 示例**：
```bash
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year": 2026, "month": 4, "day": 24, "hour": 14, "min": 30, "sec": 0}'
```

**JavaScript 示例**：
```javascript
async function setDeviceTime(date) {
  const resp = await fetch('/api/time', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Password': 'admin'
    },
    body: JSON.stringify({
      year: date.getFullYear(),
      month: date.getMonth() + 1,
      day: date.getDate(),
      hour: date.getHours(),
      min: date.getMinutes(),
      sec: date.getSeconds()
    })
  });
  return await resp.json();
}

// 将浏览器当前时间同步到设备
setDeviceTime(new Date());
```

---

### 3.9 POST /api/record?action=start|stop — 控制录像

手动开始或停止视频录制。

**认证**：需要密码认证

**源码**：`api_record_handler`（web_server.c）

**查询参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | string | 是 | 操作类型：`"start"` 开始录像、`"stop"` 停止录像 |

**成功响应（开始录像）**：
```json
{
  "ok": true,
  "data": {
    "action": "start",
    "status": "recording"
  }
}
```

**成功响应（停止录像）**：
```json
{
  "ok": true,
  "data": {
    "action": "stop",
    "status": "stopped"
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `action` | string | 回显请求的 action 参数值 |
| `status` | string | 操作结果状态，详见下表 |

**status 可能值**：

| status 值 | 触发条件 |
|-----------|----------|
| `"recording"` | `action=start` 且录像启动成功 |
| `"stopped"` | `action=stop` 且录像停止成功 |
| `"error"` | `action=start` 或 `action=stop` 操作执行失败（如无 SD 卡） |
| `"unknown_action"` | action 参数不是 `"start"` 也不是 `"stop"` |

> 即使返回 `"error"` 或 `"unknown_action"`，HTTP 状态码仍为 200。
> 只有认证失败时才返回 401。

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |

**cURL 示例**：
```bash
# 开始录像
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# 停止录像
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

**JavaScript 示例**：
```javascript
async function toggleRecording(start) {
  const action = start ? 'start' : 'stop';
  const resp = await fetch(`/api/record?action=${action}`, {
    method: 'POST',
    headers: { 'X-Password': 'admin' }
  });
  const { data } = await resp.json();
  console.log(`操作: ${data.action}, 状态: ${data.status}`);
  return data.status;
}
```

---

### 3.10 POST /api/reset — 恢复出厂设置

将设备配置恢复为出厂默认值并重启。配置立即重置，设备在发送响应后重启。

**认证**：需要密码认证

**源码**：`api_reset_handler`（web_server.c）

**请求体**：无

**响应**：
```json
{
  "ok": true,
  "data": {
    "message": "Rebooting..."
  }
}
```

> **注意**：设备在发送此响应后立即执行重启。客户端收到此响应后应预期连接断开。
> 重启后设备将使用默认配置启动（默认进入 AP 模式，密码恢复为 `admin`）。

**出厂默认值**：

| 配置项 | 默认值 |
|--------|--------|
| wifi_ssid | `""` (AP 模式) |
| wifi_pass | `""` |
| device_name | `"ParrotCam"` |
| web_password | `"admin"` |
| resolution | `1` (SVGA) |
| fps | `10` |
| segment_sec | `300` |
| jpeg_quality | `12` |
| ftp_enabled | `false` |
| webdav_enabled | `false` |

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |

**cURL 示例**：
```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

**JavaScript 示例**：
```javascript
async function factoryReset() {
  if (!confirm('确定要恢复出厂设置吗？设备将重启。')) return;
  const resp = await fetch('/api/reset', {
    method: 'POST',
    headers: { 'X-Password': 'admin' }
  });
  const result = await resp.json();
  if (result.ok) {
    console.log('设备正在重启...');
    // 等待设备重启后重新连接
    setTimeout(() => window.location.reload(), 10000);
  }
}
```

---

### 3.11 GET /stream — MJPEG 实时视频流

获取摄像头的 MJPEG 实时视频流。浏览器可直接将此端点作为 `<img>` 标签的 `src` 使用。

**认证**：无需认证

**源码**：`mjpeg_stream_handler`（mjpeg_streamer.c）

**响应头**：
```
Content-Type: multipart/x-mixed-replace; boundary=frame
Access-Control-Allow-Origin: *
```

**响应体格式**：

流式响应由连续的 JPEG 帧组成，每帧格式如下：
```
\r\n--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <帧数据字节数>\r\n
\r\n
<JPEG 二进制数据>\r\n
```

**限制**：
- 最多支持 **2 个并发客户端**连接
- 超过连接数限制时返回 `503 Service Unavailable`，响应体：`Max stream connections reached`
- 帧率由配置项 `fps` 控制（默认 10 FPS）
- 每帧 JPEG 数据以 4096 字节分块发送
- 客户端断开连接时自动释放资源

**cURL 示例**：
```bash
# 将一帧保存为 JPEG（超时后自动断开）
curl --max-time 1 http://192.168.4.1/stream > frame.jpg 2>/dev/null
```

**HTML 嵌入示例**：
```html
<!-- 最简用法：直接嵌入 img 标签 -->
<img src="http://192.168.4.1/stream" style="width: 100%;" alt="实时画面">

<!-- 带自动重连的完整页面 -->
<!DOCTYPE html>
<html>
<head>
  <title>ParrotCam 实时画面</title>
</head>
<body>
  <img id="stream" src="/stream" style="max-width: 100%;"
       onerror="setTimeout(() => this.src='/stream?t='+Date.now(), 3000)">
</body>
</html>
```

**JavaScript 播放控制示例**：
```javascript
const img = document.getElementById('stream');

function startStream() {
  img.src = '/stream?t=' + Date.now();
}

function stopStream() {
  img.src = '';  // 断开流连接
}

// 断线自动重连
img.onerror = () => {
  setTimeout(startStream, 3000);
};
```

---

### 3.12 OPTIONS /* — CORS 预检请求

处理跨域预检（Preflight）请求。所有路径的 OPTIONS 请求均由此处理器处理。

**认证**：无需认证

**源码**：`options_handler`（web_server.c）

**响应头**：
```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Password
```

**响应体**：空（HTTP 200）

> 浏览器在发送跨域 `POST` 或 `DELETE` 请求前会自动发送 OPTIONS 预检请求，无需手动处理。

---

### 3.13 GET /* — 静态文件服务

提供 Web 管理界面的静态文件。文件从 SPIFFS 分区（`/spiffs/`）读取。

**认证**：无需认证

**源码**：`static_file_handler`（web_server.c）

**路由规则**：
- 访问 `/` 自动映射到 `/index.html`
- 其他路径直接映射到 `/spiffs<路径>`

**支持的文件类型与 Content-Type**：

| 文件扩展名 | Content-Type |
|-----------|--------------|
| `.html` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.png` | `image/png` |
| `.ico` | `image/x-icon` |
| 其他 | `text/html`（默认） |

> 文件以 1024 字节分块传输。所有响应均包含 CORS 头。
> 文件不存在时返回 404。

**常见请求路径**：

| URL | 实际文件 |
|-----|----------|
| `/` | `/spiffs/index.html` |
| `/index.html` | `/spiffs/index.html` |
| `/style.css` | `/spiffs/style.css` |
| `/app.js` | `/spiffs/app.js` |

---

## 4. 错误处理

### 4.1 HTTP 状态码

| 状态码 | 含义 | 触发场景 |
|--------|------|----------|
| 200 | 成功 | 请求处理成功 |
| 400 | 请求错误 | 参数缺失、JSON 格式错误、路径遍历检测 |
| 401 | 认证失败 | 密码错误或未提供密码（需要认证的接口） |
| 404 | 资源不存在 | 文件不存在、静态文件未找到 |
| 500 | 服务器内部错误 | WiFi 扫描失败、时间设置失败 |
| 503 | 服务不可用 | MJPEG 流客户端连接数已达上限 |

### 4.2 错误响应格式

所有错误响应均为 JSON 格式：

```json
{
  "ok": false,
  "error": "错误描述信息"
}
```

### 4.3 认证相关错误

需要认证的接口（POST /api/config、DELETE /api/files、POST /api/time、POST /api/record、POST /api/reset）在密码验证失败时统一返回：

```
HTTP/1.1 401 Unauthorized
Content-Type: application/json

{"ok":false,"error":"Unauthorized"}
```

---

## 5. 认证详解

### 5.1 认证流程

```
客户端请求 → 读取 X-Password 头 → 匹配？ → 放行
                    ↓ 不匹配/不存在
             读取 ?password 参数 → 匹配？ → 放行
                    ↓ 不匹配/不存在
             返回 401 Unauthorized
```

### 5.2 需要认证的接口

| 接口 | 方法 |
|------|------|
| `/api/config` | POST |
| `/api/files` | DELETE |
| `/api/time` | POST |
| `/api/record` | POST |
| `/api/reset` | POST |

### 5.3 无需认证的接口

| 接口 | 方法 |
|------|------|
| `/api/status` | GET |
| `/api/config` | GET |
| `/api/files` | GET |
| `/api/download` | GET |
| `/api/scan` | GET |
| `/stream` | GET |
| `/*`（静态文件） | GET |
| `/*`（预检） | OPTIONS |

---

## 6. 密码字段行为详解

设备管理涉及四个密码字段，它们在 GET 和 POST 中的行为不同：

### 6.1 GET /api/config 返回的密码字段

| 字段 | 返回行为 |
|------|----------|
| `wifi_pass` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `web_password` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `ftp_pass` | **不返回**（完全不包含在响应中） |
| `webdav_pass` | **不返回**（完全不包含在响应中） |

### 6.2 POST /api/config 密码处理逻辑

```
接收密码字段值 → 值是否为 "****"？
                    ↓ 是          ↓ 否
              保持原值不变    更新为新密码
```

所有四个密码字段（`wifi_pass`、`ftp_pass`、`webdav_pass`、`web_password`）均遵循此逻辑。

### 6.3 典型使用场景

**场景 1：获取配置后原样回传（不修改密码）**
```javascript
// GET 返回 wifi_pass: "****", web_password: "****"
// 原样回传 "****" 即可保持密码不变
await fetch('/api/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', 'X-Password': 'admin' },
  body: JSON.stringify({
    device_name: 'NewName',
    wifi_pass: '****',
    web_password: '****'
  })
});
```

**场景 2：只修改部分配置（不含密码字段）**
```javascript
// 不包含密码字段时，密码不会被修改
await fetch('/api/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', 'X-Password': 'admin' },
  body: JSON.stringify({ fps: 15, resolution: 2 })
});
```

**场景 3：修改密码**
```javascript
await fetch('/api/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', 'X-Password': 'admin' },
  body: JSON.stringify({ web_password: 'newpassword' })
});
// 之后的请求需使用新密码
```

---

## 7. 完整 cURL 示例

### 7.1 设备状态与配置

```bash
# 查看设备状态
curl http://192.168.4.1/api/status

# 查看当前配置
curl http://192.168.4.1/api/config

# 修改设备名称
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"device_name": "KitchenCam"}'

# 配置 WiFi 连接（重启后生效）
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "HomeWiFi", "wifi_pass": "wifipassword"}'
```

### 7.2 录像控制

```bash
# 开始录像
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# 停止录像
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

### 7.3 文件管理

```bash
# 列出所有录制文件
curl http://192.168.4.1/api/files

# 下载指定文件
curl -o recording.avi "http://192.168.4.1/api/download?name=20260424_120000.avi"

# 删除指定文件
curl -X DELETE "http://192.168.4.1/api/files?name=20260424_120000.avi" \
  -H "X-Password: admin"
```

### 7.4 网络与时间

```bash
# 扫描 WiFi
curl http://192.168.4.1/api/scan

# 手动设置时间
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year": 2026, "month": 4, "day": 24, "hour": 14, "min": 30, "sec": 0}'
```

### 7.5 NAS 上传配置

```bash
# 配置 FTP 上传
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{
    "ftp_host": "192.168.1.200",
    "ftp_port": 21,
    "ftp_user": "camuser",
    "ftp_pass": "camsecret",
    "ftp_path": "/ParrotCam",
    "ftp_enabled": true
  }'

# 配置 WebDAV 上传
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{
    "webdav_url": "https://dav.example.com/ParrotCam",
    "webdav_user": "davuser",
    "webdav_pass": "davsecret",
    "webdav_enabled": true
  }'
```

### 7.6 摄像头参数

```bash
# 设置为 XGA 分辨率、15 FPS
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"resolution": 2, "fps": 15}'

# 设置高质量 JPEG（数值越小质量越高）
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"jpeg_quality": 6}'
```

### 7.7 恢复出厂设置

```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

---

## 8. 完整 JavaScript 示例

以下为前端应用与 ParrotCam 交互的完整代码示例。

### 8.1 API 客户端封装

```javascript
class ParrotCamAPI {
  constructor(baseURL = '', password = 'admin') {
    this.base = baseURL;
    this.password = password;
  }

  get headers() {
    return {
      'Content-Type': 'application/json',
      'X-Password': this.password
    };
  }

  async getStatus() {
    const resp = await fetch(`${this.base}/api/status`);
    return resp.json();
  }

  async getConfig() {
    const resp = await fetch(`${this.base}/api/config`);
    return resp.json();
  }

  async updateConfig(fields) {
    const resp = await fetch(`${this.base}/api/config`, {
      method: 'POST',
      headers: this.headers,
      body: JSON.stringify(fields)
    });
    return resp.json();
  }

  async getFiles() {
    const resp = await fetch(`${this.base}/api/files`);
    return resp.json();
  }

  async deleteFile(name) {
    const resp = await fetch(
      `${this.base}/api/files?name=${encodeURIComponent(name)}`,
      { method: 'DELETE', headers: this.headers }
    );
    return resp.json();
  }

  async downloadFile(name) {
    const resp = await fetch(
      `${this.base}/api/download?name=${encodeURIComponent(name)}`
    );
    return resp.blob();
  }

  async scanWifi() {
    const resp = await fetch(`${this.base}/api/scan`);
    return resp.json();
  }

  async setTime(date) {
    const resp = await fetch(`${this.base}/api/time`, {
      method: 'POST',
      headers: this.headers,
      body: JSON.stringify({
        year: date.getFullYear(),
        month: date.getMonth() + 1,
        day: date.getDate(),
        hour: date.getHours(),
        min: date.getMinutes(),
        sec: date.getSeconds()
      })
    });
    return resp.json();
  }

  async startRecording() {
    const resp = await fetch(`${this.base}/api/record?action=start`, {
      method: 'POST',
      headers: this.headers
    });
    return resp.json();
  }

  async stopRecording() {
    const resp = await fetch(`${this.base}/api/record?action=stop`, {
      method: 'POST',
      headers: this.headers
    });
    return resp.json();
  }

  async factoryReset() {
    const resp = await fetch(`${this.base}/api/reset`, {
      method: 'POST',
      headers: this.headers
    });
    return resp.json();
  }

  getStreamURL() {
    return `${this.base}/stream`;
  }
}
```

### 8.2 使用示例

```javascript
const cam = new ParrotCamAPI('http://192.168.4.1', 'admin');

// 获取并显示状态
async function showStatus() {
  const { data } = await cam.getStatus();
  console.log(`录像: ${data.recording}`);
  console.log(`WiFi: ${data.wifi_state} (${data.ip})`);
  console.log(`存储: ${data.sd_free_percent}% 可用`);
  console.log(`运行: ${Math.floor(data.uptime / 3600)}h ${Math.floor((data.uptime % 3600) / 60)}m`);
}

// 配置 WiFi 并开始录像
async function quickSetup(ssid, pass) {
  await cam.updateConfig({ wifi_ssid: ssid, wifi_pass: pass });
  console.log('WiFi 已配置，设备将连接网络');
}

// 下载所有录制文件
async function downloadAll() {
  const { data } = await cam.getFiles();
  for (const file of data.files) {
    console.log(`正在下载: ${file.name} (${(file.size / 1048576).toFixed(1)} MB)`);
    const blob = await cam.downloadFile(file.name);
    // 处理下载的文件...
  }
}
```

---

## 9. 配置参数参考

### 9.1 分辨率

| 值 | 名称 | 分辨率 |
|----|------|--------|
| 0 | VGA | 640 × 480 |
| 1 | SVGA | 800 × 600 |
| 2 | XGA | 1024 × 768 |

### 9.2 默认配置值

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `wifi_ssid` | string | `""` | 空表示 AP 模式 |
| `wifi_pass` | string | `""` | WiFi 密码 |
| `device_name` | string | `"ParrotCam"` | 设备名称 |
| `ftp_host` | string | `""` | FTP 服务器 |
| `ftp_port` | number | `21` | FTP 端口 |
| `ftp_user` | string | `""` | FTP 用户名 |
| `ftp_pass` | string | `""` | FTP 密码 |
| `ftp_path` | string | `"/ParrotCam"` | FTP 路径 |
| `ftp_enabled` | bool | `false` | FTP 上传开关 |
| `webdav_url` | string | `""` | WebDAV 地址 |
| `webdav_user` | string | `""` | WebDAV 用户名 |
| `webdav_pass` | string | `""` | WebDAV 密码 |
| `webdav_enabled` | bool | `false` | WebDAV 上传开关 |
| `resolution` | number | `1` | SVGA (800×600) |
| `fps` | number | `10` | 10 帧/秒 |
| `segment_sec` | number | `300` | 5 分钟/段 |
| `jpeg_quality` | number | `12` | JPEG 质量 |
| `web_password` | string | `"admin"` | Web 管理密码 |

---

## 10. 附录

### 10.1 SD 卡路径

| 用途 | 路径 |
|------|------|
| 录制文件 | `/sdcard/recordings/` |
| WiFi 配置覆盖 | `/sdcard/config/wifi.txt` |
| NAS 配置覆盖 | `/sdcard/config/nas.txt` |

### 10.2 配置优先级

```
SD 卡配置文件 > NVS 存储 > 编译时默认值
```

### 10.3 SPIFFS 分区

Web 界面静态文件存储在 SPIFFS 分区（约 256KB），路径前缀 `/spiffs/`。

### 10.4 服务器配置

| 参数 | 值 |
|------|-----|
| 端口 | 80 |
| 最大 URI 处理器 | 20 |
| 任务栈大小 | 8192 字节 |
| 接收超时 | 30 秒 |
| 发送超时 | 30 秒 |
| URI 匹配模式 | 通配符（wildcard） |
