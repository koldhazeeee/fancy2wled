"""Reference implementation (independent of the C++) of the finishing stage of the output, for the golden vectors in test/golden.txt.

Mirrors finishColours in include/core.h + src/core.cpp: see there for the reasoning. It works on one target's LEDs after every temporal stage (motion smoothing or easing)
and just before the colours are rounded to bytes: a near-black cut-off, a deadband that holds a channel until the colour has moved more than N levels, and temporal dithering
that carries the rounding error to the next frame while the colours are moving.
"""
import math


class Finish:
    def __init__(self, n):
        self.n = n
        self.held = [[0.0, 0.0, 0.0] for _ in range(n)]
        self.resid = [[0.0, 0.0, 0.0] for _ in range(n)]
        self.dark = [False] * n
        self.margin = 1e9             # how close any decision came to its edge (the golden generator keeps clear of them, where float and double could disagree)

    def run(self, cur, deadband, black_cut, dither, moving, fresh):
        """cur: n x 3 floats. Returns n x 3 integers (0..255). fresh: the state is not valid (first frame, or the settings changed)."""
        out = []
        for i in range(self.n):
            x = [min(max(v, 0.0), 255.0) for v in cur[i]]
            m = max(x)
            if black_cut > 0:
                self.margin = min(self.margin, abs(m - black_cut), abs(m - (black_cut + 1)))
            was_dark = self.dark[i]
            if black_cut > 0:
                if fresh:
                    self.dark[i] = m < black_cut
                elif was_dark:
                    if m >= black_cut + 1:                       # one level of hysteresis: an LED that went dark needs to reach the cut-off plus one to come back
                        self.dark[i] = False
                elif m < black_cut:
                    self.dark[i] = True
            else:
                self.dark[i] = False
            if self.dark[i]:
                self.held[i] = [0.0, 0.0, 0.0]
                self.resid[i] = [0.0, 0.0, 0.0]
                out.append([0, 0, 0])
                continue
            restart = fresh or was_dark                          # coming back from dark: take the colour as it is, do not hold it against black
            row = []
            for c in range(3):
                if deadband > 0.0:
                    self.margin = min(self.margin, abs(x[c] - 1.0), abs(self.held[i][c] - 1.0), abs(abs(x[c] - self.held[i][c]) - deadband))
                if restart:
                    self.held[i][c] = x[c]
                    self.resid[i][c] = 0.0
                elif deadband > 0.0 and x[c] >= 1.0 and self.held[i][c] >= 1.0 and abs(x[c] - self.held[i][c]) <= deadband:
                    pass                                         # inside the deadband: keep what was shown
                else:
                    self.held[i][c] = x[c]                       # (a channel under one level is never held: a fade to black must reach black)
                v = self.held[i][c]
                if dither and moving:
                    t = v + self.resid[i][c]
                    o = math.floor(t + 0.5)
                    self.resid[i][c] = t - o
                    self.margin = min(self.margin, abs(t + 0.5 - round(t + 0.5)))
                else:
                    o = math.floor(v + 0.5)
                    self.resid[i][c] = 0.0
                    self.margin = min(self.margin, abs(v + 0.5 - round(v + 0.5)))
                row.append(int(min(max(o, 0), 255)))
            out.append(row)
        return out
