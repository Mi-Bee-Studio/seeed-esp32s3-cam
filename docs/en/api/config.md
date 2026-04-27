# 配置接口

> [← 状态查询](status.md) | [文件管理 →](files.md)

---

## GET /api/config — 获取当前配置

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

## POST /api/config — 更新配置

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

## 密码字段行为详解

设备管理涉及四个密码字段，它们在 GET 和 POST 中的行为不同：

### GET /api/config 返回的密码字段

| 字段 | 返回行为 |
|------|----------|
| `wifi_pass` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `web_password` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `ftp_pass` | **不返回**（完全不包含在响应中） |
| `webdav_pass` | **不返回**（完全不包含在响应中） |

### POST /api/config 密码处理逻辑

```
接收密码字段值 → 值是否为 "****"？
                    ↓ 是          ↓ 否
              保持原值不变    更新为新密码
```

所有四个密码字段（`wifi_pass`、`ftp_pass`、`webdav_pass`、`web_password`）均遵循此逻辑。

### 典型使用场景

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

## 分辨率参考

| 值 | 名称 | 分辨率 |
|----|------|--------|
| 0 | VGA | 640 × 480 |
| 1 | SVGA | 800 × 600 |
| 2 | XGA | 1024 × 768 |

## 默认配置值

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
