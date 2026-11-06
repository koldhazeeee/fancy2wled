#!/usr/bin/env python3
"""Time the colour work of the relay's real output ticks on a board running the atoms3-tickstat firmware (the normal relay plus a counter, printed over USB serial).

    python bench/tick_stats.py --label after --windows 12 --out bench/results/tick-after.json [--smoothing 0.5]

It reads the board's own TICKSTAT lines (one per five seconds: ticks, average and worst microseconds for the colour work of one target's tick, and how many ticks
reused the wanted colours of the previous one) while the relay keeps working on whatever your Sync Box sends. With --smoothing it sets "Smoothing over time" for the
measurement and puts your value back afterwards (even if interrupted). Only the setup page's own settings are touched.
"""
import argparse
import json
import os
import re
import statistics
import sys
import time
import urllib.request

import serial
from serial.tools import list_ports

RELAY = "fancy2wled.local"


def api(path, body=None):
    req = urllib.request.Request("http://%s%s" % (RELAY, path), data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def find_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) == (0x303A, 0x1001):
            return p.device
    return None


def collect(windows, timeout):
    port = find_port()
    if not port:
        sys.exit("no ESP32-S3 serial port found")
    ser = serial.Serial(port, 115200, timeout=1.0)
    rows = []
    t0 = time.time()
    pat = re.compile(r"TICKSTAT n=(\d+) avg_us=([\d.]+) max_us=(\d+) reused=(\d+)")
    while len(rows) < windows + 1 and time.time() - t0 < timeout:
        m = pat.search(ser.readline().decode("utf-8", "replace"))
        if m and int(m.group(1)) > 0:
            rows.append({"n": int(m.group(1)), "avg_us": float(m.group(2)), "max_us": int(m.group(3)), "reused": int(m.group(4))})
    ser.close()
    return rows[1:]                                    # the first window may be partial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="run")
    ap.add_argument("--windows", type=int, default=12)
    ap.add_argument("--out", default=None)
    ap.add_argument("--smoothing", type=float, default=None, help="set Smoothing over time (0..0.95) while measuring")
    a = ap.parse_args()
    saved = None
    if a.smoothing is not None:
        saved = {"smoothing": api("/api/config")["look"]["smoothing"]}
        api("/api/config", {"look": {"smoothing": a.smoothing}})
        print("Smoothing over time set to %s for the measurement (was %s)" % (a.smoothing, saved["smoothing"]))
        time.sleep(1.0)
    try:
        rows = collect(a.windows, a.windows * 5 + 60)
    finally:
        if saved:
            api("/api/config", {"look": saved})
            print("Smoothing over time put back to", saved["smoothing"])
    if len(rows) < 3:
        sys.exit("too few windows arrived (%d): is this the atoms3-tickstat firmware, and is the Sync Box sending?" % len(rows))
    ticks = sum(r["n"] for r in rows)
    wavg = sum(r["avg_us"] * r["n"] for r in rows) / ticks
    res = {"label": a.label, "smoothing": a.smoothing, "windows": len(rows), "ticks": ticks, "avg_us_weighted": round(wavg, 1),
           "avg_us_median_of_windows": round(statistics.median(r["avg_us"] for r in rows), 1), "worst_us": max(r["max_us"] for r in rows),
           "reused_share": round(sum(r["reused"] for r in rows) / ticks, 3), "per_window": rows}
    print("%s: %d ticks in %d windows | average %.1f us per tick (median of windows %.1f) | worst %d us | %.0f %% of ticks reused the previous wanted colours"
          % (a.label, ticks, len(rows), wavg, res["avg_us_median_of_windows"], res["worst_us"], 100 * res["reused_share"]))
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        json.dump(res, open(a.out, "w", newline=chr(10)), indent=1)
        print("wrote", a.out)


if __name__ == "__main__":
    main()
