#!/usr/bin/env python3
"""MJPEG stream prober for MiBee Cam boards.

Measures a running MJPEG stream (:81/stream): frame count, delivered fps,
frame-size histogram, throughput, reconnect count. Polls /api/status in
parallel to track uptime (reboot detection), heap and chip temp.

Handles LRU-kick churn (single-slot boards with an SPA watchdog attached):
reconnects and keeps accumulating; disconnects are counted, not fatal.

Usage: mjpeg_probe.py <ip> <duration_s> [out_json_path]
Output: one JSON blob to stdout (and file if given).
"""
import json
import socket
import sys
import threading
import time
import urllib.request

SOI = b"\xff\xd8\xff"          # JPEG start (with third byte to cut false hits)
EOI = b"\xff\xd9"


def http_json(ip, path, timeout=8):
    with urllib.request.urlopen(f"http://{ip}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode())


class StatusPoller(threading.Thread):
    def __init__(self, ip, interval=5.0):
        super().__init__(daemon=True)
        self.ip = ip
        self.interval = interval
        self.samples = []
        self.stop = threading.Event()

    def run(self):
        while not self.stop.is_set():
            try:
                d = http_json(self.ip, "/api/status")["data"]
                self.samples.append({
                    "t": round(time.time(), 1),
                    "uptime": d.get("uptime"),
                    "free_heap": d.get("free_heap"),
                    "min_heap": d.get("min_heap"),
                    "chip_temp": d.get("chip_temp"),
                    "stream_clients": d.get("stream_clients"),
                    "recording": d.get("recording"),
                })
            except Exception as e:
                self.samples.append({"t": round(time.time(), 1), "error": str(e)[:80]})
            self.stop.wait(self.interval)


def stream_once(ip, port, deadline, state):
    """Read one HTTP connection until deadline; feed complete frames into state."""
    url = f"http://{ip}:{port}/stream"
    buf = b""
    with urllib.request.urlopen(url, timeout=10) as r:
        state["http_code"] = r.status
        state["content_type"] = r.headers.get("Content-Type", "")
        while time.time() < deadline:
            chunk = r.read(65536)
            if not chunk:
                break
            state["bytes"] += len(chunk)
            buf += chunk
            # extract frames: each SOI starts a frame; previous frame ends at next SOI/EOI
            while True:
                i = buf.find(SOI)
                if i < 0:
                    # keep a small tail in case SOI is split across chunks
                    buf = buf[-4:]
                    break
                j = buf.find(SOI, i + 3)
                k = buf.find(EOI, i + 3)
                if j < 0 and k < 0:
                    buf = buf[i:]           # incomplete frame, wait for more
                    break
                end = j if (0 <= j < (k if k >= 0 else 1 << 62)) else k + 2
                state["frames"].append(end - i)
                buf = buf[end:]


def probe(ip, port, duration):
    poller = StatusPoller(ip)
    poller.start()
    state = {"frames": [], "bytes": 0, "disconnects": 0}
    t0 = time.time()
    deadline = t0 + duration
    while time.time() < deadline:
        try:
            stream_once(ip, port, deadline, state)
        except Exception:
            state["disconnects"] += 1
            time.sleep(0.3)
    poller.stop.set()
    poller.join(timeout=10)

    fs = sorted(state["frames"])
    n = len(fs)
    elapsed = time.time() - t0
    out = {
        "ip": ip,
        "duration_s": round(elapsed, 1),
        "frames": n,
        "fps": round(n / elapsed, 2) if n else 0.0,
        "bytes_total": state["bytes"],
        "kBps": round(state["bytes"] / 1024 / elapsed, 1),
        "disconnects": state["disconnects"],
        "frame_bytes": {
            "min": fs[0] if n else 0,
            "p50": fs[n // 2] if n else 0,
            "p95": fs[int(n * 0.95)] if n else 0,
            "max": fs[-1] if n else 0,
            "avg": round(sum(fs) / n) if n else 0,
        },
        "status_samples": poller.samples,
    }
    return out


if __name__ == "__main__":
    ip = sys.argv[1]
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    port = 81
    res = probe(ip, port, dur)
    text = json.dumps(res, ensure_ascii=False)
    print(text)
    if len(sys.argv) > 3:
        with open(sys.argv[3], "w") as f:
            f.write(text + "\n")
