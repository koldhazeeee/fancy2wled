#!/usr/bin/env python3
"""Read the benchmark results from a board running the atoms3-bench firmware (pio run -e atoms3-bench -t upload) over USB serial.

    python bench/run_board.py --label before --out bench/results/board-before.json [--port COM3] [--cycles 3]

The board repeats the whole set forever; this reads --cycles complete passes and keeps the fastest time of each case across them. Needs pyserial.
"""
import argparse
import json
import os
import sys
import time

import serial
from serial.tools import list_ports


def find_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) == (0x303A, 0x1001):          # the ESP32-S3's built-in USB serial
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="board")
    ap.add_argument("--out", default=None)
    ap.add_argument("--port", default=None)
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=420.0)
    a = ap.parse_args()
    port = a.port or find_port()
    if not port:
        sys.exit("no ESP32-S3 serial port found")
    ser = serial.Serial(port, 115200, timeout=1.0)
    cycles = []
    cur = None
    cpu = None
    t0 = time.time()
    while len(cycles) < a.cycles and time.time() - t0 < a.timeout:
        line = ser.readline().decode("utf-8", "replace").strip()
        if line.startswith("BENCH_BEGIN"):
            cur = {}
            cpu = line.split("cpu_mhz=")[-1]
        elif line.startswith("BENCH_END"):
            if cur:
                cycles.append(cur)
                print("pass %d done (%d cases)" % (len(cycles), len(cur)))
            cur = None
        elif line.startswith("BENCH ") and cur is not None:
            try:
                d = json.loads(line[6:])
                cur[d["n"]] = d
            except Exception:
                cur = None                                 # a garbled line: throw this pass away
    ser.close()
    if len(cycles) < a.cycles:
        sys.exit("only %d complete passes arrived" % len(cycles))
    best = {}
    for cyc in cycles:
        for k, v in cyc.items():
            if k not in best or v["min"] < best[k]["min"]:
                best[k] = {"min": v["min"], "med": v["med"], "iters": v["iters"]}
    print("%-24s %14s %14s" % ("case", "fastest (ns)", "median (ns)"))
    for k, v in best.items():
        print("%-24s %14.1f %14.1f" % (k, v["min"], v["med"]))
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        json.dump({"label": a.label, "platform": "esp32-s3 @ %s MHz" % cpu, "unit": "ns per operation", "passes": len(cycles), "cases": best}, open(a.out, "w", newline=chr(10)), indent=1)
        print("wrote", a.out)


if __name__ == "__main__":
    main()
