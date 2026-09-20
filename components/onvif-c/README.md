# onvif-c

[![CI](https://github.com/mickeyzzc/onvif-c/actions/workflows/ci.yml/badge.svg)](https://github.com/mickeyzzc/onvif-c/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Coverage](https://img.shields.io/badge/line%20coverage-95%25-brightgreen.svg)](tests/coverage.sh)

**ONVIF Device (server) library for ESP-IDF in plain C** — expose a camera to
NVRs over SOAP + WS-Discovery + Pull-Point events with zero third-party
dependencies and a ~10 KB code footprint. Extracted from the production
implementation in the [MiBee Cam firmware](https://github.com/Mi-Bee-Studio)
(ESP32 family, 4 board variants), whose response XML is **byte-stable against
the MiBee NVR** and field-proven with Hikvision-class clients.

English | [中文](README.zh-CN.md)

## Features

- **SOAP Device/Media service** — the exact action set NVRs need to discover
  and add a camera: `GetSystemDateAndTime`, `GetDeviceInformation`,
  `GetCapabilities`, `GetProfiles`, `GetStreamUri`, `GetSnapshotUri`.
- **Pull-Point Events service** — `tns1:VideoSource/MotionAlarm` topic
  (`Source=CSI`, `State`, `Score 0-100`); single subscription, 1 h granted
  TerminationTime, 120 s idle expiry, no long polling (PullMessages returns
  immediately — esp_http_server workers never block).
- **WS-Discovery responder** — UDP 3702 / multicast 239.255.255.250;
  answers Probe with unicast ProbeMatches and announces Hello every ~30 s;
  retries through socket/bind/multicast-membership failures until WiFi is up.
- **Optional mDNS** — `_onvif._tcp` advertisement; compiles out cleanly when
  `espressif/mdns` is not in the build.
- **No XML parser, no dynamic state** — action detection via `strstr()`,
  responses via `snprintf()`; per-request buffers only; the motion producer
  hook is non-blocking and safe from sensor callback context.
- **One-config integration seam** — everything board-specific (identity, IP,
  stream URI, runtime gates, HTTP port) stays behind `onvif_c_config_t`
  callbacks; nothing is hardcoded.

## Usage

Vendor the tree (e.g. `components/onvif-c`), add `onvif-c` to main's
`REQUIRES`, then:

```c
#include "onvif_c.h"

static const char *my_stream_uri(void) {
    return "rtsp://192.0.2.134:554/stream";   /* or http://ip:81/stream */
}

void app_onvif_start(httpd_handle_t httpd) {
    onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "ESP32-S3-N16R8",
        .firmware_version = "v0.1.0",
        .serial           = my_serial,        /* stable hex string          */
        .uuid             = my_uuid,          /* no urn:uuid: prefix        */
        .ip               = my_ip,            /* NULL/"0.0.0.0" = not ready */
        .stream_uri       = my_stream_uri,
        .frame_rate       = my_fps,           /* NULL -> 15                 */
        .events_enabled   = my_events_gate,   /* NULL = no events service   */
        .http_port        = 80,               /* 0 -> 80; used in every URI */
        .mdns_hostname    = "mibeecam-a1b2",  /* NULL = skip mDNS           */
    };
    onvif_c_start(httpd, &cfg);
}

/* From your motion detector (e.g. WiFi-CSI callback) — never blocks: */
onvif_c_motion(true, 87);   /* MotionAlarm State=true  Score=87 */
onvif_c_motion(false, 4);
```

`tools/onvif_probe.py <ip>` is the no-hardware smoke test: it exercises every
served action and the full Pull-Point subscription cycle, exiting 0 on
success.

## API reference

Full contracts are documented inline in
[`include/onvif_c.h`](include/onvif_c.h) — the summary:

| Function | Contract |
| --- | --- |
| `onvif_c_start(httpd, cfg)` | Registers `/onvif/device_service` + `/onvif/media_service` (+ `/onvif/events_service` when `events_enabled` is set), starts WS-Discovery (+ optional mDNS). Returns `ESP_ERR_INVALID_ARG` on missing required callbacks, the first registration error otherwise; re-registration after restart is tolerated (`ESP_ERR_HTTPD_HANDLER_EXISTS` logged and ignored). |
| `onvif_c_stop()` | Stops the discovery task and removes the mDNS service. SOAP handlers stay registered (esp_http_server has no unregister API). |
| `onvif_c_motion(active, score)` | Feed a motion transition. Never blocks (lock contention drops the event); no I/O; safe from sensor/CSI callback context. Events queue only while a subscription is alive AND `events_enabled()` returns true. |
| `onvif_c_events_subscribed()` | True while a Pull-Point subscription is alive (diagnostics). |
| `onvif_c_version()` | Returns `ONVIF_C_VERSION` (`major*10000 + minor*100 + patch`, e.g. v0.1.0 → 100). |

`onvif_c_config_t` fields (strings are referenced, not copied — they must
outlive the service):

| Field | Required | Default | Notes |
| --- | --- | --- | --- |
| `serial`, `uuid`, `ip`, `stream_uri` | **yes** (validated) | — | `ip` may answer `NULL`/`"0.0.0.0"` while connecting; discovery waits. `uuid` without `urn:uuid:` prefix. |
| `manufacturer`, `model`, `hardware_id`, `firmware_version` | no | `"MiBee"`, `"MiBeeCam"`, `"ESP32"`, `"v0.1.0"` | Identity strings used by GetDeviceInformation. |
| `frame_rate` | no | 15 | GetProfiles `FrameRateLimit`. |
| `snapshot_uri` | no | derived `http://<ip>:<http_port>/api/capture` | GetSnapshotUri answer. |
| `events_enabled` | no | NULL = feature absent | Runtime gate; when NULL the events service is neither registered nor advertised. |
| `http_port` | no | 80 | Flows into **every** advertised URI. |
| `mdns_hostname`, `mdns_instance` | no | NULL = skip mDNS | instance defaults to `model`. |
| `scopes` | no | built from `model` | WS-Discovery Scopes body; resolved once at start. |

## Integration guide

1. `components/onvif-c` — vendor this tree (anything under `tests/`,
   `examples/`, `docs/`, `.github/` may be dropped), add `onvif-c` to main's
   `REQUIRES`. `espressif/mdns` is optional: declare it to enable mDNS.
2. Write your port layer — every board fact stays there. A complete real-world
   example is
   [`main/onvif_port.c` in the MiBee Cam firmware](https://github.com/Mi-Bee-Studio/esp32s3-n16r8-cam/blob/main/main/onvif_port.c)
   (~100 lines: identity from MAC/efuse, IP from the WiFi manager, stream URI
   from the RTSP server, a config-backed events gate).
3. Call `onvif_port_start()` once your httpd server is up; feed
   `onvif_c_motion()` from your detector.
4. Verify with `tools/onvif_probe.py <ip>` (exit 0 = full surface OK).

The four MiBee Cam repos (ESP32 + ESP32-S3, IDF v5.5/v6.0) carry this
component in lockstep and are the upstream production users.

## Security

**onvif-c does not implement ONVIF authentication.** There is no
WS-UsernameToken / `wsse` header parsing, no HTTP basic/digest auth, no
credential callback, and no 401 path — every served action, the Pull-Point
subscription, and the WS-Discovery responder answer any host that can reach
the device. Deploy it only on a network where every host is trusted (or
behind a reverse proxy that enforces credentials), and note that an ONVIF
client configured with credentials may refuse to add the device. This is a
deliberate scope decision for a minimal device library, not an oversight; if
you need auth, treat it as a feature request for a future minor version.

## Byte stability guarantee

Response element names, prefixes, attribute order and namespace style are
**load-bearing** — NVR integrators may match raw substrings. Device/Media
envelopes use `soap:`/`tds:`/`trt:`/`tt:` (lowercase `utf-8` declaration),
Events envelopes use `s:`/`tev:`/`wsnt:` (uppercase `UTF-8`), matching the
field-proven firmware bytes. The exact output is pinned by the host golden
tests (`tests/`); a diff there is a behavior change, not a cosmetic one.

Namespace style intentionally differs between service families because each
style is what real NVRs have been talking to in production — do not
"unify" them.

## Quality gates (TDD)

Everything the library ships is developed test-first and gated in CI:

| Gate | Command | What it enforces |
| --- | --- | --- |
| Host tests | `tests/run.sh` | 213 checks: core golden bytes + the full ESP-IDF port layer driven through stubs |
| Coverage | `tests/coverage.sh` | ≥80% line coverage over `core/` + `esp_idf/` (currently 95%) |
| Style | `tools/check_style.sh` | clang-format clean (pinned `clang-format==22.1.8`, see `.clang-format`) |
| Hygiene | `tools/check-repo-hygiene.sh` | no junk/secret files tracked |

The host harness (`tests/`) needs nothing but a C compiler and pthreads:

- **Core goldens** (`test_core.c`) — pure C, pins every response byte.
- **Port layer** (`test_service.c` / `test_events.c` / `test_discovery.c`) —
  the real `esp_idf/` handlers against ESP-IDF stubs (`tests/host_stubs/`):
  a fake httpd capturing requests/responses, a fake clock
  (`-Wl,--wrap=time`) making subscription expiry deterministic, pthread
  tasks, and a virtual UDP network feeding WS-Discovery probes and
  capturing ProbeMatches/Hello — including socket/bind/membership failure
  retry paths.

Set up the pre-commit hook once per clone: `tools/setup-hooks.sh`.

## Library hygiene

- Core (`core/`) is pure C with no ESP-IDF includes — host-testable with the
  system `cc`; the ESP-IDF surface (`esp_idf/`) is a thin transport.
- Builds on ESP-IDF v5.5.x and v6.0.x (both in CI).
- No logging outside `ESP_LOGx`, no `printf` in library code paths, no
  blocking in producer-context APIs.
- No hardcoded endpoints: `cfg->http_port` flows into every advertised URI
  (capabilities XAddrs, WS-Discovery XAddrs, subscription address); board
  specifics never leak into the library.

## Versioning

Semantic versioning; `ONVIF_C_VERSION` encodes it as
`major*10000 + minor*100 + patch` (read it at runtime with
`onvif_c_version()`). Compatibility rules:

- **Pinned bytes**: response XML of an unchanged action never changes within
  a major version — a golden-test diff is a release-blocking event.
- Core builder signatures (`core/*.h`) are internal-stable: breaking changes
  bump the minor version; the public `onvif_c.h` surface aims to never break
  within a major.
- Releases are tag-triggered (`v*`); the release workflow re-runs the full
  host suite before publishing.

## Status

v0.1.0 — first release; API seam stable; production-tested daily at
[Mi-Bee Studio](https://github.com/Mi-Bee-Studio) on four ESP32/ESP32-S3
camera boards against the MiBee NVR. Client counterpart (Go):
[onvif-go](https://github.com/mickeyzzc/onvif-go); sibling device library
(Rust): [onvif-rs](https://github.com/mickeyzzc/onvif-rs).

## Documentation

Topic guides live in the docs hub: <https://www.mlsbs.top/docs/mibeelibs>
(`docs/` here is a redirect only).

## License

MIT — see [LICENSE](LICENSE). Extracted from the MiBee Cam firmware
(Mi-Bee Studio); the firmware repos continue under GPL-3.0-or-later with
this component dual-licensed in place.
