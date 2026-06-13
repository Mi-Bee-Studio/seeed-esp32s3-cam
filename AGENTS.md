# PROJECT KNOWLEDGE BASE

**Updated:** 2026-06-13
**Commit:** d02b987
**Branch:** main

## OVERVIEW

MiBee Cam — ESP-IDF firmware that turns a XIAO ESP32-S3 Sense into a surveillance camera. C (ESP-IDF v5.x/v6.0), CMake build, dual-core RTOS, no Arduino.

## STRUCTURE

```
./
├── main/            # All firmware code (flat layout, 20+ C modules)
│   └── web_ui/      # 6 HTML pages embedded into SPIFFS partition
├── docs/            # Dual-language docs (en/ + zh/)
├── partitions.csv   # Custom partition table (dual OTA + SPIFFS)
├── sdkconfig.defaults  # All hardware pin mappings, PSRAM, watchdog config
└── .github/workflows/release.yml  # Tag-triggered build+release CI
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Boot sequence | `main/main.c` `app_main()` | 20-step init, see comment block lines 19-41 |
| REST API (16 endpoints) | `main/web_server.c` | Handler table at line 1196 |
| Recording (3 modes) | `main/video_recorder.c` | AVI MJPEG segmented, continuous/timelapse/dynamic |
| Motion detection | `main/motion_detector.c` | Frame-difference, score 0-100 |
| ONVIF auto-discovery | `main/onvif_discovery.c` + `main/onvif_service.c` | WS-Discovery + SOAP, Synology/Milestone compatible |
| Frame broadcaster | `main/frame_broadcaster.c` | Decouples camera FB consumers (record + stream) |
| Config system | `main/config_manager.c` | NVS + SD card override (wifi.txt/config.txt/nas.txt) |
| WiFi | `main/wifi_manager.c` | AP/STA dual-mode, backup SSID, auto-fallback |
| OTA | `main/ota_updater.c` | SHA-256 verified, web upload or URL trigger |
| WebSocket push | `main/ws_server.c` | Real-time status + motion events |
| Web UI | `main/web_ui/*.html` | 6 pages, embedded via SPIFFS, purple theme |
| Camera hardware | `sdkconfig.defaults` lines 31-49 | Pin mapping for XIAO ESP32-S3 Sense |
| CI/CD | `.github/workflows/release.yml` | Tag-triggered, `espressif/idf:v6.0` container, hybrid release body |

## BUILD

```bash
# Setup (first time)
source ~/.espressif/v6.0.1/esp-idf/export.sh   # or v5.4/v5.5 for older builds
idf.py set-target esp32s3

# Build
idf.py build

# Flash full image (bootloader + partition-table + app + SPIFFS)
idf.py -p /dev/ttyACM0 flash

# Flash app only (fast, for iterative development)
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x10000 build/mibee_cam.bin

# Flash all partitions manually
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/mibee_cam.bin \
  0x3b0000 build/ota_data_initial.bin \
  0x3b2000 build/spiffs.bin

# Clean rebuild
idf.py fullclean && idf.py set-target esp32s3 && idf.py build
```

- **Serial port**: XIAO ESP32-S3 Sense uses USB-Serial/JTAG → `/dev/ttyACM0` (not ttyUSB)
- **Baudrate**: 115200 (firmware default, not 9600)
- **ESP-IDF**: v6.0.1 installed at `~/.espressif/v6.0.1/esp-idf/`
- **Serial device path**: `/dev/serial/by-id/usb-Espressif_USB_JTAG_*-if00 → /dev/ttyACM0`
- **Permission**: User must be in `uucp` group (or dialout on Debian/Ubuntu)

## SERIAL MONITOR

```bash
# Quick capture (15s)
python3 -c "
import serial, time, sys
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
start = time.time()
while time.time() - start < 15:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
s.close()
"

# Reset device + capture full boot log (30s)
python3 -c "
import serial, time, sys
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.1)
s.dtr = True; time.sleep(0.1); s.dtr = False; time.sleep(0.2)
start = time.time()
while time.time() - start < 30:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
s.close()
"

# Filter for specific log tags
python3 -c "..." | grep -E 'wifi|WiFi|error|ERROR'

# Monitor continuous output (Ctrl+C to stop)
cat /dev/ttyACM0
```

Key log patterns to watch:
- `config: Config loaded from NVS` — config OK
- `config: No stored config, using defaults` — first boot or factory reset
- `wifi: STA mode connecting to SSID=...` — STA mode starting
- `wifi: WiFi connected to ..., IP=...` — connection success
- `wifi: WiFi disconnected, retrying` — connection failure loop
- `wifi: AP mode started` — fallback to AP (no WiFi config)
- `recorder: Recording started` — normal operation

## NOTES

- **PSRAM Octal** — 8MB, must use 64B cache line, boot-init, malloc enabled
- **Dual OTA slots** — 1.75MB each in partitions.csv, OTA via web UI or URL
- **SPIFFS 256KB** — holds 6 web UI HTML files only
- **Watchdog** — TWDT 30s in firmware, sdkconfig sets 10s base timeout
- **Health monitor** — 60s cycle checks heap/PSRAM/stack/chip temp
- **SD hot-plug** — 10s polling, auto stop/resume recording
- **Circular storage** — auto-cleanup at <20% free, hard stop at 30%
- **Max 2 concurrent** clients for MJPEG HTTP stream
- **espressif/esp32-camera ~2.1.6** pinned in idf_component.yml
- **CI container** — `espressif/idf:v6.0`

## RELEASE

Creating a release:

1. **Tag & push**: `git tag v0.x.x && git push origin v0.x.x` — triggers `.github/workflows/release.yml`
2. **Curated highlights**: edit the `### Highlights` section in the YAML heredoc (`release.yml` line ~52) — 5-8 key bullet points
3. **Auto-generated changelog**: `generate_release_notes: true` appends full commit list categorized by type (feat/fix/docs/ci) — no manual work needed
4. **Assets automatically included**: `release/*` glob picks up 5 binaries + `flash_command.txt`

### Release body structure (hybrid)

```
## v0.x.x

### Highlights
(curated, 5-8 bullet points — edit per release)

### Packaging
- Full flash assets: bootloader, partition-table, mibee_cam, ota_data_initial, spiffs
- Complete flash_command.txt included in assets

---

(auto-generated changelog appended by GitHub)
```

### DO
- Tag from `main` branch after all changes for the release are merged
- Write curated highlights in present tense, focusing on user-facing changes
- Verify asset count = 6 (5 binaries + flash_command.txt) in the created release
- Test auto-generated changelog by checking the release page after CI completes

### DO NOT
- Hardcode version numbers in `release.yml` — use `$GITHUB_REF_NAME` instead
- Put `body.md` inside `release/` dir — it will be included as an asset
- Set `generate_release_notes: false` — this is what ensures the changelog is complete

### Assets

| File | Offset | Purpose |
|------|--------|---------|
| `bootloader.bin` | `0x0` | ESP32-S3 bootloader |
| `partition-table.bin` | `0x8000` | Partition table (dual OTA + SPIFFS) |
| `mibee_cam.bin` | `0x10000` | Main firmware app |
| `ota_data_initial.bin` | `0x3b0000` | OTA data initializer |
| `spiffs.bin` | `0x3b2000` | Web UI HTML files (6 pages) |
| `flash_command.txt` | — | Full flash instructions + app-only upgrade command |

### Troubleshooting

- **Release body empty or sparse**: check `generate_release_notes` is `true` in the YAML
- **body.md appears as asset**: it was placed in `release/` dir — move it to `/tmp/release-body.md`
- **Missing commits in changelog**: auto-generation uses PR titles or commit messages — ensure commits follow conventional commit format (`feat:`, `fix:`, `docs:`, `ci:`)
- **Wrong version in flash_command.txt**: ensure heredoc delimiter is unquoted `EOF` (not `'EOF'`) so `$GITHUB_REF_NAME` expands
