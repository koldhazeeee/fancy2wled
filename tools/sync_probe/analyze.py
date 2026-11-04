#!/usr/bin/env python3
"""Work out how the Sync Box turns a picture into its 24 zones, from a recording made by probe.py.

    python tools/sync_probe/analyze.py captures/probe-20261104-120000.json

Prints a report and writes it next to the recording (-report.json). What it measures:

  delay      how long after the laptop drew a picture the box's packet shows it (overall, and the best case on isolated black/white flashes),
             and how many packets show a half-way picture (the box blending frames)
  cadence    how often packets arrive and how often the picture in them really changes
  levels     input grey -> the number the box reports (black level, white level, full or limited range, curve)
  colours    pure colours, hues and saturation steps: input -> reported
  sampling   where in each zone the box looks (growing white rectangles: a zone made of a few single points rises in equal steps,
             a zone averaged over its whole area rises smoothly)
  motion     the delay while a bar moves across the screen

All times come from this computer's clock (the page's own clock is mapped onto it with the /sync exchanges), so a Wi-Fi link that delivers broadcasts
in bursts adds noise but not a constant offset: look at the best case as well as the median.
"""
import bisect
import json
import math
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import plan as planmod  # noqa: E402
from reference.protocol import decode_frame  # noqa: E402

W, H = planmod.W, planmod.H
COLS, ROWS = 6, 4
ZW, ZH = W / COLS, H / ROWS
REFRESH = planmod.REFRESH


# ---------------------------------------------------------------- small helpers
def pct(values, p):
    """p-th percentile (0..100) of a list, linear between neighbours; None for an empty list."""
    v = sorted(values)
    if not v:
        return None
    k = (len(v) - 1) * p / 100.0
    lo = int(math.floor(k))
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def pearson(a, b):
    n = len(a)
    if n < 3:
        return None
    ma, mb = sum(a) / n, sum(b) / n
    sa = sum((x - ma) ** 2 for x in a)
    sb = sum((x - mb) ** 2 for x in b)
    if sa <= 1e-9 or sb <= 1e-9:
        return None
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / math.sqrt(sa * sb)


def best_offset(samples):
    """samples: [page_before_ms, page_after_ms, probe_ms]. Returns (offset_ms, page_mid_ms, rtt_ms) of the exchange with the shortest round trip."""
    if not samples:
        return None
    s = min(samples, key=lambda x: x[1] - x[0])
    return s[2] - (s[0] + s[1]) / 2.0, (s[0] + s[1]) / 2.0, s[1] - s[0]


def hsv(rgb):
    r, g, b = [c / 255.0 for c in rgb]
    mx, mn = max(r, g, b), min(r, g, b)
    d = mx - mn
    s = d / mx if mx > 0 else 0.0
    if d <= 0:
        h = 0.0
    elif mx == r:
        h = (60 * ((g - b) / d)) % 360
    elif mx == g:
        h = 60 * ((b - r) / d + 2)
    else:
        h = 60 * ((r - g) / d + 4)
    return h, s, mx


def fmt(v, nd=1, unit=""):
    return "n/a" if v is None else ("%." + str(nd) + "f%s") % (v, unit)


# ---------------------------------------------------------------- the recording
class Run:
    def __init__(self, data):
        self.meta = data["meta"]
        self.plan = data["plan"]
        page = data.get("page") or {}
        self.page = page
        self.times, self.zones = [], []
        for t, hx in data["packets"]:
            z = decode_frame(bytes.fromhex(hx))
            if z:
                self.times.append(t)
                self.zones.append(z)
        self.zl = [[(r + g + b) / 3.0 for r, g, b in z] for z in self.zones]      # per packet, per zone: (r+g+b)/3
        self.lum = [sum(z) / len(z) for z in self.zl]                              # per packet: the mean of the zones
        # clock: page milliseconds -> probe seconds
        a = best_offset(page.get("sync"))
        b = best_offset(page.get("sync_end"))
        self.offset0 = a
        self.offset1 = b
        self.drift_ms = (b[0] - a[0]) if (a and b) else None
        self.rtt_ms = max(a[2], b[2]) if (a and b) else (a[2] if a else None)
        # when each pattern (and each flash state) was drawn, in probe seconds
        n = len(self.plan)
        self.start = [None] * n
        self.states = {i: [] for i in range(n)}
        for ev in page.get("events", []):
            t = self.page_to_probe(ev["t"])
            if ev["e"] == "seg":
                self.start[ev["i"]] = t
                self.states[ev["i"]].append((0, t))
            elif ev["e"] == "state":
                self.states[ev["i"]].append((ev["k"], t))
        self.end = [None] * n
        for i in range(n):
            if self.start[i] is None:
                continue
            nxt = self.start[i + 1] if i + 1 < n else None
            self.end[i] = nxt if nxt is not None else self.start[i] + self.plan[i]["dur_ms"] / 1000.0

    def page_to_probe(self, p_ms):
        a, b = self.offset0, self.offset1
        if a and b and b[1] > a[1]:
            off = a[0] + (b[0] - a[0]) * (p_ms - a[1]) / (b[1] - a[1])
        else:
            off = a[0] if a else 0.0
        return (p_ms + off) / 1000.0

    def idx(self, t0, t1):
        return range(bisect.bisect_left(self.times, t0), bisect.bisect_left(self.times, t1))

    def zone_medians(self, t0, t1):
        ix = self.idx(t0, t1)
        if len(ix) < 1:
            return None
        return [statistics.median(self.zl[i][z] for i in ix) for z in range(24)]

    def rgb_mean(self, t0, t1):
        """(r, g, b mean over the packets and zones in the window, spread of the zone means, number of packets) or None."""
        ix = self.idx(t0, t1)
        if len(ix) < 1:
            return None
        per_zone = [[statistics.mean(self.zones[i][z][c] for i in ix) for z in range(24)] for c in range(3)]
        m = [statistics.mean(per_zone[c]) for c in range(3)]
        spread = max(max(per_zone[c]) - min(per_zone[c]) for c in range(3))
        return m, spread, len(ix)

    def segs(self, suite=None, prefix=None):
        for i, s in enumerate(self.plan):
            if self.start[i] is None:
                continue
            if suite and s["suite"] != suite:
                continue
            if prefix and not s["id"].startswith(prefix):
                continue
            yield i, s

    def settled(self, i, tau, frac=0.5):
        """The part of pattern i in which packets show it settled, in probe seconds: from `frac` of the way through (plus the delay) to the end (plus the delay)."""
        s, e = self.start[i], self.end[i]
        return s + tau + frac * (e - s), e + tau - 0.12          # the average delay overshoots the real one by about the wait for the next picture


