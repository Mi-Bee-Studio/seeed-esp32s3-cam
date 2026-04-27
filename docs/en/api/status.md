# GET /api/status — 获取设备状态

> [← API 概述](overview.md) | [配置接口 →](config.md)

---

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
