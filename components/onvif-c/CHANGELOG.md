# Changelog

## 0.2.0 (2026-09-20)

TDD hardening wave: the full ESP-IDF port layer is now host-tested, and
coverage/style gates are enforced in CI.

- **No hardcoded HTTP port** — `cfg->http_port` flows into every advertised
  URI (GetCapabilities XAddrs, WS-Discovery ProbeMatches/Hello XAddrs,
  Pull-Point subscription address). Previously these hardcoded `:80`.
  Byte-identical output for port 80 (all current boards).
- **Core builder signatures gained `unsigned port`** (breaking for direct
  core users; the public `onvif_c.h` API is unchanged):
  `onvif_xml_capabilities`, `onvif_xml_create_pull_point_response`,
  `onvif_probe_build_matches`, `onvif_probe_build_hello`.
- Removed unreachable fallback literals (`serial`, `stream_uri`) —
  `onvif_c_start()` already rejects NULL required callbacks.
- Host test harness (`tests/host_stubs/`): fake httpd, fake clock via
  `-Wl,--wrap=time`, pthread tasks, virtual UDP network — 212 checks
  across core goldens + service dispatch + events lifecycle (idle /
  termination expiry, MessageLimit parsing, ring overflow) + discovery
  responder (probe echo, placeholder RelatesTo, periodic Hello, retry
  paths).
- **Coverage gate** `tests/coverage.sh`: ≥80% lines over `core/` +
  `esp_idf/` (currently 95%), CI-enforced.
- **Style gate** `tools/check_style.sh` + `.clang-format`
  (pinned clang-format 18.1.8), CI-enforced; `tools/format.sh` to apply.
- Pre-commit hook now runs style (when clang-format is available) and the
  full host suites; `tools/setup-hooks.sh` installs it.

## 0.1.0 (2026-09-20)

Initial extraction from the MiBee Cam firmware (GPL-3.0-or-later,
single-contributor, re-licensed MIT with the owner's approval).

- SOAP Device/Media service: GetSystemDateAndTime, GetDeviceInformation,
  GetCapabilities, GetProfiles, GetStreamUri, GetSnapshotUri
- Pull-Point Events service (MotionAlarm), non-blocking producer hook
- WS-Discovery responder (Probe/ProbeMatches/Hello) + optional mDNS
- `onvif_c_config_t` callback seam replacing all board symbols
- Host golden tests pinning byte-stable responses (36 checks)
- `tools/onvif_probe.py` no-hardware smoke probe
