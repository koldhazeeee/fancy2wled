"""Reference implementation (independent of the C++) of the scene-cut test used by "Snap on scene cuts", for the golden vectors in test/golden.txt.

A packet is a cut when at least CUT_ZONES of the 24 zones changed by more than CUT_LEVELS (the largest of the three channels) since the previous packet: half the picture
jumping at once. Mirrors isSceneCut in include/core.h. On the recordings in tools/reference/data it finds the test patterns' 16-17 changes in about 40 s, and 9 in 28 s of a
TV programme (the box at 75 %).
"""
CUT_ZONES = 12
CUT_LEVELS = 60


def is_scene_cut(prev, now):
    big = 0
    for a, b in zip(prev, now):
        if max(abs(int(x) - int(y)) for x, y in zip(a, b)) > CUT_LEVELS:
            big += 1
    return big >= CUT_ZONES
