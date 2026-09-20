# Changelog

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
