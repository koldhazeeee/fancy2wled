"""Reference implementation of the Govee LAN streaming packets (the "razer" command), independent of the C++ code in src/core.cpp.

A Govee light on the LAN (LAN control switched on in the Govee Home app) takes JSON over UDP on port 4003:
    {"msg":{"cmd":"razer","data":{"pt":"<base64 of a binary packet>"}}}
Binary packets:   BB 00 01 B1 <0|1> <xor>              streaming off / on
                  BB <len16> B0 <stretch> <n> <r g b>*n <xor>      a frame: n colours, stretched evenly over the whole light
with len16 = 2 + 3n (big endian) and xor = the XOR of every byte before it. Entry 0 is the light's right-hand end (seen from the front)
on the H61C3 neon rope; checked on a real one, which also stretches any n it is given over its full length.
"""
import base64
import json


def _xor(b):
    x = 0
    for v in b:
        x ^= v
    return x


def _wrap(packet):
    pt = base64.b64encode(packet).decode()
    return json.dumps({"msg": {"cmd": "razer", "data": {"pt": pt}}}, separators=(",", ":"))


def enable_json(on):
    b = bytes([0xBB, 0x00, 0x01, 0xB1, 1 if on else 0])
    return _wrap(b + bytes([_xor(b)]))


def frame_json(rgb):
    """rgb: a list of (r, g, b) tuples, 1..60 of them."""
    n = len(rgb)
    b = bytes([0xBB]) + (2 + 3 * n).to_bytes(2, "big") + bytes([0xB0, 0x00, n]) + bytes(c for px in rgb for c in px)
    return _wrap(b + bytes([_xor(b)]))
