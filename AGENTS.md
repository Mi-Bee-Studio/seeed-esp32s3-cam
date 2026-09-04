# PROJECT KNOWLEDGE BASE

**Updated:** 2026-06-13
**Commit:** d02b987
**Branch:** main

## AT command interface (family contract v1.0, 2026-09-04)

统一契约：`docs/at-command.md`（四仓 md5 一致，地位同 api-contract）。核心集：
`AT / AT+HELP / AT+GMR / AT+STATUS / AT+WIFI?|= / AT+IP? / AT+CAMRES?|= / AT+CAMQUAL?|= /
AT+REBOOT / AT+RESTORE`（+能力裁剪项）。红线：**任何读指令不回显密码**；CAMQUAL 边界
10-63（PIT-021）。本板通道是 USB-JTAG CDC（/dev/ttyACM0，open 不复位）——主控制台是 UART0、stdin 不通 CDC，故 at_command.c 用 usb_serial_jtag 驱动直收直发（IDF v6 驱动安装收配置结构体）。CAMRES 保存+重启（PIT-019）、CAMQUAL 热应用。2026-09-04 新建模块。

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

> **2026-09-02 契约 v1.0 统一化**（权威规范：`docs/api-contract.md`，下表已部分过时）：
> 新增 `GET /api/config`（原死代码已注册）、`GET/POST /api/camera`（含 `supported_resolutions`）、
> `POST /api/reboot`、`GET /api/auth`；capabilities 增加 `api_version`/`wifi_scan`，
> `timelapse` 修正为 true（录像模式含延时）；status 字段对齐契约（`wifi_mode`→`wifi_state` 小写枚举、
> 新增 `device_name`/`min_heap`/`stream_clients`/`stream_clients_max`）。
> WS 推送已激活（motion/recording/wifi 事件，格式 `{"type","timestamp","data"}`）。
> **陷阱**：`/ws` 必须先于 `GET /*` 通配符静态处理器注册，否则被吞 404（已修，勿回退）。
> Web UI 为 4 文件 SPA（index/app/i18n/style，非 6 页紫色 MPA——旧描述有误）。
> **2026-09-02 UI v3 "Honey" 重设计**：蜂蜜琥珀主色（#FFB020/#E8930C）+ 暗色优先双主题、
> 视频主舞台 + 玻璃控制条 + 指标 chips + 分段式控制台；交互含鉴权抽屉（401 自动唤起并重试原操作）、
> 自定义模态确认（替代 confirm()）、按钮 busy 态、滑杆轨道填充、断流骨架屏、全屏。
> 资源共 ~110KB（SPIFFS 256KB）。设计令牌集中在 style.css 顶部，改样式先改令牌。
>
> **契约 v1.1（2026-09-02）**：家族统一公开默认密码 `mibeecam2026`（Kconfig `CONFIG_MIBEE_CAM_DEFAULT_WEB_PASSWORD` 默认值，可入文档；本地可在 gitignored sdkconfig 覆盖）；
> 空密码加载自动迁移 + 存量设备一次性种子（NVS `pw_seed_v1`，只跑一次）；服务端拒绝 <6 位密码；
> 新增 `GET /api/record`、UI"修改密码"模态；api_version=1.1。

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
# DEFAULT DELIVERY = Web OTA (flashing policy, root AGENTS.md 2026-09-04):
#   curl -X POST http://<ip>/api/ota/upload -H 'X-Password: <pwd>' \
#        -H 'Content-Type: application/octet-stream' --data-binary @build/mibee_cam.bin
#   (UI: same but /api/ota/spiffs with build/spiffs.bin; verify /api/ota/info
#   running_partition flip + device /app.js md5 == repo — PIT-017. Slot limit ~1.9MB.)
# USB below only for blank chips / rescue / unreachable devices.
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

### 2026-09-04 摄像头上限实测 + 三个坑修复（已烧录验证）

