# API Overview

> ESP32-S3 Camera Monitor Firmware REST API Documentation

[Status Query](status.md) | [Configuration Interface](config.md) | [File Management](files.md) | [Device Control](control.md) | [Video Stream](stream.md) | [WiFi Scan](wifi.md) | [Complete Examples](examples.md)

---

This firmware API documentation is based on `web_server.c`, `mjpeg_streamer.c`, `config_manager.c` source code. Last updated: 2026-04-24.

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

## Endpoint Overview

| # | Method | Path | Auth | Description |
|---|--------|------|------|-------------|
| 1 | GET | `/api/status` | No | Get device status |
| 2 | GET | `/api/config` | No | Get current configuration |
| 3 | POST | `/api/config` | **Yes** | Update configuration |
| 4 | GET | `/api/files` | No | Get recording file list |
| 5 | DELETE | `/api/files?name=xxx` | **Yes** | Delete specified file |
| 6 | GET | `/api/download?name=xxx` | No | Download specified file |
| 7 | GET | `/api/scan` | No | Scan WiFi networks |
| 8 | POST | `/api/time` | **Yes** | Manually set system time |
| 9 | POST | `/api/record?action=start\|stop` | **Yes** | Control recording |
| 10 | POST | `/api/reset` | **Yes** | Factory reset |
| 11 | GET | `/stream` | No | MJPEG real-time video stream |
| 12 | OPTIONS | `/*` | No | CORS preflight request |
| 13 | GET | `/*` | No | Static files (SPIFFS) |

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