# Installation Guide

## 1. Environment Preparation

### Required Tools

| Tool | Version | Description |
|------|---------|-------------|
| ESP-IDF | v5.x or v6.0 | Espressif official development framework |
| Python | 3.8+ | ESP-IDF dependency |
| Git | Latest | Source code management |
| USB Driver | — | CH340/CP2102 or onboard USB-CDC |

### Install ESP-IDF

**Windows:**

1. Download [ESP-IDF Tools Installer](https://dl.espressif.com/dl/esp-idf/)
2. Run the installer, select to download ESP-IDF v5.4 or higher
3. After installation, open "ESP-IDF 5.x CMD" terminal from Start menu

**Linux / macOS:**

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
. ./export.sh
```

> It is recommended to add `alias get_idf='. ~/esp/esp-idf/export.sh'` to your shell configuration file.

---

## 2. Get Source Code

```bash
git clone https://github.com/Mi-Bee-Studio/esp32s3-cam.git
cd esp32s3-camera
```

Project structure:

```
esp32s3-camera/
├── main/                 # Firmware source code (14 C modules)
│   ├── main.c            # Entry point, 19-step boot process
│   ├── camera_driver.c   # Camera driver
│   ├── video_recorder.c  # AVI recording engine
│   ├── mjpeg_streamer.c  # MJPEG real-time streaming
│   ├── web_server.c      # HTTP server + REST API
│   ├── nas_uploader.c    # NAS upload scheduler
│   ├── wifi_manager.c    # WiFi AP/STA management
│   ├── config_manager.c  # NVS configuration persistence
│   ├── storage_manager.c # SD card + circular cleanup
│   ├── status_led.c      # LED state machine
│   ├── time_sync.c       # SNTP time synchronization
│   ├── ftp_client.c      # FTP client
│   ├── webdav_client.c   # WebDAV client
│   ├── cJSON.c/h         # JSON parsing library
│   └── web_ui/           # Web management interface (4 HTML pages)
├── docs/                 # Project documentation
├── partitions.csv        # Partition table
└── sdkconfig.defaults    # Hardware default configuration
```

---

## 3. Compilation

Ensure ESP-IDF environment is loaded (able to execute `idf.py` in terminal), then:

```bash
idf.py set-target esp32s3
idf.py build
```

First compilation requires downloading dependent components, taking 3-5 minutes. Subsequent incremental compilations take about 30 seconds.

### Compilation Output

After successful compilation, the firmware is located at:
```
build/mibee_homecam.bin
```

---

## 4. Flashing

### Connect Device

1. Connect XIAO ESP32-S3 Sense to computer using USB Type-C cable
2. Confirm COM port appears in device manager

### Flash and Monitor

```bash
idf.py -p COM3 flash monitor
```

Replace `COM3` with the actual port number.

- **Windows**: `COM3`, `COM4`, etc.
- **Linux**: `/dev/ttyUSB0` or `/dev/ttyACM0`
- **macOS**: `/dev/cu.usbmodem*`

### Flash Only (No Monitoring)

```bash
idf.py -p COM3 flash
```

### Exit Monitoring

Press `Ctrl+]` to exit serial monitoring.

---

## 5. Initial Configuration

### Overview

```
Power on → LED solid (booting) → LED slow blink (AP mode)
    → Connect to WiFi "MiBeeHomeCam-XXXX"
    → Open browser at 192.168.4.1
    → Login with password admin
    → Fill WiFi information in config page
    → Device automatically switches to STA mode
    → LED off (normal operation, recording started)
```

### Detailed Steps

#### 1. Power On

After flashing, the device automatically restarts. Serial log shows:

```
I (xxx) main: MiBeeHomeCam v0.1 starting...
I (xxx) main: Free heap: XXXXX  Free PSRAM: XXXXXXX
```

#### 2. Confirm AP Mode

LED starts slow blinking (1-second cycle), indicating AP mode is active.

#### 3. Connect to WiFi

On your phone or computer, search for WiFi name: `MiBeeHomeCam-XXXX` (XXXX is last 4 hex digits of MAC address), and connect to this network.

#### 4. Open Management Page

Access `http://192.168.4.1` in browser to enter configuration page.

#### 5. Configure WiFi

Fill in the configuration page:
- **WiFi SSID**: Your home/office WiFi name
- **WiFi Password**: WiFi password
- Click save

#### 6. Wait for Connection

Device automatically switches to STA mode and connects to WiFi. LED becomes fast blinking (200ms), and turns off after successful connection.

#### 7. Get IP Address

During AP mode, you can view the assigned IP address through serial log:

```
I (xxx) wifi: STA connected, IP: 192.168.1.xxx
```

#### 8. Subsequent Access

Access management page through STA mode IP address: `http://192.168.1.xxx`

### SD Card Configuration Override (Optional)

If you cannot configure WiFi through web interface, create configuration files in TF card root directory:

**`/sdcard/config/wifi.txt`**:
```
WIFI_SSID=YourWiFiName
WIFI_PASS=YourWiFiPassword
```

**`/sdcard/config/nas.txt`**:
```
FTP_HOST=192.168.1.100
FTP_PORT=21
FTP_USER=username
FTP_PASS=password
FTP_PATH=/recordings
FTP_ENABLED=true
```

Insert TF card into device and restart, system will automatically read and override NVS configuration.

---

## 6. Verification

### Serial Log Check

After normal startup, you should see the following key logs:

```
I (xxx) storage: SD card mounted OK
I (xxx) camera: Sensor: OV2640, Resolution: SVGA, Quality: 12
I (xxx) web: Web server started on port 80
I (xxx) main: Recording started
I (xxx) main: MiBeeHomeCam v0.1 initialized successfully
I (xxx) main: Camera: OV2640 @ SVGA
I (xxx) main: WiFi: STA, IP: 192.168.1.xxx
```

### Web Interface Check

1. Access `http://<deviceIP>` in browser
2. Home page displays device status (WiFi connected, recording, storage space, etc.)
3. Enter "Preview" page to confirm live feed is visible

### Live Stream Test

Directly access video stream in browser:

```
http://<deviceIP>/stream
```

You should see continuous MJPEG live feed. Maximum 2 concurrent clients supported.

### API Status Check

```bash
curl http://<deviceIP>/api/status
```

Returns JSON containing `recording`, `wifi_state`, `sd_available`, `camera_sensor` and other fields. All normal indicates system is running properly.
