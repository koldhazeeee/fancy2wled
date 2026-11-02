#!/usr/bin/env python3
"""Check a capture of the Sync Box (from capture_frames.py): does the relay's decoder understand every frame, and what exactly does the
stream look like? Compare two captures to see what a box firmware update changed.

    python tools/check_capture.py captures/box-....jsonl                       # report
    python tools/check_capture.py captures/box-....jsonl --save before.json    # keep its "fingerprint"
    python tools/check_capture.py captures/after.jsonl --compare before.json   # what changed? (a capture or a saved fingerprint)
    python tools/check_capture.py fancy2wled-diagnostics.json                  # a diagnostics file downloaded from the relay's setup page

Exit status: 0 = every sync frame decodes, 1 = some do not, 2 = no sync frames at all.
"""
import argparse
import collections
import json
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reference import protocol  # noqa: E402

SYNC_PORT = protocol.PORT
B6 = {81: 0x32, 82: 0x35, 83: 0x34, 84: 0x37, 85: 0x36, 86: 0x39, 87: 0x38, 88: 0x3B}   # header byte 6 as a function of byte 3


def diagnose(data):
    """Why the relay's decoder would reject a frame ('' = it decodes). Mirrors protocol.decode_frame / core.cpp decodeFrame."""
    n = len(data)
    if n < 86:
        return "too short (%d bytes, needs 86-93)" % n
    if n > 93:
        return "too long (%d bytes, needs 86-93)" % n
    if data[0:3] != b"\x55\xaa\x5a":
        return "different magic bytes (%s)" % data[0:3].hex()
    if data[3] != n - 5:
        return "length byte %d does not match the datagram size %d" % (data[3], n)
    return "" if protocol.decode_frame(data) is not None else "the plaintext no longer starts with 06 04 06 (layout or key scheme changed)"