**实测矩阵**（重启应用法：改配置→重启→90s 探针；ch11 拥塞网，帧源确认活着）：
| 档位 | 推流 fps | 峰值温度 | 判定 |
|---|---|---|---|
| HD q12 | 7.3 | 81.5°C | 默认档，健康 |
| SXGA q12 | 1.6 | 90.5°C | 可选 |
| UXGA q12 | 0.9 | 93.5°C | **板级上限** |
| FHD q12 | 1.0 | 97.5°C | 剔除（超 85°C 规格）|
| QXGA q12 | 0.77 | 100.5°C | 剔除 |

**修复三件套**（详见 PITFALLS PIT-019/020/021）：
1. **OV5640 运行时 set_framesize 无效**（寄存器写成功但输出帧不变，HD→SXGA 后
   capture 仍 1280x720）→ 分辨率变更改"保存+应答+1s 重启"，`camera_set_resolution()`
   留 ⚠️ 注释仅供启动期；画质走新 `camera_set_quality()`（单寄存器写，热应用实测 OK）。
2. **板级上限 UXGA**：`camera_driver.h` `CAMERA_RES_BOARD_MAX`（FHD/QXGA 温度出局）；
   `camera_get_effective_max_res()` = min(传感器, 板级, 运行时 fb 预算)，
   supported_resolutions/POST 校验/camera_init/NVS 加载全部走它（测试残留的 QXGA
   配置在首次加载被自动钳到 UXGA）。
   **2026-09-04 下午三层化**：传感器层改查 esp32-camera 组件能力表
   （`esp_camera_sensor_get_info().max_size`，删掉了仓内手抄 PID 表——实戴
   OV5640 与文档 OV2640 不符的坑从此由组件自动适配）；memory 层为 PSRAM
   fb 预算校验（fb_count=3、512K floor，只能收紧）；`GET /api/camera`
   新增 `res_cap_source`（sensor/board/memory）报告钳制层。本板 OV5640
   满配不变（0-5、source=board）。
3. **画质 10-63**（`CAMERA_QUALITY_MIN/MAX`，fb=w*h/5 预算）+ `GET /api/camera`
   新增 `quality_min/quality_max`（SPA 滑杆钳制）。
- **停录像 = 杀流已修（PIT-020，2026-09-04）**：`RECORDER_PREVIEW` 状态 + 引用计数，
  有观众时停录降级预览继续供帧、恢复录像不重启任务。修复过程挖出并修掉两个潜伏
  bug（segment_open 标志提前消耗 → 恢复时写已关 FILE* 崩溃；sd_writer 状态机漏
  PREVIEW → sentinel 屏障死等 TWDT）。测流前仍建议先查 `/api/record`。

## NOTES

### 2026-09-02 套接字耗尽事故（Web UI 整体不可用的根因，已修复）

症状：设备 ping 通但 :80 连接被重置，串口持续刷 `httpd_accept_conn: error in accept (23)`（EMFILE）。
机理：NAS 上传器对不可达目标每文件发起 ≈9 个短连接（HEAD + 递归 MKCOL×N + PUT×3 重试），
连续失败 10 次才熔断 → 连接风暴把 `LWIP_MAX_SOCKETS=14` 的池打进 TIME_WAIT（2×60s MSL），
期间 httpd 无法 accept，Web UI 数分钟不可用，随每个录像段（300s）的上传周期反复发作。

修复四件套（勿单独回退任一项）：
1. `sdkconfig.defaults`：`LWIP_MAX_SOCKETS 14→24`、`LWIP_TCP_MSL 60000→15000`（TIME_WAIT 120s→30s）。
   生成文件 `sdkconfig` 已同步手改，无需 fullclean 重生成。
2. `nas_uploader.c`：`MAX_CONSEC_FAILS 10→3`（更早熔断进 5 分钟暂停）。
3. `web_server.c`：httpd `max_open_sockets 11→8`（给 :81/:554/上传器留余量）。
4. `ws_server.c`：30s ping 回收器摘除死 WS 客户端（浏览器异常断开不发 CLOSE 帧会一直占坑）；
   UI 侧 `beforeunload` 主动关闭 WS/音频连接。

### 2026-09-02 MJPEG 死帧事故（第二类槽位滞留，已修复）

