# User Guide

> Daily usage guide for ESP32-S3 Camera Monitor firmware

## Overview

This document describes the daily usage methods of ESP32-S3 Camera Monitor firmware, including Web management interface operations, configuration parameter descriptions, recording and storage management, NAS upload functions, etc.

## Web Management Interface

The device has a built-in HTTP server providing Web management interface and REST API.

### Access Address

| Mode | Address |
|------|---------|
| AP Mode | `http://192.168.4.1` |
| STA Mode | `http://<deviceIP>` |

- AP Mode: Connect to WiFi hotspot `MiBeeHomeCam-XXXX` (password `12345678`), then access `http://192.168.4.1`
- STA Mode: After device connects to router, access via the IP assigned by router

### Login Password

Default management password: `admin`. API requests involving write operations need to pass the password via `X-Password` request header or `?password=xxx` query parameter.

### Page Description

| Page | Function |
|------|----------|
| index | Status overview (recording status, WiFi, storage, camera model, uptime) |
| config | Configuration management (all parameters including WiFi, recording, NAS upload) |
| preview | Real-time video preview (MJPEG stream in browser) |
| files | Recording file management (browse, download, delete) |

### Dashboard Page

![Dashboard](images/index-dashboard.png)

The dashboard shows real-time device status: recording state, current file, WiFi connection, SD card usage, camera model, chip temperature, and uptime.

### Configuration Page

![Configuration](images/config-page.png)

All device parameters can be modified here: WiFi credentials, video resolution/FPS/quality, recording segment duration, WebDAV/HTTP(S) upload settings, camera flip/mirror, and system password.

### File Manager Page

![File Manager](images/files-page.png)

Browse recordings organized by date. Features collapsible date groups, batch selection, folder-level download/delete, and persistent authentication.

### Preview Page

![Video Preview](images/preview-page.png)

Real-time MJPEG video stream viewable directly in the browser. Supports up to 2 simultaneous viewers.

## Configuration Management

### Via Web Interface

Open the `config` page, directly modify parameters and save. Changes take effect immediately and are persisted to NVS flash.

### Via API

Use `POST /api/config` interface to modify configuration, requires `X-Password` authentication:

```bash
# Modify WiFi
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' -H 'X-Password: admin' \
  -d '{"wifi_ssid":"MyWiFi","wifi_pass":"mypassword"}'

# Modify video parameters
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' -H 'X-Password: admin' \
  -d '{"resolution":1,"fps":10,"jpeg_quality":12,"segment_sec":300}'
```

> For password fields, passing `"****"` means no modification, passing actual value updates it.

### Complete Configuration Parameter Table

#### WiFi Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `wifi_ssid` | string | `""` | WiFi name, enters AP mode if empty |
| `wifi_pass` | string | `""` | WiFi password |

#### Device Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `device_name` | string | `"MiBeeHomeCam"` | Device name |
| `web_password` | string | `"admin"` | Web management password |

#### Upload Method Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `upload_method` | uint8 | `0` | Upload method: 0=Disabled, 1=WebDAV, 2=HTTP(S) |
| `upload_base_path` | string | `"/MiBeeHomeCam"` | Upload base path |
| `webdav_url` | string | `""` | WebDAV server URL |
| `webdav_user` | string | `""` | WebDAV username |
| `webdav_pass` | string | `""` | WebDAV password (returns `****` on GET) |
| `webdav_enabled` | bool | `false` | Enable WebDAV upload |
| `http_upload_url` | string | `""` | HTTP(S) upload full URL |
| `http_upload_user` | string | `""` | HTTP(S) username |
| `http_upload_pass` | string | `""` | HTTP(S) password (returns `****` on GET) |
| `http_upload_skip_cert_verify` | bool | `false` | Skip HTTPS certificate verification |

#### Camera Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `vflip` | bool | `false` | Vertical flip |
| `hmirror` | bool | `false` | Horizontal mirror |
#### Video Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `resolution` | uint8 | `1` | Resolution: 0=VGA(640×480), 1=SVGA(800×600), 2=XGA(1024×768) |
| `fps` | uint8 | `10` | Frame rate, range 1-30 |
| `segment_sec` | uint16 | `300` | Recording segment duration (seconds) |
| `jpeg_quality` | uint8 | `12` | JPEG quality, 1-63, lower value means better quality |

## LED Indicator

The onboard LED (GPIO21, active-low) reflects the device's current status through different blinking patterns:

| Mode | LED Behavior | Meaning |
|------|-------------|---------|
| LED_STARTING | Solid | System booting |
| LED_AP_MODE | Slow blink (1-second cycle) | AP hotspot mode, waiting for configuration |
| LED_WIFI_CONNECTING | Fast blink (200ms cycle) | Connecting to WiFi |
| LED_RUNNING | Off | Normal operation, recording |
| LED_ERROR | Double blink | Error state (SD card failure, etc.) |

