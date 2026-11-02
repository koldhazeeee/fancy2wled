#!/usr/bin/env python3
"""Pretend to be a Sync Box: broadcasts synthetic frames on UDP 19875 so you can try the relay without the box.

    python tools/sim_box.py                    # moving rainbow, broadcast on the local network
    python tools/sim_box.py --target 192.168.1.77 --pattern pulse

Patterns: rainbow (colours sweep left->right), pulse (whole picture breathes), edges (left edge red, right edge blue).
"""
import argparse
import math
import socket
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reference import protocol  # noqa: E402


def hsv(h, s, v):
    h = h % 1.0
    i = int(h * 6)
    f = h * 6 - i
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i % 6]
    return int(r * 255), int(g * 255), int(b * 255)


def frame(pattern, t):
    zones = []
    for z in range(24):
        col, row = z % 6, z // 6
        if pattern == "pulse":
            v = 0.5 + 0.5 * math.sin(t * 2)
            zones.append(hsv(0.6, 0.8, 0.15 + 0.7 * v))
        elif pattern == "edges":
            zones.append((200, 0, 0) if col == 0 else (0, 0, 200) if col == 5 else (0, 0, 0))
        else:
            zones.append(hsv(col / 6.0 + t * 0.1, 1.0, 0.8))
    return zones


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", default="255.255.255.255", help="broadcast address or the relay's IP")
    ap.add_argument("--fps", type=float, default=13.5)
    ap.add_argument("--pattern", choices=("rainbow", "pulse", "edges"), default="rainbow")
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    print("sending %s frames to %s:%d at %.1f fps (Ctrl+C to stop)" % (a.pattern, a.target, protocol.PORT, a.fps))
    t0 = time.time()
    try:
        while True:
            s.sendto(protocol.encode_frame(frame(a.pattern, time.time() - t0)), (a.target, protocol.PORT))
            time.sleep(1.0 / a.fps)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
