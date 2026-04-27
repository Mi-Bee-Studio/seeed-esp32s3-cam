# ESP32-S3 Camera Monitor

> Smart monitoring camera firmware based on XIAO ESP32-S3 Sense

ESP32-S3 based monitoring camera firmware with MJPEG real-time streaming, AVI segmented recording, and automatic NAS upload. Developed with ESP-IDF, optimized for 8MB Octal PSRAM, running stable real-time video capture and transmission in resource-constrained embedded environments.

## Key Features

- 📹 MJPEG real-time video streaming, view directly in browser
- 🎬 AVI automatic segmented recording, circular storage without space concerns
- ☁️ FTP / WebDAV automatic upload to NAS
- 📡 WiFi AP/STA dual mode, plug and play
- 🌐 REST API + Web management interface
- 💾 TF card hot-plug, automatic recording resume
- 🔍 OV2640 / OV3660 auto-detection
- 🛡️ Watchdog + health monitoring, long-term stable operation

## Quick Start

```bash
git clone https://github.com/Mi-Bee-Studio/esp32s3-cam.git
cd esp32s3-camera
idf.py build
idf.py -p COM3 flash monitor
```

Requires ESP-IDF v5.x or v6.0 development environment. 👉 [Detailed Installation Guide](docs/en/getting-started.md)

## Hardware Requirements

[XIAO ESP32-S3 Sense](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) development board + onboard OV2640/OV3660 camera + TF card (FAT32, Class 10+)

👉 [Hardware Details and Pin Definitions](docs/en/hardware.md)

## Documentation

| Document | Description |
|----------|-------------|
| [Installation Guide](docs/en/getting-started.md) | Environment setup, compilation, flashing, initial configuration |
| [Hardware Manual](docs/en/hardware.md) | Pin definitions, hardware specifications, wiring instructions |
| [User Guide](docs/en/user-guide.md) | Configuration management, LED indicators, storage strategy |
| [System Architecture](docs/en/architecture.md) | Boot process, module architecture, data flow |
| [Troubleshooting](docs/en/troubleshooting.md) | Common issues, debugging methods |
| [API Reference](docs/en/api/overview.md) | Complete REST API documentation |

## API Overview

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Device status (recording, WiFi, storage, camera) |
| GET | `/stream` | MJPEG real-time video stream |
| POST | `/api/config` | Modify configuration (requires authentication) |
| POST | `/api/record?action=start\|stop` | Recording control (requires authentication) |
| GET | `/api/files` | Recording file list |
| GET | `/api/download?name=xxx` | Download recording file |

Default management password: `admin`. 👉 [Complete API Documentation](docs/en/api/overview.md)

## Project Structure

```
esp32s3-camera/
├── main/                 # Firmware source code (14 C modules)
│   ├── main.c            # Entry point, 19-step boot process
│   ├── camera_driver.c   # Camera driver (OV2640/OV3660)
│   ├── video_recorder.c  # AVI recording engine
│   ├── mjpeg_streamer.c  # MJPEG real-time streaming
│   ├── web_server.c      # HTTP server + REST API
│   ├── nas_uploader.c    # NAS upload scheduler
│   ├── wifi_manager.c    # WiFi AP/STA management
│   ├── config_manager.c  # NVS configuration persistence
│   ├── storage_manager.c # SD card + circular cleanup
│   ├── status_led.c      # LED state machine (5 modes)
│   ├── time_sync.c       # SNTP time synchronization
│   ├── ftp_client.c      # FTP protocol client
│   ├── webdav_client.c   # WebDAV protocol client
│   └── web_ui/           # Web management interface (4 pages)
├── docs/                 # Project documentation
│   ├── en/               # English documentation
│   └── zh/               # Chinese documentation
├── partitions.csv        # Partition table (factory 3.5MB + SPIFFS 256KB)
└── sdkconfig.defaults    # XIAO ESP32-S3 Sense default configuration
```

## License

[GNU General Public License v3.0](LICENSE)

This project is licensed under the GNU General Public License v3.0. For details, please refer to the [LICENSE](LICENSE) file.

You are free to use, modify, and distribute this software under the terms of the license.
