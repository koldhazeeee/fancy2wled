"""Reference implementation (independent of the C++) of colour temperature and vibrance, for the golden vectors in test/golden.txt.
Mirrors colourTempMultipliers / toneZones in include/core.h.

Colour temperature: the setting t (-100 warm .. +50 cool, 0 = neutral) is a white point of 6500 K * 2^(t / 100) (3250 K .. 9190 K). Its RGB comes from Tanner Helland's
fit of the blackbody colours, divided by the RGB of 6500 K (so 0 changes nothing) and then by the largest of the three (so no channel is ever raised and nothing clips).

Vibrance v (-1 .. 1): every zone's colour moves away from (v > 0) or toward (v < 0) the middle of its brightest and darkest channel, by a factor 1 + v * (1 - saturation),
saturation = (max - min) / 255. Dull colours change most, strong ones hardly; max + min stays the same, so the brightness does not jump (until a channel reaches 0 or 255).
"""
import math


def helland(kelvin):
    t = kelvin / 100.0
    if t <= 66:
        r = 255.0
        g = 99.4708025861 * math.log(t) - 161.1195681661
    else:
        r = 329.698727446 * math.pow(t - 60, -0.1332047592)
        g = 288.1221695283 * math.pow(t - 60, -0.0755148492)
    if t >= 66:
        b = 255.0
    elif t <= 19:
        b = 0.0
    else:
        b = 138.5177312231 * math.log(t - 10) - 305.0447927307
    return [min(255.0, max(0.0, v)) for v in (r, g, b)]


def multipliers(temp):
    temp = min(50, max(-100, temp))
    if temp == 0:
        return [1.0, 1.0, 1.0]
    k = 6500.0 * math.pow(2.0, temp / 100.0)
    a, ref = helland(k), helland(6500.0)
    m = [x / y for x, y in zip(a, ref)]
    top = max(m)
    return [x / top for x in m]


def tone(zones, white, vibrance):
    out = []
    for z in zones:
        c = [float(v) for v in z]
        if vibrance != 0.0:
            mx, mn = max(c), min(c)
            sat = (mx - mn) / 255.0
            f = 1.0 + vibrance * (1.0 - sat)
            mid = (mx + mn) / 2.0
            c = [mid + (v - mid) * f for v in c]
        c = [min(255.0, max(0.0, v * w)) for v, w in zip(c, white)]
        out.append(c)
    return out
