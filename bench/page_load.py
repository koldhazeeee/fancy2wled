#!/usr/bin/env python3
"""Make the requests the setup page makes while it is open, without a browser, to measure what an open page costs the relay.

    python bench/page_load.py fancy2wled.local --seconds 70 [--editing]

It asks for the live colours twice a second (GET /api/zones, as the picture map does while it is on screen) and for the status every 2.5 s (GET /api/status).
With --editing it also asks for the picture map of every target once a second (POST /api/usage, which the page sends after each edit on the Layout view), which
is the heaviest thing the page does regularly. It only reads: nothing on the board changes. Run it next to bench/tick_stats.py on the atoms3-tickstat firmware.
"""
import argparse
import json
import threading
import time
import urllib.request


def get(relay, path):
    with urllib.request.urlopen("http://%s%s" % (relay, path), timeout=5) as r:
        r.read()


def post(relay, path, body):
    req = urllib.request.Request("http://%s%s" % (relay, path), data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        r.read()


def every(period, stop, fn, counts, name):
    nxt = time.time()
    while not stop.is_set():
        try:
            fn()
            counts[name] = counts.get(name, 0) + 1
        except Exception:                                   # a busy board may refuse one; the page would just try again later
            counts[name + " failed"] = counts.get(name + " failed", 0) + 1
        nxt += period
        stop.wait(max(0.0, nxt - time.time()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("relay")
    ap.add_argument("--seconds", type=float, default=70)
    ap.add_argument("--editing", action="store_true", help="also POST /api/usage for every target once a second")
    a = ap.parse_args()
    targets = len(json.loads(urllib.request.urlopen("http://%s/api/config" % a.relay, timeout=5).read().decode()).get("targets", [])) or 1
    stop = threading.Event()
    counts = {}
    jobs = [(0.5, lambda: get(a.relay, "/api/zones"), "zones"), (2.5, lambda: get(a.relay, "/api/status"), "status")]
    if a.editing:
        jobs.append((1.0, lambda: [post(a.relay, "/api/usage", {"target": t}) for t in range(targets)], "usage"))
    threads = [threading.Thread(target=every, args=(p, stop, fn, counts, n), daemon=True) for p, fn, n in jobs]
    for t in threads:
        t.start()
    time.sleep(a.seconds)
    stop.set()
    for t in threads:
        t.join(6)
    print("requests made:", counts)


if __name__ == "__main__":
    main()
