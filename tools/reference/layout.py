"""Reference implementation of the zone -> LED mapping (used to generate golden test vectors).

A *table* has one entry per LED: ``[zone_a, zone_b, f, d]``

* colour = zone_a * (1 - f/255) + zone_b * (f/255)
* d = how far the LED is beyond the TV along its edge, 0..255 = 0..2 picture widths (0 = lined up with the TV)

A LED at picture position (u, v) -- (0,0) top-left of the picture, (1,1) bottom-right, values outside are allowed --
takes its colour from the border zones along the nearest picture edge. LEDs beyond the TV's extent are blended toward the
colour of their end zone so they "smooth out" instead of repeating the edge colour (see apply_table).
"""
import math

from .protocol import EDGE_ZONES

OUT_MAX = 2.0            # d = 255 means this many picture widths beyond the TV
MIN_OUT_Q = 19           # smallest smoothing length (0.15 picture widths) so a strip hugging the TV is never smeared
DEAD = 0.02              # overshoot (picture widths) ignored before a LED counts as beyond the TV
DEFAULT_ASPECT = 9.0 / 16.0


def _interp(zones, t):
    """Interpolate along an edge: zone j sits at t = (j + 0.5) / len(zones)."""
    k = len(zones)
    pos = min(max(t * k - 0.5, 0.0), k - 1.0)
    j0 = int(math.floor(pos))
    j1 = min(j0 + 1, k - 1)
    return zones[j0], zones[j1], int(math.floor((pos - j0) * 255 + 0.5))


def edge_of(u, v):
    cu = min(max(u, 0.0), 1.0)
    cv = min(max(v, 0.0), 1.0)
    if 0.0 < u < 1.0 and 0.0 < v < 1.0:          # inside the picture: nearest edge
        d = {"top": v, "bottom": 1.0 - v, "left": u, "right": 1.0 - u}
        return min(d, key=d.get)
    if cv <= 0.0:
        return "top"
    if cv >= 1.0:
        return "bottom"
    if cu <= 0.0:
        return "left"
    return "right"


def uv_to_entry(u, v, aspect=DEFAULT_ASPECT, edge=None):
    """Table entry for a LED at picture position (u, v). aspect = picture height / width.

    edge ("top"/"right"/"bottom"/"left") forces the border to take the colour from (used for a straight run of LEDs whose
    side is known); by default it is the nearest edge of the picture."""
    edge = edge or edge_of(u, v)
    cu = min(max(u, 0.0), 1.0)
    cv = min(max(v, 0.0), 1.0)
    if edge in ("top", "bottom"):
        t, over = cu, max(0.0, -u, u - 1.0)               # overshoot along the edge, in picture widths
    else:
        t, over = cv, max(0.0, -v, v - 1.0) * aspect
    a, b, f = _interp(EDGE_ZONES[edge], t)
    over = max(0.0, over - DEAD)
    d = int(math.floor(min(over, OUT_MAX) / OUT_MAX * 255 + 0.5))
    return [a, b, f, d]


def table_linear(count, led0_side="left", span=None, aspect=DEFAULT_ASPECT, edge="top"):
    """One straight run of LEDs along one side of the picture.

    edge: which side the LEDs are on -- "top" (ceiling) uses the top row of the picture's colour map, "bottom" the bottom
    row, "left"/"right" the left/right column.
    span = (first, last): LED indexes lined up with the TV's two ends along that side (default: the run is exactly as long as
    the TV). LEDs outside it get d > 0.
    led0_side: "left"/"right" for a top or bottom run (as seen facing the TV), "left" = LED 0 at the top / "right" = LED 0 at
    the bottom for a left or right run.
    """
    if led0_side not in ("left", "right"):
        raise ValueError("led0_side must be 'left' or 'right'")
    if edge not in ("top", "right", "bottom", "left"):
        raise ValueError("edge must be top, right, bottom or left")
    first, last = span if span else (0, count - 1)
    width = max(1, last - first + 1)
    out = []
    for i in range(count):
        t = (i - first + 0.5) / width
        if led0_side == "right":
            t = 1.0 - t
        u, v = {"top": (t, 0.0), "bottom": (t, 1.0), "left": (0.0, t), "right": (1.0, t)}[edge]
        out.append(uv_to_entry(u, v, aspect, edge))
    return out


