# GET /api/status — Get Device Status

> [← API Overview](overview.md) | [Configuration Interface →](config.md)

---

Get current device running status, including recording, storage, WiFi, camera, time sync, and upload queue information.

**Authentication**: None required

**Source**: `api_status_handler` (web_server.c)

**Response Example**:
#YP|
```json
#BP|{
#WM|  "ok": true,
#WV|  "data": {
#MQ|    "recording": "idle",
#KR|    "current_file": "",
#HJ|    "sd_free_percent": 85.5,
#YW|    "wifi_ssid": "MyWiFi",
#ZQ|    "wifi_state": "STA",
#NJ|    "ip": "192.168.1.100",
#SP|    "camera": "OV2640",
#VQ|    "resolution": "SVGA",
#PX|    "time_synced": true,
#QQ|    "uptime": 3600,
#SV|    "last_upload": "20260424_120000.avi",
#YH|    "upload_queue": 2,
#VQ|    "timelapse_mode": false,
#SP|    "timelapse_interval_sec": 0,
#QW|    "firmware_version": "0.2.0",
#VQ|    "frames_dropped": 0
#YJ|  }
#NW|}
#VP|```
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

**Response Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `recording` | string | Recording status: `"idle"` idle, `"recording"` recording, `"paused"` paused, `"error"` error |
| `current_file` | string | Current recording filename (AVI format), empty string `""` when idle |
| `sd_free_percent` | number | SD card free space percentage (e.g., `85.5` means 85.5%) |
| `wifi_ssid` | string | Configured WiFi SSID, empty string indicates AP mode |
| `wifi_state` | string | WiFi status: `"AP"` hotspot mode, `"STA"` connected to router, `"disconnected"` not connected |
| `ip` | string | Current IP address string |
| `camera` | string | Camera sensor model: `"OV2640"`, `"OV3660"`, `"unknown"` |
| `resolution` | string | Current resolution name: `"VGA"`, `"SVGA"`, `"XGA"` |
| `time_synced` | bool | Whether NTP time has been synced |
| `uptime` | number | Seconds running since boot |
| `last_upload` | string | Most recent uploaded filename, empty string if no upload record |
| `upload_queue` | number | Number of files waiting to upload |
#QW|| `upload_queue` | number | Number of files waiting to upload |
#SX|| `timelapse_mode` | bool | Whether timelapse recording mode is active |
#SX|| `timelapse_interval_sec` | number | Timelapse interval in seconds when timelapse mode is active |
#SX|| `firmware_version` | string | Current firmware version |
#SX|| `frames_dropped` | number | Total frames dropped during recording |
#SX|
#SX|**cURL Example**:
```bash
curl http://192.168.4.1/api/status
```

**JavaScript Example**:
```javascript
const resp = await fetch('/api/status');
const { data } = await resp.json();
console.log(`Recording: ${data.recording}, Free space: ${data.sd_free_percent}%`);
```