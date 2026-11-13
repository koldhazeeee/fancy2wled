#!/usr/bin/env python3
"""A long-run (soak) check of the relay's health: memory, stack, restarts, while it keeps working.

    python tools/soak_log.py fancy2wled.local --hours 48 --out captures/soak.csv
    python tools/soak_log.py --report captures/soak.csv

Every minute it reads /api/status and appends one line to a CSV: uptime, free memory, the lowest free memory since the start, the largest free block (a value that keeps
falling over days means the memory is fragmenting), the relay task's unused stack, the restart reason and the Sync Box's frame rate. It only reads; nothing changes on the
board. Keep the CSV in a git-ignored folder (captures/): it is not private, but it is not part of the project either.

--report says whether the run passed: no restart (the uptime never went back), the lowest free memory stayed above 100 KB, the largest free block did not trend down by more
than 10 % between the first and the last quarter of the run, and the stack kept at least 1 KB unused.
"""
import argparse
import csv
import json
import os
import sys
import time
import urllib.request

FIELDS = ["time", "uptime", "heap", "heapMin", "heapBig", "stackLoop", "reset", "boxFps", "state"]


def status(relay):
    with urllib.request.urlopen("http://%s/api/status" % relay, timeout=10) as r:
        return json.loads(r.read().decode())


def row(d):
    return {"time": int(time.time()), "uptime": d.get("uptime"), "heap": d.get("heap"), "heapMin": d.get("heapMin"), "heapBig": d.get("heapBig"),
            "stackLoop": d.get("stackLoop"), "reset": (d.get("reset") or {}).get("reason"), "boxFps": (d.get("box") or {}).get("fps"), "state": d.get("state")}


def log(relay, hours, out, every):
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    new = not os.path.exists(out)
    end = time.time() + hours * 3600
    with open(out, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, lineterminator=chr(10))
        if new:
            w.writeheader()
        while time.time() < end:
            try:
                r = row(status(relay))
            except Exception as e:                          # the board may be restarting, or Wi-Fi dropped: noted, and the next minute tries again
                r = {"time": int(time.time()), "state": "unreachable: %s" % type(e).__name__}
            w.writerow(r)
            f.flush()
            time.sleep(every)


def report(path):
    rows = [r for r in csv.DictReader(open(path)) if r.get("uptime")]
    if len(rows) < 8:
        sys.exit("too few readings (%d) for a report" % len(rows))
    num = lambda r, k: int(float(r[k])) if r.get(k) not in (None, "") else None   # noqa: E731
    hours = (num(rows[-1], "time") - num(rows[0], "time")) / 3600.0
    restarts = sum(1 for a, b in zip(rows, rows[1:]) if num(b, "uptime") < num(a, "uptime"))
    heap_min = min(num(r, "heapMin") or num(r, "heap") for r in rows)
    q = max(1, len(rows) // 4)
    big = [num(r, "heapBig") for r in rows if num(r, "heapBig") is not None]
    big_first, big_last = (sum(big[:q]) / q, sum(big[-q:]) / q) if len(big) >= 2 * q else (None, None)
    stack = min((num(r, "stackLoop") for r in rows if num(r, "stackLoop")), default=None)
    unreachable = sum(1 for r in csv.DictReader(open(path)) if (r.get("state") or "").startswith("unreachable"))
    checks = [("no restart", restarts == 0, "%d restarts" % restarts),
              ("lowest free memory above 100 KB", heap_min > 100000, "%d bytes" % heap_min),
              ("largest free block not falling by more than 10 %", big_first is None or big_last >= 0.9 * big_first,
               "first quarter %s, last quarter %s" % (big_first and int(big_first), big_last and int(big_last))),
              ("relay stack keeps 1 KB unused", stack is None or stack >= 1024, "%s bytes" % stack)]
    print("%s: %.1f hours, %d readings, %d times unreachable" % (path, hours, len(rows), unreachable))
    for name, ok, detail in checks:
        print("  %-52s %s (%s)" % (name, "ok" if ok else "FAILED", detail))
    print("PASSED" if all(ok for _, ok, _ in checks) else "FAILED")
    return 0 if all(ok for _, ok, _ in checks) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("relay", nargs="?")
    ap.add_argument("--hours", type=float, default=48)
    ap.add_argument("--every", type=float, default=60, help="seconds between readings")
    ap.add_argument("--out", default=os.path.join("captures", "soak.csv"))
    ap.add_argument("--report", metavar="CSV")
    a = ap.parse_args()
    if a.report:
        sys.exit(report(a.report))
    if not a.relay:
        ap.error("give the relay's address, or --report CSV")
    log(a.relay, a.hours, a.out, a.every)
    sys.exit(report(a.out))


if __name__ == "__main__":
    main()