# ---------------------------------------------------------------- delay
def expected_luma_fn(run):
    """e(t): the mean (r+g+b)/3 over the whole screen at probe time t, as the laptop drew it (no delay)."""
    ivs = []
    for i, seg in enumerate(run.plan):
        if run.start[i] is None:
            continue
        for k, t in run.states[i]:
            ivs.append((t, i, k, run.start[i]))
    ivs.sort()
    starts = [x[0] for x in ivs]
    last_end = max((e for e in run.end if e is not None), default=0.0)

    def e(t):
        j = bisect.bisect_right(starts, t) - 1
        if j < 0 or t > last_end:
            return 0.0
        t0, i, k, s0 = ivs[j]
        return planmod.seg_mean_luma(run.plan[i], t - s0, k)
    return e


def estimate_delay(run, lo=0.0, hi=1.0):
    """The shift that makes the box's mean brightness best follow the laptop's. (delay_s, correlation) or (None, None)."""
    if len(run.times) < 20:
        return None, None
    e = expected_luma_fn(run)
    t_lo = min(s for s in run.start if s is not None)
    t_hi = max(x for x in run.end if x is not None)
    ix = [i for i, t in enumerate(run.times) if t_lo + lo <= t <= t_hi + hi]
    if len(ix) < 20:
        return None, None
    ys = [run.lum[i] for i in ix]
    ts = [run.times[i] for i in ix]

    def score(tau):
        return pearson(ys, [e(t - tau) for t in ts])
    best = (None, -2.0)
    tau = lo
    while tau <= hi + 1e-9:
        r = score(tau)
        if r is not None and r > best[1]:
            best = (tau, r)
        tau += 0.01
    if best[0] is None:
        return None, None
    c = best[0]
    for k in range(-5, 6):                       # refine to 2 ms
        tau = c + k * 0.002
        r = score(tau)
        if r is not None and r > best[1]:
            best = (tau, r)
    return best


def flash_edges(run, tau):
    """Isolated black/white (or lo/hi) edges: [(seg index, edge time, rising, latency or None, packets in between)] plus the per-colour-pair levels."""
    levels = {}
    for i, seg in run.segs("flash"):
        p = seg["period_frames"] / REFRESH
        if p < 0.4:
            continue
        key = (tuple(seg["lo"]), tuple(seg["hi"]))
        for k, t in run.states[i]:
            ix = run.idx(t + tau + 0.05, t + tau + p - 0.15)           # packets that show this state and nothing else
            if len(ix) >= 2:
                levels.setdefault(key, {0: [], 1: []})[k % 2].append(statistics.median(run.lum[j] for j in ix))
    ref = {}
    for key, d in levels.items():
        if d[0] and d[1]:
            ref[key] = (statistics.median(d[0]), statistics.median(d[1]))
    edges = []
    for i, seg in run.segs("flash"):
        key = (tuple(seg["lo"]), tuple(seg["hi"]))
        p = seg["period_frames"] / REFRESH
        if key not in ref or p < tau + 0.25:                           # a state shorter than the delay is never seen on its own: its edges say nothing
            continue
        lo_y, hi_y = ref[key]
        if abs(hi_y - lo_y) < 8:
            continue
        for k, t in run.states[i]:
            if k == 0:
                continue
            rising = k % 2 == 1
            before, after = (lo_y, hi_y) if rising else (hi_y, lo_y)
            lat, mid = None, 0
            first90 = None
            for j in run.idx(t, t + p + 0.25):
                f = (run.lum[j] - before) / (after - before)
                if lat is None and f >= 0.5:
                    lat = run.times[j] - t
                if first90 is None and f >= 0.9:
                    first90 = run.times[j]
                if lat is None and 0.1 < f < 0.9:
                    mid += 1
                if first90 is not None:
                    break
            edges.append((i, t, rising, lat, mid))
    return edges, ref