症状：页面播放一段时间画面冻在最后一帧，重开也"连接中"，`:81` 槽位 3/3 满员。
机理：浏览器渲染器停滞时 TCP 连接依然健康（内核代答 ACK），recv 探测与 send 超时均无法
发现 —— 槽位被永久占用；重连/重载被 503 拒绝。修复（勿单独回退）：
1. `mjpeg_streamer.c`：客户端 fd 注册表 + **满员 LRU 踢除**（shutdown 最旧连接，
   新连接永远能赢，最多等 2s 后才 503）+ TCP keepalive（10s/5s×3）清 NAT 僵尸。
### 2026-09-03 整夜循环重启事故（三因叠加，已修复）

症状：设备过夜运行反复重启（实际 8h+，UI 显示 uptime 仅 ~30min），reset_reason=3（esp_restart 主动调用）。
完整链条：NAS 夜间不可达 → 每段录像(5min)入队后上传器**无失败延迟**地连环重试队列（单轮可 ~150 连接）
→ lwIP 池被 TIME_WAIT 挤占 → 健康监控的两个旧自愈逻辑误判 → esp_restart() 循环：
1. httpd 探针把 `socket()<0`（EMFILE）也判为"httpd 死"（探针自己也拿不到 socket）→ 连续 5 周期(60s) 重启。
2. "MJPEG 满员 5 分钟即重启" —— 但 3 个真实观众挂机整晚就是正常满员，必误杀。
修复（勿单独回退）：
- `main.c` probe_httpd_port80：socket/connect 的 EMFILE/ENOBUFS 归为"资源紧张不计数"，
  只有 TCP 连上但应用层无响应才累计；MJPEG 满员检查改为仅记录不重启（僵尸满员已由 LRU+keepalive 解决）。
- `nas_uploader.c`：上传失败后退避 30s 再取下一文件（原为立即连环重试）。

### 2026-09-03 内部 DRAM 枯竭（13KB → 54KB，已修复）

上述连接风暴期间暴露：内部 RAM 仅剩 ~13KB → `sdmmc allocate_dma_buf` 失败（录像丢帧风险）、
MJPEG 客户端任务栈(4KB)创建失败。注意 `/api/status.free_heap` 含 PSRAM（6MB+），看不到内部枯竭，
要看 `/metrics` 的 `node_memory_MemFree_bytes`（MALLOC_CAP_INTERNAL）。
修复：`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`（WiFi/lwIP 缓冲迁 PSRAM）。
教训：扩 `LWIP_MAX_SOCKETS`（14→24）这类修复要同时关注内部 DRAM 预算。

排查工具：`tools/overnight_log.py`（整夜串口采集，输出 overnight_serial.log + overnight_reboots.log，
自动标记 rst:0x 重启 banner 与错误行；烧录前先停采集器，否则串口冲突）。
**2026-09-03 加固**：USB 重新枚举会让 pyserial 阻塞在死 fd 上不抛错（10:43 重启因此漏抓）——
已加宽异常捕获 + 120s 静默看门狗强制重开。姐妹仓 luatos 已拷贝此版（端口改 ttyACM1）。

### 2026-09-03 停录崩溃（Guru LoadProhibited，重启循环真凶，已修复）

**现象**：停录/分段完成后 ~10s `Guru Meditation: Core 0 LoadProhibited (EXCVADDR 0x4)` → 重启；
修复前一度 46s 一崩连环重启（11:15-11:19 五连崩）。栈：`uxListRemove ← vTaskDelete ←
recorder_stop (video_recorder.c)`。
**根因**：`sd_writer_task` 自删（`vTaskDelete(NULL)`）前**不置空 `s_writer_handle`**，
`recorder_stop` 等待循环等满 10s 超时后对已释放 TCB 调 `vTaskDelete` → FreeRTOS 链表
use-after-free。对已释放内存的删除是"俄罗斯轮盘"——多数时候侥幸不崩，故此前未暴露。
修复：writer 退出前置空句柄（录制任务本就正确置空）+ drain 循环 `xTaskNotifyGive` 判空。
验证：3 轮 启动→停止→双停 压力零重启；修复后 40+ 分钟零 Guru。
副产品：修复前停录必卡 httpd ~10s（等待循环空转）也是此 bug。

