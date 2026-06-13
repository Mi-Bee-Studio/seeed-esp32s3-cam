# System Architecture

## 1. System Overview

The ESP32-S3 camera monitoring system is based on the FreeRTOS real-time operating system, running 22 loosely coupled C modules on the dual-core ESP32-S3. The system uses a hybrid event-driven and polling architecture, with camera frame data split into two outputs: real-time HTTP MJPEG streaming and recording storage.

```
  Camera ──→ Frame Broadcaster ──→ MJPEG Streamer (port 81) ──→ Browser/Client
          └→ Video Recorder ──→ SD Card (AVI segments)
                             └→ NAS Uploader ──→ WebDAV / HTTP(S) (mutually exclusive)
  Config Manager ←→ NVS ←→ SD Card Override (wifi.txt / nas.txt)
  WiFi Manager (AP/STA) → Time Sync (SNTP)
  Status LED ← LED Controller (GPIO21, active-low)
  Watchdog (30s TWDT, panic) → Health Monitor (60s)
  ONVIF Discovery (UDP multicast) → NVR auto-discovery
```

### Core Features

| Feature | Description |
|---------|-------------|
| Dual-core division | Core 0: Recording; Core 1: Upload, SD monitoring, health detection, ONVIF |
| PSRAM dependency | Camera frame buffers allocated in PSRAM (dual buffer), won't work without PSRAM |
| Circular storage | SD card free space < 20% automatically deletes oldest recordings, restores to 30% |
| Hot-plug | SD card removal automatically stops recording, insertion automatically resumes |
| Watchdog | 30s TWDT, timeout triggers panic reboot |
| ONVIF discovery | WS-Discovery multicast, auto-discoverable by NVRs |

---

## 2. Boot Process

21-step sequential initialization in `app_main()`, any critical step failure logs error but continues execution (partial function degradation).

| Step | Operation | Description |
|------|-----------|-------------|
| 1 | Initialize NVS Flash | Automatically erases and rebuilds if corrupted |
| 2 | Config Manager | Loads config from NVS, uses defaults if no config |
| 3 | Status LED | GPIO21 initialization, set to `LED_STARTING` (solid) |
| 4 | SPIFFS Mount | Loads Web UI static resources, automatically formats if mount fails |
| 5 | SD Card Storage | SPI mode, FAT32 file system |
| 5a | Clean Incomplete AVI | Deletes recording files not properly closed during last power-off |
| 6 | SD Config Override | Reads SD card `/sdcard/config/wifi.txt` and `config.txt`, overrides NVS config |
| 7 | WiFi Initialization | SSID configured → STA mode, otherwise → AP mode |
| 8 | Camera Initialization | Auto-detects OV2640/OV3660, configures resolution/FPS/quality, tests capture verification |
| 9 | Time Sync | Starts SNTP only when STA connected, blocks waiting up to 5 seconds |
| 10 | NAS Uploader | Creates upload queue (capacity 16) and upload task |
| 11 | Frame Broadcaster | Initializes frame pub/sub hub for MJPEG subscribers |
| 12 | Video Recorder | Initializes AVI recording engine, registers segment callback |
| 13 | MJPEG Streamer | Initializes independent TCP server on port 81, max 2 concurrent clients |
| 14 | Web Server | Port 80, registers URI handlers + ONVIF endpoints |
| 14a | OTA Updater | Initializes firmware OTA update capability |
| 15 | LED Status Update | Updates LED mode based on current WiFi status |
| 16 | ONVIF Discovery | Starts WS-Discovery UDP multicast listener (after WiFi connected) |
| 17 | Wait for STA + Start Recording | Waits up to 30 seconds for WiFi connection, starts recording after connection |
| 18 | BOOT Button Monitor | GPIO0, hold 5 seconds to trigger factory reset |
| 19 | Watchdog | 30s TWDT, includes both cores' idle tasks in monitoring |
| 20 | SD Monitor Task | Core 1, polls SD card status every 10 seconds, handles hot-plug |
| 21 | Health Monitor Task | Core 1, outputs heap/stack watermark info every 60 seconds |

---

