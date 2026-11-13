#!/usr/bin/env python3
"""tools/soak_log.py --report: a healthy run passes; a restart, too little memory, a fragmenting heap or a nearly full stack fail.

    python test/test_soak_log.py
"""
import csv
import io
import os
import sys
import tempfile
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import soak_log  # noqa: E402

passed = failed = 0


def check(ok, what):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what)


def run(rows):
    fd, path = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=soak_log.FIELDS, lineterminator=chr(10))
        w.writeheader()
        for r in rows:
            w.writerow(r)
    out = io.StringIO()
    with redirect_stdout(out):
        rc = soak_log.report(path)
    os.remove(path)
    return rc, out.getvalue()


def healthy(n=200, **change):
    rows = []
    for i in range(n):
        r = {"time": 1000 + 60 * i, "uptime": 50 + 60 * i, "heap": 175000, "heapMin": 160000, "heapBig": 110000 + (i % 3) * 500, "stackLoop": 3000,
             "reset": "power", "boxFps": 13.8, "state": "relaying"}
        for k, f in change.items():
            r[k] = f(i, r[k])
        rows.append(r)
    return rows


rc, text = run(healthy())
check(rc == 0 and "PASSED" in text, "a healthy run passes")
rows = healthy()
for r in rows[120:]:
    r["uptime"] = r["uptime"] - 7000
rc, text = run(rows)
check(rc == 1 and "1 restarts" in text, "a restart fails")
rc, text = run(healthy(heapMin=lambda i, v: 90000 if i > 150 else v))
check(rc == 1 and "lowest free memory" in text and "FAILED" in text, "too little free memory fails")
rc, text = run(healthy(heapBig=lambda i, v: 110000 - i * 300))
check(rc == 1 and "largest free block" in text, "a falling largest block fails")
rc, text = run(healthy(stackLoop=lambda i, v: 800))
check(rc == 1, "a nearly full stack fails")
rows = healthy()
rows.insert(50, {"time": 4000, "state": "unreachable: URLError"})
rc, text = run(rows)
check(rc == 0 and "1 times unreachable" in text, "a minute without an answer is counted, not a failure")

print("%d checks passed, %d failed" % (passed, failed))
print("ALL PASSED" if not failed else "SOME FAILED")
sys.exit(1 if failed else 0)