### 2026-09-03 浏览器全面测试（UI 四修复，三仓共享已 md5 同步）

1. **StreamWatch 画布跨源污染**：流在 :81（跨源），img 未设 crossOrigin → no-cors 加载 →
   canvas `getImageData` 每 3s 抛 SecurityError → **防死帧看门狗自上线起整体失效**
   （三 SPA 板皆中）。服务端本就发 ACAO，客户端补 `img.crossOrigin='anonymous'` 即愈。
   tick 外加 try/catch 兜底。
2. **静态资源 max-age=3600**：UI 更新后浏览器最多跑 1 小时旧 JS（本次 crossOrigin 修复
   被缓存吞掉才暴露）。改全部 no-cache（LAN ~60KB 可忽略）。n16r8 本就 no-cache。
3. **api() 401 重试用旧 headers**：密码在入口只读一次，AuthSheet 认证后重试仍无
   X-Password → 首次写操作必二次 401。密码读取移入 attempt()。
4. **video-stage 宽高比**：`max-height:58vh` 会把 4:3 容器钳成 ~2:1（画面左右黑带）。
   改 `width:min(100%, calc(58vh*4/3))` + 首帧后按流自然比例动态设 aspect-ratio
   （HD 16:9 与 VGA 4:3 都满幅）。
5'. **boot 链自愈（2026-09-03 晚追加）**：原 DOMContentLoaded 里 caps 之后的任何异常
   都会让 `pollStatus()` 永不启动 → 骨架屏死锁（ai 弱射频首载撞掉线窗口实测复现，
   chips 几分钟不填充）。修复：状态轮询无条件前置 + capabilities 失败重试
   （10×5s）成功后再应用能力/拉配置。stats-strip 同时改 `flex-wrap:wrap`
   （6 芯片 751px > 列宽 658px，原隐藏滚动条方案把"已运行"永久挡在视区外）。
   **弱射频板部署校验方法**：curl 校验 app.js 必须用 -m 60（-m 10 会在空口竞争下
   自掐造成"截断"假象，实测 50409B 完整文件 hash 与仓库一致）。
5. **构建系统陷阱**：`spiffs_create_partition_image` 目录级依赖**不随文件内容编辑触发**
   （build/spiffs.bin 比新 app.js 旧 → 烧的是旧 UI，今日实锤）。三仓 CMakeLists 已加
   `file(GLOB) + DEPENDS` 显式文件依赖；改 web_ui 后核对 `md5(设备 /app.js) == md5(仓库文件)`。

### 2026-09-03 深夜 推流吞吐 + 契约 v1.2（已烧录验证）

1. **"推流没有内容"根因 = 信道拥塞 × TCP 窗口过小**。实测 ping RTT 169-330ms、
   丢包 40%（本板在 ch11 拥塞网 MiBeeAP1；对照 ai-thinker 在 ch2：8ms/0%），
   16KB SND_BUF × 250ms RTT 把每条 MJPEG 流钳在 ~64KB/s ≈ <1fps@55KB。
   修复：`CONFIG_LWIP_TCP_SND_BUF_DEFAULT 16384→49152`、
   `CONFIG_LWIP_TCP_WND_DEFAULT 16384→32768`（defaults 与生成 sdkconfig 同步手改，
   大缓冲经 SPIRAM_USE_MALLOC 走 PSRAM）。修复后单客户端 ~4fps/79KB/s。
   **固件只能缓解；根治需把板挪到不拥塞的网/信道（用户侧动作）**。
2. **契约 v1.2**：`POST /api/files/batch` 新增 `{scope:"all|recordings|photos"}`
   （递归删除、不受 GET 列表 64 条上限约束、跳过正在写的录像段）；
   `api_version`→1.2。SPA 存储页新增类型筛选/分页/勾选批量/按类清空/格式化
   （双重确认），四仓 md5 已拉齐。
3. 审计结论（round-trip 全部键 + 越界值探测）：本板配置面 55 项全过、
   非法值全部 400；仅 /api/files 最多返回 64 条（已知，scope 清空可兜底）。
   录像期间 /api/download 被 409 拒绝（既有设计，防写坏 AVI）。

