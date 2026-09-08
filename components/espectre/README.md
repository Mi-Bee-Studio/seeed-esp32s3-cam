# ESPectre SDK (vendored)

Upstream: https://github.com/francescopace/espectre · SDK version 3.0.0
(imported 2026-09-06 from `main` via raw.githubusercontent batch fetch;
tarball download blocked on this network — no git history, hence no
`ESPECTRE_GIT_VERSION` stamp).

Scope: `src/cpp/{core,runtime}` + facades + cmake/Kconfig + license files.
Dropped vs upstream: `idf_component.yml` (its `improv` git dependency is
unfetchable here and unneeded — optional groups stay off), `frontend/`
(reference firmware), `Doxyfile`.

License: **GPL-3.0-only** for these files (see LICENSE / LICENSING.md /
THIRD_PARTY_NOTICES.md in this directory). The combined firmware of this repo
is therefore distributed under GPLv3.

Integration in this repo: `main/csi_motion.cpp`, gated by
`CONFIG_MIBEE_CSI_MOTION` (default n). Optional capability groups
(MQTT/provisioning/Direct/OTA/frontend support) must stay OFF in menuconfig —
their deps (`improv`, `esp_tinyusb`) are not vendored. Root `CMakeLists.txt`
removes `improv` via `ESPECTRE_SDK_EXCLUDED_REQUIRES`.

Sensing Kconfig (profile, pps, traffic mode) lives under
"ESPectre Sensing" in menuconfig, provided by `Kconfig.projbuild` here.
