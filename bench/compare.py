#!/usr/bin/env python3
"""Compare two benchmark result files and print a table (Markdown with --md).

    python bench/compare.py bench/results/board-before.json bench/results/board-after.json [--md]

Cases that exist under another name in the "before" file are matched through ALIAS (lead_boost_packet does the work lead_boost_72ch did, with the factor worked out once).
"""
import json
import sys

ALIAS = {"lead_boost_packet": "lead_boost_72ch"}


def load(p):
    return json.load(open(p))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    md = "--md" in sys.argv
    b, a = load(args[0]), load(args[1])
    unit_div, unit = (1000.0, "us") if "esp32" in b.get("platform", "") else (1.0, "ns")
    rows = []
    for k, v in a["cases"].items():
        kb = ALIAS.get(k, k)
        if kb in b["cases"]:
            rows.append((k, b["cases"][kb]["min"] / unit_div, v["min"] / unit_div))
    if md:
        print("| case | before (%s) | after (%s) | speed-up |" % (unit, unit))
        print("|---|---:|---:|---:|")
        for k, x, y in rows:
            print("| `%s` | %.2f | %.2f | %.2fx |" % (k, x, y, x / y))
    else:
        print("%-24s %12s %12s %9s" % ("case", "before (%s)" % unit, "after (%s)" % unit, "speed-up"))
        for k, x, y in rows:
            print("%-24s %12.2f %12.2f %8.2fx" % (k, x, y, x / y))


if __name__ == "__main__":
    main()
