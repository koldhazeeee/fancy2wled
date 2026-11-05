"""Reference implementation (independent of the C++) of the Sync Box speed detector, for the golden vectors in test/golden.txt.

Mirrors speedPush / speedResult in include/core.h + src/core.cpp: see there for the reasoning. In short: the box moves each zone channel a fraction s of the
way to the new picture with every packet, so after a sudden change the second step is (1 - s) times the first. Over many channels that start to move together,
the least squares slope of the second step on the first is q = 1 - s.

Real video is not a clean cut: the picture often keeps moving during the second step, which pushes that estimate anywhere. The clean changes all agree on q, the
polluted ones scatter. So the answer is the most common value among the latest estimates (a vote in bins of 0.05, the box's speeds being multiples of 5 %), not their
median, and it is only called sure when that value clearly wins. Measured on a real box at 75 % showing normal video: 27 % of the changes voted for the true value,
12 % for the runner-up; a median with a spread limit was wrong 10 times in 82.
"""
import math

ZONES = 24
RING = 24
MIN_EVENTS = 10
MIN_STEP = 24        # a first step smaller than this is too small to measure
QUIET = 2            # the channel must have moved at most this much on the packet before (otherwise it was already moving: not the start of a change)
FLOOR = 30           # the box's black cut-off distorts colours near black
MIN_CHANNELS = 6     # channels that must take part in one estimate (with fewer, any value fits: three channels always agree with some q)
MIN_PEAK = 5         # votes the winning value needs
PEAK_SHARE_DEN = 4    # ... and at least 1 / this of all the estimates (a quarter)
PEAK_RATIO = 2       # ... and it must have this many times the votes of the runner-up
SHOW_RUN = 3         # the answer is shown once the vote has been sure of it this many estimates in a row
CHANGE_RUN = 5       # ... and an answer that is shown is replaced by a different one only after this many in a row


def steps_for_gap(gap_ms):
    """Same rule as boxStepsForGap() in core.cpp."""
    if gap_ms > 600:
        return 0
    n = int(gap_ms / 73.0 + 0.35)
    return 1 if n < 1 else n


class Detector:
    def __init__(self):
        self.frames = []      # the last four packets, the newest last: (zones, steps since the packet before)
        self.q = []           # the latest estimates of q = 1 - s (a ring of RING entries)
        self.events = 0       # how many estimates were made since the start
        self.shown = -1       # the answer the page shows (the vote's, held steady); -1 = none yet
        self.cand = -1        # the answer the vote is sure of right now ...
        self.run = 0          # ... and for how many estimates in a row

    def push(self, zones, steps):
        self.frames.append(([list(z) for z in zones], steps))
        if len(self.frames) > 4:
            self.frames.pop(0)
        if len(self.frames) < 4:
            return
        if self.frames[1][1] != 1 or self.frames[2][1] != 1 or self.frames[3][1] != 1:   # a lost packet in between would hide steps
            return
        f0, f1, f2, f3 = (f[0] for f in self.frames)
        num = den = used = 0
        for z in range(ZONES):
            for c in range(3):
                y0, y1, y2 = f1[z][c], f2[z][c], f3[z][c]
                if abs(f0[z][c] - y0) > QUIET:
                    continue
                d1, d2 = y1 - y0, y2 - y1
                if abs(d1) < MIN_STEP or min(y0, y1, y2) < FLOOR:
                    continue
                num += d1 * d2
                den += d1 * d1
                used += 1
        if used >= MIN_CHANNELS and den > 0:
            self.events += 1
            self.q.append(num / den)
            if len(self.q) > RING:
                self.q.pop(0)
            self._settle()

    def _settle(self):
        _, pct, sure, _, _ = self.result()
        if not sure:
            self.run = 0
            return
        if pct == self.cand:
            self.run += 1
        else:
            self.cand, self.run = pct, 1
        if self.run >= (SHOW_RUN if self.shown < 0 or self.cand == self.shown else CHANGE_RUN):
            self.shown = self.cand

    def result(self):
        """(estimates in the ring, speed percent of the winning value or -1, sure, votes of the winner, votes of the runner-up)."""
        n = len(self.q)
        if n < MIN_EVENTS:
            return n, -1, False, 0, 0
        votes = {}
        for q in self.q:
            b = int(math.floor(q * 20.0 + 0.5))                            # bin of 0.05: speed 100 % (q 0) ... 10 % (q 0.9)
            if 0 <= b <= 18:                                               # an estimate outside that (a negative one is plainly polluted) votes for nobody
                votes[b] = votes.get(b, 0) + 1
        if not votes:
            return n, -1, False, 0, 0
        ranked = sorted(votes.items(), key=lambda kv: (-kv[1], kv[0]))
        peak = ranked[0][1]
        second = ranked[1][1] if len(ranked) > 1 else 0
        sure = peak >= MIN_PEAK and peak * PEAK_SHARE_DEN >= n and peak >= PEAK_RATIO * second
        return n, 100 - 5 * ranked[0][0], sure, peak, second


def simulate_box(targets, speed, rng, loss=0.0, gap_ms=73, jitter=0):
    """What the box sends when the picture jumps through `targets` (a list of (hold_packets, zones)): y = round(y + s * (target - y)) per packet.

    Returns [(ms, zones)] with some packets dropped (the box still moves on) and the arrival times of the rest."""
    y = [[float(v) for v in z] for z in targets[0][1]]
    out = []
    t = 10000
    for hold, tgt in targets:
        for _ in range(hold):
            for z in range(ZONES):
                for c in range(3):
                    y[z][c] = y[z][c] + speed * (tgt[z][c] - y[z][c])
            t += gap_ms + (rng.randrange(-jitter, jitter + 1) if jitter else 0)
            if rng.random() >= loss:
                out.append((t, [[int(v + 0.5) for v in z] for z in y]))
    return out
