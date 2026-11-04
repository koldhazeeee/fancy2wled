#!/usr/bin/env python3
"""The sync probe finds what a simulated Sync Box does.

    python test/test_sync_probe.py

A pretend box with known behaviour (a delay, a picture every 100 ms, 5 single sample points across each zone, 3 down it, an optional limited-range
output) is played the real test plan, its packets are written the way probe.py writes them, and analyze.py has to find the delay, the cadence, the
levels, the sample points and the motion delay again. If a browser is installed (Edge or Chrome) the page itself is also run headless against the real
probe server, to check that every pattern is drawn and logged and that the page's clock is mapped onto the probe's.
"""
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
sys.path.insert(0, os.path.join(ROOT, "tools", "sync_probe"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import analyze  # noqa: E402
import plan as planmod  # noqa: E402
from reference.protocol import encode_frame  # noqa: E402

checks = failed = 0


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print("FAIL: %s %s" % (name, detail))


PAGE_OFFSET_MS = 7000.0          # the page's clock is 7 s behind the probe's
T0 = 100.0                       # the probe second at which the plan starts


def simulate(plan, latency=0.12, pic_interval=0.1, packet_interval=0.073, jitter=0.02, limited=False, xs=(0, 64, 128, 192, 256), ys=(45, 135, 225), seed=1):
    """A recording as probe.py would write it, for a box that samples xs/ys inside each zone, shows the picture `latency` late and updates it every `pic_interval`."""
    rnd = random.Random(seed)
    events, starts, seg_state = [], [], []
    t = T0
    for i, seg in enumerate(plan):
        starts.append(t)
        events.append({"e": "seg", "i": i, "t": (t) * 1000.0 - PAGE_OFFSET_MS})
        marks = [(0, t)]
        if seg["kind"] == "flash":
            for k in range(1, seg["states"]):
                tk = t + k * seg["period_frames"] / planmod.REFRESH
                events.append({"e": "state", "i": i, "k": k, "t": tk * 1000.0 - PAGE_OFFSET_MS})
                marks.append((k, tk))
        seg_state.append(marks)
        t += seg["dur_ms"] / 1000.0
    end = t

    def zones_at(img_t):
        if img_t < T0 or img_t >= end:
            rgb = [(0.0, 0.0, 0.0)] * 24
        else:
            import bisect
            i = bisect.bisect_right(starts, img_t) - 1
            seg = plan[i]
            marks = seg_state[i]
            k, tk = [m for m in marks if m[1] <= img_t][-1]
            rel = img_t - starts[i]
            rgb = []
            for z in range(24):
                r0, c0 = divmod(z, 6)
                acc = [0.0, 0.0, 0.0]
                n = 0
                for dx in xs:
                    for dy in ys:
                        p = planmod.seg_pixel(seg, int(c0 * planmod.W / 6 + dx), int(r0 * planmod.H / 4 + dy), rel, k)
                        for c in range(3):
                            acc[c] += p[c]
                        n += 1
                rgb.append(tuple(a / n for a in acc))
        out = []
        for z in rgb:
            v = []
            for c in z:
                if limited:
                    c = 16 + c * 219.0 / 255.0
                v.append(int(max(0, min(255, round(c)))))
            out.append(tuple(v))
        return out

    pics, tp = [], T0 - 1.0                                  # the times the box makes a new picture: about every pic_interval, not exactly
    while tp < end + 2.0:
        pics.append(tp)
        tp += rnd.uniform(0.7, 1.3) * pic_interval
    packets, cache = [], {}
    k = 0
    import bisect
    while True:
        tn = (T0 - 1.0) + k * packet_interval
        if tn > end + 1.5:
            break
        tp = pics[bisect.bisect_right(pics, tn) - 1]
        if tp not in cache:
            cache[tp] = zones_at(tp - latency)
        packets.append([round(tn + rnd.uniform(0, jitter), 6), encode_frame(cache[tp]).hex()])
        k += 1
    sync = []
    for n in range(40):
        p0 = 1000.0 + n * 20.0
        rtt = 0.5 + (n % 7) * 0.4
        sync.append([p0, p0 + rtt, p0 + rtt / 2.0 + PAGE_OFFSET_MS + rnd.uniform(-0.05, 0.05)])
    sync_end = [[s[0] + 400000.0, s[1] + 400000.0, s[2] + 400000.0] for s in sync[:10]]
    return {"meta": {"tool": "simulated", "started": "now", "note": "", "box_settings": "", "quick": False, "complete": True},
            "plan": plan, "page": {"events": events, "sync": sync, "sync_end": sync_end, "env": {"innerWidth": 1920, "innerHeight": 1080, "devicePixelRatio": 1, "frames": 1, "long_frames": 0}},
            "packets": packets}


def run_simulation():
    plan = planmod.build_plan()
    check("plan: every suite builds and takes about six minutes", 300 < planmod.total_seconds(plan) < 420, planmod.total_seconds(plan))
    ids = [s["id"] for s in plan]
    check("plan: pattern ids are unique except the gaps", len(set(i for i in ids if i != "gap")) == len([i for i in ids if i != "gap"]))
    check("plan: --no-flash leaves the flicker out", not any(s["suite"] == "flash" for s in planmod.build_plan(flash=False)))

    for limited in (False, True):
        data = simulate(plan, latency=0.12, limited=limited)
        rep = analyze.analyse(analyze.Run(data))
        tag = "limited range" if limited else "full range"
        # delay
        d = rep["delay_overall"]["seconds"]
        check("%s: overall delay is found (0.12 s plus about half a picture interval)" % tag, d is not None and 0.12 <= d <= 0.26, d)
        f = rep["delay_flash"]
        check("%s: flash edges are found" % tag, f["found"] >= 20, f["found"])
        check("%s: the best-case flash delay is the box's delay" % tag, f["min"] is not None and 0.115 <= f["min"] <= 0.20, f["min"])
        check("%s: the median flash delay is later than the best case but not by more than a picture" % tag, f["median"] is not None and f["min"] <= f["median"] <= f["min"] + 0.14, (f["min"], f["median"]))
        check("%s: this box never shows a half-way picture" % tag, f["median_halfway_packets"] == 0, f["median_halfway_packets"])
        sr = rep["step"]
        check("%s: the step response is found" % tag, sr and sr["edges"] >= 10, sr and sr["edges"])
        if sr:
            check("%s: half of a change is shown after the delay plus about half a picture" % tag, sr["t50"] is not None and 0.12 <= sr["t50"] <= 0.30, sr["t50"])
            check("%s: a box that only samples rises within about a picture interval, change by change" % tag, sr["each_rise"] is not None and sr["each_rise"] <= 0.12, sr["each_rise"])
            check("%s: and shows almost no half-way pictures" % tag, sr["share_with_transit"] <= 0.2, sr["share_with_transit"])
        fl = {r["id"]: r for r in rep["flicker"]}
        check("%s: fast flicker is reported" % tag, "flash-1-bw" in fl and "flash-4-bw" in fl, list(fl))
        if "flash-1-bw" in fl:
            check("%s: a box that samples instantly follows a 30 Hz flicker with a mean near half" % tag, 0.25 <= fl["flash-1-bw"]["mean"] <= 0.75, fl["flash-1-bw"])
        # clock link
        check("%s: the page clock is mapped onto the probe clock" % tag, abs(analyze.Run(data).page_to_probe(1000000.0) - (1000.0 + PAGE_OFFSET_MS / 1000.0)) < 0.002)
        # cadence
        cm = rep["cadence_motion"]
        check("%s: packets arrive about 13.7 a second" % tag, cm and 12.5 <= cm["rate"] <= 15.0, cm and cm["rate"])
        check("%s: the picture changes about 10 times a second while a fast bar moves" % tag, cm and cm["change_rate"] and 8.0 <= cm["change_rate"] <= 11.0, cm and cm["change_rate"])
        # levels
        lv = rep["levels"]["summary"]
        check("%s: level table is complete" % tag, len(rep["levels"]["table"]) == len(planmod.GREYS), len(rep["levels"]["table"]))
        if limited:
            check("limited range: black reads 16 and white 235", abs(lv["black_out"] - 16) < 1 and abs(lv["white_out"] - 235) < 1, lv)
            check("limited range: the verdict says so", any("limited" in v for v in lv["verdict"]), lv["verdict"])
        else:
            check("full range: black reads 0 and white 255", lv["black_out"] < 1 and lv["white_out"] > 254, lv)
            check("full range: the verdict says so", any("full range" in v for v in lv["verdict"]), lv["verdict"])
            check("full range: a straight curve has a gamma of 1", lv["gamma_effective"] is not None and abs(lv["gamma_effective"] - 1.0) < 0.05, lv["gamma_effective"])
        # colours
        col = {r["id"]: r for r in rep["colours"]}
        want = col["R-255"]["out"] if "R-255" in col else None
        check("%s: pure red comes back as red" % tag, want and want[0] > want[1] + 100 and want[2] < want[0] / 2 + 20, want)
        # sampling points
        px, py = rep["points_x"], rep["points_y"]
        check("%s: 5 sample points across a zone" % tag, px and px["count"] == 5 and px["lines_with_that_count"] >= 5, px)
        if px and px["count"] == 5:
            check("%s: their positions are found to within a step" % tag, all(abs(o - e) <= 12 for o, e in zip(px["offsets"], (0, 64, 128, 192, 256))), px["offsets"])
        check("%s: 3 sample points down a zone" % tag, py and py["count"] == 3, py)
        if py and py["count"] == 3:
            check("%s: their positions are found to within a step" % tag, all(abs(o - e) <= 10 for o, e in zip(py["offsets"], (45, 135, 225))), py["offsets"])
        # motion
        mx, my = rep["motion_x"], rep["motion_y"]
        check("%s: the delay of a moving bar is found, horizontally" % tag, mx and 0.10 <= mx["delay"] <= 0.27, mx)
        check("%s: ... and vertically" % tag, my and 0.10 <= my["delay"] <= 0.27, my)

    # a box that averages whole areas instead of sampling points must not be reported as points
    data = simulate(planmod.build_plan(["grow"]), xs=tuple(range(0, 320, 20)), ys=tuple(range(0, 270, 30)))
    rep = analyze.analyse(analyze.Run(data))
    check("an area-averaging box is not reported as a few points", rep["points_x"] is None, rep["points_x"])
    gx = rep["geometry_x"]
    check("an area-averaging box is described as ramps over about a zone width", len(gx) >= 20 and all(g["kind"] == "ramp" and 250 <= g["window"][1] - g["window"][0] <= 340 for g in gx.values()),
          {z: (g["kind"], g["window"]) for z, g in list(gx.items())[:3]})

    # a longer delay is found too
    data = simulate(plan, latency=0.35)
    rep = analyze.analyse(analyze.Run(data))
    check("a 0.35 s delay is found", rep["delay_overall"]["seconds"] and 0.35 <= rep["delay_overall"]["seconds"] <= 0.5, rep["delay_overall"])
    check("... also by the flash edges", rep["delay_flash"]["min"] and 0.345 <= rep["delay_flash"]["min"] <= 0.43, rep["delay_flash"])

    # the report prints and the file round trip works
    lines = []
    analyze.print_report(rep, out=lines.append)
    text = "\n".join(lines)
    check("the report prints every section", all(s in text for s in ("DELAY", "CADENCE", "LEVELS", "COLOURS", "WHERE THE BOX LOOKS")), text[:200])
    check("the report is plain ASCII", all(ord(c) < 128 for c in text))
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "probe-x.json")
        json.dump(data, open(p, "w"))
        r2 = analyze.report_file(p, quiet=True)
        check("report_file writes the report next to the recording", os.path.exists(os.path.join(d, "probe-x-report.json")) and r2["packets"] == len(data["packets"]))
        p2 = os.path.join(d, "incomplete.json")
        json.dump({"meta": {}, "plan": plan, "page": None, "packets": []}, open(p2, "w"))
        check("an unfinished recording is refused politely", analyze.report_file(p2, quiet=True) is None)


def run_page_smoke():
    exe = None
    sys.path.insert(0, os.path.join(ROOT, "tools", "sync_probe"))
    import probe
    exe = probe.find_browser(None)
    if not exe:
        print("(no Edge or Chrome found: the page smoke test is skipped)")
        return
    tiny = [
        {"suite": "gap", "id": "lead", "kind": "rects", "dur_ms": 600, "bg": [0, 0, 0], "rects": []},
        {"suite": "levels", "id": "grey-128", "kind": "rects", "dur_ms": 500, "bg": [128, 128, 128], "rects": []},
        {"suite": "grow", "id": "growx-960", "kind": "rects", "dur_ms": 400, "bg": [0, 0, 0], "rects": [{"x": 0, "y": 0, "w": 960, "h": 1080, "rgb": [255, 255, 255]}]},
        {"suite": "grad", "id": "grad-x", "kind": "grad", "dur_ms": 300, "axis": "x", "from": [0, 0, 0], "to": [255, 255, 255]},
        {"suite": "flash", "id": "flash-6", "kind": "flash", "dur_ms": 600, "lo": [0, 0, 0], "hi": [255, 255, 255], "period_frames": 6, "states": 6},
        {"suite": "motion", "id": "bar-x-1", "kind": "bar", "dur_ms": 500, "axis": "x", "width": 240, "speed": 4000, "bg": [0, 0, 0], "rgb": [255, 255, 255]},
        {"suite": "gap", "id": "gap", "kind": "rects", "dur_ms": 300, "bg": [0, 0, 0], "rects": []},
    ]
    with tempfile.TemporaryDirectory() as d:
        pf, out = os.path.join(d, "plan.json"), os.path.join(d, "run.json")
        json.dump(tiny, open(pf, "w"))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "sync_probe", "probe.py"), "--plan-file", pf, "--headless", "--yes", "--no-analyze", "--port", "8791",
                            "--udp-port", "19877", "--out", out], capture_output=True, text=True, timeout=120)
        check("page: the probe finishes a run with the real page in a headless browser", r.returncode == 0 and os.path.exists(out), r.stdout[-300:] + r.stderr[-300:])
        if r.returncode != 0 or not os.path.exists(out):
            return
        run = json.load(open(out))
        pg = run["page"]
        segs = [e for e in pg["events"] if e["e"] == "seg"]
        check("page: every pattern was drawn and logged, in order", [e["i"] for e in segs] == list(range(len(tiny))), [e["i"] for e in segs])
        states = [e for e in pg["events"] if e["e"] == "state"]
        check("page: the flash pattern logged its 5 changes", len(states) == 5 and [e["k"] for e in states] == [1, 2, 3, 4, 5], len(states))
        if len(states) == 5:
            gaps = [states[k + 1]["t"] - states[k]["t"] for k in range(4)]
            check("page: flash changes come every 6 frames (100 ms at 60 Hz)", all(80 <= g <= 125 for g in gaps), gaps)
        check("page: the clock exchanges were done", len(pg["sync"]) == 40 and len(pg["sync_end"]) == 20)
        a = analyze.best_offset(pg["sync"])
        check("page: the clock round trip is short", a and a[2] < 30.0, a)
        durs = [(segs[k + 1]["t"] - segs[k]["t"]) for k in range(len(segs) - 1)]
        check("page: patterns last as long as planned (within 60 ms)", all(abs(du - tiny[k]["dur_ms"]) < 60 for k, du in enumerate(durs) if tiny[k]["kind"] != "flash"), durs)
        check("page: the window size and scale are reported", pg["env"].get("innerWidth", 0) >= 1800 and pg["env"].get("devicePixelRatio") == 1, pg["env"])


if __name__ == "__main__":
    run_simulation()
    run_page_smoke()
    print("%d checks passed, %d failed" % (checks - failed, failed))
    print("ALL PASSED" if not failed else "FAILED")
    sys.exit(1 if failed else 0)