## 3. Module Description

The system has 22 modules, all located in the `main/` directory, each module with one `.c`/`.h` file pair.

| Module | File | Responsibility | Key Functions |
|--------|------|----------------|---------------|
| Main Entry | `main.c` | Boot process, task creation | `app_main()`, `on_segment_complete()` |
| Camera Driver | `camera_driver.c` | OV2640/OV3660 auto-detection, frame capture | `camera_init()`, `camera_capture()` |
| Video Recorder | `video_recorder.c` | AVI MJPEG segmented recording, state machine | `recorder_start()`, `recorder_stop()` |
| Frame Broadcaster | `frame_broadcaster.c` | Frame pub/sub hub, distributes JPEG frames to MJPEG subscribers | `fbroadcast_publish()`, `fbroadcast_subscribe()` |
| MJPEG Streamer | `mjpeg_streamer.c` | MJPEG real-time stream, independent TCP server on port 81 | `mjpeg_streamer_init()` |
| Web Server | `web_server.c` | HTTP server + REST API + ONVIF endpoints | `web_server_start()`, `web_server_get_handle()` |
| WiFi Manager | `wifi_manager.c` | AP/STA dual mode, auto-selection | `wifi_init()`, `wifi_scan()` |
| Config Manager | `config_manager.c` | NVS persistence, SD card override | `config_init()`, `config_save()`, `config_reset()` |
| Storage Manager | `storage_manager.c` | SD card mount/unmount, circular cleanup | `storage_init()`, `storage_cleanup()` |
| NAS Upload | `nas_uploader.c` | Mutually exclusive upload scheduling (WebDAV or HTTP/HTTPS) | `nas_uploader_init()`, `nas_uploader_enqueue()` |
| HTTP Upload Client | `http_upload_client.c` | HTTP/HTTPS PUT streaming upload | — |
| WebDAV Client | `webdav_client.c` | WebDAV protocol upload implementation | — |
| ONVIF Discovery | `onvif_discovery.c` | WS-Discovery UDP multicast, NVR auto-discovery | `onvif_discovery_init()` |
| ONVIF Service | `onvif_service.c` | SOAP handlers for GetDeviceInformation, GetCapabilities, GetProfiles, GetStreamUri | `onvif_register_handlers()` |
| Status LED | `status_led.c` | 5 LED mode control | `led_init()`, `led_set_status()` |
| Time Sync | `time_sync.c` | SNTP sync, manual time setting | `time_sync_init()`, `time_is_synced()` |
| Motion Detector | `motion_detector.c` | Frame-difference based motion detection | `motion_detector_init()`, `motion_detector_process()` |
| OTA Updater | `ota_updater.c` | Firmware OTA updates via URL | `ota_updater_init()`, `ota_update_start()` |
| Webhook | `webhook.c` | HTTP event notifications for motion/recordings | `webhook_send()`, `webhook_init()` |
| WebSocket Server | `ws_server.c` | Real-time push updates to WebUI clients | `ws_server_init()`, `ws_broadcast()` |
| SHA-256 | `sha256.c` | Standalone SHA-256 for firmware hash verification | `sha256_hash()`, `sha256_verify()` |
| JSON Parser | `cJSON.c` | Third-party JSON library (vendored for IDF v6.0) | — |

### Configuration Structure

