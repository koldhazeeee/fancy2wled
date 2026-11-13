"""Reference implementation (independent of the C++) of adaptive smoothing, for the golden vectors in test/golden.txt and for tools/smoothness.py.

The One Euro filter (Casiez, Roussel and Vogel, CHI 2012): a low-pass filter whose cutoff rises with the speed of the change. Still or slowly drifting colours get a
low cutoff (heavily smoothed, so noise and one-level flicker disappear); fast changes get a high one (they pass with little delay). Mirrors oneEuro* in include/core.h:
  * it works on the 24 zone colours (72 channels), once per output tick, with the real time since the previous tick;
  * the speed is estimated per zone (the fastest of its three channels, low-passed at DCUTOFF), so the three channels of a zone share one cutoff and its hue does not
    shift while it changes;
  * one setting, calm 1..100 (0 = off), sets the cutoff at rest and how strongly speed raises it (see params()).
"""
import math

ZONES = 24
DCUTOFF = 1.0          # Hz: the low-pass on the speed estimate
MAX_DT_MS = 100.0      # a longer gap between ticks (the relay stops ticking while the picture is still) counts as this much: the new picture only just arrived


def params(calm):
    """(minimum cutoff in Hz, beta in Hz per level/second) for the setting 1..100: 1 = quick (little smoothing at rest), 100 = very calm."""
    c = min(100, max(1, calm)) / 100.0
    min_cutoff = 4.0 * math.pow(0.01, c)          # 3.8 Hz at 1 .. 0.04 Hz at 100 (geometric)
    beta = 0.02 * math.pow(0.1, c)                # 0.02 .. 0.002: how much the speed raises the cutoff
    return min_cutoff, beta


def alpha(cutoff, dt):
    tau = 1.0 / (2.0 * math.pi * cutoff)
    return 1.0 / (1.0 + tau / dt)


class OneEuro:
    def __init__(self):
        self.valid = False
        self.x = [[0.0, 0.0, 0.0] for _ in range(ZONES)]       # the filtered colours
        self.prev = [[0.0, 0.0, 0.0] for _ in range(ZONES)]    # the previous input
        self.speed = [0.0] * ZONES                              # the low-passed speed of each zone (levels per second)

    def reset(self, zones):
        self.x = [list(map(float, z)) for z in zones]
        self.prev = [list(map(float, z)) for z in zones]
        self.speed = [0.0] * ZONES
        self.valid = True

    def step(self, zones, dt_ms, calm):
        """Filter one tick: zones = the picture now, dt_ms = time since the previous tick. Returns (filtered zones, settled): settled = within half a level everywhere."""
        if not self.valid or dt_ms <= 0.0:
            if not self.valid:
                self.reset(zones)
            return [list(z) for z in self.x], self._settled(zones)
        dt = min(dt_ms, MAX_DT_MS) / 1000.0
        min_cutoff, beta = params(calm)
        ad = alpha(DCUTOFF, dt)
        for z in range(ZONES):
            raw = max(abs(float(zones[z][c]) - self.prev[z][c]) for c in range(3)) / dt
            self.speed[z] += ad * (raw - self.speed[z])
            a = alpha(min_cutoff + beta * self.speed[z], dt)
            for c in range(3):
                v = float(zones[z][c])
                self.x[z][c] += a * (v - self.x[z][c])
                self.prev[z][c] = v
        return [list(z) for z in self.x], self._settled(zones)

    def _settled(self, zones):
        return all(abs(self.x[z][c] - float(zones[z][c])) <= 0.5 for z in range(ZONES) for c in range(3))
