#!/usr/bin/env python3
"""Show known test pictures on a laptop whose HDMI output feeds the Sync Box, and record what the box broadcasts about them.

    python tools/sync_probe/probe.py                       # the whole plan, about 6 minutes
    python tools/sync_probe/probe.py --quick               # about half as long, coarser
    python tools/sync_probe/probe.py --suites levels,colors --note "box brightness 50"
    python tools/sync_probe/probe.py --dry-run             # only list what would be shown

How it works: this script starts a tiny web server on this computer and opens pattern.html full screen in a browser of its own (a fresh profile, so your
own browser is left alone). The page draws the test plan (plan.py) and reports when it drew each picture. At the same time this script listens to the
box's UDP broadcast (port 19875) and stamps every packet with this computer's clock. Because the pictures and the packets are timed by the same
computer, the analysis (analyze.py) can tell the real delay of the box, how it samples the picture, and how it changes colours.

Set the laptop up first: Windows display mode "Duplicate" (the box's HDMI input and the laptop screen show the same picture), 1920 x 1080, 100 % scale,
HDR off, Night light off, and "RGB 4:4:4 Full" pixel format in the graphics software if it has one. Note the box's own settings (brightness, mode ...) with
--box-settings: they change what the box does.

WARNING: the "flash" tests flicker the whole screen between black and white at up to 30 times a second. Do not run them (use --no-flash), or look away,
if flashing lights are a problem for you or anyone who can see the screen. The lights that follow the box flash too.

The recording goes to captures/probe-<date time>.json (next to tools/, which git ignores). It holds only timestamps and the box's packets, no
addresses. Nothing is sent to the box or the network: this only listens.
"""
import argparse
import datetime
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import plan as planmod  # noqa: E402

UDP_PORT = 19875
BROWSERS = ['C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
            'C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
            '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
            '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome']


class State:
    def __init__(self, plan):
        self.plan_json = json.dumps(plan).encode()
        self.page = open(os.path.join(HERE, "pattern.html"), "rb").read()
        self.page_log = None
        self.done = threading.Event()
        self.plan_requested = threading.Event()
        self.progress = (0, len(plan))


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, *args):
            pass

        def _send(self, body, ctype="application/json"):
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            t = time.perf_counter()
            path, _, query = self.path.partition("?")
            if path == "/sync":
                self._send(json.dumps({"s": t * 1000.0}).encode())
            elif path == "/plan":
                state.plan_requested.set()
                self._send(state.plan_json)
            elif path == "/progress":
                q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
                try:
                    state.progress = (int(q.get("i", 0)), int(q.get("n", 0)))
                except ValueError:
                    pass
                self._send(b"{}")
            elif path in ("/", "/index.html"):
                self._send(state.page, "text/html; charset=utf-8")
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path.split("?")[0] == "/log":
                n = int(self.headers.get("Content-Length", "0"))
                try:
                    state.page_log = json.loads(self.rfile.read(n).decode("utf-8"))
                except ValueError:
                    state.page_log = None
                self._send(b"{}")
                state.done.set()
            else:
                self.send_error(404)

    return Handler


def udp_loop(port, packets, stop, errors):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    try:
        s.bind(("", port))
    except OSError as e:
        errors.append("cannot listen on UDP %d: %s (another program may be using it)" % (port, e))
        return
    s.settimeout(0.2)
    while not stop.is_set():
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        packets.append((time.perf_counter(), data))
    s.close()


