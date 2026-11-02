#!/usr/bin/env python3
"""Measure how fast the relay reacts, without a Sync Box or WLED.

This PC pretends to be both: it sends the relay Sync Box frames shaped like the real box's output (the box smooths its own
colours, 40 % of the way to the target each frame) and listens for the DDP packets the relay streams back, then reports how long
the lights take to follow a step between black and red.

While it runs, the script temporarily points the relay at this PC (the relay then ignores the real box and streams to this PC
instead of WLED, so your lights go quiet for the test) and puts your settings back afterwards, even if you press Ctrl+C.

    python tools/latency_test.py fancy2wled.local                 # 30 seconds
    python tools/latency_test.py <relay IP> --seconds 60 --led 100

Try it with "Reaction boost" at 0 and at 100 (setup page, Look) to see what the boost does. Times are measured on this PC, so
they include both Wi-Fi hops; the script sends a trickle of tiny packets so this PC's Wi-Fi radio does not go to sleep.
"""
import argparse
import os
import socket
import statistics as st
import sys
import threading
import time
import json
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reference import protocol  # noqa: E402

PERIOD = 0.07          # the real box sends about 14 frames a second
STEP_EVERY = 40        # frames between target changes
AMP = 200


def api(relay, path, body=None):
    req = urllib.request.Request("http://%s%s" % (relay, path), data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("relay", help="IP address of the relay (AtomS3)")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--led", type=int, default=100, help="which LED to watch (one that is lined up with the TV)")
    ap.add_argument("--raw", action="store_true", help="send un-smoothed steps instead of the box's smoothed output")
    ap.add_argument("--no-configure", action="store_true", help="do not touch the relay's settings (you pointed it at this PC yourself)")
    a = ap.parse_args()

    saved = None
    if not a.no_configure:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((a.relay, 80))
        me = probe.getsockname()[0]
        cfg = api(a.relay, "/api/config")
        saved = {"box": {"host": cfg["box"]["host"]}, "wled": {"host": cfg["wled"]["host"]}}
        print("pointing the relay at this PC (%s); it will be put back to WLED %r afterwards" % (me, saved["wled"]["host"]))
        api(a.relay, "/api/config", {"box": {"host": me}, "wled": {"host": me}})
    try:
        return run(a)
    finally:
        if saved:
            api(a.relay, "/api/config", saved)
            print("relay settings restored")


def run(a):
    ddp = []
    stop = threading.Event()
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    rx.bind(("", 4048))
    rx.settimeout(0.2)

    def listen():
        while not stop.is_set():
            try:
                data, _ = rx.recvfrom(2048)
            except socket.timeout:
                continue
            t = time.perf_counter()
            if len(data) > 10 and data[2] == 0x0B:             # DDP, RGB
                first = int.from_bytes(data[4:8], "big") // 3    # index of the first LED in this packet
                n = (len(data) - 10) // 3
                if first <= a.led < first + n:
                    ddp.append((t, data[10 + 3 * (a.led - first)]))

    def keep_awake():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        while not stop.is_set():
            s.sendto(b"x", (a.relay, 9))
            time.sleep(0.015)

    threading.Thread(target=listen, daemon=True).start()
    threading.Thread(target=keep_awake, daemon=True).start()

    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    steps = []                                   # (time sent, new target)
    y = 0.0
    target = 0.0
    n = 0
    t0 = time.perf_counter()
    nxt = t0
    time.sleep(0.3)
    print("sending frames to %s for %.0f s, watching LED %d ..." % (a.relay, a.seconds, a.led))
    while time.perf_counter() - t0 < a.seconds:
        now = time.perf_counter()
        if n % STEP_EVERY == 10:
            target = AMP if target == 0 else 0
            steps.append((now, target))
        y = float(target) if a.raw else 0.6 * y + 0.4 * target
        tx.sendto(protocol.encode_frame([(int(round(y)), 0, 0)] * 24), (a.relay, protocol.PORT))
        n += 1
        nxt += PERIOD
        while time.perf_counter() < nxt - 0.002:
            time.sleep(0.001)
        while time.perf_counter() < nxt:
            pass
    time.sleep(0.6)
    stop.set()

    if not ddp:
        print("no DDP packets arrived. Is the relay's WLED address set to this PC, and its Sync Box address too?")
        return 1
    results = []
    for k, (ts, tgt) in enumerate(steps):
        t_end = steps[k + 1][0] if k + 1 < len(steps) else ts + 2.5
        before = [p for p in ddp if ts - 3.0 <= p[0] < ts - 0.01]
        seg = [(p[0] - ts, p[1]) for p in ddp if ts <= p[0] < t_end]
        if not before or not seg:
            continue
        r0 = before[-1][1]
        rising = tgt > 0

        def cross(frac):
            level = r0 + (AMP * frac if rising else -AMP * frac)
            for t, r in seg:
                if (rising and r >= level) or (not rising and r <= level):
                    return t * 1000
            return None
        first = next((t * 1000 for t, r in seg if abs(r - r0) >= 3), None)
        results.append((first, cross(0.5), cross(0.9), cross(0.98)))
    print("\n%d steps measured. Milliseconds after the first frame that carries the change was sent:\n" % len(results))
    for i, name in enumerate(("first visible change", "50 % of the way", "90 % of the way", "98 % of the way")):
        v = [r[i] for r in results if r[i] is not None]
        if v:
            print("  %-22s median %5.0f ms   (best %4.0f, worst %4.0f)" % (name, st.median(v), min(v), max(v)))
    t = [p[0] for p in ddp]
    dur = t[-1] - t[0]
    print("\nThe relay sent %.1f packets/s on average (a still picture sends only a keep-alive every second)." % (len(t) / dur))
    return 0


if __name__ == "__main__":
    sys.exit(main())
