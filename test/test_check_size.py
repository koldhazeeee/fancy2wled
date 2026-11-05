#!/usr/bin/env python3
"""tools/check_size.py reports the slot usage and fails above the limit.

    python test/test_check_size.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
TOOL = os.path.join(ROOT, "tools", "check_size.py")
checks = failed = 0


def check(name, ok, detail=""):
    global checks, failed
    checks += 1
    if not ok:
        failed += 1
        print("FAIL: %s %s" % (name, detail))


def run(*args, env=None):
    r = subprocess.run([sys.executable, TOOL] + list(args), capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


with tempfile.TemporaryDirectory() as d:
    csv = os.path.join(d, "parts.csv")
    with open(csv, "w") as f:
        f.write("# Name,   Type, SubType, Offset,  Size, Flags\nnvs, data, nvs, 0x9000, 0x5000,\napp0, app, ota_0, 0x10000, 0x100000,\napp1, app, ota_1, 0x110000, 0x100000,\n")
    slot = 0x100000                                              # a 1 MB slot makes the sizes easy to read

    def image(name, size):
        p = os.path.join(d, name, "firmware.bin")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(b"\xe9" + bytes(size - 1))
        return p

    small, mid, big, huge = image("small", slot // 3), image("mid", int(slot * 0.89)), image("big", int(slot * 0.95)), image("huge", slot + 10)
    rc, out = run("--partitions", csv, small)
    check("a third of the slot passes", rc == 0 and "33.3 %" in out and "| ok |" in out, out)
    rc, out = run("--partitions", csv, small, mid)
    check("89 % is still under the default limit of 90", rc == 0 and "89.0 %" in out, out)
    rc, out = run("--partitions", csv, small, big)
    check("95 % fails, and the output says which image", rc == 1 and "over the limit" in out and "95.0 %" in out and "FAILED" in out, out)
    rc, out = run("--partitions", csv, huge)
    check("an image bigger than the slot fails as too big", rc == 1 and "TOO BIG" in out, out)
    rc, out = run("--partitions", csv, "--limit", "85", mid)
    check("--limit changes the threshold", rc == 1, out)
    rc, out = run("--partitions", csv, os.path.join(d, "nothing.bin"))
    check("a missing file is an error", rc == 2 and "missing" in out, out)
    summary = os.path.join(d, "summary.md")
    rc, out = run("--partitions", csv, small, env=dict(os.environ, GITHUB_STEP_SUMMARY=summary))
    text = open(summary).read() if os.path.exists(summary) else ""
    check("on GitHub the table goes to the job summary", rc == 0 and "### Firmware size" in text and "| small |" in text, text)

# with the real partition table of this project: the slot is the 3264 KB the update has to fit
rc, out = run(os.path.join(ROOT, "platformio.ini"))
check("the project's own partition table gives a 3264 KB slot", "3264 KB" in out, out)
print("%d checks passed, %d failed" % (checks - failed, failed))
print("ALL PASSED" if not failed else "FAILED")
sys.exit(1 if failed else 0)