def load(path):
    """A capture (.jsonl from capture_frames.py) or a diagnostics file downloaded from the relay's setup page (.json)."""
    if path.endswith(".json"):
        o = json.load(open(path))
        if o.get("format") == "fancy2wled-diagnostics":
            frames = [s["hex"] for s in o.get("relay", {}).get("samples", [])]
            if o.get("relay", {}).get("lastUnderstood"):
                frames.append(o["relay"]["lastUnderstood"]["hex"])
            rows = [{"t": i * 0.075, "port": SYNC_PORT, "src": "diag:0", "len": len(h) // 2, "hex": h} for i, h in enumerate(frames)]
            return {"note": "diagnostics file from the relay (firmware %s): %d odd frame(s) kept, counters %s" % (o.get("firmware", "?"), len(frames), o.get("relay", {}).get("frames")),
                    "started": "", "seconds": ""}, rows
    meta, rows = {}, []
    with open(path) as f:
        for line in f:
            o = json.loads(line)
            if "meta" in o:
                meta = o["meta"]
            else:
                rows.append(o)
    return meta, rows

def fingerprint(rows):
    """What the stream looks like, as a small dict you can compare with another one."""
    sync = [(r["t"], bytes.fromhex(r["hex"]), r["src"]) for r in rows if r["port"] == SYNC_PORT]
    fp = {"frames": len(sync)}
    if not sync:
        return fp
    reasons = collections.Counter(diagnose(d) or "ok" for _, d, _ in sync)
    fp["decoded"] = reasons.get("ok", 0)
    fp["failures"] = {k: v for k, v in reasons.items() if k != "ok"}
    classes = [(d,) + protocol.classify_frame(d)[:3] for _, d, _ in sync]          # (datagram, class, checksum_ok, head)
    odd = [c for c in classes if c[1] == protocol.NEW_LAYOUT]
    if any(c[1] != protocol.OK for c in classes):
        fp["unknown_layout"] = {"frames": len(odd), "of_undecoded": sum(1 for c in classes if c[1] != protocol.OK),
                                "sizes": sorted({len(c[0]) for c in odd}), "checksum_ok": sum(1 for c in odd if c[2]),
                                "payload_starts_with": sorted({c[3].hex() for c in odd if c[3]})}
    lens = collections.Counter(len(d) for _, d, _ in sync)
    fp["lengths"] = sorted(lens)
    fp["magic"] = sorted({d[0:3].hex() for _, d, _ in sync})
    fp["byte4"] = sorted({d[4] for _, d, _ in sync if len(d) > 4})
    fp["length_byte_is_len_minus_5"] = all(len(d) > 3 and d[3] == len(d) - 5 for _, d, _ in sync)
    fp["byte6_by_length"] = {str(l): sorted({d[6] for _, d, _ in sync if len(d) == l}) for l in sorted(lens) if l - 5 in B6}
    fp["byte6_matches_known_table"] = all(d[6] == B6.get(len(d) - 5) for _, d, _ in sync if len(d) - 5 in B6)
    fp["source_ports"] = sorted({int(s.rsplit(":", 1)[1]) for _, _, s in sync})
    ok = [(t, d) for t, d, _ in sync if not diagnose(d)]
    if ok:
        q = collections.Counter(len(d) - 83 for _, d in ok)
        fp["key_lengths"] = sorted(q)
        fp["byte7_equals_last_key_byte"] = round(sum(1 for _, d in ok if d[7] == d[8 + len(d) - 83 - 1]) / len(ok), 4)
        fp["marker"] = sorted({bytes(a ^ d[8 + (i % (len(d) - 83))] for i, a in enumerate(d[8 + len(d) - 83:][:3])).hex() for _, d in ok})
        fp["data_bytes"] = sorted({len(d) - 8 - (len(d) - 83) for _, d in ok})
        zones = [protocol.decode_frame(d) for _, d in ok]
        fp["zones"] = len(zones[0])
        flat = [c for z in zones for px in z for c in px]
        fp["zone_value_min"], fp["zone_value_max"] = min(flat), max(flat)
        fp["frames_all_black_pct"] = round(100.0 * sum(1 for z in zones if all(c == 0 for px in z for c in px)) / len(zones), 2)
        fp["identical_to_previous_pct"] = round(100.0 * sum(1 for a, b in zip(zones, zones[1:]) if a == b) / max(1, len(zones) - 1), 2)
        # header byte 5 is a checksum: 167 + the sum of every byte except bytes 3 and 5, modulo 256
        fp["checksum_holds_pct"] = round(100.0 * sum(1 for _, d in ok if (167 + sum(d) - d[3] - d[5]) & 255 == d[5]) / len(ok), 2)
        # how the box smooths its own colours: while a colour settles, each change is this fraction of the one before
        # (the relay's Reaction boost assumes 0.6; see docs/LATENCY.md)
        ratios = []
        for z in range(len(zones[0])):
            for c in range(3):
                s = [fr_[z][c] for fr_ in zones]
                d = [s[i + 1] - s[i] for i in range(len(s) - 1)]
                ratios += [d[i + 1] / d[i] for i in range(len(d) - 1) if abs(d[i]) >= 12 and d[i] * d[i + 1] > 0 and 0.2 < d[i + 1] / d[i] < 1.0]
        fp["box_smoothing_ratio"] = round(st.median(ratios), 3) if len(ratios) >= 50 else None
    ts = [t for t, _, _ in sync]
    gaps = [(b - a) * 1000 for a, b in zip(ts, ts[1:])]
    fp["fps"] = round((len(ts) - 1) / (ts[-1] - ts[0]), 2) if ts[-1] > ts[0] else 0
    fp["gap_ms_median"] = round(st.median(gaps), 1) if gaps else 0
    tuya = [r for r in rows if r["port"] in (6667, 6666)]
    fp["tuya_announcements"] = {"count": len(tuya), "sizes": sorted({r["len"] for r in tuya}), "ports": sorted({r["port"] for r in tuya})}
    return fp


def show(fp, meta):
    print("capture: %s, %s s%s" % (meta.get("started", "?"), meta.get("seconds", "?"), (", box firmware " + meta["box_firmware"]) if meta.get("box_firmware") else ""))
    if meta.get("note"):
        print("note:    " + meta["note"])
    n = fp.get("frames", 0)
    if not n:
        print("\nNO SYNC FRAMES on UDP %d. Is the box on, in sync mode, on this computer's network? (Tuya announcements: %s)" % (SYNC_PORT, fp.get("tuya_announcements")))
        return
    print("\nsync stream (UDP %d): %d frames, %.1f fps, median gap %s ms (this computer's Wi-Fi may batch broadcasts)" % (SYNC_PORT, n, fp["fps"], fp["gap_ms_median"]))
    bad = n - fp["decoded"]
    print("  decoded by the relay's decoder: %d of %d%s" % (fp["decoded"], n, "" if not bad else "   <-- %d NOT understood" % bad))
    for why, c in sorted(fp["failures"].items(), key=lambda kv: -kv[1]):
        print("     %5d x  %s" % (c, why))
    u = fp.get("unknown_layout")
    if u:
        if u["frames"]:
            print("  %d of the %d frames that do not decode still have the Sync Box framing (magic bytes, length byte) but a payload we cannot read: sizes %s, payload starts with %s, checksum valid on %d of them."
                  " That looks like a NEW PAYLOAD LAYOUT, not corruption." % (u["frames"], u["of_undecoded"], u["sizes"], u["payload_starts_with"] or "(size unknown)", u["checksum_ok"]))
        else:
            print("  none of the %d frames that do not decode has the Sync Box framing: the header itself has changed, or this is not Sync Box data." % u["of_undecoded"])
    print("  datagram sizes: %s   magic: %s   header byte 4: %s   length byte = size - 5: %s" % (fp["lengths"], fp["magic"], fp["byte4"], fp["length_byte_is_len_minus_5"]))
    print("  header byte 6 follows the known size table: %s   source UDP port(s): %s" % (fp["byte6_matches_known_table"], fp["source_ports"]))
    if "marker" in fp:
        print("  key lengths: %s   data bytes: %s   marker after decoding: %s   zones: %d" % (fp["key_lengths"], fp["data_bytes"], fp["marker"], fp["zones"]))
        print("  header byte 7 = last key byte in %.1f%% of frames; header byte 5 is a checksum that holds in %.1f%% of frames" % (100 * fp["byte7_equals_last_key_byte"], fp["checksum_holds_pct"]))
        print("  the box smooths its own colours: each change is %s of the one before (the Reaction boost assumes 0.6)" % fp["box_smoothing_ratio"])
        print("  colours: %d..%d   all-black frames %.1f%%   identical to the previous frame %.1f%%" % (fp["zone_value_min"], fp["zone_value_max"], fp["frames_all_black_pct"], fp["identical_to_previous_pct"]))
    print("  Tuya announcements (UDP 6667/6666): %s" % fp["tuya_announcements"])


def compare(a, b):
    """Print what differs between two fingerprints (a = before, b = after). Returns the number of differences."""
    ignore = {"frames", "fps", "gap_ms_median", "decoded", "frames_all_black_pct", "identical_to_previous_pct", "zone_value_min", "zone_value_max",
              "box_smoothing_ratio"}
    diffs = 0
    for k in sorted(set(a) | set(b)):
        if k in ignore:
            continue
        if a.get(k) != b.get(k):
            diffs += 1
            print("  CHANGED  %-28s before: %s\n           %-28s after:  %s" % (k, a.get(k), "", b.get(k)))
    ra, rb = a.get("box_smoothing_ratio"), b.get("box_smoothing_ratio")
    if ra and rb and abs(ra - rb) > 0.08:
        diffs += 1
        print("  CHANGED  the box smooths its colours differently: %.2f -> %.2f per step (the Reaction boost assumes 0.6)" % (ra, rb))
    if a.get("fps") and b.get("fps") and abs(a["fps"] - b["fps"]) / a["fps"] > 0.25:
        diffs += 1
        print("  CHANGED  frame rate: %.1f -> %.1f fps" % (a["fps"], b["fps"]))
    return diffs


def read_fp(path):
    if path.endswith(".json") and json.load(open(path)).get("format") != "fancy2wled-diagnostics":
        return json.load(open(path)), {}
    meta, rows = load(path)
    return fingerprint(rows), meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--save", help="write this capture's fingerprint to a small .json file")
    ap.add_argument("--compare", help="a capture (.jsonl) or fingerprint (.json) from BEFORE; print what changed")
    a = ap.parse_args()
    fp, meta = read_fp(a.capture)
    show(fp, meta)
    if a.save:
        json.dump(fp, open(a.save, "w"), indent=1, sort_keys=True)
        print("\nfingerprint saved to " + a.save)
    if a.compare:
        before, _ = read_fp(a.compare)
        print("\ncomparison with %s:" % a.compare)
        d = compare(before, fp)
        print("  nothing that matters changed" if not d else "  %d difference(s) above" % d)
    if not fp.get("frames"):
        return 2
    return 0 if fp["decoded"] == fp["frames"] else 1


if __name__ == "__main__":
    sys.exit(main())
