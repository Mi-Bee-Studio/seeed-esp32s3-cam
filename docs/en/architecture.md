# System Architecture

## 1. System Overview

The ESP32-S3 camera monitoring system is based on the FreeRTOS real-time operating system, running 14 loosely coupled C modules on the dual-core ESP32-S3. The system uses a hybrid event-driven and polling architecture, with camera frame data split into two outputs: real-time streaming and recording storage.

```
  Camera ──→ MJPEG Streamer ──→ HTTP Server ──→ Browser/Client
          └→ Video Recorder ──→ SD Card (AVI segments)
                             └→ NAS Uploader ──→ FTP / WebDAV
  Config Manager ←→ NVS ←→ SD Card Override (wifi.txt / nas.txt)
  WiFi Manager (AP/STA) → Time Sync (SNTP)
  Status LED ← LED Controller (GPIO21, active-low)
  Watchdog (30s TWDT, panic) → Health Monitor (60s)
```

### Core Features

| Feature | Description |
|---------|-------------|
| Dual-core division | Core 0: Recording; Core 1: Upload, SD monitoring, health detection |
| PSRAM dependency | Camera frame buffers allocated in PSRAM (dual buffer), won't work without PSRAM |
| Circular storage | SD card free space < 20% automatically deletes oldest recordings, restores to 30% |
| Hot-plug | SD card removal automatically stops recording, insertion automatically resumes |
| Watchdog | 30s TWDT, timeout triggers panic reboot |

---

## 2. Boot Process

19-step sequential initialization in `app_main()`, any critical step failure logs error but continues execution (partial function degradation).

| Step | Operation | Description |
|------|-----------|-------------|
| 1 | Initialize NVS Flash | Automatically erases and rebuilds if corrupted |
| 2 | Config Manager | Loads config from NVS, uses defaults if no config |
| 3 | Status LED | GPIO21 initialization, set to `LED_STARTING` (solid) |
| 4 | SPIFFS Mount | Loads Web UI static resources, automatically formats if mount fails |
| 5 | SD Card Storage | 1-line SDMMC mode, FAT32 file system |
| 5a | Clean Incomplete AVI | Deletes recording files not properly closed during last power-off |
| 6 | SD Config Override | Reads SD card `/sdcard/config/wifi.txt` and `nas.txt`, overrides NVS config |
| 7 | WiFi Initialization | SSID configured → STA mode, otherwise → AP mode |
| 8 | Camera Initialization | Auto-detects OV2640/OV3660, configures resolution/FPS/quality, tests capture verification |
| 9 | Time Sync | Starts SNTP only when STA connected, blocks waiting up to 5 seconds |
| 10 | NAS Uploader | Creates upload queue (capacity 16) and upload task |
| 11 | Video Recorder | Initializes AVI recording engine, registers segment callback |
| 12 | MJPEG Streamer | Initializes real-time streaming module, max 2 concurrent clients |
| 13 | Web Server | Port 80, registers 12 URI handlers (10 API + 2 wildcard) |
| 14 | LED Status Update | Updates LED mode based on current WiFi status |
| 15 | Wait for STA + Start Recording | Waits up to 30 seconds for WiFi connection, starts recording after connection |
| 16 | BOOT Button Monitor | GPIO0, hold 5 seconds to trigger factory reset |
| 17 | Watchdog | 30s TWDT, includes both cores' idle tasks in monitoring |
| 18 | SD Monitor Task | Core 1, polls SD card status every 10 seconds, handles hot-plug |
| 19 | Health Monitor Task | Core 1, outputs heap/stack watermark info every 60 seconds |

---

## 3. Module Description

The system has 14 modules, all located in the `main/` directory, each module with one `.c`/`.h` file pair.

| Module | File | Responsibility | Key Functions |
|--------|------|----------------|---------------|
| Main Entry | `main.c` | 19-step boot process, task creation | `app_main()`, `on_segment_complete()` |
| Camera Driver | `camera_driver.c` | OV2640/OV3660 auto-detection, frame capture | `camera_init()`, `camera_capture()` |
| Video Recorder | `video_recorder.c` | AVI MJPEG segmented recording, state machine | `recorder_start()`, `recorder_stop()` |
| MJPEG Streamer | `mjpeg_streamer.c` | MJPEG real-time video stream push | `mjpeg_streamer_init()`, `mjpeg_streamer_register()` |
| Web Server | `web_server.c` | HTTP server + REST API (10 endpoints) | `web_server_start()`, `web_server_get_handle()` |
| WiFi Manager | `wifi_manager.c` | AP/STA dual mode, auto-selection | `wifi_init()`, `wifi_scan()` |
| Config Manager | `config_manager.c` | NVS persistence, SD card override | `config_init()`, `config_save()`, `config_reset()` |
| Storage Manager | `storage_manager.c` | SD card mount/unmount, circular cleanup | `storage_init()`, `storage_cleanup()` |
| NAS Upload | `nas_uploader.c` | Queued upload scheduling, auto-retry | `nas_uploader_init()`, `nas_uploader_enqueue()` |
| FTP Client | `ftp_client.c` | FTP protocol upload implementation | — |
| WebDAV Client | `webdav_client.c` | WebDAV protocol upload implementation | — |
| Status LED | `status_led.c` | 5 LED mode control | `led_init()`, `led_set_status()` |
| Time Sync | `time_sync.c` | SNTP sync, manual time setting | `time_sync_init()`, `time_is_synced()` |
| JSON Parser | `cJSON.c` | Third-party JSON library (removed in IDF v6.0) | — |

### Configuration Structure