def find_browser(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for p in BROWSERS:
        if os.path.exists(p):
            return p
    return None


def launch_browser(exe, url, profile, headless=False):
    args = [exe, "--user-data-dir=" + profile, "--no-first-run", "--no-default-browser-check", "--window-position=0,0", "--force-device-scale-factor=1",
            "--force-color-profile=srgb", "--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows",
            "--disable-features=Translate"]
    if headless:                                   # for the automated test: no window
        args += ["--headless=new", "--window-size=1920,1080", "--disable-gpu", url]
    else:
        args += ["--kiosk", url, "--edge-kiosk-type=fullscreen"]
    return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def kill_tree(proc):
    """Close only the browser this script started (and its helpers), never other browser windows."""
    if proc is None or proc.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suites", default=",".join(planmod.DEFAULT_SUITES), help="comma separated, of: " + ", ".join(planmod.ALL_SUITES) + " (default %(default)s)")
    ap.add_argument("--quick", action="store_true", help="a shorter, coarser plan")
    ap.add_argument("--no-flash", action="store_true", help="leave out the flicker tests")
    ap.add_argument("--scan-step", type=int, default=4, help="pixels per step of the optional 'scan' suite (default %(default)s)")
    ap.add_argument("--note", default="", help="what you did / what is on the box (stored in the file)")
    ap.add_argument("--box-settings", default="", help="the box's own settings: brightness, mode ... (stored in the file)")
    ap.add_argument("--label", default="", help="short word for the file name")
    ap.add_argument("--out", help="file to write (default captures/probe-<date time>[-<label>].json)")
    ap.add_argument("--port", type=int, default=8765, help="local web server port (default %(default)s)")
    ap.add_argument("--udp-port", type=int, default=UDP_PORT)
    ap.add_argument("--browser", help="path of a Chrome or Edge to use")
    ap.add_argument("--no-browser", action="store_true", help="do not open a browser: open the printed address yourself and press F11")
    ap.add_argument("--no-analyze", action="store_true", help="only record, do not print the analysis at the end")
    ap.add_argument("--dry-run", action="store_true", help="list the plan and stop")
    ap.add_argument("--plan-file", help="use the plan in this JSON file instead of building one (for tests)")
    ap.add_argument("--headless", action="store_true", help="run the browser without a window (for tests)")
    ap.add_argument("--yes", action="store_true", help="do not wait for Enter before starting")
    a = ap.parse_args()

    suites = [s.strip() for s in a.suites.split(",") if s.strip()]
    if a.plan_file:
        with open(a.plan_file) as f:
            plan = json.load(f)
    else:
        plan = planmod.build_plan(suites, quick=a.quick, scan_step=a.scan_step, flash=not a.no_flash)
    total = planmod.total_seconds(plan)
    print("plan: %d patterns in %d group(s), %.0f s (%.1f min)" % (len(plan), len({s["suite"] for s in plan if s["suite"] != "gap"}), total, total / 60.0))
    if a.dry_run:
        for s in plan:
            print("  %6.1f s  %-8s %s" % (s["dur_ms"] / 1000.0, s["suite"], s["id"]))
        return 0
    if "flash" in suites and not a.no_flash:
        print("WARNING: this plan flickers the screen between black and white at up to 30 times a second (and the lights that follow the box).")
        print("         Stop now (Ctrl+C) or use --no-flash if that is a problem for anyone who can see the screen.")
    if not a.yes and sys.stdin.isatty():
        input("Press Enter to start (the screen goes full screen and black for 3 s first) ...")

    state = State(plan)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(state))
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % a.port

    packets, errors, stop = [], [], threading.Event()
    udp = threading.Thread(target=udp_loop, args=(a.udp_port, packets, stop, errors), daemon=True)
    udp.start()
    time.sleep(0.3)
    if errors:
        print(errors[0])
        srv.shutdown()
        return 1

    proc, profile = None, None
    if a.no_browser:
        print("open %s in a browser on the screen that feeds the box, press F11 (full screen), and leave it alone" % url)
    else:
        exe = find_browser(a.browser)
        if not exe:
            print("no Edge or Chrome found: install one, pass --browser PATH, or use --no-browser and open %s yourself" % url)
            stop.set()
            srv.shutdown()
            return 1
        profile = tempfile.mkdtemp(prefix="fwb-probe-")
        proc = launch_browser(exe, url, profile, a.headless)

    started = datetime.datetime.now()
    t0 = time.perf_counter()
    ok = True
    try:
        if not state.plan_requested.wait(60):
            print("the browser did not open the pattern page within 60 s")
            ok = False
        last_print = 0.0
        while ok and not state.done.wait(1.0):
            now = time.perf_counter() - t0
            if now - last_print >= 5.0:
                i, n = state.progress
                print("  pattern %d of %d, %d box packets so far, %.0f s" % (i + 1, n, len(packets), now))
                last_print = now
            if now > total + 120:
                print("timed out waiting for the page to finish")
                ok = False
        time.sleep(1.5)                          # packets still on their way
    except KeyboardInterrupt:
        print("stopped")
        ok = False
    finally:
        stop.set()
        udp.join(2)
        kill_tree(proc)
        srv.shutdown()
        if profile:
            shutil.rmtree(profile, ignore_errors=True)

    out = a.out
    if not out:
        folder = os.path.join(HERE, "..", "..", "captures")
        os.makedirs(folder, exist_ok=True)
        out = os.path.join(folder, "probe-%s%s.json" % (started.strftime("%Y%m%d-%H%M%S"), ("-" + a.label) if a.label else ""))
    run = {"meta": {"tool": "sync_probe 1", "started": started.isoformat(timespec="seconds"), "note": a.note, "box_settings": a.box_settings,
                    "suites": suites, "quick": a.quick, "complete": bool(ok and state.page_log), "udp_port": a.udp_port},
           "plan": plan, "page": state.page_log, "packets": [[round(t, 6), d.hex()] for t, d in packets]}
    with open(out, "w", newline="\n") as f:
        json.dump(run, f, separators=(",", ":"))
    print("%d box packets recorded; saved %s" % (len(packets), os.path.normpath(out)))
    if not packets:
        print("no packets heard: is the box on this network, and is this computer's firewall letting UDP 19875 in?")
    if not ok or not state.page_log:
        print("the run did not finish, so it cannot be analysed")
        return 1
    if not a.no_analyze:
        import analyze
        analyze.report_file(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
