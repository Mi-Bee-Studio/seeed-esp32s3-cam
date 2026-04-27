# GET /api/scan — WiFi 扫描

> [← 视频流](stream.md) | [完整示例 →](examples.md)

---

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