def side_counts(count, aspect=(16, 9), override=None):
    """(top, right, bottom, left) LED counts for a loop around the picture."""
    if override:
        t, r, b, l = override
        if t + r + b + l != count:
            raise ValueError("sides %s add up to %d, expected %d" % (list(override), t + r + b + l, count))
        return tuple(override)
    w, h = aspect
    top = int(math.floor(count * w / (2.0 * (w + h)) + 0.5))
    right = int(math.floor(count * h / (2.0 * (w + h)) + 0.5))
    return top, right, top, count - 2 * top - right


def table_loop(count, start_corner="top-left", direction="clockwise", sides=None, shift=0, aspect=(16, 9)):
    """LEDs running around the four edges of the picture (all lined up with the TV: d = 0)."""
    top, right, bottom, left = side_counts(count, aspect, sides)
    perimeter = []                                # clockwise from the top-left corner
    for edge, n in (("top", top), ("right", right), ("bottom", bottom), ("left", left)):
        zones = EDGE_ZONES[edge]
        for i in range(n):
            t = (i + 0.5) / n
            if edge in ("bottom", "left"):        # these edges are walked right->left / bottom->top
                t = 1.0 - t
            a, b, f = _interp(zones, t)
            perimeter.append([a, b, f, 0])
    offsets = {"top-left": 0, "top-right": top, "bottom-right": top + right, "bottom-left": top + right + bottom}
    if start_corner not in offsets:
        raise ValueError("start_corner must be one of %s" % ", ".join(sorted(offsets)))
    if direction not in ("clockwise", "counterclockwise"):
        raise ValueError("direction must be clockwise or counterclockwise")
    off = offsets[start_corner] + shift
    out = []
    for i in range(count):
        idx = (off + i) % count if direction == "clockwise" else (off - 1 - i) % count
        out.append(list(perimeter[idx]))
    return out


def zone_usage(table):
    """How many LEDs follow each of the 24 zones (an LED adds 1 - f/255 to zone a and f/255 to zone b). Mirrors zoneUsage() in src/core.cpp."""
    out = [0.0] * 24
    for a, b, f, _d in table:
        out[a] += 1.0 - f / 255.0
        out[b] += f / 255.0
    return out


def smoothstep(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3.0 - 2.0 * x)


def smooth_info(table, percent):
    """How the two tails of the strip (before / after the part lined up with the TV) ease to the average.

    Returns (first_zero, last_zero, len_start, len_end, zones_start, zones_end): the LEDs before first_zero ease over
    len_start (in d units), the LEDs after last_zero over len_end, each reaching its target colour at the end of the strip
    when percent = 100. A length of 0 means no smoothing. zones_start / zones_end are the zone indexes each tail eases
    toward: just the zone the strip's outermost LED shows (the corner zone for a strip along an edge); None if there is no tail."""
    n = len(table)
    zeros = [i for i, e in enumerate(table) if e[3] == 0]
    first_zero = zeros[0] if zeros else n
    last_zero = zeros[-1] if zeros else -1

    def tail(lo, hi):
        dmax = max((table[i][3] for i in range(lo, hi)), default=0)
        if percent <= 0 or dmax == 0:
            return 0
        return max(MIN_OUT_Q, int(math.floor(dmax * percent / 100.0 + 0.5)))

    def outer_zone(entry):
        return (entry[0] if entry[2] < 128 else entry[1],)
    zones_start = outer_zone(table[0]) if 0 < first_zero < n else None
    zones_end = outer_zone(table[-1]) if 0 <= last_zero < n - 1 else None
    return first_zero, last_zero, tail(0, first_zero), tail(last_zero + 1, n), zones_start, zones_end

def apply_table(table, zones, info):
    """Colours for every LED given 24 zone colours (each a 3-sequence of floats 0..255).

    LEDs beyond the TV (d > 0) ease toward the colour of the zone at their end of the strip (the corner zone), reaching it at the
    end of the strip. Colours from any other zone never reach them."""
    first_zero, last_zero, len_start, len_end, zones_start, zones_end = info

    def average(idx):
        zs = [zones[j] for j in idx] if idx else zones
        return [sum(z[c] for z in zs) / len(zs) for c in range(3)]
    avg_start, avg_end = average(zones_start), average(zones_end)
    out = []
    for i, (a, b, f, d) in enumerate(table):
        w = f / 255.0
        col = [zones[a][c] + (zones[b][c] - zones[a][c]) * w for c in range(3)]
        length = len_start if i < first_zero else (len_end if i > last_zero else 0)
        if length > 0 and d > 0:
            g = smoothstep(d / float(length))
            avg = avg_start if i < first_zero else avg_end
            col = [col[c] + (avg[c] - col[c]) * g for c in range(3)]
        out.append(col)
    return out