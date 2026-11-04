"""The matching logic of tools/latency_e2e.py on made-up signals with a known delay: the delay it reports must be the one that was built in.
    python test/test_latency_e2e.py
"""
import importlib.util
import math
import os
import random
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
spec = importlib.util.spec_from_file_location("latency_e2e", os.path.join(ROOT, "tools", "latency_e2e.py"))
le = importlib.util.module_from_spec(spec)
spec.loader.exec_module(le)

passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL: " + name + ((" -- " + str(detail)) if detail else ""))


def picture(seed, secs=40.0):
    """brightness of a 'picture' that changes now and then, as frames about every 70 ms with a little jitter: [(t, value)]"""
    rnd = random.Random(seed)
    t, v, out = 0.0, 100.0, []
    while t < secs:
        if rnd.random() < 0.15:
            v = rnd.uniform(20, 235)
        out.append((t, v))
        t += 0.07 + rnd.uniform(-0.01, 0.01)
    return out


def through(samples, delay_s, smooth=0.5, tick=1 / 30.0, secs=40.0):
    """what a strip shows: the picture `delay_s` later, eased toward it at 30 fps like the relay's smoothing: [(t, value)]"""
    out, cur, j, t = [], samples[0][1], 0, 0.0
    while t < secs:
        while j + 1 < len(samples) and samples[j + 1][0] + delay_s <= t:
            j += 1
        tgt = samples[j][1]
        cur += (tgt - cur) * (1 - smooth)
        out.append((t, cur))
        t += tick
    return out


for delay in (0.060, 0.120, 0.250):
    src = picture(3)
    box = le.resample(src, 1.0, 39.0)
    strip = le.resample(through(src, delay), 1.0, 39.0)
    lag, corr = le.best_lag(box, strip)
    check("a strip %d ms behind the picture is found at about %d ms (%d, correlation %.2f)" % (delay * 1000, delay * 1000, lag, corr), abs(lag - (delay * 1000 + 30)) <= 40 and corr > 0.7, (lag, corr))

src = picture(5)
box = le.resample(src, 1.0, 39.0)
ahead = le.resample(through(src, -0.050, smooth=0.0), 1.0, 39.0)
lag, corr = le.best_lag(box, ahead)
check("a strip that is ahead of the picture gives a negative delay (%d ms)" % lag, lag < 0 and corr > 0.7, (lag, corr))

rnd = random.Random(9)
noise_a = [(i * 0.05, rnd.uniform(0, 255)) for i in range(800)]
noise_b = [(i * 0.05, rnd.uniform(0, 255)) for i in range(800)]
lag, corr = le.best_lag(le.resample(noise_a, 1.0, 39.0), le.resample(noise_b, 1.0, 39.0))
check("two unrelated signals give a weak match (correlation %.2f)" % corr, corr < 0.5, corr)

flat = [(i * 0.07, 120.0) for i in range(500)]
lag, corr = le.best_lag(le.resample(flat, 1.0, 30.0), le.resample(flat, 1.0, 30.0))
check("a picture that never changes gives no result instead of a wrong one", corr < 0.0 or lag == 0, (lag, corr))
check("lum is the usual weighting", abs(le.lum((255, 0, 0)) - 76.245) < 1e-3 and abs(le.lum((255, 255, 255)) - 255.0) < 1e-3)

print("%d checks passed, %d failed" % (passed, failed))
print("ALL PASSED" if not failed else "SOME FAILED")
sys.exit(1 if failed else 0)
