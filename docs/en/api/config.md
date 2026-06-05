# Configuration Interface

> [← Status Query](status.md) | [File Management →](files.md)

---

## GET /api/config — Get Current Configuration

Get all current configuration items of the device. Password fields are masked.

**Authentication**: None required

**Source**: `api_config_get_handler` (web_server.c)

**Response Example**:
```json
{
  "ok": true,
  "data": {
    "wifi_ssid": "MickeyMiRoute",
    "wifi_pass": "****",
    "device_name": "MiBeeHomeCam",
    "upload_method": 1,
    "upload_base_path": "/MiBeeHomeCam",
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

**Response Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `wifi_ssid` | string | WiFi network name, empty string indicates AP mode |
| `wifi_pass` | string | WiFi password, shows `"****"` if set, `""` if not set |
| `device_name` | string | Device name (default `"MiBeeHomeCam"`) |
| `upload_method` | uint8 | Upload method: `0`=Disabled, `1`=WebDAV, `2`=HTTP(S) |
| `upload_base_path` | string | Upload base path (default `"/MiBeeHomeCam"`) |
| `webdav_url` | string | WebDAV server address |
| `webdav_user` | string | WebDAV username |
| `webdav_enabled` | bool | Enable WebDAV upload (only effective when `upload_method=1`) |
| `http_upload_url` | string | HTTP(S) upload full URL |
| `http_upload_user` | string | HTTP(S) username |
| `http_upload_pass` | string | HTTP(S) password, shows `"****"` if set, `""` if not set |
| `http_upload_skip_cert_verify` | bool | Skip HTTPS certificate verification |
| `resolution` | number | Resolution code: `0`=VGA(640×480), `1`=SVGA(800×600), `2`=XGA(1024×768) |
| `fps` | number | Recording frame rate (default `10`) |
| `segment_sec` | number | Video segment duration in seconds (default `300`, i.e., 5 minutes) |
| `jpeg_quality` | number | JPEG image quality (default `12`, lower value means better quality) |
| `vflip` | bool | Vertical flip |
| `hmirror` | bool | Horizontal mirror |
| `web_password` | string | Web management password, shows `"****"` if set, `""` if not set |

| `web_password` | string | Web management password, shows `"****"` if set, `""` if not set |
| `timelapse_interval_sec` | number | Timelapse interval in seconds (default `0` = continuous recording). When > 0, captures one frame every N seconds |
| `cleanup_low_pct` | number | Storage cleanup trigger threshold percentage (default `20`) |
| `cleanup_high_pct` | number | Storage cleanup target percentage (default `30`) |
| `alert_webhook_url` | string | Webhook URL for alert notifications |
| `alert_webhook_enabled` | bool | Enable webhook alert notifications |
| `wifi_ssid_2` | string | Backup WiFi SSID |
| `wifi_pass_2` | string | Backup WiFi password, shows `"****"` if set |
| `allow_ap_fallback` | bool | Allow fallback to AP mode when both WiFi networks fail |
> **Important**: `webdav_pass` is **NOT** returned by this endpoint (completely excluded from response). This is a security design consideration.
> Only `wifi_pass`, `http_upload_pass`, `web_password` and `wifi_pass_2` are returned in masked form (`"****"`).
> Only `wifi_pass`, `http_upload_pass` and `web_password` are returned in masked form (`"****"`).

**cURL Example**:
```bash
curl http://192.168.4.1/api/config
```

**JavaScript Example**:
```javascript
const resp = await fetch('/api/config');
const { data } = await resp.json();
console.log(`Device: ${data.device_name}, Resolution: ${data.resolution}`);
```

---

## POST /api/config — Update Configuration

Update device configuration. Request body is JSON format, only need to include fields to modify. Configuration is immediately saved to NVS non-volatile storage after modification.

**Authentication**: Password required

**Source**: `api_config_post_handler` (web_server.c)

**Request Body**:
```json
{
  "wifi_ssid": "NewWiFi",
  "wifi_pass": "newpassword",
  "device_name": "MyCamera",
  "upload_method": 1,
  "upload_base_path": "/MiBeeHomeCam",
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

**Modifiable Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `wifi_ssid` | string | WiFi network name |
| `wifi_pass` | string | WiFi password |
| `device_name` | string | Device name |
| `upload_method` | uint8 | Upload method: `0`=Disabled, `1`=WebDAV, `2`=HTTP(S) |
| `upload_base_path` | string | Upload base path |
| `webdav_url` | string | WebDAV server address |
| `webdav_user` | string | WebDAV username |
| `webdav_pass` | string | WebDAV password |
| `webdav_enabled` | bool | Enable/disable WebDAV upload |
| `http_upload_url` | string | HTTP(S) upload full URL |
| `http_upload_user` | string | HTTP(S) username |
| `http_upload_pass` | string | HTTP(S) password |
| `http_upload_skip_cert_verify` | bool | Skip HTTPS certificate verification |
| `resolution` | number | Resolution: `0`=VGA, `1`=SVGA, `2`=XGA |
| `fps` | number | Recording frame rate |
| `segment_sec` | number | Segment duration (seconds) |
| `jpeg_quality` | number | JPEG quality |
| `vflip` | bool | Vertical flip |
| `hmirror` | bool | Horizontal mirror |
| `web_password` | string | Web management password |
| `timelapse_interval_sec` | number | Timelapse interval in seconds (`0` = continuous) |
| `cleanup_low_pct` | number | Cleanup trigger threshold (%) |
| `cleanup_high_pct` | number | Cleanup target percentage (%) |
| `timezone` | string | Timezone string (e.g., `"CST-8"`) |
| `alert_webhook_url` | string | Alert webhook URL |
| `alert_webhook_enabled` | bool | Enable alert webhook |
| `wifi_ssid_2` | string | Backup WiFi SSID |
| `wifi_pass_2` | string | Backup WiFi password |
| `allow_ap_fallback` | bool | Allow AP fallback |

**Password Field Special Behavior**:

**Password Field Special Behavior**:

The four password fields (`wifi_pass`, `webdav_pass`, `http_upload_pass`, `web_password`) have special handling logic:
- If value is `"****"` (four asterisks), the field is **ignored** and current password is kept unchanged
- If value is any other string, it is updated to the new password value
- This design allows clients to echo back data retrieved from `GET /api/config` without leaking passwords

**Success Response**:
```json
{
  "ok": true
}
```

> Note: No `data` field on success.

**Error Responses**:

| Status Code | Condition | Error Message |
|-------------|-----------|---------------|
| 401 | Wrong or missing password | `"Unauthorized"` |
| 400 | Empty body or exceeds 2048 bytes | `"Empty or too large body"` |
| 400 | JSON parsing failed | `"Invalid JSON"` |

**cURL Examples**:
```bash
# Modify WiFi configuration
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "HomeWiFi", "wifi_pass": "mypassword"}'

# Only modify resolution and frame rate
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"resolution": 2, "fps": 15}'

# Keep original password unchanged (pass ****)
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "NewNet", "wifi_pass": "****", "web_password": "****"}'
```

**JavaScript Example**:
```javascript
// Switch upload method to HTTP(S)
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

## Password Field Behavior Details

The device management involves four password fields, with different behavior in GET and POST:

### Password Fields Returned by GET /api/config

| Field | Return Behavior |
|-------|-----------------|
| `wifi_pass` | Returns `"****"` if set, `""` if not set |
| `web_password` | Returns `"****"` if set, `""` if not set |
| `http_upload_pass` | Returns `"****"` if set, `""` if not set |
| `webdav_pass` | **Not returned** (completely excluded from response) |

### POST /api/config Password Handling Logic

```
Receive password field value → Is value "****"?
                    ↓ Yes          ↓ No
              Keep original      Update to new password
```

All four password fields (`wifi_pass`, `webdav_pass`, `http_upload_pass`, `web_password`) follow this logic.

### Typical Usage Scenarios

**Scenario 1: Echo back after GET without modifying password**
```javascript
// GET returns wifi_pass: "****", web_password: "****"
// Echoing back "****" keeps password unchanged
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

**Scenario 2: Modify only part of config (without password fields)**
```javascript
// When password fields are not included, passwords are not modified
await fetch('/api/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', 'X-Password': 'admin' },
  body: JSON.stringify({ fps: 15, resolution: 2 })
});
```

**Scenario 3: Change password**
```javascript
await fetch('/api/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json', 'X-Password': 'admin' },
  body: JSON.stringify({ web_password: 'newpassword' })
});
// Subsequent requests must use new password
```

---

## Resolution Reference

| Value | Name | Resolution |
|-------|------|------------|
| 0 | VGA | 640 × 480 |
| 1 | SVGA | 800 × 600 |
| 2 | XGA | 1024 × 768 |

## Default Configuration Values

| Config Item | Type | Default | Description |
|-------------|------|---------|-------------|
| `wifi_ssid` | string | `""` | Empty means AP mode |
| `wifi_pass` | string | `""` | WiFi password |
| `device_name` | string | `"MiBeeHomeCam"` | Device name |
| `upload_method` | uint8 | `0` | Upload method: 0=Disabled, 1=WebDAV, 2=HTTP(S) |
| `upload_base_path` | string | `"/MiBeeHomeCam"` | Upload base path |
| `webdav_url` | string | `""` | WebDAV address |
| `webdav_user` | string | `""` | WebDAV username |
| `webdav_pass` | string | `""` | WebDAV password |
| `webdav_enabled` | bool | `false` | WebDAV upload switch |
| `http_upload_url` | string | `""` | HTTP(S) upload URL |
| `http_upload_user` | string | `""` | HTTP(S) username |
| `http_upload_pass` | string | `""` | HTTP(S) password |
| `http_upload_skip_cert_verify` | bool | `false` | Skip HTTPS certificate verification |
| `resolution` | number | `1` | SVGA (800×600) |
| `fps` | number | `10` | 10 fps |
| `segment_sec` | number | `300` | 5 minutes/segment |
| `jpeg_quality` | number | `12` | JPEG quality |
| `vflip` | bool | `false` | Vertical flip |
| `hmirror` | bool | `false` | Horizontal mirror |
| `web_password` | string | `"admin"` | Web management password |
| `timelapse_interval_sec` | number | `0` | 0 = continuous recording, >0 = timelapse interval (seconds) |
| `cleanup_low_pct` | number | `20` | Cleanup trigger threshold (%) |
| `cleanup_high_pct` | number | `30` | Cleanup target (%) |
| `timezone` | string | `"CST-8"` | Timezone |
| `frame_drop_enabled` | bool | `true` | Enable smart frame dropping |
| `alert_webhook_url` | string | `""` | Alert webhook URL |
| `alert_webhook_enabled` | bool | `false` | Enable alert webhook |
| `wifi_ssid_2` | string | `""` | Backup WiFi SSID |
| `wifi_pass_2` | string | `""` | Backup WiFi password |
| `allow_ap_fallback` | bool | `false` | Allow AP mode fallback |