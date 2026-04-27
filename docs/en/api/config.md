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
    "wifi_ssid": "MyWiFi",
    "wifi_pass": "****",
    "device_name": "MiBeeHomeCam",
    "ftp_host": "192.168.1.200",
    "ftp_port": 21,
    "ftp_user": "ftpuser",
    "ftp_path": "/MiBeeHomeCam",
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

**Response Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `wifi_ssid` | string | WiFi network name, empty string indicates AP mode |
| `wifi_pass` | string | WiFi password, shows `"****"` if set, `""` if not set |
| `device_name` | string | Device name (default `"MiBeeHomeCam"`) |
| `ftp_host` | string | FTP server address |
| `ftp_port` | number | FTP port (default `21`) |
| `ftp_user` | string | FTP username |
| `ftp_path` | string | FTP upload path (default `"/MiBeeHomeCam"`) |
| `ftp_enabled` | bool | Enable FTP upload |
| `webdav_url` | string | WebDAV server address |
| `webdav_user` | string | WebDAV username |
| `webdav_enabled` | bool | Enable WebDAV upload |
| `resolution` | number | Resolution code: `0`=VGA(640×480), `1`=SVGA(800×600), `2`=XGA(1024×768) |
| `fps` | number | Recording frame rate (default `10`) |
| `segment_sec` | number | Video segment duration in seconds (default `300`, i.e., 5 minutes) |
| `jpeg_quality` | number | JPEG image quality (default `12`, lower value means better quality) |
| `web_password` | string | Web management password, shows `"****"` if set, `""` if not set |

> **Important**: `ftp_pass` and `webdav_pass` are **NOT** returned by this endpoint. This is a security design consideration.
> Only `wifi_pass` and `web_password` are returned in masked form (`"****"`).

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

**Modifiable Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `wifi_ssid` | string | WiFi network name |
| `wifi_pass` | string | WiFi password |
| `device_name` | string | Device name |
| `ftp_host` | string | FTP server address |
| `ftp_port` | number | FTP port number |
| `ftp_user` | string | FTP username |
| `ftp_pass` | string | FTP password |
| `ftp_path` | string | FTP upload path |
| `ftp_enabled` | bool | Enable/disable FTP upload |
| `webdav_url` | string | WebDAV server address |
| `webdav_user` | string | WebDAV username |
| `webdav_pass` | string | WebDAV password |
| `webdav_enabled` | bool | Enable/disable WebDAV upload |
| `resolution` | number | Resolution: `0`=VGA, `1`=SVGA, `2`=XGA |
| `fps` | number | Recording frame rate |
| `segment_sec` | number | Segment duration (seconds) |
| `jpeg_quality` | number | JPEG quality |
| `web_password` | string | Web management password |

**Password Field Special Behavior**:

The four password fields (`wifi_pass`, `ftp_pass`, `webdav_pass`, `web_password`) have special handling logic:
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
// Update FTP configuration
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

## Password Field Behavior Details

The device management involves four password fields, with different behavior in GET and POST:

### Password Fields Returned by GET /api/config

| Field | Return Behavior |
|-------|-----------------|
| `wifi_pass` | Returns `"****"` if set, `""` if not set |
| `web_password` | Returns `"****"` if set, `""` if not set |
| `ftp_pass` | **Not returned** (completely excluded from response) |
| `webdav_pass` | **Not returned** (completely excluded from response) |

### POST /api/config Password Handling Logic

```
Receive password field value → Is value "****"?
                    ↓ Yes          ↓ No
              Keep original      Update to new password
```

All four password fields (`wifi_pass`, `ftp_pass`, `webdav_pass`, `web_password`) follow this logic.

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
| `ftp_host` | string | `""` | FTP server |
| `ftp_port` | number | `21` | FTP port |
| `ftp_user` | string | `""` | FTP username |
| `ftp_pass` | string | `""` | FTP password |
| `ftp_path` | string | `"/MiBeeHomeCam"` | FTP path |
| `ftp_enabled` | bool | `false` | FTP upload switch |
| `webdav_url` | string | `""` | WebDAV address |
| `webdav_user` | string | `""` | WebDAV username |
| `webdav_pass` | string | `""` | WebDAV password |
| `webdav_enabled` | bool | `false` | WebDAV upload switch |
| `resolution` | number | `1` | SVGA (800×600) |
| `fps` | number | `10` | 10 fps |
| `segment_sec` | number | `300` | 5 minutes/segment |
| `jpeg_quality` | number | `12` | JPEG quality |
| `web_password` | string | `"admin"` | Web management password |