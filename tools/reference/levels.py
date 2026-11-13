"""Reference implementation (independent of the C++) of "Box levels", for the golden vectors in test/golden.txt. Mirrors boxLevelOffset / boxLevels in include/core.h.

The Sync Box broadcasts every colour value lowered by a constant and clipped at 0: out = max(0, code - offset), with offset 26 at Speed of Diffusion 100 % and 28 at 40 %
(measured with tools/sync_probe on 2026-11-04; the speeds in between are not measured and are interpolated). Hue and saturation are kept, and limited-range video is not
expanded (video white 235 arrives as about 209).

  mode 0 "as sent": nothing changes.
  mode 1 "restore": a lit value gets the offset back (x > 0 -> x + offset). This is the exact inverse for every value the box did not clip; 0 stays 0, because what was
         below the offset is gone for good (so the first lit level is offset + 1).
  mode 2 "restore, video range": as 1, then limited-range video (16-235) is stretched to the full range: (x - 16) * 255 / 219, clipped to 0..255.
"""


def offset(box_speed):
    s = min(100, max(10, box_speed))
    return 26.0 + 2.0 * (100 - s) / 60.0 if s >= 40 else 28.0


def apply(zones, mode, box_speed):
    if mode == 0:
        return [list(map(float, z)) for z in zones]
    off = offset(box_speed)
    out = []
    for z in zones:
        row = []
        for v in z:
            v = float(v)
            if v > 0.0:
                v += off
                if mode == 2:
                    v = (v - 16.0) * 255.0 / 219.0
            row.append(min(255.0, max(0.0, v)))
        out.append(row)
    return out
