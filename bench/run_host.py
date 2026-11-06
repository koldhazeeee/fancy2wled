#!/usr/bin/env python3
"""Build and run the benchmark cases on this computer.

    python bench/run_host.py --label before --out bench/results/host-before.json
    python bench/run_host.py --runs 3 ...        # run the whole set several times, keep the fastest of each case

Compiles bench/bench_host.cpp with the core (src/core.cpp) using zig's C++ compiler (pip install ziglang), g++ or clang++. The optimisation level is -O2, like the host
tests; the board build uses -Os for most files but the core is built -O3 (a pragma in core.cpp), so the numbers differ from the board's: compare only before and after
of the same machine, and use the board's own run (bench/run_board.py) for the real figures.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def compiler():
    for cand in (["g++"], ["clang++"], [sys.executable, "-m", "ziglang", "c++"]):
        try:
            subprocess.run(cand + ["--version"], capture_output=True, check=True)
            return cand
        except Exception:
            pass
    sys.exit("no C++ compiler found (pip install ziglang)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", default=None)
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--reps", type=int, default=15)
    a = ap.parse_args()
    build = os.path.join(ROOT, "bench", "build")
    os.makedirs(build, exist_ok=True)
    exe = os.path.join(build, "bench_host.exe" if os.name == "nt" else "bench_host")
    subprocess.check_call(compiler() + ["-O2", "-std=c++17", "-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(ROOT, "bench"),
                                        os.path.join(ROOT, "bench", "bench_host.cpp"), os.path.join(ROOT, "bench", "bench_cases.cpp"),
                                        os.path.join(ROOT, "src", "core.cpp"), "-o", exe])
    best = {}
    for r in range(a.runs):
        tmp = os.path.join(build, "run%d.json" % r)
        out = subprocess.run([exe, "--label", a.label, "--out", tmp, "--reps", str(a.reps)], capture_output=True, text=True)
        if r == 0:
            sys.stdout.write(out.stdout)
        d = json.load(open(tmp))
        for k, v in d["cases"].items():
            if k not in best or v["min"] < best[k]["min"]:
                best[k] = v
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        json.dump({"label": a.label, "platform": "host", "unit": "ns per operation", "runs": a.runs, "cases": best}, open(a.out, "w", newline=chr(10)), indent=1)
        print("wrote", a.out)


if __name__ == "__main__":
    main()
