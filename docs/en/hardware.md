# Hardware Manual

## 1. Development Board

**XIAO ESP32-S3 Sense** — Ultra-small ESP32-S3 development board from Seeed Studio with onboard camera interface and TF card slot.

- Dimensions: 21 × 17.5 mm
- USB Type-C power and flashing
- Onboard OV2640 camera module (replaceable with OV3660)
- Onboard TF card slot (Micro SD)
- 1 user LED (GPIO21, active-low)
- 1 BOOT button (GPIO0)

---

## 2. Chip Specifications

| Parameter | Specification |
|-----------|---------------|
| Chip Model | ESP32-S3-WROOM-1-N8R8 |
| CPU | Xtensa dual-core LX7, up to 240 MHz |
| Flash | 8 MB (embedded) |
| PSRAM | 8 MB Octal (embedded) |
| WiFi | 802.11 b/g/n, 2.4 GHz |
| Bluetooth | Bluetooth 5 (BLE) |
| USB | USB OTG (supports CDC + JTAG) |
| Operating Voltage | 3.3 V |
| Operating Temperature | -40°C ~ 85°C |

### PSRAM Configuration

This project uses Octal PSRAM mode (8-line), initialized at boot (`CONFIG_SPIRAM_BOOT_INIT=y`). Camera frame buffers are allocated in PSRAM, dual-buffer mode (`fb_count = 2`), each frame size depends on resolution and JPEG quality.

- Reserved internal RAM: 16 KB (`SPIRAM_MALLOC_ALWAYSINTERNAL`)
- Reserved for DMA/peripherals: 32 KB (`SPIRAM_MALLOC_RESERVE_INTERNAL`)
- Allow external stack: Yes (`SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`)

---

## 3. Pin Definitions

### Camera Interface (DVP)

| Function | GPIO | Description |
|----------|------|-------------|
| XCLK | 10 | Main clock output (20 MHz) |
| SIOD (SDA) | 40 | SCCB data line (I²C) |
| SIOC (SCL) | 39 | SCCB clock line (I²C) |
| D0 | 18 | Data bit 0 |
| D1 | 17 | Data bit 1 |
| D2 | 16 | Data bit 2 |
| D3 | 15 | Data bit 3 |
| D4 | 14 | Data bit 4 |
| D5 | 12 | Data bit 5 |
| D6 | 11 | Data bit 6 |
| D7 | 48 | Data bit 7 |
| VSYNC | 38 | Vertical sync |
| HREF | 47 | Horizontal reference |
| PCLK | 13 | Pixel clock |
| PWDN | -1 | Not used |
| RESET | -1 | Not used |

### TF Card Interface (SDMMC)

| Function | GPIO | Description |
|----------|------|-------------|
| CLK | 7 | SD clock |
| CMD | 10 | SD command/response (**shared with camera XCLK**) |
| D0 | 8 | SD data line 0 |

### Other

| Function | GPIO | Description |
|----------|------|-------------|
| Status LED | 21 | Active-low |
| BOOT Button | 0 | Active-low, hold 5 seconds for factory reset |

> **Important**: GPIO10 is used for both camera XCLK and SD card CMD. By using 1-line SDMMC mode (only CLK + CMD + D0), GPIO10 is shared between camera and SD card. 4-line SDMMC mode is not available for this hardware.

---

## 4. Camera

### Supported Models

| Model | PID | Max Resolution | Description |
|-------|-----|---------------|-------------|
| OV2640 | 0x2642 | 2MP (UXGA) | Default configuration, XIAO onboard |
| OV3660 | 0x3660 | 3MP (QXGA) | Replaceable module |

The system automatically detects the model by reading sensor PID via SCCB during initialization, no manual configuration required.

### Resolution Configuration

| Config Value | Resolution | Pixels |
|-------------|------------|--------|
| 0 | VGA | 640 × 480 |
| 1 | SVGA | 800 × 600 (default) |
| 2 | XGA | 1024 × 768 |

### Capture Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| FPS | 1-30 | — | Frame rate target |
| JPEG Quality | 1-63 | — | Lower value = higher quality |
| Frame Buffer | 2 (fixed) | — | Dual buffer, allocated in PSRAM |
| Grab Mode | `CAMERA_GRAB_LATEST` | — | Always get latest frame |
| XCLK Frequency | 20 MHz | — | Camera main clock |

---

## 5. TF Card

### Hardware Requirements

| Parameter | Requirement |
|-----------|-------------|
| Card Type | Micro SD / SDHC / SDXC |
| File System | FAT32 |
| Speed Class | Class 10 or higher |
| Interface Mode | SDMMC 1-line |
| Allocation Unit | 64 KB |
| Max Simultaneous Open Files | 8 |

### Storage Structure

```
/sdcard/
├── recordings/              # Recording root directory
│   └── YYYY-MM/
│       └── DD/
│           ├── REC_YYYYMMDD_HHMMSS.avi
│           └── ...
└── config/                  # Configuration override directory
    ├── wifi.txt             # KEY=VALUE format
    └── nas.txt              # KEY=VALUE format
```

### Circular Storage Strategy

- Check free space after each recording segment
- Free space < 20%: Automatically delete oldest recording files
- Continue deleting until free space ≥ 30%
- Single cleanup safe limit: 100 files

---

## 6. LED Status Indication

Status LED is connected to GPIO21, **active-low** (output 0 = on, output 1 = off).

| Status | LED Behavior | Trigger Condition |
|--------|--------------|-------------------|
| `LED_STARTING` | Solid | System booting |
| `LED_AP_MODE` | Slow blink (1-second cycle) | WiFi AP mode (waiting for configuration) |
| `LED_WIFI_CONNECTING` | Fast blink (200ms cycle) | WiFi STA connecting |
| `LED_RUNNING` | Off | Normal operation |
| `LED_ERROR` | Double blink (two blinks, 1-second pause) | Error state (SD card failure, etc.) |

### Double Blink Timing

```
ON 200ms → OFF 200ms → ON 200ms → OFF 1000ms → Loop
```

Implemented via FreeRTOS software timer, timer period 200ms, using state machine counter to control double blink logic.

---

## 7. Power

| Parameter | Specification |
|-----------|---------------|
| Power Supply | USB Type-C |
| Voltage | 5V |
| Minimum Current | 1A (recommended) |
| Operating Voltage | 3.3V (onboard LDO) |

### Power Consumption Reference

| Mode | Estimated Consumption |
|------|------------------------|
| Standby (WiFi connected) | ~100 mA |
| Recording (SVGA 15fps) | ~200 mA |
| Recording + Streaming + NAS Upload | ~300-400 mA |
| WiFi AP Mode | ~120 mA |

> **Note**: Use USB 3.0 port or standalone 5V/1A+ power adapter to avoid instability due to insufficient USB port power.
