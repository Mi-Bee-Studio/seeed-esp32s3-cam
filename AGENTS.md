# PROJECT KNOWLEDGE BASE

**Updated:** 2026-06-13
**Commit:** d02b987
**Branch:** main

## OVERVIEW

MiBee Cam — ESP-IDF firmware that turns a XIAO ESP32-S3 Sense into a surveillance camera. C (ESP-IDF v5.x/v6.0), CMake build, dual-core RTOS, no Arduino.

**Sensor note (2026-09-02):** the unit on this desk reports `Camera: OV5640 @ HD` at
boot even though this board is documented as OV2640 everywhere — the esp32-camera driver
auto-detects whichever sensor is wired. Do not assume a specific sensor model when
testing; read it from the boot log or `GET /api/status` (`camera` field).

## STRUCTURE

```
./
├── main/            # All firmware code (flat layout, 27 C modules)
│   └── web_ui/      # 6 HTML pages embedded into SPIFFS partition
├── docs/            # Dual-language docs (en/ + zh/)
├── partitions.csv   # Custom partition table (dual OTA + SPIFFS)
├── sdkconfig.defaults  # All hardware pin mappings, PSRAM, watchdog config
└── .github/workflows/release.yml  # Tag-triggered build+release CI
```

## RTOS TASK LAYOUT

| Task | Core | Priority | Role |
|------|------|----------|------|
| `audio_capture` | 0 | 5 | I2S PDM mic capture, G.711 encode, audio_bcast_publish (HIGHEST user task on Core 0) |
| `recorder` | 0 | 3 | Camera capture, motion detect, frame_buf alloc/publish, enqueue to sd_writer (was prio 5 - lowered to fix audio dropouts during SD writes) |
| `mjpeg_cli` | 1 | 2 | MJPEG HTTP streamer per-client task (port 81) |
| `rtsp_video` | 1 | 2 | RTSP video feed (espp) |
| `rtsp_audio` | 0 | 2 | RTSP audio feed (G.711) - moved to Core 0 for low-latency near I2S capture (was Core 1) |
| `sd_writer` | 1 | 2 | NEW: Dedicated SD write task. Drains write queue from recorder, performs async write_avi_frame + audio mux. Eliminates capture-loop blocking on SD I/O |
| WebSocket/httpd | 0/1 | 1 | HTTP server + WebSocket handlers |

**Rationale:**
- Audio capture at prio 5 is the highest user task - never preempted by SD writes.
- Recorder lowered to prio 3 (was 5) - stays on Core 0 to avoid preempting streamers on Core 1.
- sd_writer on Core 1 - I/O bound, doesn't need CPU proximity to camera.


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
| Audio capture | `main/audio_driver.c` | I2S PDM mic, GPIO41/42, DSR_16S (1.024MHz), 8kHz + DC removal + spectral NS + noise gate |
| Audio noise suppression | `main/audio_ns.c` | 256-pt FFT spectral subtraction, Wiener gain, min-statistics noise est |
| Audio codec | `main/g711_codec.c` | G.711 μ-law encode/decode |
| RTSP server | `main/rtsp_server.cpp` | MJPEG+G.711, port 554, digest auth |
| SD logging | `main/sd_log.c` | Event log to /sdcard/logs/, 512KB rotation |
| SD writer task | `main/video_recorder.c` | Async SD I/O task on Core 1, drains write queue from recorder, performs write_avi_frame + audio mux |

## REST API Endpoints

