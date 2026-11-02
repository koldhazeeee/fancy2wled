#!/usr/bin/env python3
"""Record the Sync Box's UDP broadcasts to a file, as a reference for when a firmware update changes them.

    python tools/capture_frames.py                              # 2 minutes of the sync stream (and the Tuya announcements)
    python tools/capture_frames.py --seconds 600 --note "bright movie scene" --box-firmware "main 3.1.0 / mcu 2.0.14"

Files go to captures/ (next to tools/), which git ignores. Each line of a file is one JSON object; the first line describes the
capture, the others are datagrams:  {"t": seconds since the start, "port": 19875, "src": "ip:port", "len": 88, "hex": "55aa5a..."}

Keep a capture from before every box firmware update you take, and one after: `tools/check_capture.py` compares them and tells you what changed.
Captures contain your network addresses (the box's IP and the Tuya announcement): never commit or post them.

Ports: 19875 is the sync stream; 6667 and 6666 are Tuya's device announcements (they show the box is alive and how it describes itself;
the sync stream does not depend on them). Nothing is sent: this only listens. Timing is as this computer's Wi-Fi delivers it
(a Wi-Fi adapter in power-save delivers broadcasts in bursts), so use the relay's own debug statistics to study timing.
"""
import argparse
import datetime
import json
import os
import select
import socket
import sys
import time

DEFAULT_PORTS = (19875, 6667, 6666)
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--ports", default=",".join(str(p) for p in DEFAULT_PORTS), help="UDP ports to listen on (default %(default)s)")
    ap.add_argument("--from-ip", help="only keep datagrams from this address (the box)")
    ap.add_argument("--out", help="file to write (default captures/box-<date time>[-<label>].jsonl)")
    ap.add_argument("--label", default="", help="short word for the file name, e.g. before-update")
    ap.add_argument("--note", default="", help="what was on screen / what you did (stored in the file)")
    ap.add_argument("--box-firmware", default="", help="the box's firmware versions, from the Smart Life / Tuya app (stored in the file)")
    a = ap.parse_args()

    ports = [int(p) for p in a.ports.split(",") if p.strip()]
    started = datetime.datetime.now()
    out = a.out
    if not out:
        folder = os.path.join(HERE, "..", "captures")
        os.makedirs(folder, exist_ok=True)
        label = ("-" + a.label) if a.label else ""
        out = os.path.join(folder, "box-%s%s.jsonl" % (started.strftime("%Y%m%d-%H%M%S"), label))
    socks = {}
    for p in ports:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        try:
            s.bind(("", p))
        except OSError as e:
            print("cannot listen on UDP %d: %s (another program may be using it)" % (p, e))
            continue
        socks[s] = p
    if not socks:
        return 1
    meta = {"meta": {"tool": "capture_frames.py 1", "started": started.isoformat(timespec="seconds"), "seconds": a.seconds,
                     "ports": sorted(socks.values()), "from_ip": a.from_ip or "", "note": a.note, "box_firmware": a.box_firmware,
                     "host": socket.gethostname()}}
    counts = {p: 0 for p in socks.values()}
    sizes = {}
    t0 = time.perf_counter()
    print("listening on UDP %s for %.0f s -> %s" % (sorted(socks.values()), a.seconds, os.path.normpath(out)))
    with open(out, "w", newline="\n") as f:
        f.write(json.dumps(meta) + "\n")
        while True:
            left = a.seconds - (time.perf_counter() - t0)
            if left <= 0:
                break
            ready, _, _ = select.select(list(socks), [], [], min(0.5, left))
            for s in ready:
                data, addr = s.recvfrom(4096)
                t = time.perf_counter() - t0
                if a.from_ip and addr[0] != a.from_ip:
                    continue
                port = socks[s]
                counts[port] += 1
                sizes.setdefault(port, {}).setdefault(len(data), 0)
                sizes[port][len(data)] += 1
                f.write(json.dumps({"t": round(t, 4), "port": port, "src": "%s:%d" % addr, "len": len(data), "hex": data.hex()}) + "\n")
            f.flush()
    for p in sorted(counts):
        print("  UDP %5d: %6d datagrams  %s" % (p, counts[p], ("sizes " + ", ".join("%d x%d" % kv for kv in sorted(sizes[p].items()))) if counts[p] else ""))
    print("saved %s" % os.path.normpath(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