def flicker_response(run, tau, ref):
    """For the fast black/white flicker: the swing of the box's brightness as a share of the full swing, and the mean level."""
    out = []
    for i, seg in run.segs("flash"):
        key = (tuple(seg["lo"]), tuple(seg["hi"]))
        if seg["period_frames"] >= 15 or key not in ref:
            continue                                                    # slower flicker is the delay test above
        lo_y, hi_y = ref[key]
        if abs(hi_y - lo_y) < 8:
            continue
        ix = run.idx(run.start[i] + tau + 0.4, run.end[i] + tau - 0.1)
        if len(ix) < 8:
            continue
        ys = [run.lum[j] for j in ix]
        swing = (pct(ys, 95) - pct(ys, 5)) / abs(hi_y - lo_y)
        mean_level = (statistics.mean(ys) - lo_y) / (hi_y - lo_y)
        hz = REFRESH / (2.0 * seg["period_frames"])
        out.append({"id": seg["id"], "hz": hz, "swing": swing, "mean": mean_level, "packets": len(ix)})
    return out


def step_response(run, tau, min_change=25.0, width=0.9, bin_s=0.025):
    """How the box's brightness follows a sudden change of the picture, pooled over every change between two patterns that is big enough
    (no flicker needed). Time is counted from the moment the laptop drew the new pattern. Returns None if there are no usable changes, else
    {"edges", "t10", "t50", "t90" (seconds; None if never reached), "bins": [(time, median, p10, p90, n)]} where each value is the share of the
    change completed (0 before, 1 after)."""
    pts = []
    edges = 0
    per_edge = []                                           # per change: (time of 10 %, 50 %, 90 % of the change, packets caught half way)
    for i in range(1, len(run.plan)):
        if run.start[i] is None or run.start[i - 1] is None:
            continue
        a, c = run.plan[i - 1], run.plan[i]
        if c["kind"] != "rects" or a["kind"] != "rects":
            continue
        if abs(planmod.seg_mean_luma(c) - planmod.seg_mean_luma(a)) < min_change:
            continue
        if run.end[i - 1] - run.start[i - 1] < 1.0 or run.end[i] - run.start[i] < 1.0:     # both patterns must be on long enough to settle
            continue
        T = run.start[i]
        before = run.idx(T + tau - 0.6, T + tau - 0.12)
        after = run.idx(*run.settled(i, tau, 0.5))
        if len(before) < 2 or len(after) < 2:
            continue
        yb = statistics.median(run.lum[j] for j in before)
        ya = statistics.median(run.lum[j] for j in after)
        if abs(ya - yb) < 8:
            continue
        edges += 1
        cross = {0.1: None, 0.5: None, 0.9: None}
        transit = 0
        for j in run.idx(T - 0.05, T + width):
            f = (run.lum[j] - yb) / (ya - yb)
            s = run.times[j] - T
            pts.append((s, f))
            if s >= 0.02:
                for level in cross:
                    if cross[level] is None and f >= level:
                        cross[level] = s
                if 0.15 < f < 0.85:
                    transit += 1
        per_edge.append((cross[0.1], cross[0.5], cross[0.9], transit))
    if edges < 3:
        return None
    bins = []
    t = -0.05
    while t < width:
        v = [f for s, f in pts if t <= s < t + bin_s]
        if len(v) >= 3:
            bins.append((t + bin_s / 2.0, pct(v, 50), pct(v, 10), pct(v, 90), len(v)))
        t += bin_s

    def cross(level):
        for k in range(1, len(bins)):
            if bins[k - 1][1] < level <= bins[k][1]:
                (t0, f0), (t1, f1) = bins[k - 1][:2], bins[k][:2]
                return t0 + (t1 - t0) * (level - f0) / (f1 - f0) if f1 > f0 else t1
        return None
    t10s = [e[0] for e in per_edge if e[0] is not None]
    t50s = [e[1] for e in per_edge if e[1] is not None]
    t90s = [e[2] for e in per_edge if e[2] is not None]
    rises = [e[2] - e[0] for e in per_edge if e[0] is not None and e[2] is not None]
    return {"edges": edges, "t10": cross(0.1), "t50": cross(0.5), "t90": cross(0.9), "bins": bins,
            "each_t10": pct(t10s, 50), "each_t50": pct(t50s, 50), "each_t90": pct(t90s, 50), "each_t50_p10": pct(t50s, 10), "each_t50_p90": pct(t50s, 90),
            "each_rise": pct(rises, 50), "each_rise_p90": pct(rises, 90), "each_transit": pct([e[3] for e in per_edge], 50),
            "share_with_transit": sum(1 for e in per_edge if e[3] > 0) / float(len(per_edge))}


