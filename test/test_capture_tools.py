#!/usr/bin/env python3
"""Tests for tools/capture_frames.py and tools/check_capture.py (the "did the Sync Box firmware change?" tools).

    python test/test_capture_tools.py
"""
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tools")
sys.path.insert(0, TOOLS)
from reference import protocol  # noqa: E402

passed = failed = 0


def check(name, cond, extra=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL: %s  %s" % (name, extra))


def run(*args):
    p = subprocess.run([sys.executable, "-I", *args], capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def synthetic_frames(n=400, seed=7):
    """Frames shaped like the box's: the colours move 40 % of the way to a new target every frame."""
    rnd = random.Random(seed)
    y = [[0.0] * 3 for _ in range(24)]
    tgt = [[0] * 3 for _ in range(24)]
    out = []
    for i in range(n):
        if i % 12 == 0:
            tgt = [[rnd.choice((0, 0, 60, 120, 200)) for _ in range(3)] for _ in range(24)]
        y = [[0.6 * y[z][c] + 0.4 * tgt[z][c] for c in range(3)] for z in range(24)]
        out.append(protocol.encode_frame([tuple(int(round(v)) for v in z) for z in y]))
    return out


def write_capture(path, frames, port=protocol.PORT, tuya=3, mutate=None, note="synthetic"):
    with open(path, "w", newline="\n") as f:
        f.write(json.dumps({"meta": {"started": "2026-01-01T00:00:00", "seconds": len(frames) * 0.075, "note": note}}) + "\n")
        for i, d in enumerate(frames):
            if mutate:
                d = mutate(d)
            f.write(json.dumps({"t": round(i * 0.075, 4), "port": port, "src": "192.0.2.9:19876", "len": len(d), "hex": d.hex()}) + "\n")
        for i in range(tuya):
            f.write(json.dumps({"t": i * 5.7, "port": 6667, "src": "192.0.2.9:50000", "len": 204, "hex": "00" * 204}) + "\n")


def checksum_fix(d):
    d = bytearray(d)
    d[5] = (167 + sum(d) - d[3] - d[5]) & 255
    return bytes(d)


def marker_change(d):          # the payload marker 06 04 06 becomes 08 06 06 (as a bigger grid might)
    q = len(d) - 83
    d = bytearray(d)
    for i, b in enumerate(b"\x08\x06\x06"):
        d[8 + q + i] = b ^ d[8 + (i % q)]
    return checksum_fix(d)


def main():
    frames = synthetic_frames()
    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.jsonl")
        write_capture(good, frames)
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), good, "--save", os.path.join(tmp, "good.json"))
        check("check_capture: a good capture passes (exit 0)", rc == 0, out)
        check("check_capture: says every frame decodes", "decoded by the relay's decoder: 400 of 400" in out, out)
        check("check_capture: finds all key lengths 3..10, the marker and 24 zones", "key lengths: [3, 4, 5, 6, 7, 8, 9, 10]" in out and "060406" in out and "zones: 24" in out, out)
        check("check_capture: the checksum rule holds on every frame", "checksum that holds in 100.0%" in out, out)
        fp = json.load(open(os.path.join(tmp, "good.json")))
        check("check_capture: measures the box's smoothing (0.6)", fp.get("box_smoothing_ratio") and 0.55 < fp["box_smoothing_ratio"] < 0.65, fp.get("box_smoothing_ratio"))
        check("check_capture: counts the Tuya announcements", fp["tuya_announcements"]["count"] == 3)

        bad = os.path.join(tmp, "bad.jsonl")
        write_capture(bad, frames, mutate=marker_change)
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), bad)
        check("check_capture: a changed payload layout fails (exit 1) and says why", rc == 1 and "NOT understood" in out and "plaintext no longer starts with 06 04 06" in out, out)
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), bad, "--compare", os.path.join(tmp, "good.json"))
        check("check_capture: --compare shows what changed", "CHANGED  marker" in out and "060406" in out and "080606" in out, out)
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), good, "--compare", os.path.join(tmp, "good.json"))
        check("check_capture: --compare of identical streams reports nothing", "nothing that matters changed" in out, out)

        shifted = os.path.join(tmp, "port.jsonl")
        write_capture(shifted, frames, port=19999)
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), shifted)
        check("check_capture: no frames on the sync port -> exit 2 with advice", rc == 2 and "NO SYNC FRAMES" in out, out)

        wrong_magic = os.path.join(tmp, "magic.jsonl")
        write_capture(wrong_magic, frames, mutate=lambda d: b"\x56" + d[1:])
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), wrong_magic)
        check("check_capture: different magic bytes are named", rc == 1 and "different magic bytes" in out, out)

        # a layout change is described precisely: the payload start and the size are reported
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), bad)
        check("check_capture: names the new payload start and says it is a new layout, not corruption", "NEW PAYLOAD LAYOUT" in out and "080606" in out and "checksum valid on 400" in out, out)

        def grown(d):                  # 8 more zones: the size leaves the known 86-93, framing and checksum stay valid
            q = len(d) - 83
            pt = bytes(b ^ d[8 + (i % q)] for i, b in enumerate(d[8 + q:])) + bytes(24)
            o = bytearray(d[:8 + q]) + bytes(b ^ d[8 + (i % q)] for i, b in enumerate(pt))
            o[3] = len(o) - 5
            return checksum_fix(bytes(o))
        big = os.path.join(tmp, "big.jsonl")
        write_capture(big, frames[:50], mutate=grown)
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), big)
        check("check_capture: a bigger payload (outside the known sizes) is still recognised as the Sync Box framing", rc == 1 and "NEW PAYLOAD LAYOUT" in out and "(size unknown)" in out, out)

        # a diagnostics file downloaded from the relay's setup page can be analysed directly
        diag = {"format": "fancy2wled-diagnostics", "version": 1, "firmware": "0.1.0",
                "relay": {"frames": {"understood": 12, "newLayout": 300, "unreadable": 0},
                          "samples": [{"ageMs": 100, "class": "x", "len": len(d), "checksumOk": True, "hex": d.hex()} for d in (marker_change(frames[0]), marker_change(frames[1]))],
                          "lastUnderstood": {"ageMs": 9000, "len": len(frames[2]), "hex": frames[2].hex()}}}
        dpath = os.path.join(tmp, "fancy2wled-diagnostics.json")
        json.dump(diag, open(dpath, "w"))
        rc, out = run(os.path.join(TOOLS, "check_capture.py"), dpath)
        check("check_capture: reads a diagnostics file from the relay and finds the new layout in it", "diagnostics file from the relay" in out and "NEW PAYLOAD LAYOUT" in out and "080606" in out and "decoded by the relay's decoder: 1 of 3" in out, out)

        # capture_frames.py: record real datagrams arriving on a UDP port (loopback)
        outfile = os.path.join(tmp, "rec.jsonl")
        port = 29875 + random.randrange(500)
        proc = subprocess.Popen([sys.executable, "-I", os.path.join(TOOLS, "capture_frames.py"), "--seconds", "3", "--ports", str(port), "--out", outfile,
                                 "--note", "test", "--box-firmware", "x 1.0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(1.2)
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sent = frames[:6]
        for d in sent:
            tx.sendto(d, ("127.0.0.1", port))
            time.sleep(0.05)
        out, _ = proc.communicate(timeout=30)
        lines = [json.loads(l) for l in open(outfile)]
        meta, rows = lines[0]["meta"], lines[1:]
        check("capture_frames: writes a description line first (note and firmware)", meta.get("note") == "test" and meta.get("box_firmware") == "x 1.0" and meta.get("ports") == [port], str(meta))
        check("capture_frames: records every datagram, byte for byte, in order", [r["hex"] for r in rows] == [d.hex() for d in sent], "%d rows" % len(rows))
        check("capture_frames: rows carry time, port, source and length", all(set(r) >= {"t", "port", "src", "len", "hex"} and r["port"] == port and r["len"] == len(bytes.fromhex(r["hex"])) for r in rows))
        check("capture_frames: the file replays through check_capture", True)   # (the ports differ from 19875, so only the format is exercised above)

    print("%d checks passed, %d failed" % (passed, failed))
    print("SOME FAILED" if failed else "ALL PASSED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
