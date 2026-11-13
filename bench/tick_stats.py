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
                                 headers=dict({"Content-Type": "application/json"}, **({"X-Fwb-Password": os.environ["FANCY2WLED_PAGE_PASSWORD"]}
                                                                                  if os.environ.get("FANCY2WLED_PAGE_PASSWORD") else {})) if body is not None else {})   # the setup page password, if set
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
    # newer firmware (after 0.4.12) also prints the relay's waits for the shared lock (see Lock in include/app.h); older firmware does not print them
    lock = re.compile(r"lockmax_us=(\d+) locksum_us=(\d+) lockslow=(\d+) lockby=(\S+) lockfor=(\S+) locktask=(\S+) lockheld=(\d+)")
    while len(rows) < windows + 1 and time.time() - t0 < timeout:
        line = ser.readline().decode("utf-8", "replace")
        m = pat.search(line)
        if m and int(m.group(1)) > 0:
            row = {"n": int(m.group(1)), "avg_us": float(m.group(2)), "max_us": int(m.group(3)), "reused": int(m.group(4))}
            k = lock.search(line)
            if k:
                row.update({"lock_max_us": int(k.group(1)), "lock_sum_us": int(k.group(2)), "lock_slow": int(k.group(3)), "lock_by": k.group(4), "lock_for": k.group(5),
                            "lock_task": k.group(6), "lock_slow_held": int(k.group(7))})
            rows.append(row)
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
    if all("lock_max_us" in r for r in rows):
        worst = max(rows, key=lambda r: r["lock_max_us"])
        res["lock_worst_us"] = worst["lock_max_us"]
        res["lock_wait_us_per_s"] = round(sum(r["lock_sum_us"] for r in rows) / (5.0 * len(rows)), 1)
        res["lock_slow_waits"] = sum(r["lock_slow"] for r in rows)
        res["lock_slow_waits_held"] = sum(r["lock_slow_held"] for r in rows)
        holders = {}
        for r in rows:
            if r["lock_slow"]:
                key = "%s (%s)" % (r["lock_by"], r["lock_task"])
                holders[key] = holders.get(key, 0) + 1
        res["lock_slow_holders"] = holders
        print("  relay waiting for the lock: worst %d us (held by %s in task %s, wanted by %s) | %.1f us per second | %d waits over 200 us, %d of them while another task"
              " held the lock (the rest: the lock was free and the relay was not running) | the windows' worst slow waits: %s"
              % (worst["lock_max_us"], worst["lock_by"], worst["lock_task"], worst["lock_for"], res["lock_wait_us_per_s"], res["lock_slow_waits"],
                 res["lock_slow_waits_held"], holders or "none"))
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        json.dump(res, open(a.out, "w", newline=chr(10)), indent=1)
        print("wrote", a.out)


if __name__ == "__main__":
    main()