**Double Blink Timing Details** (LED_ERROR):

```
ON 200ms → OFF 200ms → ON 200ms → OFF 1000ms → Loop
|← 1st blink →|← 2nd blink →|← long interval →|
```

## WiFi Configuration

### AP Mode

Device automatically enters AP mode on first boot or when WiFi is not configured:

- SSID: `MiBeeHomeCam-XXXX` (XXXX is last 4 hex digits of MAC address)
- Password: `12345678`
- IP Address: `192.168.4.1`
- Encryption: WPA2-PSK
- Max connections: 4

### Switch to STA Mode

After configuring `wifi_ssid` and `wifi_pass`, restart the device. Device automatically connects to router. Restart required to switch modes.

### Auto Reconnect

In STA mode, after disconnection, device automatically attempts to reconnect every 60 seconds without manual intervention.

## Recording Management

### Auto Recording

After device boots, if SD card is ready, recording starts automatically.

### Manual Control

Manually start or stop recording via API:

```bash
# Start recording
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# Stop recording
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

### Recording Format

- **Format**: AVI (MJPEG encoding)
- **Segmentation**: Auto-segmented by `segment_sec` (default 300 seconds = 5 minutes)
- **File naming**: `REC_YYYYMMDD_HHMMSS.avi` (e.g., `REC_20260424_143000.avi`)
- **Storage path**: `/sdcard/recordings/YYYY-MM/DD/` (organized by date)

After each segment completes: NAS upload → Storage cleanup → Start next segment.

## Storage Management

### Recording Path

```
/sdcard/recordings/
├── 2026-04/
│   ├── 24/
│   │   ├── REC_20260424_080000.avi
│   │   ├── REC_20260424_080500.avi
│   │   └── ...
│   └── 25/
│       └── ...
```

### Circular Storage

Automatic cleanup when SD card free space is insufficient:

- **Below 20%**: Start deleting oldest recording files
- **Continue cleanup until 30%**: Check after each segment recording completes
- Safety limit: Max 100 files deleted per single cleanup

### Startup Cleanup

On device startup, automatically scans `/sdcard/recordings/` directory and deletes incomplete AVI files with RIFF header size of 0 (usually caused by abnormal power-off).

### SD Card Hot-plug

Device checks SD card status every 10 seconds:

| Event | Behavior |
|-------|----------|
| SD card removed | Stop current recording, LED shows error state |
| SD card inserted | Remount, automatically resume recording, LED returns to normal |

## NAS Upload

### Upload Method Configuration

Select upload method via the `upload_method` field (mutually exclusive, only one can be active):

| Value | Method | Description |
|-------|--------|-------------|
| `0` | Disabled | No upload, local SD card storage only |
| `1` | WebDAV | Upload via WebDAV protocol to NAS |
| `2` | HTTP(S) | Upload via HTTP PUT, supports HTTPS |
#### WebDAV Upload Configuration

| Parameter | Description |
|-----------|-------------|
| `webdav_url` | WebDAV server complete URL |
| `webdav_user` | WebDAV username |
| `webdav_pass` | WebDAV password |
| `webdav_enabled` | Set to `true` to enable (requires `upload_method=1`) |

#### HTTP(S) Upload Configuration

| Parameter | Description |
|-----------|-------------|
| `http_upload_url` | HTTP(S) upload full URL |
| `http_upload_user` | HTTP(S) username |
| `http_upload_pass` | HTTP(S) password |
| `http_upload_skip_cert_verify` | Skip HTTPS certificate verification (set to `true` for self-signed certs) |
| `upload_base_path` | Upload base path (default `/MiBeeHomeCam`) |

**Mutually exclusive**: Only one upload method can be active at a time. Previous upload configurations are preserved in NVS when switching methods but will not be used.

### Upload Queue

- Queue capacity: 16 files
- Max 3 retries per file
- After 10 consecutive failures, pause upload for 5 minutes
- Background task processes asynchronously without affecting recording

### View Upload Status

```bash
curl http://192.168.4.1/api/status
```

Returns `upload_queue` (pending upload count) and `last_upload` (last upload time).

## TF Card Configuration Override

On startup, device reads configuration files on SD card, which have higher priority than configurations saved in NVS.

### wifi.txt

Path: `/sdcard/config/wifi.txt`

```
SSID=YourWiFiName
PASS=YourWiFiPassword
```

### nas.txt

Path: `/sdcard/config/nas.txt`

UPLOAD_METHOD=webdav
WEBDAV_URL=https://dav.example.com/path
WEBDAV_USER=user
WEBDAV_PASS=pass
WEBDAV_ENABLED=false
HTTP_UPLOAD_URL=
HTTP_UPLOAD_USER=
HTTP_UPLOAD_PASS=
HTTP_UPLOAD_SKIP_CERT=true
UPLOAD_BASE_PATH=/MiBeeHomeCam
```

