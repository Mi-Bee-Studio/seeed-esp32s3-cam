# Changelog

## [Unreleased]

Robustness wave from the v0.1.0 code review (issues #6–#10):

- **Send paths no longer trust snprintf would-be lengths as byte counts**
  (#6). `SEND_BUILT` degrades an overflowing response to the standard SOAP
  fault instead of reading past the buffer; the three WS-Discovery `sendto`
  sites (initial Hello, periodic Hello, ProbeMatches) skip truncated frames
  with a warning. Oversized integrator strings (model / serial / stream URI
  / scopes) can no longer cause heap or stack over-reads — pinned by tests
  that run the huge-string paths and by the new ASan/UBSan CI job.
- **SOAP body reads loop over `httpd_req_recv`** (#7): a partial read (one
  TCP segment) is reassembled instead of degrading to
  `ter:ActionNotSupported`.
- **PullMessages assembly checks bounds before every write** (#8): per-append
  truncation checks make `cap - off` underflow impossible; the response
  buffer size is a named constant (`ONVIF_EV_RESP_MAX`).
- **CI runs the host suites under ASan/UBSan** (#9) — the stub harness
  drives the shipped port layer, so length-arithmetic regressions trap.
- **Security posture documented in the README** (#10): no ONVIF
  authentication is implemented; the deployment assumption (trusted LAN) is
  now stated explicitly in both languages.

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
