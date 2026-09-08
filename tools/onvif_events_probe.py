#!/usr/bin/env python3
"""ONVIF Pull-Point 事件探针（契约 v1.5 §13）。

模拟 NVR 侧最小订阅流：CreatePullPointSubscription → 循环 PullMessages，
解析 tns1:VideoSource/MotionAlarm 的 State/Score/UtcTime。
设备端无长轮询（立即返回），本探针以 1s 节奏轮询。

用法: onvif_events_probe.py <ip> [duration_s]
输出: 人类可读事件行（stderr 不用，直接 stdout）+ 末行 JSON 汇总（机器可读）。
"""
import json
import re
import sys
import time
import urllib.request

NS = "http://www.onvif.org/ver10/events/wsdl"
CREATE = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
    f'<tev:CreatePullPointSubscription xmlns:tev="{NS}"/>'
    '</s:Body></s:Envelope>'
)
PULL = (
    '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
    f'<tev:PullMessages xmlns:tev="{NS}">'
    '<tev:Timeout>PT1S</tev:Timeout>'
    '<tev:MessageLimit>5</tev:MessageLimit>'
    '</tev:PullMessages></s:Body></s:Envelope>'
)

RX_EVT = re.compile(
    r'UtcTime="([^"]+)".*?Name="State" Value="(true|false)"'
    r'.*?Name="Score" Value="(\d+)"', re.S)


def post(ip, body, timeout=8):
    req = urllib.request.Request(
        f"http://{ip}/onvif/events_service", data=body.encode(),
        headers={"Content-Type": "application/soap+xml; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ip = sys.argv[1]
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

    code, body = post(ip, CREATE)
    if "CreatePullPointSubscriptionResponse" not in body:
        print(f"create failed: http={code} body[:200]={body[:200]}")
        sys.exit(2)
    term = re.search(r"TerminationTime>([^<]+)<", body)
    print(f"subscribed (termination={term.group(1) if term else '?'})")

    events, pulls, errors = [], 0, 0
    t0 = time.time()
    while time.time() - t0 < dur:
        try:
            code, body = post(ip, PULL)
            pulls += 1
            for ts, state, score in RX_EVT.findall(body):
                ev = {"utc": ts, "state": state == "true", "score": int(score)}
                events.append(ev)
                print(f"[{time.strftime('%H:%M:%S')}] MotionAlarm "
                      f"{'MOTION' if ev['state'] else 'clear'} score={ev['score']}")
        except Exception as e:
            errors += 1
            print(f"pull error: {e}")
        time.sleep(1)

    print(json.dumps({
        "ip": ip, "duration_s": round(time.time() - t0, 1),
        "pulls": pulls, "errors": errors, "events": events,
        "motion_started": sum(1 for e in events if e["state"]),
        "motion_cleared": sum(1 for e in events if not e["state"]),
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
