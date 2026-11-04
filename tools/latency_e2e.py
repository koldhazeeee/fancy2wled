#!/usr/bin/env python3
"""How long after the Sync Box sends a picture does a WLED strip show it? Measured on this computer's clock.

    python tools/latency_e2e.py fancy2wled.local                  # 40 seconds, every WLED target
    python tools/latency_e2e.py <relay IP> --seconds 60 --target "TV strip"

It listens to the Sync Box's UDP broadcast (port 19875) and, at the same time, to each WLED's live view (the WebSocket the WLED web page uses to draw
the strip), and finds the delay at which the brightness the box describes best matches the brightness the strip shows (the peak of their
cross-correlation). Nothing is sent to the box or the lights, and no setting is changed. A Govee light has no live view, so it is skipped.

You need something moving on the TV during the run (a video), otherwise there is nothing to line up. What the number includes: the Wi-Fi hop to this
computer, the relay's colour mixing, smoothing and reaction boost, the Wi-Fi hop to the WLED, and the WLED's own refresh. A negative number means the
strip is ahead of the broadcast (reaction boost looks forward). What it does not include: the box's own smoothing before it broadcasts.

Limits: a computer's Wi-Fi adapter in power-save may hold broadcasts back and deliver them in bursts (the relay's adapter does not), which would show
as a longer delay than the relay sees. Use an adapter with power-save off or a wired connection if you can. The WLED sends its live view about 20 times a second, so one
frame is 50 ms; the correlation over many frames is much finer than that, but it is an average, not a single frame's delay.
"""
import argparse
import base64
import json
import math
import os
import socket
import struct
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reference import protocol  # noqa: E402

GRID_MS = 5            # the common time grid the two signals are put on
MIN_LAG_MS = -150
MAX_LAG_MS = 600


def lum(rgb):
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def resample(samples, t0, t1, step_ms=GRID_MS):
    """samples: [(t seconds, value)] sorted by time -> values on a uniform grid from t0 to t1, holding the last value until the next one arrives."""
    out = []
    j = 0
    n = len(samples)
    t = t0
    while t < t1:
        while j + 1 < n and samples[j + 1][0] <= t:
            j += 1
        out.append(samples[j][1] if samples and samples[0][0] <= t else samples[0][1])
        t += step_ms / 1000.0
    return out


def highpass(x, tau_samples):
    """subtract a slow running average, so only the changes are left"""
    a = 1.0 / tau_samples
    avg = x[0]
    out = []
    for v in x:
        avg += a * (v - avg)
        out.append(v - avg)
    return out


def best_lag(box, led, min_lag_ms=MIN_LAG_MS, max_lag_ms=MAX_LAG_MS, step_ms=GRID_MS):
    """The delay (ms) of `led` behind `box` (both on the same uniform grid) with the highest correlation, and that correlation (-1..1).
    A negative delay means the led signal is ahead."""
    n = min(len(box), len(led))
    a = highpass(box[:n], 200)             # 1 s at 5 ms steps
    b = highpass(led[:n], 200)
    best = (-2.0, 0)
    for lag in range(int(min_lag_ms / step_ms), int(max_lag_ms / step_ms) + 1):
        sab = saa = sbb = 0.0
        lo = max(0, -lag)
        hi = min(n, n - lag)
        for i in range(lo, hi):
            x = a[i]
            y = b[i + lag]
            sab += x * y
            saa += x * x
            sbb += y * y
        if saa > 0 and sbb > 0:
            c = sab / math.sqrt(saa * sbb)
            if c > best[0]:
                best = (c, lag * step_ms)
    return best[1], best[0]


# ---------------------------------------------------------------- live data
def api(relay, path):
    with urllib.request.urlopen("http://%s%s" % (relay, path), timeout=10) as r:
        return json.loads(r.read().decode())


