# PROJECT KNOWLEDGE BASE

**Generated:** 2026-04-24
**Commit:** b1ff215
**Branch:** main

## OVERVIEW

ESP32-S3 parrot monitoring camera firmware. ESP-IDF v5.x/v6.0, C, dual-core RTOS. Captures MJPEG video, records AVI segments to SD card, uploads to NAS via FTP/WebDAV, serves REST API + web UI.

## STRUCTURE

```
.
├── main/           # All firmware source (flat, no subdirs)
│   ├── main.c      # Entry point — 19-step boot sequence, watchdog, tasks
│   ├── web_ui/     # Static HTML pages (SPIFFS-served)
│   └── cJSON.c/h   # Vendored JSON parser (removed from IDF v6.0)
├── docs/           # API documentation
├── partitions.csv  # factory 3.5MB + SPIFFS 256KB + NVS 24KB
├── sdkconfig.defaults  # XIAO ESP32-S3 Sense pin config, PSRAM Octal, 8MB flash
└── CMakeLists.txt  # Top-level project (parrot_cam)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Add a new API endpoint | `main/web_server.c` → `s_uris[]` table + handler func | Auth via `check_password()`, respond via `json_ok()`/`json_error()` |
| Change camera pins | `main/camera_driver.c` → `CAM_PIN_*` defines | Also update `sdkconfig.defaults` |
| Change SD card pins | `main/storage_manager.c` → `SD_PIN_*` defines | 1-line SDMMC mode |
| Add config field | `main/config_manager.h` → `cam_config_t` struct | Must add to: defaults, NVS read/write, SD parser, GET/POST handlers |
| Change LED behavior | `main/status_led.c` → `led_timer_cb()` | Active-low on GPIO21 |
| Modify web UI | `main/web_ui/*.html` | 4 pages: index, config, files, preview |
| Add boot step | `main/main.c` → `app_main()` | Sequential init, numbered comments |
| Change recording format | `main/video_recorder.c` | AVI MJPEG, segment callback → NAS upload |
| FTP/WebDAV upload logic | `main/nas_uploader.c` → dispatches to `ftp_client.c` or `webdav_client.c` | Queue-based, background task |

## CODE MAP

| Symbol | Location | Role |
|--------|----------|------|
| `app_main` | main.c:212 | Entry — 19-step init, spawns 5 tasks |
| `cam_config_t` | config_manager.h:7 | Central config struct (17 fields, NVS-persisted) |
| `s_uris[]` | web_server.c:537 | All 10 API endpoints + 2 wildcard handlers |
| `recorder_state_t` | video_recorder.h:7 | IDLE/RECORDING/PAUSED/ERROR state machine |
| `wifi_state_t` | wifi_manager.h:13 | AP/STA_CONNECTING/STA_CONNECTED/STA_DISCONNECTED |
| `on_segment_complete` | main.c:61 | Segment callback → enqueues NAS upload + cleanup |
| `boot_button_monitor` | main.c:94 | GPIO0 hold 5s → factory reset |
| `sd_monitor_task` | main.c:130 | 10s polling, hot-plug detect, auto-resume recording |
| `health_monitor_task` | main.c:181 | 60s heap/PSRAM/stack logging |

### FreeRTOS Tasks

| Task | Priority | Core | Stack | Source |
|------|----------|------|-------|--------|
| `recording_task` | 5 | 0 | 4096 | video_recorder.c:553 |
| `upload_task` | 3 | 1 | 6144 | nas_uploader.c:156 |
| `sd_monitor` | 2 | 1 | 3072 | main.c:320 |
| `boot_btn` | 1 | any | 2048 | main.c:308 |
| `health_mon` | 1 | 1 | 3072 | main.c:323 |

## CONVENTIONS

- **Static prefix**: `s_` for module-level statics (e.g., `s_server`, `s_config`, `s_card`)
- **TAG pattern**: `static const char *TAG = "module";` in every .c file for ESP_LOG*
- **Error returns**: `ESP_OK`/`ESP_FAIL` everywhere. No custom error codes.
- **Mutex pattern**: `xSemaphoreTake(s_mutex, portMAX_DELAY)` / `xSemaphoreGive()` for shared state
- **Config access**: `config_get()` returns pointer to PSRAM singleton — no locking, single-writer
- **Module init**: Each module has `xxx_init()` called once from `app_main()`, returns `esp_err_t`
- **Headers**: `#pragma once` (not include guards)
- **Comments**: `/* ---- section ---- */` separator style, numbered boot steps in main.c
- **Web handlers**: `api_*_handler()` naming, `static` functions, registered in `s_uris[]`
- **JSON responses**: `json_ok(req, cJSON*)` → `{"ok":true,"data":...}`, `json_error(req, msg, status)` → `{"ok":false,"error":...}`
- **CORS**: `set_cors_headers()` called in all JSON + static responses; wildcard OPTIONS handler for preflight
- **Auth**: `check_password(req)` tries `X-Password` header then `?password=` query param

## ANTI-PATTERNS (THIS PROJECT)

- **Do NOT add cJSON as IDF component** — vendored in main/ because IDF v6.0 removed it
- **Do NOT use `esp_vfs_fat_sdmmc_unmount`** — renamed to `esp_vfs_fat_sdcard_unmount` in IDF v6.0
- **Do NOT use `config.timeout_sec`** — split into `recv_wait_timeout` + `send_wait_timeout` in IDF v6.0
- **Do NOT use `HTTPD_503_SERVICE_UNAVAILABLE`** — removed in IDF v6.0, use `httpd_resp_set_status()` manually
- **Do NOT use `esp_transport_set_keep_alive`** — renamed to `esp_transport_tcp_set_keep_alive` in IDF v6.0
- **Do NOT include `lwip/esp_netif.h`** — use `esp_netif.h` directly in IDF v6.0
- **Do NOT commit `setup_idf_env.ps1`** — contains local Windows paths
- **Do NOT commit `.sisyphus/`** — internal orchestration directory
- **Path traversal**: DELETE/download handlers block `..` in filename param — maintain this

## UNIQUE STYLES

- **Flat module layout**: All 14 C modules in `main/` with no subdirectories — each module = one .c/.h pair
- **SD card override**: Reads `/sdcard/config/wifi.txt` and `nas.txt` at boot — KEY=VALUE format, overrides NVS
- **Segment callback chain**: `video_recorder` → `on_segment_complete()` → `nas_uploader_enqueue()` + `storage_cleanup()`
- **Circular storage**: Auto-deletes oldest files when free < 20%, stops at 30%
- **Hot-plug SD**: 10s polling task detects removal (stops recording) and insertion (auto-resumes)
- **Web UI in SPIFFS**: 4 HTML pages flashed to SPIFFS partition, served by wildcard GET handler
- **Watchdog**: 30s TWDT with panic — all tasks must feed periodically
- **Health monitor**: 60s task logs heap/PSRAM/stack HWM, alerts below 20KB heap / 500KB PSRAM
- **Camera auto-detect**: OV2640 vs OV3660 detected at init, reported in API status
- **AP SSID**: `ParrotCam-XXXX` (last 4 hex of MAC), default IP `192.168.4.1`

## COMMANDS

```bash
# Build (requires ESP-IDF v5.x or v6.0 environment)
idf.py build

# Flash + monitor
idf.py -p COMx flash monitor

# Menuconfig (change WiFi SSID, partition table, etc.)
idf.py menuconfig

# Erase flash (factory reset)
idf.py erase-flash
```

## NOTES

- **Hardware**: XIAO ESP32-S3 Sense board. GPIO10 shared between camera XCLK and SD CMD (1-line SDMMC mode frees it)
- **PSRAM dependency**: Camera framebuffers live in PSRAM — won't work without it
- **No OTA partition**: Factory app only (3.5MB), no OTA slots. To add OTA, modify `partitions.csv`
- **No tests**: Embedded firmware, no unit test infrastructure
- **Default password**: `admin` (NOT `12345678` — that was wrong in early docs)
- **NAS upload**: FTP password and WebDAV password are accepted in POST config but NOT returned in GET config
- **Max stream clients**: 2 (hardcoded `MAX_STREAM_CLIENTS` in mjpeg_streamer.c)
- **Config masking**: Password fields returned as `"****"` in GET, POST with `"****"` = no change
