"""The test plan of the sync probe: what is shown on the screen, in what order, and a small reference model of what each pattern looks like.

The probe (probe.py) shows these patterns full screen on a laptop whose HDMI output feeds the Sync Box, and records the box's UDP broadcast
at the same time. Because we know exactly what went in, the analysis (analyze.py) can work out how the box turns a picture into its 24 zones.

A plan is a list of segments. Every segment has "suite" (which group of tests), "id", "kind" and "dur_ms" (how long it is shown) and:

    rects   {"bg": [r,g,b], "rects": [{"x","y","w","h","rgb"}]}   a background and some rectangles (a solid colour has no rectangles)
    grad    {"axis": "x"|"y", "from": [r,g,b], "to": [r,g,b]}     a linear ramp across the screen
    flash   {"lo", "hi", "period_frames", "states"}               lo, hi, lo, hi ... one state per `period_frames` screen refreshes (60 Hz)
    bar     {"axis", "width", "speed", "bg", "rgb"}               a bar that moves across the screen at `speed` pixels a second

Coordinates are for a 1920 x 1080 screen; the page scales them if the window is another size.
"""
import colorsys

W, H = 1920, 1080
REFRESH = 60.0                      # the laptop's refresh rate the flash patterns are counted in
DEFAULT_SUITES = ["levels", "colors", "flash", "grow", "grad", "motion"]
ALL_SUITES = DEFAULT_SUITES + ["scan"]

GREYS = [0, 1, 2, 3, 4, 6, 8, 12, 16, 20, 24, 32, 48, 64, 96, 128, 160, 192, 224, 235, 240, 246, 250, 253, 254, 255]
GREYS_QUICK = [0, 8, 16, 32, 64, 128, 192, 235, 255]
FLASH_PERIODS = [(60, 10), (45, 8), (30, 12), (15, 12), (8, 24), (4, 40), (2, 60), (1, 120)]     # (frames per state, number of states)
FLASH_PERIODS_QUICK = [(60, 6), (45, 6), (30, 8), (15, 10), (4, 30), (1, 90)]


def mean(rgb):
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0


def hsv_rgb(h, s, v):
    r, g, b = colorsys.hsv_to_rgb(h / 360.0, s, v)
    return [int(round(r * 255)), int(round(g * 255)), int(round(b * 255))]


def build_plan(suites=None, quick=False, scan_step=4, flash=True):
    """The list of segments for the chosen suites (see the module text). quick = about half as long, coarser."""
    suites = list(suites) if suites else list(DEFAULT_SUITES)
    if not flash and "flash" in suites:
        suites.remove("flash")
    segs = []

    def add(suite, sid, kind, dur_ms, **kw):
        s = {"suite": suite, "id": sid, "kind": kind, "dur_ms": int(round(dur_ms))}
        s.update(kw)
        segs.append(s)

    def solid(suite, sid, rgb, hold_ms):
        add(suite, sid, "rects", hold_ms, bg=list(rgb), rects=[])

    def gap(ms=1000, sid="gap"):
        solid("gap", sid, [0, 0, 0], ms)

    gap(3000, "lead")
    for suite in suites:
        if suite == "levels":
            hold = 1500 if quick else 2000
            for v in (GREYS_QUICK if quick else GREYS):
                solid(suite, "grey-%d" % v, [v, v, v], hold)
        elif suite == "colors":
            hold = 1200 if quick else 1500
            for name, ch in (("R", 0), ("G", 1), ("B", 2)):
                for v in ([64, 128, 255] if quick else [32, 64, 128, 192, 255]):
                    rgb = [0, 0, 0]
                    rgb[ch] = v
                    solid(suite, "%s-%d" % (name, v), rgb, hold)
            for name, rgb in (("Y", [1, 1, 0]), ("C", [0, 1, 1]), ("M", [1, 0, 1])):
                for v in ([255] if quick else [128, 255]):
                    solid(suite, "%s-%d" % (name, v), [c * v for c in rgb], hold)
            for h in range(0, 360, 60 if quick else 30):
                solid(suite, "hue-%d" % h, hsv_rgb(h, 1.0, 1.0), hold)
            for h in ([0, 240] if quick else [0, 120, 240]):
                for s in ([0.5] if quick else [0.25, 0.5, 0.75]):
                    solid(suite, "sat-%d-%d" % (h, int(s * 100)), hsv_rgb(h, s, 1.0), hold)
        elif suite == "flash":
            for frames, states in (FLASH_PERIODS_QUICK if quick else FLASH_PERIODS):
                add(suite, "flash-%d-bw" % frames, "flash", states * frames * 1000.0 / REFRESH, lo=[0, 0, 0], hi=[255, 255, 255], period_frames=frames, states=states)
            add(suite, "flash-30-small", "flash", 10 * 30 * 1000.0 / REFRESH, lo=[0, 0, 0], hi=[64, 64, 64], period_frames=30, states=10)
            add(suite, "flash-30-red", "flash", 10 * 30 * 1000.0 / REFRESH, lo=[0, 0, 0], hi=[255, 0, 0], period_frames=30, states=10)
        elif suite == "grow":
            hold = 700 if quick else 800                 # the box makes about 10 pictures a second: this leaves several of them showing each step
            sx, sy = (32, 24) if quick else (16, 12)
            solid(suite, "growx-0", [0, 0, 0], 1500)
            for w in range(sx, W + 1, sx):
                add(suite, "growx-%d" % w, "rects", hold, bg=[0, 0, 0], rects=[{"x": 0, "y": 0, "w": w, "h": H, "rgb": [255, 255, 255]}])
            solid(suite, "growy-0", [0, 0, 0], 1500)
            for h in range(sy, H + 1, sy):
                add(suite, "growy-%d" % h, "rects", hold, bg=[0, 0, 0], rects=[{"x": 0, "y": 0, "w": W, "h": h, "rgb": [255, 255, 255]}])
        elif suite == "grad":
            add(suite, "grad-x", "grad", 3000, axis="x", **{"from": [0, 0, 0], "to": [255, 255, 255]})
            add(suite, "grad-y", "grad", 3000, axis="y", **{"from": [0, 0, 0], "to": [255, 255, 255]})
        elif suite == "motion":
            for n in (1, 2):
                add(suite, "bar-x-%d" % n, "bar", (W + 240) / 480.0 * 1000, axis="x", width=240, speed=480, bg=[0, 0, 0], rgb=[255, 255, 255])
            for n in (1, 2):
                add(suite, "bar-y-%d" % n, "bar", (H + 135) / 270.0 * 1000, axis="y", width=135, speed=270, bg=[0, 0, 0], rgb=[255, 255, 255])
            for n in (1, 2, 3):            # so fast that every new picture the box makes must change the zones: this shows how often the box makes one
                add(suite, "bar-fast-%d" % n, "bar", (W + 240) / 1500.0 * 1000, axis="x", width=240, speed=1500, bg=[0, 0, 0], rgb=[255, 255, 255])
        elif suite == "scan":
            solid(suite, "scanx-base", [0, 0, 0], 1500)
            for x in range(0, W, scan_step):
                add(suite, "scanx-%d" % x, "rects", 500, bg=[0, 0, 0], rects=[{"x": x, "y": 0, "w": scan_step, "h": H, "rgb": [255, 255, 255]}])
            solid(suite, "scany-base", [0, 0, 0], 1500)
            for y in range(0, H, scan_step):
                add(suite, "scany-%d" % y, "rects", 500, bg=[0, 0, 0], rects=[{"x": 0, "y": y, "w": W, "h": scan_step, "rgb": [255, 255, 255]}])
        else:
            raise ValueError("unknown suite %r (known: %s)" % (suite, ", ".join(ALL_SUITES)))
        gap(1500)
    return segs


