#!/usr/bin/env python3
"""How smooth and how quick the lights are under each timing setting, replayed through the Python reference (no board needed).

    python tools/smoothness.py [--fps 30] [recording ...]

Recordings: tools/reference/data/*.txt by default (packets of a real Sync Box: milliseconds, then 24 zones as hex), or a JSON list of {"ms": ..., "zones": [[r,g,b] x 24]}.
For every setting the packets are replayed at the times they arrived and the output is sampled at a steady frame rate (the 24 zone colours: every LED is a fixed mix of
them, so this is what the lights do). Three numbers per setting:

  * roughness on the recordings: the average change of speed between output frames, |x[t+1] - 2 x[t] + x[t-1]| over every colour channel, in levels. Holding each
    picture is rough (all of a change in one frame); a slide is smooth. Lower is smoother. Real video is dominated by what the picture does, so differences are small.
  * roughness on a slow pan: the same on a synthetic picture that drifts slowly (a sine per zone, sampled like the box does: about every 100 ms, with jitter). This is
    where steps show most.
  * delay: the average time from a packet that carries a step to the output reaching half way, with the recording's own packet timing, for a big step (black to
    full) and a small one (20 levels). Lower is quicker. Adaptive smoothing treats the two differently on purpose.

The box's own smoothing is not modelled (as at Speed of Diffusion 100 %). LATENCY.md's figures come from this tool.
"""
import argparse
import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reference import motion  # noqa: E402
try:
    from reference import adaptive  # noqa: E402
except ImportError:                                    # adaptive smoothing is not in every version
    adaptive = None

REF_MS = 1000.0 / 30.0           # Smoothing over time is defined against a 30 fps tick (smoothDecay's reference in the relay)


def load(path):
    if path.endswith(".json"):
        d = json.load(open(path))
        return [(int(p["ms"]), [list(map(float, z)) for z in p["zones"]]) for p in d if p.get("ms") is not None]
    out = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        ms, hexz = line.split()
        b = bytes.fromhex(hexz)
        out.append((int(ms), [[float(b[i * 3 + c]) for c in range(3)] for i in range(24)]))
    return out


def slow_pan(seconds=20, seed=1):
    rnd = random.Random(seed)
    out, t = [], 0
    while t < seconds * 1000:
        out.append((t, [[127 + 100 * math.sin(2 * math.pi * (t / 4000.0 + z / 24.0 + c / 3.0)) for c in range(3)] for z in range(24)]))
        t += 100 + rnd.randint(-15, 15)
    return out


def steps(packets, lo=0.0, hi=255.0):
    """The recording's packet times with a synthetic picture that steps between lo and hi every 1.2 s (black and full white by default). Returns (packets, the steps:
    the time of each step's packet and its new level)."""
    out, edges, level, last = [], [], lo, None
    for t, _ in packets:
        if last is None or t - last >= 1200:
            level = hi if level == lo else lo
            edges.append((t, level))
            last = t
        out.append((t, [[level] * 3 for _ in range(24)]))
    return out, edges


class Run:
    """Replays packets and samples the output at `fps`: a subclass says what is shown at each frame."""
    def __init__(self, fps):
        self.fps = fps

    def frames(self, packets):
        t, end = float(packets[0][0]), packets[-1][0]
        while t <= end:
            yield t
            t += 1000.0 / self.fps

    def __call__(self, packets):
        out, j, newest = [], 0, None
        for t in self.frames(packets):
            arrived = []
            while j < len(packets) and packets[j][0] <= t:
                arrived.append(packets[j])
                newest = packets[j][1]
                j += 1
            out.append((t, self.show(t, arrived, newest)))
        return out


class Hold(Run):
    def show(self, t, arrived, newest):
        return [v for z in newest for v in z]


class Ease(Run):
    def __init__(self, fps, s):
        super().__init__(fps)
        self.s, self.cur, self.last = s, None, None

    def show(self, t, arrived, newest):
        tgt = [v for z in newest for v in z]
        if self.cur is None:
            self.cur = list(tgt)
        else:
            k = self.s ** ((t - self.last) / REF_MS)
            self.cur = [g + (c - g) * k for c, g in zip(self.cur, tgt)]
        self.last = t
        return list(self.cur)


class Motion(Run):
    def __init__(self, fps, pct, curve=False):
        super().__init__(fps)
        self.m, self.pct, self.curve = motion.Motion(), pct, curve

    def show(self, t, arrived, newest):
        for pt, z in arrived:
            self.m.push(pt, z)
        q = int(t) - int(self.m.delay(self.pct))
        zs = pchip_at(self.m, q) if self.curve else self.m.at(q)[0]
        return [v for z in zs for v in z]


class Adaptive(Run):
    def __init__(self, fps, calm):
        super().__init__(fps)
        self.f, self.calm, self.last = adaptive.OneEuro(), calm, None

    def show(self, t, arrived, newest):
        dt = 0.0 if self.last is None else t - self.last
        self.last = t
        zs, _ = self.f.step(newest, dt, self.calm)
        return [v for z in zs for v in z]


