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
    "wifi_ssid": "MickeyMiRoute",
    "wifi_pass": "****",
    "device_name": "MiBee Cam",
    "upload_method": 1,
    "upload_base_path": "/MiBee Cam",
    "webdav_url": "http://192.168.63.31:9090/dav",
    "webdav_user": "admin",
    "webdav_enabled": true,
    "http_upload_url": "",
    "http_upload_user": "",
    "http_upload_pass": "",
    "http_upload_skip_cert_verify": false,
    "resolution": 1,
    "fps": 10,
    "segment_sec": 300,
    "jpeg_quality": 12,
    "vflip": false,
    "hmirror": false,
    "web_password": "****",
    "timezone": "CST-8",
    "timelapse_interval_sec": 0,
    "cleanup_low_pct": 20,
    "cleanup_high_pct": 30,
    "frame_drop_enabled": true,
    "alert_webhook_url": "",
    "alert_webhook_enabled": false,
    "wifi_ssid_2": "",
    "wifi_pass_2": "****",
    "allow_ap_fallback": false
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `wifi_ssid` | string | WiFi 网络名称，空字符串表示 AP 模式 |
| `wifi_pass` | string | WiFi 密码，已设置时显示 `"****"`，未设置时显示 `""` |
| `device_name` | string | 设备名称（默认 `"MiBee Cam"`） |
| `upload_method` | uint8 | 上传方式：`0`=禁用、`1`=WebDAV、`2`=HTTP(S) |
| `upload_base_path` | string | 上传基础路径（默认 `"/MiBee Cam"`） |
| `webdav_url` | string | WebDAV 服务器地址 |
| `webdav_user` | string | WebDAV 用户名 |
| `webdav_enabled` | bool | 是否启用 WebDAV 上传（仅 `upload_method=1` 时有效） |
| `http_upload_url` | string | HTTP(S) 上传完整 URL |
| `http_upload_user` | string | HTTP(S) 用户名 |
| `http_upload_pass` | string | HTTP(S) 密码，已设置时显示 `"****"`，未设置时显示 `""` |
| `http_upload_skip_cert_verify` | bool | 跳过 HTTPS 证书验证 |
| `resolution` | number | 分辨率编号：`0`=VGA(640×480)、`1`=SVGA(800×600)、`2`=XGA(1024×768) |
| `fps` | number | 录像帧率（默认 `10`） |
| `segment_sec` | number | 视频分段时长，单位秒（默认 `300`，即 5 分钟） |
| `jpeg_quality` | number | JPEG 图像质量（默认 `12`，数值越小质量越好） |
    `vflip` | bool | 垂直翻转 |
    `hmirror` | bool | 水平镜像 |
    `web_password` | string | Web 管理密码，已设置时显示 `"****"`，未设置时显示 `""` |
    `timelapse_interval_sec` | number | 延时摄影间隔时间，单位秒（默认 `0` = 连续录制）。当 > 0 时，每隔 N 秒拍摄一帧 |
    `cleanup_low_pct` | number | 存储清理触发阈值百分比（默认 `20`） |
    `cleanup_high_pct` | number | 存储清理目标百分比（默认 `30`） |
    `frame_drop_enabled` | bool | 启用智能帧丢弃 |
    `alert_webhook_url` | string | 告警通知的 Webhook URL |
    `alert_webhook_enabled` | bool | 启用 Webhook 告警通知 |
    `wifi_ssid_2` | string | 备用 WiFi SSID |
    `wifi_pass_2` | string | 备用 WiFi 密码，已设置时显示 `"****"` |
    `allow_ap_fallback` | bool | 允许在两个 WiFi 网络均失败时回退到 AP 模式