# ---------------------------------------------------------------- cadence
def cadence(run, t0=None, t1=None):
    ix = list(run.idx(t0, t1)) if t0 is not None else list(range(len(run.times)))
    if len(ix) < 3:
        return None
    gaps = [run.times[ix[k + 1]] - run.times[ix[k]] for k in range(len(ix) - 1)]
    changes = [ix[k + 1] for k in range(len(ix) - 1) if run.zones[ix[k + 1]] != run.zones[ix[k]]]
    cg = [run.times[changes[k + 1]] - run.times[changes[k]] for k in range(len(changes) - 1)]
    span = run.times[ix[-1]] - run.times[ix[0]]
    return {"packets": len(ix), "rate": (len(ix) - 1) / span if span > 0 else None, "gap_p10": pct(gaps, 10), "gap_median": pct(gaps, 50), "gap_p90": pct(gaps, 90),
            "identical_share": 1.0 - len(changes) / float(len(ix) - 1), "change_rate": (len(changes) - 1) / span if span > 0 and len(changes) > 1 else None,
            "change_gap_median": pct(cg, 50), "change_gap_p10": pct(cg, 10), "change_gap_p90": pct(cg, 90), "close_pairs": (sum(1 for g in cg if g < 0.03) / float(len(cg))) if cg else None}


# ---------------------------------------------------------------- levels and colours
def level_table(run, tau):
    rows = []
    for i, seg in run.segs("levels"):
        v = seg["bg"][0]
        a, b = run.settled(i, tau)
        m = run.rgb_mean(a, b)
        if m is None:
            continue
        rows.append({"in": v, "out": m[0], "spread": m[1], "packets": m[2]})
    rows.sort(key=lambda r: r["in"])
    return rows


def level_summary(rows):
    if len(rows) < 4:
        return None
    by = {r["in"]: r["out"] for r in rows}
    grey = {v: statistics.mean(o) for v, o in by.items()}
    black = grey.get(0)
    white = grey.get(255)
    first_visible = next((v for v in sorted(grey) if black is not None and grey[v] > black + 1.0), None)
    last_below_white = max((v for v in sorted(grey) if white is not None and grey[v] < white - 1.0), default=None)
    gam = []
    for v in (32, 48, 64, 96, 128, 160, 192, 224):
        o = grey.get(v)
        if o and 0 < o < 255:
            gam.append(math.log(o / 255.0) / math.log(v / 255.0))
    out = {"black_out": black, "white_out": white, "first_level_above_black": first_visible, "last_level_below_white": last_below_white,
           "gamma_effective": statistics.median(gam) if gam else None}
    verdict = []
    if black is not None and black >= 12:
        verdict.append("black is reported as %.0f: limited range (16-235) or lifted blacks" % black)
    if white is not None and white <= 245:
        verdict.append("white is reported as %.0f: limited range (16-235) or dimmed whites" % white)
    if black is not None and white is not None and black < 6 and white > 250:
        verdict.append("full range, 0-255 end to end")
    out["verdict"] = verdict
    return out


def colour_table(run, tau):
    rows = []
    for i, seg in run.segs("colors"):
        a, b = run.settled(i, tau)
        m = run.rgb_mean(a, b)
        if m is None:
            continue
        rows.append({"id": seg["id"], "in": seg["bg"], "out": m[0], "spread": m[1]})
    return rows


# ---------------------------------------------------------------- where the box looks
def plateau_rises(xs, fs, tol=0.07):
    """The steps of a growing-rectangle response: [(x_before, x_after, increase)]. fs rises as the rectangle grows over xs; every sample point of a zone
    adds one step. Values within `tol` of each other are one plateau, and a lone half-way value between two plateaus is a picture caught in transit."""
    cm, m = [], 0.0
    for f in fs:                                            # the response cannot fall as the rectangle grows: a dip is a late or early picture
        m = max(m, f)
        cm.append(m)
    groups = []                                             # [first index, last index, mean]
    for i, f in enumerate(cm):
        if groups and abs(f - groups[-1][2]) <= tol:
            g = groups[-1]
            n = g[1] - g[0] + 1
            g[2] = (g[2] * n + f) / (n + 1)
            g[1] = i
        else:
            groups.append([i, i, f])
    keep = [g for k, g in enumerate(groups) if g[1] > g[0] or k == 0 or k == len(groups) - 1]
    merged = []
    for g in keep:
        if merged and abs(g[2] - merged[-1][2]) <= tol:
            h = merged[-1]
            n1, n2 = h[1] - h[0] + 1, g[1] - g[0] + 1
            h[2] = (h[2] * n1 + g[2] * n2) / (n1 + n2)
            h[1] = g[1]
        else:
            merged.append(list(g))
    return [(xs[a[1]], xs[b[0]], b[2] - a[2]) for a, b in zip(merged, merged[1:]) if b[2] - a[2] > tol]