def total_seconds(plan):
    return sum(s["dur_ms"] for s in plan) / 1000.0


# ---------------------------------------------------------------- reference model: what a segment looks like
def bar_position(seg, rel_s):
    """The left (or top) edge of a moving bar `rel_s` seconds after its segment started."""
    return -seg["width"] + seg["speed"] * rel_s


def seg_mean_luma(seg, rel_s=0.0, state=0):
    """The mean of (r+g+b)/3 over the whole screen for this segment at `rel_s` seconds after it started (`state` = flash state number)."""
    k = seg["kind"]
    if k == "rects":
        bg = mean(seg["bg"])
        tot = bg * W * H
        for r in seg["rects"]:
            tot += (mean(r["rgb"]) - bg) * r["w"] * r["h"]
        return tot / (W * H)
    if k == "grad":
        return (mean(seg["from"]) + mean(seg["to"])) / 2.0
    if k == "flash":
        return mean(seg["lo"] if state % 2 == 0 else seg["hi"])
    if k == "bar":
        length = W if seg["axis"] == "x" else H
        p = bar_position(seg, rel_s)
        overlap = max(0.0, min(p + seg["width"], length) - max(p, 0.0)) / float(length)
        return mean(seg["bg"]) + (mean(seg["rgb"]) - mean(seg["bg"])) * overlap
    raise ValueError(k)


def seg_pixel(seg, x, y, rel_s=0.0, state=0):
    """The colour of pixel (x, y) in this segment (the model the tests use to play the box)."""
    k = seg["kind"]
    if k == "rects":
        rgb = seg["bg"]
        for r in seg["rects"]:
            if r["x"] <= x < r["x"] + r["w"] and r["y"] <= y < r["y"] + r["h"]:
                rgb = r["rgb"]
        return rgb
    if k == "grad":
        f = ((x if seg["axis"] == "x" else y) + 0.5) / float(W if seg["axis"] == "x" else H)
        return [seg["from"][i] + (seg["to"][i] - seg["from"][i]) * f for i in range(3)]
    if k == "flash":
        return seg["lo"] if state % 2 == 0 else seg["hi"]
    if k == "bar":
        p = bar_position(seg, rel_s)
        c = x if seg["axis"] == "x" else y
        return seg["rgb"] if p <= c < p + seg["width"] else seg["bg"]
    raise ValueError(k)
