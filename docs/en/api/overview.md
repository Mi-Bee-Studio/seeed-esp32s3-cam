# API Overview

> ESP32-S3 Camera Monitor Firmware REST API Documentation

[Status Query](status.md) | [Configuration Interface](config.md) | [File Management](files.md) | [Device Control](control.md) | [Video Stream](stream.md) | [WiFi Scan](wifi.md) | [Complete Examples](examples.md)

---

This firmware API documentation is based on `web_server.c`, `mjpeg_streamer.c`, `config_manager.c` source code. Last updated: 2026-07-21.

## Base Address

| Mode | Address |
|------|---------|
| AP Mode (default) | `http://192.168.4.1` |
| STA Mode | `http://<deviceIP>` |

## Authentication

Some endpoints require password authentication. Two methods are supported:

| Method | Format | Example |
|--------|--------|---------|
| Request Header | `X-Password: <password>` | `X-Password: admin` |
| Query Parameter | `?password=<password>` | `?password=admin` |

- **Default Password**: `admin` (can be modified via `POST /api/config` by changing `web_password` field)
- Authentication logic first checks `X-Password` request header, then checks `password` query parameter
- Authentication failure returns `401 Unauthorized` with response body: `{"ok": false, "error": "Unauthorized"}`

## Unified Response Format

All API endpoints return `Content-Type: application/json` with unified format:

**Success Response**:
```json
{
  "ok": true,
  "data": { ... }
}
```

**Error Response**:
```json
{
  "ok": false,
  "error": "Error description message"
}
```

> Note: `POST /api/config` returns `{"ok": true}` on success without `data` field.

## CORS Cross-Origin Support

