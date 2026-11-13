"""Reference implementation (independent of the C++) of the single-colour layout (a lamp or bulb that shows one colour), for the golden vectors in test/golden.txt.

Mirrors singleColour / singleMask / singleUsage in include/core.h. The 24 zones are not equally big: each is the average over a window of the picture (docs/PROTOCOL.md):
the top and bottom rows look at bands 300 x 60 px, the side zones of the middle rows at strips 60 x 240 px and the inner zones of the middle rows at windows 300 x 240 px.
An average over the picture weighs each zone by its window's area (relative to the 60 x 240 strips: 1.25, 1 and 5).

Modes: 0 average (by area), 1 dominant (by area times saturation squared, so the strongest colour wins over a grey background), 2 ambient (by area times darkness
squared, so the room's overall tone counts more than a few bright highlights), 3 lightest (the zone with the highest luma). Zone sets: 0 all, 1 top row, 2 right column,
3 bottom row, 4 left column.
"""
ZONES = 24


def weight(z):
    row, col = divmod(z, 6)
    if row in (0, 3):
        return 1.25
    return 1.0 if col in (0, 5) else 5.0


def mask(zones_set):
    if zones_set == 1:
        return [z for z in range(6)]
    if zones_set == 2:
        return [5, 11, 17, 23]
    if zones_set == 3:
        return [18 + z for z in range(6)]
    if zones_set == 4:
        return [0, 6, 12, 18]
    return list(range(ZONES))


def colour(zones, zone_list, mode):
    if not zone_list:
        return [0.0, 0.0, 0.0]
    if mode == 3:
        best, best_l = None, -1.0
        for z in zone_list:
            r, g, b = zones[z]
            lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
            if lum > best_l:
                best, best_l = z, lum
        return [float(v) for v in zones[best]]
    tot, acc = 0.0, [0.0, 0.0, 0.0]
    for z in zone_list:
        c = [float(v) for v in zones[z]]
        w = weight(z)
        if mode == 1:
            chroma = (max(c) - min(c)) / 255.0
            w *= chroma * chroma + 0.02
        elif mode == 2:
            dark = 1.05 - max(c) / 255.0
            w *= dark * dark
        tot += w
        for k in range(3):
            acc[k] += w * c[k]
    if tot <= 0.0:
        return [0.0, 0.0, 0.0]
    return [a / tot for a in acc]


def usage(zone_list, n):
    tot = sum(weight(z) for z in zone_list)
    out = [0.0] * ZONES
    if tot <= 0 or n <= 0:
        return out
    for z in zone_list:
        out[z] = n * weight(z) / tot
    return out