> **重要**：`webdav_pass` **不会**在此接口中返回（完全不包含在响应中）。这是设计上的安全考量。
> 只有 `wifi_pass`、`http_upload_pass`、`web_password` 和 `wifi_pass_2` 会以遮掩形式（`"****"`）返回。

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
  "upload_method": 1,
  "upload_base_path": "/MiBee Cam",
  "webdav_url": "https://dav.example.com",
  "webdav_user": "davuser",
  "webdav_pass": "davpassword",
  "webdav_enabled": true,
  "http_upload_url": "https://upload.example.com/api/cam",
  "http_upload_user": "httpuser",
  "http_upload_pass": "httppassword",
  "http_upload_skip_cert_verify": false,
  "resolution": 2,
  "fps": 15,
  "segment_sec": 600,
  "jpeg_quality": 8,
  "vflip": true,
  "hmirror": false,
  "web_password": "newpass",
  "timelapse_interval_sec": 0,
  "cleanup_low_pct": 20,
  "cleanup_high_pct": 30,
  "timezone": "CST-8",
  "alert_webhook_url": "",
  "alert_webhook_enabled": false,
  "wifi_ssid_2": "",
  "wifi_pass_2": "",
  "allow_ap_fallback": false
}
```

**可修改字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `wifi_ssid` | string | WiFi 网络名称 |
| `wifi_pass` | string | WiFi 密码 |
| `device_name` | string | 设备名称 |
| `upload_method` | uint8 | 上传方式：`0`=禁用、`1`=WebDAV、`2`=HTTP(S) |
| `upload_base_path` | string | 上传基础路径 |
| `webdav_url` | string | WebDAV 服务器地址 |
| `webdav_user` | string | WebDAV 用户名 |
| `webdav_pass` | string | WebDAV 密码 |
| `webdav_enabled` | bool | 启用/禁用 WebDAV 上传 |
| `http_upload_url` | string | HTTP(S) 上传完整 URL |
| `http_upload_user` | string | HTTP(S) 用户名 |
| `http_upload_pass` | string | HTTP(S) 密码 |
| `http_upload_skip_cert_verify` | bool | 跳过 HTTPS 证书验证 |
| `resolution` | number | 分辨率：`0`=VGA、`1`=SVGA、`2`=XGA |
| `fps` | number | 录像帧率 |
| `segment_sec` | number | 分段时长（秒） |
| `jpeg_quality` | number | JPEG 质量 |
| `vflip` | bool | 垂直翻转 |
| `hmirror` | bool | 水平镜像 |
    `web_password` | string | Web 管理密码 |
    `timelapse_interval_sec` | number | 延时摄影间隔时间（秒）（`0` = 连续录制） |
    `cleanup_low_pct` | number | 清理触发阈值（%） |
    `cleanup_high_pct` | number | 清理目标百分比（%） |
    `timezone` | string | 时区字符串（如 `"CST-8"`） |
    `alert_webhook_url` | string | 告警 Webhook URL |
    `alert_webhook_enabled` | bool | 启用告警 Webhook |
    `wifi_ssid_2` | string | 备用 WiFi SSID |
    `wifi_pass_2` | string | 备用 WiFi 密码 |
    `allow_ap_fallback` | bool | 允许 AP 回退

**密码字段特殊行为**：

四个密码字段（`wifi_pass`、`webdav_pass`、`http_upload_pass`、`web_password`）具有特殊处理逻辑：
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
// 切换上传方式为 HTTP(S)
async function switchToHttp() {
  const resp = await fetch('/api/config', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Password': 'admin'
    },
    body: JSON.stringify({
      upload_method: 2,
      http_upload_url: 'https://upload.example.com/api/cam',
      http_upload_user: 'httpuser',
      http_upload_pass: 'secret'
    })
  });
  return await resp.json();
}

---

## 密码字段行为详解

设备管理涉及四个密码字段，它们在 GET 和 POST 中的行为不同：

### GET /api/config 返回的密码字段

| 字段 | 返回行为 |
|------|----------|
| `wifi_pass` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `web_password` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `http_upload_pass` | 已设置时返回 `"****"`，未设置时返回 `""` |
| `webdav_pass` | **不返回**（完全不包含在响应中） |

### POST /api/config 密码处理逻辑

```
接收密码字段值 → 值是否为 "****"？
                    ↓ 是          ↓ 否
              保持原值不变    更新为新密码
```

所有四个密码字段（`wifi_pass`、`webdav_pass`、`http_upload_pass`、`web_password`）均遵循此逻辑。

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
| `device_name` | string | `"MiBee Cam"` | 设备名称 |
| `upload_method` | uint8 | `0` | 上传方式：0=禁用, 1=WebDAV, 2=HTTP(S) |
| `upload_base_path` | string | `"/MiBee Cam"` | 上传基础路径 |
| `webdav_url` | string | `""` | WebDAV 地址 |
| `webdav_user` | string | `""` | WebDAV 用户名 |
| `webdav_pass` | string | `""` | WebDAV 密码 |
| `webdav_enabled` | bool | `false` | WebDAV 上传开关 |
| `http_upload_url` | string | `""` | HTTP(S) 上传 URL |
| `http_upload_user` | string | `""` | HTTP(S) 用户名 |
| `http_upload_pass` | string | `""` | HTTP(S) 密码 |
| `http_upload_skip_cert_verify` | bool | `false` | 跳过 HTTPS 证书验证 |
| `resolution` | number | `1` | SVGA (800×600) |
| `fps` | number | `10` | 10 帧/秒 |
| `segment_sec` | number | `300` | 5 分钟/段 |
| `jpeg_quality` | number | `12` | JPEG 质量 |
| `vflip` | bool | `false` | 垂直翻转 |
| `hmirror` | bool | `false` | 水平镜像 |
    `web_password` | string | `"admin"` | Web 管理密码 |
    `timelapse_interval_sec` | number | `0` | 0 = 连续录制，>0 = 延时摄影间隔（秒） |
    `cleanup_low_pct` | number | `20` | 清理触发阈值（%） |
    `cleanup_high_pct` | number | `30` | 清理目标（%） |
    `timezone` | string | `"CST-8"` | 时区 |
    `frame_drop_enabled` | bool | `true` | 启用智能帧丢弃 |
    `alert_webhook_url` | string | `""` | 告警 Webhook URL |
    `alert_webhook_enabled` | bool | `false` | 启用告警 Webhook |
    `wifi_ssid_2` | string | `""` | 备用 WiFi SSID |
    `wifi_pass_2` | string | `""` | 备用 WiFi 密码 |
    `allow_ap_fallback` | bool | `false` | 允许 AP 模式回退