All business endpoints use the `/api/` prefix. Returns JSON envelope `{"ok":true,"data":...}` on success, `{"ok":false,"error":"..."}` on failure.

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/status` | open | Device status (WiFi, camera, system, storage) |
| GET | `/api/config` | open | Current configuration (passwords masked) |
| POST | `/api/config` | write | Partial config update; first-time password setup when `web_password` is empty |
| GET | `/api/camera` | open | Camera sensor settings |
| POST | `/api/camera` | write | Update camera settings (framesize/quality may reinit) |
| GET | `/api/capabilities` | open | Board capability flags (12 booleans) |
| GET | `/api/capture` | open | Single JPEG snapshot (`image/jpeg`, not JSON) |
| GET | `/api/scan` | open | WiFi AP scan |
| POST | `/api/time` | write | Manually set system time |
| POST | `/api/record` | write | Start/stop recording (`?action=start|stop`) |
| GET | `/api/record` | open | Recording status |
| GET | `/api/files` | open | List SD files |
| DELETE | `/api/files` | write | Delete a file (`?name=...`) |
| GET | `/api/download` | open | Download file (`?name=...&type=photo|recording`) |
| POST | `/api/format` | write | Format SD card |
| GET | `/api/ota/info` | open | OTA status/info |
| POST | `/api/ota/upload` | write | Upload firmware binary |
| POST | `/api/ota/spiffs` | write | Upload SPIFFS image |
| GET | `/api/led` | open | Flash LED state |
| OPTIONS | `/*` | — | CORS preflight (204 No Content) |

**Auth:** `X-Password` header for write operations. When `web_password` is empty (first boot), all writes return 401 `SET_PASSWORD_FIRST` except `POST /api/config` with a `web_password` field (first-time setup).

**MJPEG stream:** Separate TCP server on port `:81` (independent of main web server on port 80).

**Exempt paths:** `/metrics` (Prometheus), `/ws` (WebSocket), `/onvif/*` (SOAP), `/api/audio` (audio stream) are not under the unified business API surface.

## Web UI

Single-page application served from SPIFFS. Four files:
- `index.html` — page structure
- `app.js` — logic and API calls
- `style.css` — light/dark theme styles
- `i18n.js` — zh/en bilingual translations (auto-detect, persisted in localStorage)

Controls are shown or hidden based on `GET /api/capabilities`. The SPA baseline originates from `esp32s3-n16r8-cam` and is ported to all boards.

## Capabilities

This board returns the following from `GET /api/capabilities`:

| Capability | Supported |
|------------|-----------|
| ai | ❌ |
| sd | ✅ |
| audio | ✅ |
| ota | ✅ |
| mic | ✅ |
| flash_led | ❌ |
| recording | ✅ |
| timelapse | ❌ |
| onvif | ✅ |
| rtsp | ✅ |
| websocket | ✅ |
| mdns | ✅ |

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
- **Vendored espp__rtsp**: the RTSP component lives in `components/espp__rtsp/` (NOT
  managed_components) — heavily patched: digest auth (mbedtls), TCP-interleaved RTP,
  max_sessions cap. `components/` shadows the registry version, so builds are identical
  locally and in CI. Do NOT re-add `espp/rtsp` to `main/idf_component.yml`; its deps
  (base_component/socket/task) are declared directly instead, and `main/CMakeLists.txt`
  REQUIRES `espp__rtsp` explicitly. Edit the vendored copy in place.

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
- **Audio DSP pipeline** — I2S PDM (DSR_16S) → DC removal (IIR) → spectral noise subtraction (256-pt FFT, Wiener gain α=4.0) → 4× gain → noise gate → G.711 μ-law
- **Web audio playback** - `AudioContext({sampleRate:8000})`, separate read/schedule loops with 100ms lookahead, mu-law decode via lookup table

### Perf Optimization Architecture (v0.4.0)
- **Async SD write**: `recording_task` captures and enqueues frame_buf references to a depth-2 queue; `sd_writer_task` on Core 1 drains the queue and performs write_avi_frame. Sentinel barrier (NULL fb + xTaskNotifyGive) ensures segment rotation waits for all queued frames to flush before close_segment.
- **Audio frames in internal RAM**: 160-byte G.711 frames allocate from `MALLOC_CAP_DMA | MALLOC_CAP_8BIT` (internal SRAM), not PSRAM. Avoids cache contention with JPEG DMA traffic.
- **MJPEG TCP_NODELAY**: Client sockets have Nagle's algorithm disabled for lower latency on small part-headers. Combined with 16KB TCP buffer (sdkconfig) and 8KB send chunks for throughput.
- **Cycle-relative frame delay**: MJPEG streamer records cycle_start at loop top, computes target_ticks from fps, only delays if elapsed < target. Actual delivered FPS matches config (was lower because send time was added to absolute delay).
- **WS mutex snapshot pattern**: `ws_broadcast` snapshots client req[] array under mutex, releases mutex, then sends without holding it. Prevents connect/disconnect starvation during slow client sends.
- **frame_buf_t.capture_us**: Added int64_t field for future RTSP AV sync (requires espp upstream patch to send_frame_with_ts; currently unused, default 0).

## ANTI-PATTERNS (Don't)

- **Don't raise recording_task above priority 3** - it preempts audio_capture (prio 5) on Core 0 during SD writes, causing audio dropouts.
- **Don't move recording_task to Core 1** - would make it the highest-prio user task there (prio 3 > streamers' prio 2), preempting MJPEG/RTSP video.
- **Don't hold the ws_server mutex during httpd_ws_send_frame** - use the snapshot pattern (copy req[] array under mutex, release, then send).
- **Don't allocate 160-byte audio frames in PSRAM** - use MALLOC_CAP_DMA (internal RAM). PSRAM cache contention with JPEG traffic hurts audio path latency.
- **Don't reduce TCP_MSS below 1440** - sister repo (ai-thinker-esp32-cam) tried MSS=760, halves throughput. Keep MSS=1440, raise RTO/MAXRTX instead for weak WiFi.

## RELEASE

**HARD RULE: never tag or publish a release unless the user explicitly asks for it in
the current conversation** (see root workspace AGENTS.md — 2026-09-02 rule). The steps
below describe HOW, not WHEN.

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