def pchip_at(m, time):
    """An experiment, not in the relay: Motion smoothing through a monotone cubic (Fritsch-Carlson) instead of straight lines. Measured in 0.4.13 and not adopted: on
    the slow pan it is no smoother (the knots' timing jitter dominates, not the corners) and on video 1-3 % (see improvement notes in docs/LATENCY.md)."""
    k = m.k
    if len(k) < 3 or time - k[-1][0] >= 0 or time - k[0][0] <= 0:
        return m.at(time)[0]
    for i in range(len(k) - 1):
        t0, t1 = k[i][0], k[i + 1][0]
        if t0 <= time < t1:
            f = (time - t0) / float(t1 - t0)
            out = []
            for zi in range(24):
                row = []
                for c in range(3):
                    ts = [p[0] for p in k]
                    ys = [p[1][zi][c] for p in k]
                    h = [float(ts[q + 1] - ts[q]) for q in range(len(k) - 1)]
                    d = [(ys[q + 1] - ys[q]) / h[q] for q in range(len(k) - 1)]

                    def slope(j):
                        if 0 < j < len(k) - 1:
                            if d[j - 1] * d[j] <= 0:
                                return 0.0
                            w1, w2 = 2 * h[j] + h[j - 1], h[j] + 2 * h[j - 1]
                            return (w1 + w2) / (w1 / d[j - 1] + w2 / d[j])
                        h0, h1, d0, d1 = (h[0], h[1], d[0], d[1]) if j == 0 else (h[-1], h[-2], d[-1], d[-2])
                        s = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1)
                        if s * d0 <= 0:
                            return 0.0
                        return 3 * d0 if d0 * d1 <= 0 and abs(s) > 3 * abs(d0) else s
                    m0, m1 = slope(i) * h[i], slope(i + 1) * h[i]
                    f2, f3 = f * f, f * f * f
                    row.append((2 * f3 - 3 * f2 + 1) * ys[i] + (f3 - 2 * f2 + f) * m0 + (-2 * f3 + 3 * f2) * ys[i + 1] + (f3 - f2) * m1)
                out.append(row)
            return out
    return m.at(time)[0]


def roughness(seq):
    tot, n = 0.0, 0
    for (_, a), (_, b), (_, c) in zip(seq, seq[1:], seq[2:]):
        for x, y, z in zip(a, b, c):
            tot += abs(z - 2 * y + x)
            n += 1
    return tot / max(1, n)


def half_way_ms(seq, edges):
    """For each step: the time from the step's packet until the output (zone 0, red) is half way to the new level."""
    out = []
    for i, (t0, level) in enumerate(edges[1:], 1):
        prev = edges[i - 1][1]
        half = (prev + level) / 2.0
        for t, v in seq:
            if t < t0:
                continue
            if (level > prev and v[0] >= half) or (level < prev and v[0] <= half):
                out.append(t - t0)
                break
    return sum(out) / len(out) if out else float("nan")


def settings(fps, extra=False):
    yield "hold (no smoothing)", lambda: Hold(fps)
    yield "smoothing over time 0.5", lambda: Ease(fps, 0.5)
    yield "smoothing over time 0.8", lambda: Ease(fps, 0.8)
    for pct in (50, 100):
        yield "motion %d" % pct, (lambda pct: lambda: Motion(fps, pct))(pct)
    if extra:
        yield "motion 100, cubic (experiment)", lambda: Motion(fps, 100, True)
    if adaptive is None:
        return
    for calm in (25, 50, 75):
        yield "adaptive %d" % calm, (lambda calm: lambda: Adaptive(fps, calm))(calm)


def table(recordings, fps, extra=False):
    pan = slow_pan()
    rows = []
    for name, make in settings(fps, extra):
        rough = sum(roughness(make()(p)) for p in recordings) / len(recordings)
        pan_r = roughness(make()(pan))
        delays, small = [], []
        for p in recordings:
            sp, edges = steps(p)
            delays.append(half_way_ms(make()(sp), edges))
            sp, edges = steps(p, 100.0, 120.0)
            small.append(half_way_ms(make()(sp), edges))
        rows.append((name, rough, pan_r, sum(delays) / len(delays), sum(small) / len(small)))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recordings", nargs="*")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--experiments", action="store_true", help="also the experiments that were not adopted")
    a = ap.parse_args()
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reference", "data")
    paths = a.recordings or [os.path.join(here, f) for f in sorted(os.listdir(here)) if f.endswith(".txt")]
    recs = [load(p) for p in paths]
    print("%d recordings (%s), output at %g fps" % (len(recs), ", ".join(os.path.basename(p) for p in paths), a.fps))
    print("  %-32s %16s %16s %13s %13s" % ("setting", "roughness, video", "roughness, pan", "half way, big", "half way, small"))
    for name, r, pr, d, ds in table(recs, a.fps, a.experiments):
        print("  %-32s %16.3f %16.3f %10.0f ms %12.0f ms" % (name, r, pr, d, ds))


if __name__ == "__main__":
    main()