```c
typedef struct {
    char wifi_ssid[33];       // WiFi name
    char wifi_pass[64];       // WiFi password
    char ftp_host[64];        // FTP server address
    uint16_t ftp_port;        // FTP port
    char ftp_user[32];        // FTP username
    char ftp_pass[32];        // FTP password
    char ftp_path[128];       // FTP remote path
    bool ftp_enabled;         // FTP upload switch
    char webdav_url[128];     // WebDAV URL
    char webdav_user[32];     // WebDAV username
    char webdav_pass[32];     // WebDAV password
    bool webdav_enabled;      // WebDAV upload switch
    uint8_t resolution;       // 0=VGA, 1=SVGA, 2=XGA
    uint8_t fps;              // 1-30
    uint16_t segment_sec;     // Segment duration (seconds)
    uint8_t jpeg_quality;     // 1-63
    char web_password[32];    // Web management password
    char device_name[32];     // Device name
} cam_config_t;
```

---

## 4. Data Flow

### Recording Data Flow

JPEG frames captured by the camera are written to AVI files by the recorder, triggering a callback chain after segment completion:

```
camera_capture()
    │
    ▼
Video Recorder (recording_task, Core 0)
    │  ← Frame data written to AVI file
    │  ← Segmented by segment_sec duration
    │  ← Writes AVI idx1 index
    │
    ▼  Segment complete
on_segment_complete(filepath, size)
    │
    ├──→ nas_uploader_enqueue(filepath)
    │       │
    │       ▼
    │    Upload Task (Core 1)
    │       ├── FTP upload (ftp_enabled)
    │       └── WebDAV upload (webdav_enabled)
    │       Retry 3 times on failure, pause 5 minutes after 10 consecutive failures
    │
    └──→ storage_cleanup()
            │
            ▼
         Check free space < 20%?
            ├── Yes → Delete oldest recording files until ≥ 30%
            └── No → No operation
```

### MJPEG Real-time Stream Data Flow

```
Browser → GET /stream
    │
    ▼
HTTP Server → mjpeg_stream_handler()
    │
    ▼  Loop capture (limited to 2 concurrent clients)
camera_capture() → JPEG frames
    │
    ▼
HTTP chunked transfer (multipart/x-mixed-replace)
    │  Content-Type: image/jpeg
    │  Boundary: --frame
    │
    ▼
Browser real-time rendering
```

---

## 5. FreeRTOS Task Table

| Task Name | Function | Priority | Core | Stack Size | Period/Trigger | File |
|-----------|----------|----------|------|------------|----------------|------|
| `recorder` | `recording_task` | 5 (configMAX-2) | Core 0 | 4096 B | Continuous loop (frame capture) | video_recorder.c |
| `upload` | `upload_task` | 3 | Core 1 | 6144 B | Queue blocking wait | nas_uploader.c |
| `sd_monitor` | `sd_monitor_task` | 2 | Core 1 | 3072 B | 10-second polling | main.c |
| `boot_btn` | `boot_button_monitor` | 1 | Any | 2048 B | 200ms polling | main.c |
| `health_mon` | `health_monitor_task` | 1 | Core 1 | 3072 B | 60-second polling | main.c |
| `httpd` | ESP-IDF built-in | Default | — | 8192 B | Event-driven | web_server.c |
| `main` | `app_main` | 1 | Core 0 | 8192 B | Returns after initialization | main.c |

### Task Scheduling Strategy

- **Core 0**: Runs recording task (high priority), ensuring frame capture is not preempted causing frame drops
- **Core 1**: Runs upload, SD monitoring, health monitoring and other non-real-time tasks
- **BOOT button monitor**: Not bound to core, low priority polling
- **Web server**: Managed by ESP-IDF httpd library, 8192-byte stack

---

## 6. Partition Table

From `partitions.csv`, using custom partition table:

| Name | Type | Subtype | Offset | Size | Description |
|------|------|---------|--------|------|-------------|
| `nvs` | data | nvs | 0x9000 | 24 KB (0x6000) | Non-volatile storage (config persistence) |
| `phy_init` | data | phy | 0xF000 | 4 KB (0x1000) | PHY calibration data |
| `factory` | app | factory | 0x10000 | 3584 KB (0x380000) | Firmware application |
| `storage` | data | spiffs | Auto | 256 KB (0x40000) | Web UI static resources |

**Total Flash Size**: 8 MB

> **Note**: No OTA partition currently, factory app only. To add OTA support, modify `partitions.csv` to split factory into `ota_0` + `ota_1` + `ota_data`.

---

## 7. Web API Endpoints

Server runs on port 80, total 12 URI handlers:

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | No | Device status (recording, WiFi, storage, camera) |
| GET | `/api/config` | No | Current config (password fields return `****`) |
| POST | `/api/config` | Yes | Modify config |
| GET | `/api/files` | No | Recording file list |
| DELETE | `/api/files` | Yes | Delete specified file |
| GET | `/api/download?name=xxx` | No | Download recording file |
| GET | `/api/scan` | No | WiFi AP scan |
| POST | `/api/time` | Yes | Manually set time |
| POST | `/api/record?action=start\|stop` | Yes | Recording control |
| POST | `/api/reset` | Yes | Factory reset |
| OPTIONS | `/*` | No | CORS preflight |
| GET | `/*` | No | Static files (Web UI) |

Authentication: Pass management password via `X-Password` request header or `?password=xxx` query parameter.

---

## 8. Web UI

4 HTML pages, flashed to SPIFFS partition, served by wildcard GET handler:

| Page | File | Function |
|------|------|----------|
| Home | `index.html` | Status overview |
| Config | `config.html` | WiFi / NAS / camera parameter configuration |
| Files | `files.html` | Recording file browsing, downloading, deleting |
| Preview | `preview.html` | MJPEG real-time stream preview |
