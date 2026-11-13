#!/usr/bin/env python3
"""tools/smoothness.py on the recordings in tools/reference/data: the figures docs/LATENCY.md quotes, as ranges, so a change to the motion clock, the easing or the
measuring itself that moves them is noticed.

    python test/test_smoothness.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import smoothness  # noqa: E402

passed = failed = 0


def check(ok, what):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what)


here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "reference", "data")
recs = [smoothness.load(os.path.join(here, f)) for f in sorted(os.listdir(here)) if f.endswith(".txt")]
rows = {name: (r, pan, d, ds) for name, r, pan, d, ds in smoothness.table(recs, 30.0)}
hold, ease, m50, m100 = rows["hold (no smoothing)"], rows["smoothing over time 0.5"], rows["motion 50"], rows["motion 100"]
check(4.4 < hold[0] < 5.0 and 6.3 < hold[1] < 7.0 and 14 < hold[2] < 20, "hold: %r" % (hold,))
check(2.0 < ease[0] < 2.4 and 2.6 < ease[1] < 3.0, "smoothing over time 0.5: %r" % (ease,))
check(1.4 < m100[0] < 1.7 and m100[1] < 0.35 and 45 < m100[2] < 65,
      "motion 100 slides after lost packets too (2.5x smoother than before 0.4.13, half way about 54 ms): %r" % (m100,))
check(m100[1] < ease[1] / 5 and m100[1] < hold[1] / 15, "motion 100 is far smoother than the default and than holding on a slow pan")
check(m50[0] > m100[0] and m50[1] > m100[1] and m50[2] < m100[2], "motion 50 is quicker but rougher than motion 100")
if "adaptive 50" in rows:
    a50 = rows["adaptive 50"]
    check(a50[0] < ease[0] and a50[1] < ease[1] and a50[2] <= ease[2] + 1,
          "adaptive 50 is smoother than smoothing over time 0.5 on video and on the pan, and as quick on a big change: %r against %r" % (a50, ease))
    check(100 < a50[3] < 140, "adaptive 50 slows a small change on purpose (half way after about 120 ms): %r" % (a50,))
    check(rows["adaptive 75"][0] < a50[0] < rows["adaptive 25"][0], "adaptive: higher is calmer")
pan = smoothness.slow_pan()
check(abs(smoothness.roughness(smoothness.Motion(30, 100, True)(pan)) - m100[1]) < 0.05, "the cubic experiment is no smoother on the slow pan (why it was not adopted)")

print("%d checks passed, %d failed" % (passed, failed))
print("ALL PASSED" if not failed else "SOME FAILED")
sys.exit(1 if failed else 0)
