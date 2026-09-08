#!/usr/bin/env python3
"""gzip -9 the SPA text assets into main/web_ui/*.gz (build inputs, gitignored).

Why: tight-heap boards (ai classic-ESP32 internal RAM, luatos no-PSRAM) with
WiFi CSI enabled cannot push the raw ~62KB app.js through lwIP TX buffers —
transfers die at 0-4KB (PIT-038). web_server.c handler_static prefers
<path>.gz when the client sends Accept-Encoding: gzip (~4x smaller body).
Browsers negotiate transparently; curl needs --compressed.

Run after editing any web_ui file:
    python3 tools/compress_ui.py

Notes:
- *.gz are gitignored (main/web_ui/*.gz) and packed into SPIFFS by the same
  file(GLOB) rule; ADDING the .gz files the first time needs idf.py
  reconfigure (PIT-026 GLOB caveat).
- mtime=0 for reproducible images.
"""
import gzip
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "main" / "web_ui"
EXTS = {".js", ".css", ".html", ".svg", ".json"}

def main() -> int:
    if not ROOT.is_dir():
        print(f"web_ui dir not found: {ROOT}", file=sys.stderr)
        return 1
    total_raw = total_gz = 0
    for p in sorted(ROOT.iterdir()):
        if p.suffix in EXTS:
            raw = p.read_bytes()
            out = ROOT / (p.name + ".gz")
            with open(out, "wb") as rawf:
                with gzip.GzipFile(fileobj=rawf, mode="wb", compresslevel=9, mtime=0) as f:
                    f.write(raw)
            total_raw += len(raw)
            total_gz += out.stat().st_size
            print(f"{p.name}: {len(raw):>7} -> {out.stat().st_size:>7}")
    if total_raw == 0:
        print("no text assets found", file=sys.stderr)
        return 1
    print(f"total: {total_raw} -> {total_gz} ({total_gz * 100 // total_raw}%)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
