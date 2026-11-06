#!/usr/bin/env python3
"""A fingerprint of everything a relay sends to a WLED for a fixed set of pictures, to show that two firmware versions produce the same output.

    python tools/output_fingerprint.py fancy2wled.local --out before.json
    (flash the other firmware)
    python tools/output_fingerprint.py fancy2wled.local --out after.json
    python tools/output_fingerprint.py --compare before.json after.json

It pretends to be the Sync Box and the first target's WLED: it sends the relay a series of fixed pictures (Sync Box frames) and keeps the complete LED frame (DDP) the
relay sends back for each. To make it exact it temporarily sets Smoothing over time 0, Reaction boost 0 and Motion smoothing off, runs once with the finishing stage
(Black cut-off, Hold steady, Dither) off and once with all three on, and puts your settings back afterwards, even if interrupted. Your lights on the first target go quiet
for about a minute while it runs.
"""
import argparse
import json
import os
import random
import socket
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reference import protocol  # noqa: E402


def api(relay, path, body=None):
    req = urllib.request.Request("http://%s%s" % (relay, path), data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def pictures():
    rnd = random.Random(2026)
    out = []
    for k in range(18):
        if k == 0:
            out.append([(0, 0, 0)] * 24)
        elif k == 1:
            out.append([(255, 255, 255)] * 24)
        elif k == 2:
            out.append([(3, 2, 1)] * 24)                                         # nearly black
        elif k < 8:
            out.append([tuple(rnd.randrange(256) for _ in range(3)) for _ in range(24)])
        elif k < 13:
            base = [rnd.randrange(30, 220) for _ in range(3)]
            out.append([tuple(max(0, min(255, b + rnd.randrange(-25, 26))) for b in base) for _ in range(24)])
        else:
            out.append([tuple(rnd.randrange(0, 12) for _ in range(3)) for _ in range(24)])   # dark noise
    return out


def run(relay):
    cfg = api(relay, "/api/config")
    saved = {"box": {"host": cfg["box"]["host"]}, "wled": {"host": cfg["wled"]["host"]}, "look": dict(cfg["look"])}
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((socket.gethostbyname(relay), 80))
    me = probe.getsockname()[0]
    last = {"data": None}
    stop = threading.Event()
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    rx.bind(("", 4048))
    rx.settimeout(0.2)
    frames = {}                                           # byte offset -> payload of the newest packet at that offset

    def listen():
        while not stop.is_set():
            try:
                data, _ = rx.recvfrom(2048)
            except socket.timeout:
                continue
            if len(data) > 10 and data[2] == 0x0B:
                frames[int.from_bytes(data[4:8], "big")] = bytes(data[10:])

    def keep_awake():
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        while not stop.is_set():
            s.sendto(b"x", (socket.gethostbyname(relay), 9))
            time.sleep(0.015)

    threading.Thread(target=listen, daemon=True).start()
    threading.Thread(target=keep_awake, daemon=True).start()
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ip = socket.gethostbyname(relay)
    result = {"version": None, "runs": {}}
    base = {"brightness": 1.0, "gamma": 1.0, "saturation": 1.0, "smoothing": 0.0, "boost": 0.0, "motion": 0, "boxSpeed": 100, "smoothOut": 100,
            "blackCut": 0, "deadband": 0.0, "dither": False}
    try:
        result["version"] = api(relay, "/api/status")["ver"]
        api(relay, "/api/config", {"box": {"host": me}, "wled": {"host": me}})
        for name, extra in (("stage off", {}), ("stage on", {"blackCut": 6, "deadband": 3.0, "dither": True})):
            api(relay, "/api/config", {"look": dict(base, **extra)})
            time.sleep(0.8)
            shots = []
            for pic in pictures():
                frames.clear()
                t0 = time.perf_counter()
                nxt = t0
                while time.perf_counter() - t0 < 1.4:
                    tx.sendto(protocol.encode_frame(pic), (ip, protocol.PORT))
                    nxt += 0.07
                    while time.perf_counter() < nxt:
                        time.sleep(0.001)
                time.sleep(0.35)
                shots.append("".join(frames[k].hex() for k in sorted(frames)))
            result["runs"][name] = shots
    finally:
        stop.set()
        api(relay, "/api/config", saved)
        after = api(relay, "/api/config")
        ok = after["box"]["host"] == saved["box"]["host"] and after["wled"]["host"] == saved["wled"]["host"] and all(after["look"].get(k) == v for k, v in saved["look"].items())
        print("your settings are back:", "yes" if ok else "NO - CHECK THE SETUP PAGE")
    return result


def compare(a, b):
    ra, rb = json.load(open(a)), json.load(open(b))
    print("comparing %s (%s) with %s (%s)" % (a, ra["version"], b, rb["version"]))
    bad = 0
    for name in ra["runs"]:
        for i, (x, y) in enumerate(zip(ra["runs"][name], rb["runs"].get(name, []))):
            same = x == y and len(x) > 0
            if not same:
                bad += 1
                print("  DIFFERENT: %s, picture %d (%d vs %d bytes)" % (name, i, len(x) // 2, len(y) // 2))
        print("  %s: %d pictures compared" % (name, len(ra["runs"][name])))
    print("identical output" if bad == 0 else "%d pictures differ" % bad)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("relay", nargs="?")
    ap.add_argument("--out")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    a = ap.parse_args()
    if a.compare:
        sys.exit(compare(*a.compare))
    if not a.relay or not a.out:
        ap.error("give the relay's address and --out, or --compare A B")
    res = run(a.relay)
    json.dump(res, open(a.out, "w", newline=chr(10)))
    print("wrote", a.out, "(%d + %d pictures, firmware %s)" % (len(res["runs"].get("stage off", [])), len(res["runs"].get("stage on", [])), res["version"]))


if __name__ == "__main__":
    main()