```c
typedef struct {
char wifi_ssid[33];       // WiFi name
    char wifi_pass[64];       // WiFi password
    char wifi_ssid_2[33];     // WiFi backup SSID
    char wifi_pass_2[64];     // WiFi backup password
uint8_t upload_method;    // 0=Disabled, 1=WebDAV, 2=HTTP/HTTPS
char upload_base_path[128]; // Upload base path
char webdav_url[128];      // WebDAV URL
char webdav_user[32];      // WebDAV username
char webdav_pass[32];      // WebDAV password
bool webdav_enabled;       // WebDAV upload switch
char http_upload_url[256]; // HTTP(S) upload URL
char http_upload_user[64]; // HTTP(S) username
char http_upload_pass[64]; // HTTP(S) password
bool http_upload_skip_cert_verify; // Skip HTTPS certificate verification
uint8_t resolution;       // 0=VGA, 1=SVGA, 2=XGA
uint8_t fps;              // 1-30
uint16_t segment_sec;     // Segment duration (seconds)
uint8_t jpeg_quality;     // 1-63
bool vflip;               // Vertical flip
bool hmirror;             // Horizontal mirror
char web_password[32];    // Web management password
char device_name[32];     // Device name
bool allow_ap_fallback;   // Allow fallback to AP mode when WiFi fails
uint16_t timelapse_interval_sec; // Timelapse interval (0=continuous, >0=timelapse)
uint8_t timelapse_mode;   // 0=continuous, 1=normal timelapse, 2=dynamic timelapse
uint8_t motion_sensitivity; // Motion detection sensitivity (0-100)
uint16_t motion_active_interval_sec; // Motion capture interval in dynamic timelapse
uint16_t motion_idle_interval_sec;   // Idle interval in dynamic timelapse
uint16_t cleanup_low_pct;  // Auto-cleanup when free < this %
uint16_t cleanup_high_pct; // Stop cleanup when free > this %
bool frame_drop_enabled;  // Enable frame dropping under resource pressure
char alert_webhook_url[256]; // Alert webhook URL
bool alert_webhook_enabled; // Enable alert webhook
} cam_config_t;
```

---

## 4. Data Flow

### Recording Data Flow

JPEG frames captured by the camera are written to AVI files by the recorder, triggering a callback chain after segment completion:

```c
camera_capture()
    │
    ▼
    Video Recorder (recording_task, Core 0)
    │  ← Frame data written to AVI file
    │  ← Segmented by segment_sec duration (continuous vs timelapse mode)
    │  ← Writes AVI idx1 index
    │
    ▼  Segment complete
    on_segment_complete(filepath, size)
    │
    ├──→ nas_uploader_enqueue(filepath)
    │       │
    │       ▼
    │    Upload Task (Core 1)
    │       ├── upload_method=1 → WebDAV upload
    │       ├── upload_method=2 → HTTP(S) upload
    │       └── upload_method=0 → Skip upload
    │       Retry 3 times on failure, pause 5 minutes after 10 consecutive failures
    │
    └──→ storage_cleanup()
            │
            ▼
         Check free space < cleanup_low_pct? (default 20%)
            ├── Yes → Delete oldest recording files until ≥ cleanup_high_pct (default 30%)
            │        Also attempts cleanup+retry on write failure
            └── No → No operation
```

**Timelapse Mode**: When `timelapse_interval_sec > 0`, frames are captured at the specified interval (1-300s) instead of continuous. Files use `TLM_` prefix and play at 15fps.

### MJPEG Real-time Stream Data Flow

The MJPEG streamer runs on **port 81** as an independent TCP server, completely separate from the main httpd on port 80. See [Port Separation Design](port-separation-design.md) for details.

