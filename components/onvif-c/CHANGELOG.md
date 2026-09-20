# Changelog

## 0.1.0 (2026-09-20)

First release. Initial extraction from the MiBee Cam firmware (GPL-3.0-or-later,
single-contributor, re-licensed MIT with the owner's approval), hardened with
a full TDD harness before release.

### Library

- SOAP Device/Media service: `GetSystemDateAndTime`, `GetDeviceInformation`,
  `GetCapabilities`, `GetProfiles`, `GetStreamUri`, `GetSnapshotUri`.
- Pull-Point Events service (`tns1:VideoSource/MotionAlarm`, `Source=CSI`,
  `State`, `Score 0-100`): single subscription (new replaces old), 1 h granted
  TerminationTime, 120 s idle expiry, no long polling; non-blocking producer
  hook safe from sensor callback context.
- WS-Discovery responder (UDP 3702 / multicast 239.255.255.250): unicast
  ProbeMatches answering Probes, periodic multicast Hello, retry paths on
  socket/bind/multicast-membership failure. Optional mDNS (`_onvif._tcp`)
  compiles out when `espressif/mdns` is not in the build.
- `onvif_c_config_t` callback seam: every board-specific value (identity, IP,
  stream URI, runtime gates, HTTP port) is injected — nothing is hardcoded.
  `cfg->http_port` flows into every advertised URI (capabilities XAddrs,
  WS-Discovery XAddrs, subscription address); default WS-Discovery scopes are
  resolved once at config time (no cross-thread static-buffer rebuild).
- No XML parser, no dynamic state, ~10 KB code footprint; response bytes are
  byte-stable and pinned by golden tests.

### Quality gates (CI-enforced)

- Host tests (`tests/run.sh`): 213 checks — core goldens plus the full
  ESP-IDF port layer driven through stubs (fake httpd, fake clock via
  `-Wl,--wrap=time`, pthread tasks, virtual UDP network). gcc + clang.
- Coverage gate (`tests/coverage.sh`): ≥80% lines over `core/` + `esp_idf/`
  (this release measures 95%).
- Style gate (`tools/check_style.sh` + `.clang-format`): clang-format clean,
  pinned `clang-format==22.1.8`.
- Hygiene gate (`tools/check-repo-hygiene.sh`) and a pre-commit hook
  (`tools/setup-hooks.sh`).

### Tools / docs

- `tools/onvif_probe.py` — no-hardware smoke probe (every served action +
  full Pull-Point cycle).
- Bilingual README (EN / zh-CN) with quality-gate, byte-stability and
  integration documentation; CONTRIBUTING with the TDD workflow.
