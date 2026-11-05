#!/usr/bin/env python3
"""Report how full the firmware slot is, and fail before it overflows.

    python tools/check_size.py .pio/build/atoms3/firmware.bin .pio/build/atoms3-phonecal/firmware.bin
    python tools/check_size.py --limit 85 file.bin

An over-the-air update writes the image into one of two app slots (app0 / app1 in the partition table named by board_build.partitions in platformio.ini, 3264 KB
each for default_8MB.csv). An image bigger than the slot cannot be updated over Wi-Fi, and the build only says so when it is already too late, so CI prints the
sizes (and, on GitHub, puts them in the job summary) and fails when an image uses more than --limit percent of the slot (default 90).
"""
import argparse
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
FALLBACK_SLOT = 0x330000          # default_8MB.csv, used only if the partition table cannot be found


def partitions_name():
    try:
        with open(os.path.join(ROOT, "platformio.ini")) as f:
            m = re.search(r"^board_build\.partitions\s*=\s*(\S+)", f.read(), re.M)
        return m.group(1) if m else None
    except OSError:
        return None


def slot_size(csv_path=None):
    """(bytes, where it came from) for the app slot: the size of app0 in the partition table."""
    candidates = []
    if csv_path:
        candidates.append(csv_path)
    name = partitions_name()
    if name:
        candidates.append(os.path.join(ROOT, name))
        candidates.append(os.path.join(os.path.expanduser("~"), ".platformio", "packages", "framework-arduinoespressif32", "tools", "partitions", name))
    for path in candidates:
        if not os.path.isfile(path):
            continue
        with open(path) as f:
            for line in f:
                parts = [p.strip() for p in line.split("#")[0].split(",")]
                if len(parts) >= 5 and parts[0] == "app0":
                    return int(parts[4], 0), path
    return FALLBACK_SLOT, "built-in default (partition table not found)"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="firmware images (.bin)")
    ap.add_argument("--limit", type=float, default=90.0, help="fail above this percentage of the slot (default %(default)s)")
    ap.add_argument("--partitions", help="partition table CSV (default: the one named in platformio.ini)")
    a = ap.parse_args(argv)
    slot, src = slot_size(a.partitions)
    rows, bad = [], False
    for path in a.files:
        if not os.path.isfile(path):
            print("missing: %s" % path)
            return 2
        size = os.path.getsize(path)
        pct = 100.0 * size / slot
        status = "TOO BIG" if size > slot else ("over the limit" if pct > a.limit else "ok")
        bad = bad or status != "ok"
        rows.append((os.path.basename(os.path.dirname(path)) or path, size, pct, status))
    lines = ["| image | size | slot | used | |", "|---|---|---|---|---|"]
    for name, size, pct, status in rows:
        lines.append("| %s | %d KB | %d KB | %.1f %% | %s |" % (name, (size + 1023) // 1024, slot // 1024, pct, status))
    text = "\n".join(lines)
    print(text)
    print("(app slot %d bytes, from %s; limit %.0f %%)" % (slot, src, a.limit))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write("### Firmware size\n\n%s\n\nApp slot %d KB, limit %.0f %%.\n\n" % (text, slot // 1024, a.limit))
    if bad:
        print("FAILED: an image is above %.0f %% of its slot (over-the-air updates need it to fit with room to grow)" % a.limit)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