```
Browser → GET /stream (port 81)
    │
    ▼
mjpeg_listen_task (Core 1, priority 3)
    └── accept() → spawn mjpeg_client_task per client
         │
         ▼
frame_broadcaster → fbroadcast_receive()
         │
         ▼
video_recorder (frame producer) → fbroadcast_publish()
         │  ← camera_capture() in recording_task
         │  ← PSRAM copy → decouple from camera buffer
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
| `mjpeg_listen` | `mjpeg_listen_task` | 3 | Core 1 | 4096 B | Connection accept loop | mjpeg_streamer.c |
| `mjpeg_cli_N` | `mjpeg_client_task` | 2 | Core 1 | 4096 B | Frame receive + send loop | mjpeg_streamer.c |
| `onvif_disc` | `onvif_discovery_task` | 2 | Core 1 | 6144 B | UDP multicast listen | onvif_discovery.c |
| `sd_monitor` | `sd_monitor_task` | 2 | Core 1 | 3072 B | 10-second polling | main.c |
| `boot_btn` | `boot_button_monitor` | 1 | Any | 2048 B | 200ms polling | main.c |
| `health_mon` | `health_monitor_task` | 1 | Core 1 | 3072 B | 60-second polling | main.c |
| `ws_server` | `websocket_server_task` | 3 | Core 1 | 2048 B | Client event-driven | ws_server.c |
| `httpd` | ESP-IDF built-in | Default | — | 16384 B | Event-driven | web_server.c |
| `main` | `app_main` | 1 | Core 0 | 8192 B | Returns after initialization | main.c |

### Task Scheduling Strategy

- **Core 0**: Runs recording task (high priority), ensuring frame capture is not preempted causing frame drops
- **Core 1**: Runs upload, MJPEG streaming, ONVIF discovery, SD monitoring, health monitoring, WebSocket server and other non-real-time tasks
- **BOOT button monitor**: Not bound to core, low priority polling
- **Web server**: Managed by ESP-IDF httpd library, 16384-byte stack
- **WebSocket server**: Runs on Core 1, broadcasts status updates to connected clients

---

## 6. Partition Table

From `partitions.csv`, using custom dual-OTA partition table:

| Name | Type | Subtype | Offset | Size | Description |
|------|------|---------|--------|------|-------------|
| `nvs` | data | nvs | 0x9000 | 24 KB (0x6000) | Non-volatile storage (config persistence) |
| `phy_init` | data | phy | 0xF000 | 4 KB (0x1000) | PHY calibration data |
| `ota_0` | app | ota_0 | 0x10000 | 1792 KB (0x1D0000) | OTA slot A |
| `ota_1` | app | ota_1 | 0x1E0000 | 1792 KB (0x1D0000) | OTA slot B |
| `otadata` | data | ota | 0x3B0000 | 8 KB (0x2000) | OTA selection data |
| `spiffs` | data | spiffs | 0x3B2000 | 256 KB (0x40000) | Web UI static resources |

**Total Flash Size**: 8 MB

> **Note**: Dual OTA slots support seamless firmware updates. `esp_https_ota` handles slot switching automatically.

---

## 7. Web API Endpoints

Server runs on port 80, with additional ONVIF SOAP endpoints:

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | No | Device status (recording, WiFi, storage, camera, temperature) |
| GET | `/api/config` | No | Current config (password fields return `****`) |
| POST | `/api/config` | Yes | Modify config |
| GET | `/api/files` | No | Recording file list |
| POST | `/api/files/batch` | Yes | Batch delete files |
| DELETE | `/api/files` | Yes | Delete specified file |
| GET | `/api/download?name=xxx` | No | Download recording file |
| GET | `/api/scan` | No | WiFi AP scan |
| POST | `/api/time` | Yes | Manually set time |
| POST | `/api/record?action=start\|stop` | Yes | Recording control |
| POST | `/api/ota` | Yes | Firmware OTA update via URL |
| POST | `/api/format` | Yes | Format SD card (requires confirmation) |
| POST | `/api/reset` | Yes | Factory reset |
| GET | `/metrics` | No | Prometheus metrics (text format) |
| POST | `/onvif/device_service` | No | ONVIF Device Service (SOAP) |
| POST | `/onvif/media_service` | No | ONVIF Media Service (SOAP) |
| OPTIONS | `/*` | No | CORS preflight |
| GET | `/*` | No | Static files (Web UI) |

Authentication: Pass management password via `X-Password` request header or `?password=xxx` query parameter.

---

## 8. Web UI

6 HTML pages, flashed to SPIFFS partition, served by wildcard GET handler:

| Page | File | Function |
|------|------|----------|
| Home | `index.html` | Status overview |
| Config | `config.html` | WiFi / NAS / camera parameter configuration (accordion layout) |
| Files | `files.html` | Recording file browsing, downloading, batch operations with collapsible date groups |
| Preview | `preview.html` | MJPEG real-time stream preview with fullscreen and screenshot buttons |
| OTA | `ota.html` | Firmware update interface with URL input and progress display |
| Setup | `setup.html` | First-time WiFi configuration wizard for new devices |

**Prometheus Integration**: `/metrics` endpoint provides 15+ system metrics in text exposition format.

**Temperature Monitoring**: ESP32-S3 chip temperature available via `/api/status` with color-coded dashboard indicators.
