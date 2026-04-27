# GET /api/scan — WiFi Scan

> [← Video Stream](stream.md) | [Complete Examples →](examples.md)

---

Scan surrounding WiFi access points, return available network list.

**Authentication**: None required

**Source**: `api_scan_handler` (web_server.c)

**Response Example**:
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

**Response Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `networks` | array | Scanned networks array, returns max 20 networks |
| `networks[].ssid` | string | Network name (SSID) |
| `networks[].rssi` | number | Signal strength (negative value, closer to 0 means stronger signal, e.g., `-45` stronger than `-85`) |
| `networks[].auth` | number | Authentication mode (`wifi_auth_mode_t` enum value) |

**Common auth Values**:

| Value | Meaning |
|-------|---------|
| 0 | Open network (no password) |
| 1 | WEP |
| 2 | WPA-PSK |
| 3 | WPA2-PSK |
| 4 | WPA/WPA2-PSK |
| 5 | WPA2-Enterprise |

**Error Responses**:

| Status Code | Condition | Error Message |
|-------------|-----------|---------------|
| 500 | Scan failed | `"Scan failed"` |

**cURL Example**:
```bash
curl http://192.168.4.1/api/scan
```

**JavaScript Example**:
```javascript
async function scanWifi() {
  const resp = await fetch('/api/scan');
  const { data } = await resp.json();
  data.networks
    .sort((a, b) => b.rssi - a.rssi)
    .forEach(n => {
      console.log(`${n.ssid} — RSSI: ${n.rssi} — Auth: ${n.auth}`);
    });
}
```