def ws_connect(ip):
    s = socket.create_connection((ip, 80), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (ip, key)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(1024)
    head, _, rest = buf.partition(b"\r\n\r\n")
    if b" 101 " not in head.split(b"\r\n")[0]:
        raise RuntimeError("the WLED did not accept a WebSocket (is 'live view' available in this WLED version?)")
    mask = os.urandom(4)
    data = b'{"lv":true}'
    s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
    return s, rest


def ws_frames(s, rest, deadline):
    buf = rest

    def need(n):
        nonlocal buf
        while len(buf) < n:
            s.settimeout(max(0.05, deadline - time.time()))
            chunk = s.recv(65536)
            if not chunk:
                raise EOFError
            buf += chunk
    while time.time() < deadline:
        try:
            need(2)
            op = buf[0] & 0x0F
            ln = buf[1] & 0x7F
            p = 2
            if ln == 126:
                need(4)
                ln = struct.unpack(">H", buf[2:4])[0]
                p = 4
            elif ln == 127:
                need(10)
                ln = struct.unpack(">Q", buf[2:10])[0]
                p = 10
            need(p + ln)
            payload = buf[p:p + ln]
            buf = buf[p + ln:]
            if op == 2:
                yield time.time(), payload
        except (socket.timeout, EOFError):
            break


def record_strip(ip, secs, out):
    try:
        s, rest = ws_connect(ip)
        for t, p in ws_frames(s, rest, time.time() + secs):
            if p[:1] == b"L" and len(p) > 5:
                body = p[2:] if (len(p) - 2) % 3 == 0 else p[1:]
                out.append((t, [lum(body[i * 3:i * 3 + 3]) for i in range(len(body) // 3)]))
        s.close()
    except Exception as e:
        out.append(("error", str(e)))


def record_box(secs, out):
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.bind(("", 19875))
    end = time.time() + secs
    while time.time() < end:
        rx.settimeout(max(0.05, end - time.time()))
        try:
            d, _ = rx.recvfrom(2048)
        except socket.timeout:
            break
        z = protocol.decode_frame(d)
        if z:
            out.append((time.time(), [lum(c) for c in z]))
    rx.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("relay", help="address of the relay (AtomS3), e.g. fancy2wled.local")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--target", help="only this target (its name)")
    a = ap.parse_args()

    cfg = api(a.relay, "/api/config")
    st = api(a.relay, "/api/status")
    ips = {t["name"]: t.get("ip") for t in st["targets"]}
    targets = [t for t in cfg["targets"] if t.get("kind", "wled") == "wled" and t.get("enabled", True) and (not a.target or t["name"] == a.target)]
    if not targets:
        sys.exit("no WLED target to measure")
    box, strips, threads = [], {t["name"]: [] for t in targets}, []
    print("recording %.0f s: the Sync Box broadcast and the live view of %s ... (play something moving)" % (a.seconds, ", ".join(t["name"] for t in targets)))
    threads.append(threading.Thread(target=record_box, args=(a.seconds, box)))
    for t in targets:
        if ips.get(t["name"]):
            threads.append(threading.Thread(target=record_strip, args=(ips[t["name"]], a.seconds, strips[t["name"]])))
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    if len(box) < 20:
        sys.exit("heard only %d frames from the Sync Box on this computer (is it on the same network, and is port 19875 free?)" % len(box))
    print("%d box frames, %s strip frames" % (len(box), ", ".join("%s %d" % (n, len([x for x in v if x[0] != "error"])) for n, v in strips.items())))
    for t in targets:
        rows = strips[t["name"]]
        if rows and rows[0][0] == "error":
            print("%s: %s" % (t["name"], rows[0][1]))
            continue
        if len(rows) < 20:
            print("%s: too few live view frames" % t["name"])
            continue
        usage = t["usage"]
        tot = sum(usage) or 1.0
        s_box = [(ts, sum(usage[z] * v[z] for z in range(24)) / tot) for ts, v in box]
        first, count = t["wled"]["start"], t["wled"]["count"]
        s_led = []
        for ts, v in rows:
            seg = v[first:first + count] if len(v) >= first + count else v
            s_led.append((ts, sum(seg) / len(seg)))
        t0 = max(s_box[0][0], s_led[0][0]) + 1.0
        t1 = min(s_box[-1][0], s_led[-1][0]) - 1.0
        if t1 - t0 < 10:
            print("%s: not enough overlapping time" % t["name"])
            continue
        xb, xl = resample(s_box, t0, t1), resample(s_led, t0, t1)
        swing = max(xb) - min(xb)
        lag, corr = best_lag(xb, xl)
        thirds = []
        k = len(xb) // 3
        for q in range(3):
            lg, cr = best_lag(xb[q * k:(q + 1) * k], xl[q * k:(q + 1) * k])
            thirds.append(lg)
        print("\n%s" % t["name"])
        print("  the strip follows the broadcast after about %d ms  (correlation %.2f; in the three thirds of the run: %s ms)" % (lag, corr, ", ".join(str(x) for x in thirds)))
        if swing < 15:
            print("  but the picture hardly changed during the run (brightness swing %.0f of 255): play something moving and run it again" % swing)
        elif corr < 0.5:
            print("  the match is weak: treat this number with caution")


if __name__ == "__main__":
    main()