def grow_analysis(run, tau, axis):
    """What each zone sees, from a white rectangle that grows from the left (axis 'x') or the top ('y'): per zone the steps of its response."""
    pre = "growx-" if axis == "x" else "growy-"
    items = []
    for i, seg in run.segs("grow", pre):
        items.append((int(seg["id"][len(pre):]), i))
    items.sort()
    if len(items) < 8 or items[0][0] != 0:
        return None
    step = items[1][0] - items[0][0]
    meds = []
    for size, i in items:
        a, b = run.settled(i, tau, 0.3)
        m = run.zone_medians(a, b)
        if m is not None:
            meds.append((size, m))
    if len(meds) < 8 or meds[0][0] != 0:
        return None
    base, white = meds[0][1], meds[-1][1]
    zsize = ZW if axis == "x" else ZH
    xs = [s for s, _ in meds]
    zones = {}
    for z in range(24):
        span = white[z] - base[z]
        if span <= 20:
            continue
        fs = [(m[z] - base[z]) / span for _, m in meds]
        start = (z % COLS) * zsize if axis == "x" else (z // COLS) * zsize
        rises = plateau_rises(xs, fs)
        zones[z] = {"total": sum(d for _, _, d in rises), "series": (xs, fs),
                    "rises": [{"from": a, "to": c, "centre": (a + c) / 2.0, "offset": (a + c) / 2.0 - start, "delta": d, "width": c - a} for a, c, d in rises]}
    return {"axis": axis, "step": step, "steps_with_data": len(meds), "steps": len(items), "zones": zones}


def is_sharp(r, step):
    """A rise made by a single sample point: it happens within one or two steps of the growing rectangle and is a fraction of the whole."""
    return 0.08 <= r["delta"] <= 0.45 and r["width"] <= 2.5 * step


def fit_ramp(xs, fs):
    """Where a straight line through the rising part of a response (between 3 % and 97 %) reaches 0 and 1: (from, to) in pixels, or None."""
    pts = [(x, f) for x, f in zip(xs, fs) if 0.03 < f < 0.97]
    if len(pts) < 3:
        return None
    n = len(pts)
    sx, sy = sum(x for x, _ in pts), sum(f for _, f in pts)
    sxx, sxy = sum(x * x for x, _ in pts), sum(x * f for x, f in pts)
    den = n * sxx - sx * sx
    if den <= 0:
        return None
    slope = (n * sxy - sx * sy) / den
    if slope <= 0:
        return None
    icp = (sy - slope * sx) / n
    return (0.0 - icp) / slope, (1.0 - icp) / slope


def zone_geometry(grow):
    """{zone: {"kind", "window": (from, to) in pixels, "points": [(position, weight)]}} along the axis of a growing-rectangle test. Kinds:
    points  a few single sample points (steps of equal size); ramp  an average over the window (one long smooth rise);
    spot  everything within one or two steps (a thin line or a spot at that position); mixed  anything else."""
    out = {}
    if not grow:
        return out
    step = grow["step"]
    for z, info in grow["zones"].items():
        rises = info["rises"]
        if not rises:
            continue
        sharp = [r for r in rises if is_sharp(r, step)]
        fit = fit_ramp(*info["series"]) if "series" in info else None
        if len(rises) >= 7:                                  # so many small steps that they cannot be told from a smooth ramp at this resolution
            out[z] = {"kind": "ramp", "window": fit or (rises[0]["from"], rises[-1]["to"]), "points": []}
        elif len(rises) == 1 and rises[0]["delta"] >= 0.9:
            r = rises[0]
            kind = "spot" if r["width"] <= 2.5 * step else "ramp"
            out[z] = {"kind": kind, "window": (fit if kind == "ramp" and fit else (r["from"], r["to"])), "points": [(r["centre"], 1.0)]}
        elif sharp and info["total"] > 0 and sum(r["delta"] for r in sharp) >= 0.8 * info["total"]:
            out[z] = {"kind": "points", "window": (min(r["from"] for r in sharp), max(r["to"] for r in sharp)), "points": [(r["centre"], r["delta"]) for r in sharp]}
        else:
            out[z] = {"kind": "mixed", "window": (rises[0]["from"], rises[-1]["to"]), "points": []}
    return out


def sample_points(grow):
    """The most common pattern of sample points among the zones: {"count", "offsets" (pixels from the start of the zone), "lines_with_that_count" (zones
    that have it), "lines" (zones measured)}, or None if the zones look like an average over an area."""
    geo = zone_geometry(grow)
    if not geo:
        return None
    zsize = ZW if grow["axis"] == "x" else ZH
    pts = {}
    for z, g in geo.items():
        if g["kind"] == "points":
            start = (z % COLS) * zsize if grow["axis"] == "x" else (z // COLS) * zsize
            pts[z] = sorted(c - start for c, _ in g["points"])
    if len(pts) * 2 < len(geo):
        return None
    common = statistics.mode([len(p) for p in pts.values()])
    if common < 2:
        return None
    same = [p for p in pts.values() if len(p) == common]
    typical = [statistics.median(p[k] for p in same) for k in range(common)]
    return {"count": common, "offsets": typical, "lines_with_that_count": len(same), "lines": len(geo)}


def scan_analysis(run, tau, axis):
    pre = "scanx-" if axis == "x" else "scany-"
    items = []
    base_i = None
    for i, seg in run.segs("scan", pre):
        tail = seg["id"][len(pre):]
        if tail == "base":
            base_i = i
        else:
            items.append((int(tail), i))
    if base_i is None or len(items) < 8:
        return None
    items.sort()
    a, b = run.settled(base_i, tau, 0.4)
    base = run.zone_medians(a, b)
    if base is None:
        return None
    step = items[1][0] - items[0][0]
    zones = {}
    for pos, i in items:
        a, b = run.settled(i, tau, 0.4)
        m = run.zone_medians(a, b)
        if m is None:
            continue
        for z in range(24):
            d = (m[z] - base[z]) / 255.0
            if d > 0.08:
                zones.setdefault(z, []).append((pos, d))
    out = {}
    for z, lst in zones.items():
        groups = []
        for pos, d in lst:
            if groups and pos - groups[-1][1] <= step:
                groups[-1][1] = pos
                groups[-1][2] = max(groups[-1][2], d)
            else:
                groups.append([pos, pos, d])
        out[z] = [{"from": g[0], "to": g[1] + step, "peak": g[2]} for g in groups]
    return {"axis": axis, "step": step, "zones": out}


# ---------------------------------------------------------------- motion
def motion_delay(run, axis, grow=None):
    """The delay while a bar moves across the screen: the shift of the expected zone brightness that best fits what the box reports.
    Each zone's expected brightness comes from what it was found to look at (`grow`, from the growing rectangle: single points, or a window it averages);
    for a zone not measured, its whole area."""
    zsize = ZW if axis == "x" else ZH
    geo = zone_geometry(grow)
    total = []
    for i, seg in run.segs("motion", "bar-" + axis):
        t0, t1 = run.start[i], run.end[i]
        ix = list(run.idx(t0, t1 + 1.0))
        if len(ix) < 20:
            continue
        ts = [run.times[j] for j in ix]
        meas = {}
        for z in range(24):
            m = [run.zl[j][z] for j in ix]
            lo, hi = min(m), max(m)
            if hi - lo > 20:
                meas[z] = [(v - lo) / (hi - lo) for v in m]
        if not meas:
            continue

        def expect(z, t):
            p = planmod.bar_position(seg, t - t0)
            g = geo.get(z)
            if g and g["kind"] == "points":
                tot = sum(w for _, w in g["points"])
                return sum(w for c, w in g["points"] if p <= c < p + seg["width"]) / tot
            if g and g["kind"] in ("ramp", "spot"):
                a, c = g["window"]
                w = max(c - a, 1.0)
                return max(0.0, min(p + seg["width"], c) - max(p, a)) / min(seg["width"], w)
            a = ((z % COLS) if axis == "x" else (z // COLS)) * zsize
            return max(0.0, min(p + seg["width"], a + zsize) - max(p, a)) / min(seg["width"], zsize)
        best = (None, -2.0)
        tau = 0.0
        while tau <= 0.8 + 1e-9:
            rs = []
            for z, series in meas.items():
                r = pearson(series, [expect(z, t - tau) for t in ts])
                if r is not None:
                    rs.append(r)
            if rs and statistics.mean(rs) > best[1]:
                best = (tau, statistics.mean(rs))
            tau += 0.01
        if best[0] is not None:
            total.append(best)
    if not total:
        return None
    return {"delay": statistics.mean(b[0] for b in total), "correlation": statistics.mean(b[1] for b in total), "passes": len(total), "each": [round(b[0], 3) for b in total],
            "zones_with_own_geometry": len(geo)}


# ---------------------------------------------------------------- the report
def analyse(run):
    rep = {"meta": run.meta}
    rep["packets"] = len(run.times)
    env = run.page.get("env", {})
    rep["page"] = {"env": env, "clock_drift_ms": run.drift_ms, "clock_rtt_ms": run.rtt_ms}
    tau, corr = estimate_delay(run)
    rep["delay_overall"] = {"seconds": tau, "correlation": corr}
    use_tau = tau if tau is not None else 0.15
    edges, ref = flash_edges(run, use_tau)
    lats = [e[3] for e in edges if e[3] is not None]
    mids = [e[4] for e in edges if e[3] is not None]
    rep["delay_flash"] = {"edges": len(edges), "found": len(lats), "min": min(lats) if lats else None, "p10": pct(lats, 10), "median": pct(lats, 50), "p90": pct(lats, 90),
                          "median_halfway_packets": statistics.median(mids) if mids else None,
                          "rising_median": pct([e[3] for e in edges if e[3] is not None and e[2]], 50), "falling_median": pct([e[3] for e in edges if e[3] is not None and not e[2]], 50)}
    rep["flicker"] = flicker_response(run, use_tau, ref)
    rep["step"] = step_response(run, use_tau)
    fast = [(run.start[i], run.end[i]) for i, _ in run.segs("motion", "bar-fast")]
    rep["cadence_all"] = cadence(run)
    rep["cadence_motion"] = cadence(run, min(a for a, _ in fast), max(b for _, b in fast)) if fast else None
    rows = level_table(run, use_tau)
    rep["levels"] = {"table": rows, "summary": level_summary(rows)}
    rep["colours"] = colour_table(run, use_tau)
    gx, gy = grow_analysis(run, use_tau, "x"), grow_analysis(run, use_tau, "y")
    rep["grow_x"], rep["grow_y"] = gx, gy
    rep["geometry_x"] = {z: g for z, g in zone_geometry(gx).items()}
    rep["geometry_y"] = {z: g for z, g in zone_geometry(gy).items()}
    px, py = sample_points(gx), sample_points(gy)
    rep["points_x"], rep["points_y"] = px, py
    rep["scan_x"], rep["scan_y"] = scan_analysis(run, use_tau, "x"), scan_analysis(run, use_tau, "y")
    rep["motion_x"] = motion_delay(run, "x", gx)
    rep["motion_y"] = motion_delay(run, "y", gy)
    return rep


def ms(v):
    return "n/a" if v is None else "%.0f ms" % (v * 1000.0)


def print_report(rep, out=print):
    m = rep["meta"]
    out("=" * 78)
    out("Sync probe report   (%s%s)" % (m.get("started", "?"), ", quick plan" if m.get("quick") else ""))
    if m.get("note"):
        out("note: " + m["note"])
    if m.get("box_settings"):
        out("box settings: " + m["box_settings"])
    env = rep["page"]["env"]
    if env:
        flags = []
        if env.get("innerWidth") != 1920 or env.get("innerHeight") != 1080:
            flags.append("window is %sx%s, not 1920x1080 (patterns are scaled)" % (env.get("innerWidth"), env.get("innerHeight")))
        if env.get("devicePixelRatio") not in (1, None):
            flags.append("scale is %s, not 100%%" % env.get("devicePixelRatio"))
        if env.get("hdr"):
            flags.append("HDR is on")
        if env.get("long_frames", 0) > 5:
            flags.append("%d slow frames in the browser (patterns may be a frame late)" % env["long_frames"])
        out("screen: %sx%s, scale %s, %s frames drawn%s" % (env.get("innerWidth"), env.get("innerHeight"), env.get("devicePixelRatio"), env.get("frames"),
                                                          ("  ** " + "; ".join(flags)) if flags else ""))
    if rep["page"]["clock_drift_ms"] is not None:
        out("clock link: round trip %.1f ms, drift %.2f ms over the run" % (rep["page"]["clock_rtt_ms"], rep["page"]["clock_drift_ms"]))
    out("packets decoded: %d" % rep["packets"])

    out("")
    out("DELAY (from the laptop drawing a picture to the box's packet showing it; includes the laptop's own output, about 1-2 frames = 17-33 ms)")
    d = rep["delay_overall"]
    out("  overall, best fit of the whole run: %s   (match %s)" % (ms(d["seconds"]), fmt(d["correlation"], 3)))
    f = rep["delay_flash"]
    if f["found"]:
        out("  isolated black/white flashes: %d edges  best %s   10%% %s   median %s   90%% %s" % (f["found"], ms(f["min"]), ms(f["p10"]), ms(f["median"]), ms(f["p90"])))
        out("     rising edges median %s, falling edges median %s; packets showing a half-way picture per edge: %s"
            % (ms(f["rising_median"]), ms(f["falling_median"]), fmt(f["median_halfway_packets"], 1)))
        out("     (the best case is the box's own delay plus the laptop's; the median adds the wait for the next picture and Wi-Fi bursts)")
    for k in ("motion_x", "motion_y"):
        mo = rep[k]
        if mo:
            out("  a moving bar, %s direction: %s  (match %.3f%s, %d passes: %s; %d zones judged by where they were found to look; this lag includes the box's own smoothing)" % (
                "horizontal" if k.endswith("x") else "vertical", ms(mo["delay"]), mo["correlation"], " - poor, do not rely on it" if mo["correlation"] < 0.7 else "",
                mo["passes"], ", ".join("%.0f" % (e * 1000) for e in mo["each"]), mo["zones_with_own_geometry"]))
    sr = rep["step"]
    if sr:
        out("  after a sudden change (%d changes of the picture, each looked at on its own; median over the changes):" % sr["edges"])
        out("     the first sign (10%%) after %s, half after %s (the middle 80%% of the changes: %s to %s), nearly all (90%%) after %s" % (
            ms(sr["each_t10"]), ms(sr["each_t50"]), ms(sr["each_t50_p10"]), ms(sr["each_t50_p90"]), ms(sr["each_t90"])))
        out("     the rise from 10%% to 90%% takes %s (90%% of the changes within %s); %.0f%% of the changes were caught half way by at least one packet (median %s packets)" % (
            ms(sr["each_rise"]), ms(sr["each_rise_p90"]), sr["share_with_transit"] * 100, fmt(sr["each_transit"], 0)))
        out("     a box that only delays and samples rises within one picture (about 0-100 ms, no packets half way); a longer rise with half-way packets is the box smoothing in time")
        out("     median progress by time: " + "  ".join("%.0fms:%.0f%%" % (t * 1000, m * 100) for t, m, lo, hi, n in sr["bins"][::2] if 0 <= t <= 0.7))
    if rep["flicker"]:
        out("  fast flicker (the box's own smoothing in time):")
        for r in rep["flicker"]:
            out("     %5.1f Hz  swing %3.0f%% of full, mean level %3.0f%% (%d packets)" % (r["hz"], r["swing"] * 100, r["mean"] * 100, r["packets"]))

    out("")
    out("CADENCE")
    for name, key in (("whole run", "cadence_all"), ("while a fast bar moves", "cadence_motion")):
        c = rep[key]
        if c:
            out("  %-18s %5.1f packets/s, gaps 10%%/median/90%% = %s / %s / %s;  %.0f%% of packets repeat the previous picture" % (
                name, c["rate"] or 0, ms(c["gap_p10"]), ms(c["gap_median"]), ms(c["gap_p90"]), c["identical_share"] * 100))
            if c["change_gap_median"] is not None:
                out("  %-18s the picture really changes %4.1f times a second: gap 10%%/median/90%% = %s / %s / %s; %.0f%% of changes follow within 30 ms of another" % (
                    "", c["change_rate"] or 0, ms(c["change_gap_p10"]), ms(c["change_gap_median"]), ms(c["change_gap_p90"]), (c["close_pairs"] or 0) * 100))

    lv = rep["levels"]
    out("")
    out("LEVELS (grey in -> what the box reports, mean of the 24 zones; spread = largest difference between zones)")
    if lv["table"]:
        line = ""
        for r in lv["table"]:
            line += "  %3d -> %5.1f %5.1f %5.1f (sp %4.1f)" % (r["in"], r["out"][0], r["out"][1], r["out"][2], r["spread"])
            if len(line) > 100:
                out(line)
                line = ""
        if line:
            out(line)
        s = lv["summary"]
        if s:
            out("  black %s, white %s, first level above black: %s, last below white: %s, effective gamma %s" % (
                fmt(s["black_out"]), fmt(s["white_out"]), s["first_level_above_black"], s["last_level_below_white"], fmt(s["gamma_effective"], 2)))
            for v in s["verdict"]:
                out("  -> " + v)
    else:
        out("  (not recorded)")

    out("")
    out("COLOURS (in -> out)")
    if rep["colours"]:
        for r in rep["colours"]:
            hi, si, vi = hsv(r["in"])
            ho, so, vo = hsv(r["out"])
            out("  %-9s in %3d %3d %3d -> out %5.1f %5.1f %5.1f   hue %3.0f -> %3.0f   sat %3.0f%% -> %3.0f%%   value %3.0f%% -> %3.0f%%" % (
                r["id"], r["in"][0], r["in"][1], r["in"][2], r["out"][0], r["out"][1], r["out"][2], hi, ho, si * 100, so * 100, vi * 100, vo * 100))
    else:
        out("  (not recorded)")

    out("")
    out("WHERE THE BOX LOOKS (a white rectangle growing from the left / the top: every sample point a zone has makes one step in its value)")
    for key, label, zsize in (("grow_x", "across (x)", ZW), ("grow_y", "down (y)", ZH)):
        g = rep[key]
        if not g:
            out("  %s: (not recorded)" % label)
            continue
        out("  %s: zone size %d px, rectangle grew in steps of %d px (%d of %d steps had packets); positions are good to about +-%d px" % (
            label, zsize, g["step"], g["steps_with_data"], g["steps"], g["step"] // 2))
        geo = rep["geometry_x" if key == "grow_x" else "geometry_y"]
        groups = {}
        for z in sorted(g["zones"]):
            info = g["zones"][z]
            sig = tuple(int(round(r["centre"] / float(g["step"]))) for r in info["rises"])
            groups.setdefault(sig, []).append(z)
        for sig, zs in sorted(groups.items(), key=lambda kv: kv[1][0]):
            gz = geo.get(zs[0]) or geo.get(str(zs[0]))
            names = ", ".join(str(z) for z in zs)
            if not gz:
                out("     zones %s: no change at all" % names)
            elif gz["kind"] == "ramp":
                out("     zones %s: an average over %.0f to %.0f px (a %.0f px wide window; the zone's own area is %.0f px)" % (names, gz["window"][0], gz["window"][1], gz["window"][1] - gz["window"][0], zsize))
            elif gz["kind"] == "spot":
                out("     zones %s: one thin spot or line at %.0f px (everything within %.0f px)" % (names, (gz["window"][0] + gz["window"][1]) / 2.0, gz["window"][1] - gz["window"][0]))
            elif gz["kind"] == "points":
                out("     zones %s: %d single sample points at %s px" % (names, len(gz["points"]), ", ".join("%.0f" % c for c, _ in gz["points"])))
            else:
                out("     zones %s: a mixed response between %.0f and %.0f px" % (names, gz["window"][0], gz["window"][1]))
        p = rep["points_x" if key == "grow_x" else "points_y"]
        if p:
            out("     -> the most common zone has %d single sample points, at about %s px from the start of the zone (%d of %d zones)" % (
                p["count"], ", ".join("%.0f" % o for o in p["offsets"]), p["lines_with_that_count"], p["lines"]))
    for key in ("scan_x", "scan_y"):
        sc = rep[key]
        if sc:
            out("  scan %s (steps of %d px): zones that responded and where:" % (sc["axis"], sc["step"]))
            for z in sorted(sc["zones"]):
                out("     zone %2d: %s" % (z, ", ".join("%d-%d (%.0f%%)" % (g["from"], g["to"], g["peak"] * 100) for g in sc["zones"][z])))
    out("=" * 78)


def report_file(path, quiet=False):
    with open(path) as f:
        data = json.load(f)
    if not (data.get("page") or {}).get("events"):
        if not quiet:
            print("this recording has no page log (the run did not finish), so there is nothing to analyse")
        return None
    run = Run(data)
    rep = analyse(run)
    if not quiet:
        print_report(rep)
    out = os.path.splitext(path)[0] + "-report.json"
    with open(out, "w", newline="\n") as f:
        json.dump(rep, f, indent=1, default=str)
    if not quiet:
        print("report saved: " + os.path.normpath(out))
    return rep


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    report_file(sys.argv[1])
