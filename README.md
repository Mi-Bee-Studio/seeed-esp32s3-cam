# MiBee Cam — ESP32-S3 Camera Monitor

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x%2Fv6.0-1C7C3D?logo=espressif)](https://idf.espressif.com/)
[![Platform](https://img.shields.io/badge/Platform-XIAO%20ESP32--S3%20Sense-EA4C89?logo=seeedstudio)](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
[![CI](https://img.shields.io/github/actions/workflow/status/Mi-Bee-Studio/seeed-esp32s3-cam/release.yml?logo=github&label=CI)](https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam/actions)
[![Release](https://img.shields.io/github/v/release/Mi-Bee-Studio/seeed-esp32s3-cam?logo=github)](https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam/releases)
[![Firmware](https://img.shields.io/badge/Firmware-v0.3.0-7C3AED)](https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam/releases)

> **Home-grade AI-powered surveillance camera firmware** — Tiny footprint, enterprise-class features.
> MiBee Cam transforms a $15 XIAO ESP32-S3 Sense board into a fully-featured network surveillance camera.

[中文文档](README.zh.md) | English

---

## Why MiBee Cam?

Most ESP32 camera projects stop at "it streams video." MiBee Cam goes **all the way** — segmented recording, smart timelapse with motion detection, dual-protocol NAS upload (WebDAV + HTTPS), ONVIF auto-discovery (Synology/Milestone compatible), OTA updates, and a polished purple-themed Web UI — all on a single-core-friendly dual-core RTOS architecture. No Linux required. No cloud dependency. **Your camera, your data, your network.**

---

## What's Inside

- **AI‑assisted recording** — Continuous, timelapse, and **dynamic timelapse** (motion‑adaptive capture, saves 90% storage when nothing happens)
- **Real‑time video** — MJPEG over HTTP (port 81), max 2 concurrent clients
- **Cloud‑grade NAS upload** — WebDAV or HTTP(S) PUT, queue‑based, 3‑retry backoff, mutually exclusive
- **Motion detection engine** — Frame‑difference analysis, 0–100 score, triggers dynamic timelapse and webhook alerts
- **Web dashboard** — 6‑page responsive UI: status, config (accordion), file manager (batch ops), live preview, OTA update, first‑time setup wizard
- **OTA firmware updates** — Upload via web UI or trigger from URL, SHA‑256 verified
- **WebSocket real‑time push** — Live status updates, motion events pushed to dashboard
- **Webhook notifications** — HTTP callbacks for motion, recording errors, system events
- **Prometheus `/metrics`** — 15+ system metrics for Grafana / home‑lab monitoring
- **Circular storage** — Auto‑cleans when SD < 20% free, stops at 30%, crash‑recovery cleanup on boot
- **Hot‑plug SD** — Removal stops recording, insertion resumes automatically (10‑second polling)
- **Dual‑band WiFi** — AP mode for setup, STA for production, backup SSID + auto‑fallback
- **SNTP time sync** — Dual‑server (cn.pool.ntp.org + ntp.aliyun.com), manual override, TZ config
- **Health watchdog** — 30s TWDT + 60s health monitor (heap / PSRAM / stack / chip temp)
- **Purple theme**  — Mi&Bee branding across all pages

---

## Web Interface

![Dashboard](docs/images/index-dashboard.png)
*Dashboard — Real-time status, recording controls, system health at a glance*

| Page | What it does |
|------|-------------|
| **Dashboard** (`/`) | Recording status, WiFi info, storage usage, chip temperature, firmware version |
| **Config** (`/config.html`) | All settings: WiFi, video, recording modes, NAS, webhooks, motion detection |
| **Files** (`/files.html`) | Browse, download, batch-delete recordings by date group |
| **Preview** (`/preview.html`) | Live MJPEG stream with fullscreen & screenshot |
| **OTA** (`/ota.html`) | Firmware upload & update from URL |
| **Setup** (`/setup.html`) | First-time WiFi configuration wizard |

![File Manager](docs/images/files-page.png)
![Configuration](docs/images/config-page.png)

---

## Quick Start

```bash
git clone --recursive https://github.com/Mi-Bee-Studio/seeed-esp32s3-cam.git
cd seeed-esp32s3-cam
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

> Requires ESP-IDF v5.x or v6.0. 👉 [Full installation guide](docs/en/getting-started.md)

### Hardware

- [XIAO ESP32-S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) (any ESP32-S3 with PSRAM)
- TF card (FAT32, Class 10+)
- USB-C cable for power & flashing

👉 [Pin definitions & hardware details](docs/en/hardware.md)

---

## Recording Modes

| Mode | Description | Storage Impact |
|------|-------------|---------------|
| **Continuous** (default) | Standard AVI segmented recording | Highest (full FPS) |
| **Timelapse** | Fixed-interval capture (5–300s), 15fps AVI playback | Low (~1/interval factor) |
| **Dynamic Timelapse** | Motion-adaptive: fast capture when active, slow when idle | Lowest (90%+ savings in quiet periods) |

Dynamic timelapse is the killer feature — set `timelapse_mode=2`, tune sensitivity (0–100), and let motion detection switch capture intervals automatically. Ideal for parking lots, warehouses, or watching a 3D printer.

---

## API Overview

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | No | Device status (recording, WiFi, storage, temp, motion, firmware) |
| GET | `/api/config` | No | Current configuration (passwords masked as `****`) |
| POST | `/api/config` | Yes | Update configuration |
| POST | `/api/record?action=start\|stop` | Yes | Recording control |
| POST | `/api/ota` | Yes | OTA firmware update from URL |
| POST | `/api/format` | Yes | Format SD card |
| POST | `/api/reset` | Yes | Factory reset |
| POST | `/api/time` | Yes | Set system time manually |
| GET | `/api/files` | No | Recording file list |
| POST | `/api/files/batch` | Yes | Batch delete files |
| DELETE | `/api/files?name=xxx` | Yes | Delete single file |
| GET | `/api/download?name=xxx` | No | Download recording |
| GET | `/api/scan` | No | Scan WiFi networks |
| GET | `/stream` | No | MJPEG live stream |
| GET | `/metrics` | No | Prometheus metrics (text format) |
| GET | `/onvif/*` | No | ONVIF WS-Discovery + SOAP services |
| WS | `ws://<IP>/` | No | WebSocket real-time push |

Default password: `admin` 👉 [Complete API docs](docs/en/api/overview.md)

---

## Architecture

```
main/  —  22 C modules + main.c + cJSON (flat layout)
├── main.c                 # Entry, 20-step boot, watchdog
├── camera_driver.c/h      # OV2640/OV3660 + JPEG capture
├── video_recorder.c/h     # AVI MJPEG segmented recorder (3 modes)
├── motion_detector.c/h    # Frame-difference motion analysis
├── mjpeg_streamer.c/h     # HTTP MJPEG live stream
├── frame_broadcaster.c/h  # Frame buffer distributor (decouples FB consumers)
├── onvif_service.c/h      # ONVIF SOAP service handlers
├── onvif_discovery.c/h    # ONVIF WS-Discovery multicast
├── web_server.c/h         # HTTP server + REST API (16 endpoints)
├── ws_server.c/h          # WebSocket real-time push
├── ota_updater.c/h        # OTA firmware update (esp_https_ota)
├── sha256.c/h             # SHA-256 for OTA verification
├── webhook.c/h            # HTTP event notifications
├── nas_uploader.c/h       # Upload dispatcher (WebDAV | HTTPS)
├── webdav_client.c/h      # WebDAV PUT + MKCOL
├── http_upload_client.c/h # HTTP/HTTPS chunked PUT
├── wifi_manager.c/h       # AP/STA dual-mode
├── config_manager.c/h     # NVS config + SD card override
├── storage_manager.c/h    # SD card FAT32 + circular cleanup
├── time_sync.c/h          # SNTP dual-server
├── status_led.c/h         # GPIO21 LED state machine (5 modes)
├── logging.c/h            # Logging utility
└── cJSON.c/h              # Vendored JSON (IDF v6.0 removed)
```

👉 [System architecture deep dive](docs/en/architecture.md)

---

## Documentation

| Guide | Contents |
|-------|----------|
| [Installation Guide](docs/en/getting-started.md) | Environment, build, flash, first-time setup |
| [Hardware Manual](docs/en/hardware.md) | Pinout, specs, wiring |
| [User Guide](docs/en/user-guide.md) | Config, recording modes, NAS, OTA, motion detection |
| [System Architecture](docs/en/architecture.md) | Boot process, module design, data flow |
| [API Reference](docs/en/api/overview.md) | Complete REST API + WebSocket + ONVIF |
| [Troubleshooting](docs/en/troubleshooting.md) | Common issues & fixes |
| [LED Status](docs/en/led-status.md) | LED blink patterns decoded |

---

## License

[GNU General Public License v3.0](LICENSE) — Free to use, modify, and distribute.