悬案（未证实）：两次无解释 POWERON 重启（10:43、13:0x 附近）与**同集线器上刷写其他板**
的时间窗重合——疑似 hub 电流冲击；无证据不下结论，采集器继续盯着。

### 2026-09-03 晚 Web OTA 实战验证 + wifi_net 字段（未走串口烧录）

1. **OTA 能力端到端可用**（本次固件+UI 全程 WiFi 刷写，无串口参与）：
   `POST /api/ota/upload`（裸二进制流，非 multipart；Content-Length 即文件大小，
   X-Password 认证）→ 写入 next 分区 → 应答后 1s 自动重启；
   `POST /api/ota/spiffs` 同为裸流，先整擦 SPIFFS 再写、随后自动重启。
   实测 1.64MB 固件 17.6s（~93KB/s @ ch11 拥塞网）、256KB SPIFFS 2.7s；
   切换 ota_0→ota_1 干净启动无回滚。**上限：镜像 ≤1.9MB（OTA 槽尺寸）**。
2. **status 新增 `wifi_net`**（`primary|secondary`，`wifi_using_backup()`）——与
   ai-thinker/luatos 契约对齐，SPA WiFi 页"主网络/备用网络"标识依赖它。
3. **板载 SPA 已更新到双 WiFi 版本**（含备用 SSID 表单 + 当前连接行），
   `md5(设备 /app.js) == md5(仓库)` 验证通过（5e5c9a5d…）。
   OTA 场景下这条 md5 校验尤其重要——SPIFFS 上传失败中途 = UI 丢，只能串口救。

### 温度读数 0°C / 超量程（2026-09-02 修复）

温度传感器原本在 health_monitor_task 内安装、且循环先睡 60s 才首读 → 开机后
`/api/status.chip_temp` 长时间为 0。已改为 `chip_temp_init()` 在 app_main 序列提前安装并
立即首读。**S3 温度传感器只有固定测量档**（[-10,80] / [20,100] / [50,125] / [-30,50]），
请求区间必须完整落入某一档，跨档会被驱动拒绝（"Out of testing range"）。
本机高负载 96~103°C，`chip_temp_init()` 按 (50,125) → (20,100) → (-10,80) 依次尝试。
 luatos 同步改量程 (10,50)→(20,100)。

2. UI `app.js`：StreamWatch 看门狗 —— 3s 像素采样（24×18 签名），连续 3 次不变判定死帧
   自动重连（退避 0/5/30/60s）；服务端报 `stream_clients:0` 立即重连；每 90s 无条件换新流兜底。

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

## 2026-09-04 上午：WS 洪水防护 + SD 慢速重试 + record_on_boot（PIT-023）
- **WS 洪水事故**：LAN 某客户端 ~50Hz 发未掩码帧，旧 ws_handler 违例后 `return ESP_OK`
  → httpd 会话不关 → 无限重入打满 CPU0 → IDLE0 饿死 → TWDT 复位 ×19。连锁表象：
  SD `sdmmc_card_init 0x107` 失败"消失"、每次重启后开机自动录制"复活"、85°C+。
  **修复**：违例/recv 失败一律 `return ESP_FAIL` 关会话；WS connect 记录对端 IP。
- **SD 周期重试**：boot init 失败后 stat 探测永不翻转、无恢复路径 → sd_monitor 每 60s
  `storage_remount()` 重试（卡常在数分钟内自愈，本次重烧后自愈已实证）。
- **record_on_boot 配置**（默认 on=家族原行为）：开机自动录像可关；SD 未就绪时不硬启，
  等 sd_monitor 恢复后自动补启。GET/POST /api/config 已暴露。
- MJPEG accept 补 `SO_SNDTIMEO=10s`（PIT-024 家族同步，探测本仓已有）。
- 静态服务 MIME 表补 `.svg`/`.json`（favicon.svg 曾被当 text/html 回退）。
- 统一 logo favicon.svg（四仓同 md5）；web_ui 新增文件需 `idf.py reconfigure`（PIT-026）。
