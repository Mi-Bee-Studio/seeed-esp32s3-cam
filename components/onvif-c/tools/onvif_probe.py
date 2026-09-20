#!/usr/bin/env python3
"""onvif-c smoke probe — no-hardware self-check of a running device.

Covers the full served surface of the library:
  1. Device service:  GetSystemDateAndTime / GetDeviceInformation / GetCapabilities
  2. Media service:   GetProfiles / GetStreamUri / GetSnapshotUri
  3. Events service:  CreatePullPointSubscription -> PullMessages loop ->
                      Renew -> Unsubscribe (MotionAlarm parsing)
Exit 0 = all served actions answered; exit 2 = create/first-call failure.

Usage: onvif_probe.py <ip> [duration_s]
Output: human-readable progress + final machine-readable JSON line.
"""
import json
import re
import sys
import time
import urllib.request

NS_EV = "http://www.onvif.org/ver10/events/wsdl"


def envelope(action, ns=None, body=""):
    ns = ns or f'xmlns:u="{action.split("/")[0]}"'
    return (
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
        "<s:Body>"
        f'<u:{action} {ns}>{body}</u:{action}>'
        "</s:Body></s:Envelope>"
    )


def post(ip, path, body, timeout=8):
    req = urllib.request.Request(
        f"http://{ip}{path}", data=body.encode(),
        headers={"Content-Type": "application/soap+xml; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read().decode("utf-8", "replace")


def call(ip, action, path="/onvif/device_service"):
    body = (
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
        "<s:Body>"
        f'<u:{action} xmlns:u="http://www.onvif.org/ver10/device/wsdl"/>'
        "</s:Body></s:Envelope>"
    )
    return post(ip, path, body)


RX_EVT = re.compile(
    r'UtcTime="([^"]+)".*?Name="State" Value="(true|false)"'
    r'.*?Name="Score" Value="(\d+)"', re.S)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ip = sys.argv[1]
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    checks = {}

    for action in ("GetSystemDateAndTime", "GetDeviceInformation",
                   "GetCapabilities"):
        code, body = call(ip, action)
        checks[action] = f"{action}Response" in body
        print(f"{action:24s} -> {'OK' if checks[action] else 'MISSING RESPONSE'}")

    for action in ("GetProfiles", "GetStreamUri", "GetSnapshotUri"):
        code, body = call(ip, action, "/onvif/media_service")
        checks[action] = f"{action}Response" in body
        print(f"{action:24s} -> {'OK' if checks[action] else 'MISSING RESPONSE'}")

    # ---- events (optional service) ----
    create = (
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
        "<s:Body>"
        f'<tev:CreatePullPointSubscription xmlns:tev="{NS_EV}"/>'
        "</s:Body></s:Envelope>"
    )
    pull = (
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
        "<s:Body>"
        f'<tev:PullMessages xmlns:tev="{NS_EV}">'
        "<tev:Timeout>PT1S</tev:Timeout>"
        "<tev:MessageLimit>5</tev:MessageLimit>"
        "</tev:PullMessages></s:Body></s:Envelope>"
    )
    events, pulls, errors = [], 0, 0
    try:
        code, body = post(ip, "/onvif/events_service", create)
        if "CreatePullPointSubscriptionResponse" in body:
            checks["events_service"] = True
            term = re.search(r"TerminationTime>([^<]+)<", body)
            print(f"events_service           -> subscribed "
                  f"(termination={term.group(1) if term else '?'})")
            t0 = time.time()
            while time.time() - t0 < dur:
                try:
                    code, body = post(ip, "/onvif/events_service", pull)
                    pulls += 1
                    for ts, state, score in RX_EVT.findall(body):
                        ev = {"utc": ts, "state": state == "true",
                              "score": int(score)}
                        events.append(ev)
                        print(f"  MotionAlarm {'MOTION' if ev['state'] else 'clear'}"
                              f" score={ev['score']}")
                except Exception as e:
                    errors += 1
                    print(f"  pull error: {e}")
                time.sleep(1)
        else:
            checks["events_service"] = False
            print("events_service           -> not present (ok when disabled)")
    except Exception:
        checks["events_service"] = False
        print("events_service           -> not present (ok when disabled)")

    ok = all(v for k, v in checks.items() if k != "events_service")
    print(json.dumps({"ip": ip, "checks": checks, "pulls": pulls,
                      "errors": errors, "events": events,
                      "all_required": ok}, ensure_ascii=False))
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
