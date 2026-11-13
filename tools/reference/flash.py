"""Reference implementation (independent of the C++) of "Limit flashes", for the golden vectors in test/golden.txt. Mirrors flashLimitStep in include/core.h.

Once per output tick each zone's brightness (its brightest channel) may change by at most max_per_second * dt (dt counted as at most 100 ms, like adaptive smoothing).
A zone that would change more is shown at the limited brightness: with the new picture's colour scaled down (getting brighter, or darker towards a still lit colour),
or with the previous colour scaled down when the new picture is black there. The setting 1..100 is the time a change from black to full white may take, in tens of
milliseconds (50 = half a second): max_per_second = 25500 / setting.
"""
ZONES = 24
MAX_DT_MS = 100.0


def rate(setting):
    return 25500.0 / min(100, max(1, setting))


class FlashLimit:
    def __init__(self):
        self.valid = False
        self.z = [[0.0, 0.0, 0.0] for _ in range(ZONES)]

    def step(self, zones, dt_ms, max_per_second):
        """(shown zones, settled): settled = every zone shows its input (within half a level)."""
        zin = [[float(v) for v in z] for z in zones]
        if not self.valid:
            self.z = [list(z) for z in zin]
            self.valid = True
            return [list(z) for z in self.z], True
        step = max_per_second * min(max(dt_ms, 0.0), MAX_DT_MS) / 1000.0
        settled = True
        for i in range(ZONES):
            prev, new = self.z[i], zin[i]
            lp, ln = max(prev), max(new)
            if abs(ln - lp) <= step:
                self.z[i] = list(new)
                continue
            settled = False
            lvl = lp + step if ln > lp else lp - step
            if ln > 0.0:
                self.z[i] = [v * lvl / ln for v in new]
            else:
                self.z[i] = [v * lvl / lp for v in prev]
        if settled:
            settled = all(abs(a - b) <= 0.5 for z, w in zip(self.z, zin) for a, b in zip(z, w))
        return [list(z) for z in self.z], settled