Lines starting with `#` are ignored.

### Configuration Priority

```
TF Card Config Files > NVS Flash > Default Values
```

Configurations read from TF card are synchronized back to NVS to ensure consistency.

## Video Stream

### Browser Viewing

Open directly in browser:

```
http://<deviceIP>/stream
```

### Client Limit

Supports maximum **2** simultaneously connected streaming clients. Returns 503 error when limit exceeded.

### Adjust Quality

Modify video parameters via `POST /api/config`:

| Parameter | Values | Description |
|-----------|--------|-------------|
| `resolution` | 0, 1, 2 | VGA(640×480), SVGA(800×600), XGA(1024×768) |
| `fps` | 1-30 | Frame rate, higher is smoother but uses more bandwidth |
| `jpeg_quality` | 1-63 | Quality, lower value means better quality (recommended 10-20) |

### RTSP Real-time Stream

In addition to the HTTP MJPEG stream, the device also provides an RTSP real-time stream service:

```
rtsp://<deviceIP>:554/stream
```

**VLC Playback**: Open VLC → Media → Open Network Stream → Enter `rtsp://<deviceIP>:554/stream`

**Max Concurrent Clients**: 2

### Camera Flip Settings

Modify camera flip/mirror via `POST /api/config`:

```bash
curl -X POST http://192.168.4.1/api/config \
  -H 'Content-Type: application/json' -H 'X-Password: admin' \
  -d '{"vflip":true,"hmirror":true}'
```

| Parameter | Description |
|-----------|-------------|
| `vflip` | Vertical flip (upside down) |
| `hmirror` | Horizontal mirror (left-right flip) |

> ⚠️ Flip settings take effect immediately on **newly captured frames**. Already recorded AVI files are not affected. It is recommended to modify flip settings while recording is stopped.
## Time Management

### Auto Sync

In STA mode, device automatically syncs time via NTP after startup:

- Servers: `cn.pool.ntp.org`, `ntp.aliyun.com`
- Sync timeout: 5 seconds (continues asynchronously after timeout)

### Manual Setting

```bash
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year":2026,"month":4,"day":24,"hour":14,"min":30,"sec":0}'
```

## Factory Reset

### Method 1: BOOT Button

Hold BOOT button (GPIO0) for **5 seconds**, device automatically restores factory settings and restarts.

### Method 2: Web Interface

Click "Factory Reset" button on Web configuration page.

### Method 3: API

```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

### Reset Result

All configuration parameters restored to default values (WiFi cleared, password reset to `admin`), device enters AP mode after restart. Recording files on TF card are not deleted.

---

## Feature Mutual Exclusion & Resource Limits

### ⚠️ Upload Method Mutual Exclusion
Upload method (`upload_method`) can only be one of: Disabled, WebDAV, or HTTP(S). When switching methods, previous upload configurations are preserved in NVS but will not be used.

### ⚠️ Video Stream Concurrency Limits
The system has 3 video stream consumers sharing the camera frame buffer:
1. **Recording task** (recording_task) — Core 0, highest priority
2. **MJPEG HTTP stream** (/stream) — Max 2 clients
3. **RTSP stream** (rtsp://<IP>:554/stream) — Max 2 clients

The camera frame buffer pool size is 2 (double buffer). When all 3 consumers are active simultaneously, frame contention may occur. Recommendations:
- Normal use: Recording + 1 HTTP stream OR Recording + 1 RTSP stream
- Not recommended: Multiple HTTP and RTSP stream clients simultaneously
- Recording task has highest priority and will not drop frames due to stream clients

### ⚠️ RTSP vs HTTP Stream
- RTSP and HTTP MJPEG streams are **independent**, can run simultaneously
- Both share camera frame buffers but not network connections
- RTSP uses TCP-interleaved mode (port 554), no UDP
- RTSP v1 does not support authentication

### ⚠️ Flip Settings Don't Affect Recorded Videos
vflip/hmirror settings take effect immediately on **newly captured frames**, but:
- Already recorded AVI files are not affected
- Flip can be toggled during recording, but frames within the same segment may have inconsistent orientation
- It is recommended to modify flip settings after stopping recording

### ⚠️ Upgrading from Older Versions
When upgrading from an older version (with FTP support), the device automatically migrates NVS configuration:
- Old `ftp_enabled=true` → `upload_method=1` (WebDAV)
- Old `webdav_enabled=true` → `upload_method=1` (WebDAV)
- Old FTP-related NVS keys are automatically deleted