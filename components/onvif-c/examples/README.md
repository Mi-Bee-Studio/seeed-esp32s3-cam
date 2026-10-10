# Examples

The usage snippet in the top-level README is the integration contract; see
`onvif_cam/` for a compilable main wiring the config seam to static values
(adapt the callbacks to your board).

No-hardware smoke test of a running device:

    python3 tools/onvif_probe.py <device-ip> [seconds]