All HTTP responses (including error responses and static files) include the following CORS headers:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Password
```

`OPTIONS` requests (preflight requests) return the above CORS headers and empty response body with status code 200.

#MJ|## Endpoint Overview
#JQ|
#HT|| # | Method | Path | Auth | Description |
#MN||---|--------|------|------|-------------|
#XB|| 1 | GET | `/api/status` | No | Get device status |
#RZ|| 2 | GET | `/api/config` | No | Get current configuration |
#PV|| 3 | POST | `/api/config` | **Yes** | Update configuration |
#PW|| 4 | GET | `/api/files` | No | Get recording file list |
#BZ|| 5 | DELETE | `/api/files?name=xxx` | **Yes** | Delete specified file |
#SW|| 6 | POST | `/api/files/batch` | **Yes** | Batch delete files |
#XN|| 7 | GET | `/api/download?name=xxx` | No | Download specified file |
#YS|| 8 | GET | `/api/scan` | No | Scan WiFi networks |
#VZ|| 9 | POST | `/api/time` | **Yes** | Manually set system time |
#JR|| 10 | POST | `/api/record?action=start\|stop` | **Yes** | Control recording |
#ZH|| 11 | POST | `/api/reset` | **Yes** | Factory reset |
#HT|| 12 | POST | `/api/ota` | **Yes** | Trigger OTA firmware update from URL |
#XP|| 13 | POST | `/api/format` | **Yes** | Format SD card |
#XB|| 14 | GET | `/metrics` | No | Prometheus metrics (text format) |
#RR|| 15 | GET | `/setup` | No | First-time WiFi setup wizard page |
#PP|| 16 | GET | `/ota` | No | OTA firmware update web page |
#ZN|| 17 | GET | `/*` | No | Static files (Web UI) |
#NJ|| 18 | OPTIONS | `/*` | No | CORS preflight request |
#SQ|| 19 | POST | `/onvif/device_service` | No | ONVIF Device Service (SOAP) |
#BJ|| 20 | POST | `/onvif/media_service` | No | ONVIF Media Service (SOAP) |
#HX|
| RTSP | `rtsp://<IP>:554/stream` | Digest | MJPEG+G.711 dual-track stream |
| MJPEG | `http://<IP>:81/stream` | No | MJPEG live stream (video only) |
| Audio | `http://<IP>/api/audio` | No | HTTP chunked G.711 μ-law audio stream (for Web preview) |

Note: RTSP provides audio+video for NVR integration. MJPEG (port 81) remains video-only.
#NJ|## `/api/status` Response Fields


`/api/status` returns JSON with following fields:

```json
{

  "ok": true,

  "data": {

    "recording": false,

    "wifi_state": "STA_CONNECTED",

    "sd_available": 75.5,

    "sd_total": 29.7,

    "sd_free_percent": 75.5,

    "camera_sensor": "OV2640",

    "camera_res": "SVGA",

    "camera_quality": 12,

    "chip_temp": 42.5,

    "free_heap": 185432,

    "free_psram": 4194304,
    "free_psram": 4194304,
    "timelapse_mode": 0,
    "motion_score": 0,
    "firmware_version": "0.4.0",
    "upload_queue": 0,
    "last_upload": "",
    "frames_dropped": 0

  }
}
}
```

#### New Field: `chip_temp`

- **Type**: `number` (temperature in Celsius)
- **Range**: 20-80°C (typical ESP32-S3 operating range)
- **Description**: Internal ESP32-S3 die temperature
- **Added in**: Version with temperature monitoring support

### `/api/files/batch` - Batch File Operations

Delete multiple recording files in a single request.

**Request**:
```json
{
  "names": ["file1.avi", "file2.avi", "file3.avi"]
}
```

**Response**:
```json
{
  "ok": true,
  "data": {
    "deleted": 2,
    "failed": 1
  }
}
```

### `/api/files/batch` - Batch File Operations

**Authentication**: Required (X-Password header or ?password= query param)

---

### POST /api/ota - OTA Firmware Update

Trigger firmware update from a URL.

**Request:**
```json
{"url": "https://example.com/firmware/mibee_cam.bin"}
```

**Response:** `{"ok": true, "data": {"message": "OTA update started, device will reboot..."}}`

**Authentication:** Required

### WebSocket Real-time Push

The device provides WebSocket at the root path `/` for real-time updates (used by the Web UI dashboard).

**Events (JSON):**
- `{"type":"status","data":{...}}` — Status update (every 5 seconds)
- `{"type":"motion","data":{"score":30}}` — Motion detection event

---

### `/metrics` - Prometheus Metrics

---

### `/metrics` - Prometheus Metrics

Expose device metrics in Prometheus text exposition format for external monitoring.

**Request**: `GET /metrics`

**Response**: `text/plain` (no authentication required)

**Available Metrics**:

```text
# HELP esp_free_heap_bytes Free heap memory bytes
# TYPE esp_free_heap_bytes gauge
esp_free_heap_bytes 185432

# HELP esp_free_psram_bytes Free PSRAM memory bytes
# TYPE esp_free_psram_bytes gauge
esp_free_psram_bytes 4194304

# HELP esp_chip_temp_celsius ESP32-S3 chip temperature
# TYPE esp_chip_temp_celsius gauge
esp_chip_temp_celsius 42.5

# HELP esp_recording_state Recording state (0=IDLE, 1=RECORDING, 2=PAUSED, 3=ERROR)
# TYPE esp_recording_state gauge
esp_recording_state 0

# HELP sd_free_bytes SD card free bytes
# TYPE sd_free_bytes gauge
sd_free_bytes 22385920

# HELP sd_total_bytes SD card total bytes
# TYPE sd_total_bytes gauge
sd_total_bytes 31457280

# HELP sd_free_percent SD card free percentage
# TYPE sd_free_percent gauge
sd_free_percent 71.1

# HELP wifi_state WiFi state (0=AP, 1=STA_CONNECTING, 2=STA_CONNECTED, 3=STA_DISCONNECTED)
# TYPE wifi_state gauge
wifi_state 2

# HELP upload_queue_size NAS upload queue size
# TYPE upload_queue_size gauge
upload_queue_size 0

# HELP esp_uptime_seconds ESP32-S3 uptime in seconds
# TYPE esp_uptime_seconds gauge
esp_uptime_seconds 3600

# HELP esp_min_free_heap_bytes Minimum free heap memory bytes since boot
# TYPE esp_min_free_heap_bytes gauge
esp_min_free_heap_bytes 150000

# HELP esp_wifi_rssi_dbm WiFi RSSI in dBm
# TYPE esp_wifi_rssi_dbm gauge
esp_wifi_rssi_dbm -45

# HELP esp_upload_success_total Total successful uploads
# TYPE esp_upload_success_total counter
esp_upload_success_total 42

# HELP esp_upload_failure_total Total failed upload attempts
# TYPE esp_upload_failure_total counter
esp_upload_failure_total 3

```

Total: 15 metrics (9 original + 6 new).

**Prometheus Scrape Configuration**:

```yaml
scrape_configs:
  - job_name: 'esp32-cam'
    scrape_interval: 15s
    static_configs:
      - targets: ['192.168.1.100:80']
```

## HTTP Status Codes

| Status Code | Meaning | Trigger Scenario |
|-------------|---------|------------------|
| 200 | Success | Request processed successfully |
| 400 | Bad Request | Missing parameters, JSON format error, path traversal detected |
| 401 | Unauthorized | Wrong password or no password provided (for authenticated endpoints) |
| 404 | Not Found | File does not exist, static file not found |
| 500 | Internal Server Error | WiFi scan failed, time setting failed |
| 503 | Service Unavailable | MJPEG stream client connection limit reached |

## Appendix

### SD Card Paths

| Purpose | Path |
|---------|------|
| Recording files | `/sdcard/recordings/` |
| WiFi configuration override | `/sdcard/config/wifi.txt` |
| NAS configuration override | `/sdcard/config/nas.txt` |

### Configuration Priority

```
SD Card Config Files > NVS Storage > Compile-time Default Values
```

### SPIFFS Partition

Web interface static files are stored in SPIFFS partition (~256KB), path prefix `/spiffs/`.

### Server Configuration

| Parameter | Value |
|-----------|-------|
| Port | 80 |
| Max URI Handlers | 20 |
| Task Stack Size | 8192 bytes |
| Receive Timeout | 30 seconds |
| Send Timeout | 30 seconds |
| URI Match Mode | Wildcard |
