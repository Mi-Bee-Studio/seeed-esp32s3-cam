# ONVIF Protocol Guide

## Overview

ONVIF (Open Network Video Interface Forum) is an open standard for IP-based physical security products. This firmware implements a minimal ONVIF streaming profile to allow Network Video Recorders (NVRs) to automatically discover and connect to the MiBee Cam as an IP camera.

**Compliance Level**: Minimal Profile Streaming only (no Profile S, no PTZ)  
**Purpose**: Allow NVRs to auto-discover and add this camera  
**Stream Format**: MJPEG over HTTP (not H.264)

The implementation focuses on the core streaming functionality needed for NVR integration, providing device identification, service discovery, and stream access through standard ONVIF SOAP messages.

## Discovery Flow with ASCII Flowchart

```
NVR                          Multicast Network                    MiBee Cam
 │                                │                                  │
 │──── Probe (UDP multicast) ────>│──── 239.255.255.250:3702 ──────>│
 │                                │                                  │
 │                                │    onvif_discovery_task()        │
 │                                │    ├─ is_probe_message() ✓       │
 │                                │    ├─ extract_message_id()       │
 │                                │    └─ build_probe_matches()      │
 │                                │                                  │
 │<── ProbeMatches (unicast) ────│<─── UDP unicast response ────────│
 │                                │                                  │
 │  NVR now knows device IP       │                                  │
 │                                │                                  │
 │──── POST /onvif/device_service (SOAP) ──────────────────────────>│
 │    Action: GetDeviceInformation│                                  │
 │<─── Response: Manufacturer, Model, FW, Serial ──────────────────│
 │                                │                                  │
 │──── POST /onvif/device_service (SOAP) ──────────────────────────>│
 │    Action: GetCapabilities     │                                  │
 │<─── Response: Device + Media service URLs ──────────────────────│
 │                                │                                  │
 │──── POST /onvif/media_service (SOAP) ───────────────────────────>│
 │    Action: GetProfiles         │                                  │
 │<─── Response: Video profile (resolution, JPEG encoding) ────────│
 │                                │                                  │
 │──── POST /onvif/media_service (SOAP) ───────────────────────────>│
 │    Action: GetStreamUri        │                                  │
 │<─── Response: http://<IP>/stream ───────────────────────────────│
 │                                │                                  │
 │  NVR connects to MJPEG stream  │                                  │
 │──── GET http://<IP>:81/stream ─────────────────────────────────>│
```

Also show the periodic Hello announcement:
```
MiBee Cam ──── Hello (multicast) ────> 239.255.255.250:3702
              (every ~30 seconds)
```

## Implemented SOAP Actions

| Service | Action | Purpose | Key Response Fields |
|---------|--------|---------|-------------------|
| Device | GetDeviceInformation | Device identity | Manufacturer=MiBee, Model=MiBee Cam, FW version, Serial=MAC |
| Device | GetCapabilities | Service endpoints | Device XAddr, Media XAddr |
| Device | GetServices | Available services | Device + Media namespace URLs |
| Device | GetSystemDateAndTime | Current time | UTC date/time |
| Media | GetProfiles | Video configuration | Resolution, JPEG encoding, FPS |
| Media | GetStreamUri | Stream URL | http://<IP>/stream |

## Technical Architecture

- **Discovery**: `onvif_discovery.c` — UDP multicast listener on Core 1, priority 2, 6144B stack
  - Multicast group: 239.255.255.250:3702
  - UUID: MAC-based (f472b01e-0000-1000-8000-{MAC})
  - Hello period: ~30 seconds
  - Scopes: video_encoder, NetworkVideoTransmitter, Profile/Streaming
  
- **SOAP Service**: `onvif_service.c` — HTTP handlers registered on port 80 httpd
  - Two endpoints: `/onvif/device_service` and `/onvif/media_service`
  - No XML parser — uses strstr() for action detection
  - Response generation via snprintf()
  - Max request body: 4096 bytes

- **Stream URI**: Returns `http://<IP>/stream` — the MJPEG stream on port 81

## NVR Compatibility

The following NVRs have been tested with MiBee Cam's ONVIF implementation:

| NVR | Discovery | Video Playback | Notes |
|-----|-----------|----------------|-------|
| **MiBeeNVR** 🏆 | ✅ | ✅ | **Native companion NVR** — Go-based, single binary, ONVIF auto-discovery, full MJPEG support. [github.com/Mi-Bee-Studio/MiBeeNvr](https://github.com/Mi-Bee-Studio/MiBeeNvr) |
| Generic ONVIF | ✅ | ✅ (MJPEG) | Best compatibility with standards-compliant NVRs |
| Hikvision | ✅ | ⚠️ | May expect H.264, falls back to MJPEG |
| Dahua | ✅ | ⚠️ | Similar to Hikvision |
| Blue Iris | ✅ | ✅ | Full MJPEG support |
| Shinobi | ✅ | ✅ | Full MJPEG support |
| Frigate | ✅ | ✅ | Full MJPEG support |

**MiBeeNVR** is the recommended companion NVR for MiBee Cam, providing the best integration experience with seamless ONVIF auto-discovery, live view, recording management, and a modern web UI — all in a single binary that runs on anything from a Raspberry Pi 3B to a full server.

Important notes:
- This camera outputs **MJPEG only** (not H.264). Some NVRs expect H.264 and may not work well.
- The `GetStreamUri` returns `http://<IP>/stream` which is the HTTP MJPEG stream.
- No ONVIF authentication — the camera does not implement `wsse:Security` headers.

## FAQ / Troubleshooting

Q: My NVR can't discover the camera
A: Checklist:
1. Camera must be in STA mode (connected to router, not AP mode)
2. NVR and camera must be on the same subnet (multicast doesn't cross subnets)
3. Check serial log for "Received Probe from..." — if you see this, discovery is working
4. Some NVRs need directed discovery — they POST to /onvif/device_service directly
5. Router multicast settings — some routers filter multicast traffic

Q: NVR finds the camera but can't play video
A: 
1. The stream URI is http://<IP>/stream which is HTTP MJPEG
2. NVR must support MJPEG — many expect H.264 and won't play MJPEG
3. Try manually adding the stream URL in the NVR
4. Check if the MJPEG stream works in a browser first

Q: Why does ONVIF discovery not work in AP mode?
A: The ONVIF discovery task starts only after WiFi STA connection. In AP mode, there's no multicast routing. Connect the camera to your router via STA mode.

Q: Is there RTSP support?
A: No. The RTSP server was removed. Use the HTTP MJPEG stream on port 81 instead.

Q: Can I set up ONVIF authentication?
A: Not currently. The implementation is minimal and does not support username/password authentication.

Q: What ONVIF profiles are supported?
A: Profile Streaming (minimal). No PTZ, no analytics, no event service.

## Configuration Reference
- ONVIF is always enabled — no configuration needed
- Device name shown in ONVIF: "MiBeeCam" (hardcoded)
- Manufacturer: "MiBee" (hardcoded)
- Stream URI resolution and FPS follow the camera config settings