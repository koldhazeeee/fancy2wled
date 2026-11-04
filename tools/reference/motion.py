"""Reference implementation (independent of the C++) of motion smoothing and the blur along a strip, for the golden vectors in test/golden.txt.

Mirrors motionPush / motionAt / motionDelayMs and blurHalfWidth / blurColours in include/core.h + src/core.cpp: see there for the reasoning.
"""
import math

KNOTS = 4
ZONES = 24


def _round(x):
    return int(math.floor(x + 0.5))


class Motion:
    def __init__(self):
        self.k = []                  # [(ms, zones)] the latest pictures, the newest last; zones = 24 (r, g, b) lists
        self.last_raw = 0
        self.period = 100.0

    def push(self, now, zones):
        zones = [list(z) for z in zones]
        if self.k and ((now - self.last_raw) & 0xFFFFFFFF) < 25:   # a burst: the newer picture replaces the one just before it
            self.k[-1] = (self.k[-1][0], zones)
            self.last_raw = now
            return
        if not self.k or ((now - self.last_raw) & 0xFFFFFFFF) > 1500:   # the first picture, or the box was silent: start over
            self.k = []
            self.period = 100.0
            t = now
        else:
            gap = float(now - self.last_raw)
            if 40.0 < gap < 160.0:
                self.period = 0.95 * self.period + 0.05 * gap
            prev_t = self.k[-1][0]
            raw_rel = float(now - prev_t)
            rel = self.period + 0.15 * (raw_rel - self.period)
            if rel < 1.0:
                rel = 1.0
            t = prev_t + _round(rel)
        if len(self.k) == KNOTS:
            self.k.pop(0)
        self.k.append((t, zones))
        self.last_raw = now

    def at(self, time):
        """(zones, reached): the picture to show at `time`, and whether the newest picture has been reached"""
        if not self.k:
            return [[0.0, 0.0, 0.0] for _ in range(ZONES)], True
        last_t, last_z = self.k[-1]
        if time - last_t >= 0:
            return [list(z) for z in last_z], True
        if time - self.k[0][0] <= 0:
            return [list(z) for z in self.k[0][1]], self._same_from(0)
        for i, ((t0, z0), (t1, z1)) in enumerate(zip(self.k, self.k[1:])):
            a = time - t0
            span = t1 - t0
            if 0 <= a < span:
                if self._same_from(i):                         # a picture that does not change: settled
                    return [list(z) for z in z0], True
                f = a / float(span)
                return [[p + (q - p) * f for p, q in zip(za, zb)] for za, zb in zip(z0, z1)], False
        return [list(z) for z in last_z], True

    def _same_from(self, i):
        return all(self.k[j][1] == self.k[j + 1][1] for j in range(i, len(self.k) - 1))

    def delay(self, percent):
        p = min(160.0, max(50.0, self.period))
        return p * min(100, max(0, percent)) / 100.0


def blur_half_width(blur_leds):
    if blur_leds <= 0:
        return 0
    sigma = blur_leds * 0.5
    h = int(math.floor((math.sqrt(4.0 * sigma * sigma + 1.0) - 1.0) * 0.5 + 0.5))
    return max(1, h)


def _box(colours, h):
    m = len(colours)
    out = []
    for i in range(m):
        acc = [0.0, 0.0, 0.0]
        for d in range(-h, h + 1):
            j = min(m - 1, max(0, i + d))
            for c in range(3):
                acc[c] += colours[j][c]
        out.append([v / (2 * h + 1) for v in acc])
    return out


def blur_colours(rgb, first, last, blur_leds):
    """rgb: list of [r, g, b] for the whole strip; returns the blurred list (LEDs outside first..last untouched; first < 0 = the whole strip)"""
    n = len(rgb)
    h = blur_half_width(blur_leds)
    if n < 2 or h < 1:
        return [list(c) for c in rgb]
    a, b = 0, n - 1
    if first >= 0:
        a = min(first, n - 1)
        if last >= first:
            b = min(last, n - 1)
    if b - a + 1 < 2:
        return [list(c) for c in rgb]
    region = [list(c) for c in rgb[a:b + 1]]
    for _ in range(3):
        region = _box(region, h)
    return [list(c) for c in rgb[:a]] + region + [list(c) for c in rgb[b + 1:]]
