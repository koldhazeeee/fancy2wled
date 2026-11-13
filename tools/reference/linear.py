"""Reference implementation (independent of the C++) of blending in linear light, for the test in test/test_core.cpp (golden vectors LINMIX in test/golden.txt).

The zones (0..255, gamma encoded) are decoded to linear light with a plain gamma of 2.2, the LEDs are mixed from them (and blurred) in linear light, and the result is
encoded back. Mixing in linear light keeps the brightness between two zones (half red and half green is a brighter yellow than when the gamma-encoded values are averaged).
The C++ uses tables for speed (decode: 256 entries, encode: 256 entries indexed by the square root of the linear value); this version uses the exact powers.
"""
GAMMA = 2.2


def decode(v):
    v = min(255.0, max(0.0, float(v)))
    return 255.0 * (v / 255.0) ** GAMMA


def encode(v):
    v = min(255.0, max(0.0, float(v)))
    return 255.0 * (v / 255.0) ** (1.0 / GAMMA)
