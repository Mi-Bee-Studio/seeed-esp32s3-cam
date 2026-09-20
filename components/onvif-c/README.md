# onvif-c

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
  answers Probe with unicast ProbeMatches and announces Hello every ~30 s.
- **Optional mDNS** — `_onvif._tcp` advertisement.
- **No XML parser, no dynamic state** — action detection via `strstr()`,
  responses via `snprintf()`; per-request buffers only; the motion producer
  hook is non-blocking and safe from sensor callback context.
- **One-config integration seam** — everything board-specific (identity, IP,
  stream URI, runtime gates) stays behind `onvif_c_config_t` callbacks.

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

## Library hygiene

- Core (`core/`) is pure C with no ESP-IDF includes — host-testable with the
  system `cc`; the ESP-IDF surface (`esp_idf/`) is a thin transport.
- Builds on ESP-IDF v5.5.x and v6.0.x.
- No logging outside `ESP_LOGx`, no `printf` in library code paths, no
  blocking in producer-context APIs.
- `tests/run.sh` — golden tests, zero dependencies, CI-enforced.

## Status

v0.1.0 — API seam stable; production-tested daily at
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
