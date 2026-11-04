"""Decoder / encoder for the FancyLEDs Sync Box UDP broadcast.

The box broadcasts ~13 frames per second to <subnet>.255 on UDP port 19875.
Frame layout (86..93 bytes), see docs/PROTOCOL.md for how it was worked out:

    55 AA 5A n 00 b5 b6 b7 | KEY (len-83 bytes) | DATA (75 bytes)

    n  = total length - 5
    b5 = checksum: 167 + the sum of every byte except bytes 3 and 5, modulo 256 (holds on every frame of a real capture)
    b6 = a fixed function of n (the table in docs/PROTOCOL.md)
    b7 = the last byte of the key
    The decoder does not check b4..b7 on purpose: the relay keeps working if the box changes them.
    plaintext = DATA xor KEY (key repeats)  =  06 04 06 + 24 x RGB

The 24 RGB values are numbered as a 6 x 4 grid: zone = row * 6 + col, row 0 = top, col 0 = left.  Only the border zones feed the box's own strip.
Each zone is an exact average over one window of the picture, and the windows do not tile it evenly: see "Where each zone looks" in docs/PROTOCOL.md.
"""
import os

PORT = 19875
ZONES = 24
COLS = 6
ROWS = 4

_MAGIC = b"\x55\xaa\x5a"
_MARK = b"\x06\x04\x06"

# Border zones in screen order along each edge (corners appear on two edges).
EDGE_ZONES = {
    "top": [0, 1, 2, 3, 4, 5],          # left -> right
    "right": [5, 11, 17, 23],           # top -> bottom
    "bottom": [18, 19, 20, 21, 22, 23],  # left -> right
    "left": [0, 6, 12, 18],             # top -> bottom
}


def decode_frame(data):
    """Return a list of 24 (r, g, b) tuples, or None if `data` is not a valid sync frame."""
    n = len(data)
    if n < 86 or n > 93 or data[0:3] != _MAGIC or data[3] != n - 5:
        return None
    q = n - 83                       # key length, 3..10
    key = data[8:8 + q]
    body = data[8 + q:]
    pt = bytes(b ^ key[i % q] for i, b in enumerate(body))
    if len(pt) != 75 or pt[0:3] != _MARK:
        return None                  # built-in integrity check
    return [(pt[3 + i * 3], pt[4 + i * 3], pt[5 + i * 3]) for i in range(ZONES)]


OK, NEW_LAYOUT, UNREADABLE = 0, 1, 2


def frame_checksum_ok(data):
    """Header byte 5: 167 + the sum of every byte except bytes 3 and 5, modulo 256."""
    return len(data) >= 8 and ((167 + sum(data) - data[3] - data[5]) & 255) == data[5]


def classify_frame(data):
    """(class, checksum_ok, head, zones) for any datagram on the sync port. Mirrors classifyFrame() in src/core.cpp.

    OK            decodes (zones is the list of colours)
    NEW_LAYOUT    has the Sync Box framing (magic, length byte = size - 5) but the payload is not readable: head is the first
                  3 payload bytes after undoing the key when the size is one of the known 86-93 (else None)
    UNREADABLE    anything else
    """
    n = len(data)
    zones = decode_frame(data)
    if zones is not None:
        return OK, frame_checksum_ok(data), None, zones
    if 12 <= n <= 260 and data[0:3] == _MAGIC and data[3] == n - 5:
        head = None
        if 86 <= n <= 93:
            q = n - 83
            head = bytes(data[8 + q + i] ^ data[8 + (i % q)] for i in range(3))
        return NEW_LAYOUT, frame_checksum_ok(data), head, None
    return UNREADABLE, False, None, None


def encode_frame(zones, key=None):
    """Build a frame the way the box does. Used by tests and simulators."""
    if key is None:
        key = os.urandom(3 + os.urandom(1)[0] % 8)
    q = len(key)
    if not 3 <= q <= 10:
        raise ValueError("key must be 3..10 bytes")
    pt = _MARK + bytes(v for z in zones for v in z)
    body = bytes(b ^ key[i % q] for i, b in enumerate(pt))
    n = q + 83 - 5
    b6 = n - 0x1F if n % 2 else n - 0x1D       # observed relation (see the table in docs/PROTOCOL.md)
    frame = bytearray(_MAGIC + bytes([n, 0, 0, b6, key[-1]]) + key + body)
    frame[5] = (167 + sum(frame) - frame[3]) & 255          # byte 5 is still 0 here, so it adds nothing to the sum
    return bytes(frame)
