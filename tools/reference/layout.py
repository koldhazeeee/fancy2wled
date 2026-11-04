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


# Where along its edge each zone looks, as a fraction of the picture width (top, bottom) or height (left, right): the middle of the window the box
# averages for that zone (measured with tools/sync_probe on 2026-11-04 (a white rectangle growing across a known picture): each zone is an exact average over a window and the windows do not tile the picture evenly).
POS_ACROSS = [0.0797, 0.2359, 0.3922, 0.5797, 0.7359, 0.8922]      # top and bottom edge, left -> right
POS_DOWN = [0.0287, 0.3356, 0.6134, 0.9736]                         # left and right edge, top -> bottom
EDGE_POS = {"top": POS_ACROSS, "bottom": POS_ACROSS, "left": POS_DOWN, "right": POS_DOWN}


def _interp(edge, t):
    """Blend the two zones either side of position t along an edge (before the first / after the last zone: that zone alone). Returns (zone a, zone b, f)."""
    zones, p = EDGE_ZONES[edge], EDGE_POS[edge]
    k = len(zones)
    if t <= p[0]:
        return zones[0], zones[1], 0
    if t >= p[k - 1]:
        return zones[k - 1], zones[k - 1], 0
    j0 = 0
    while j0 + 2 < k and t >= p[j0 + 1]:
        j0 += 1
    f = int(math.floor((t - p[j0]) / (p[j0 + 1] - p[j0]) * 255 + 0.5))
    return zones[j0], zones[j0 + 1], min(255, max(0, f))


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
    a, b, f = _interp(edge, t)
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
        for i in range(n):
            t = (i + 0.5) / n
            if edge in ("bottom", "left"):        # these edges are walked right->left / bottom->top
                t = 1.0 - t
            a, b, f = _interp(edge, t)
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

def _to_hsv(c):
    r, g, b = c
    mx, mn = max(r, g, b), min(r, g, b)
    d = mx - mn
    v = mx / 255.0
    s = d / mx if mx > 0 else 0.0
    if d <= 0:
        return 0.0, s, v
    if mx == r:
        h = 60.0 * (((g - b) / d) % 6.0)
    elif mx == g:
        h = 60.0 * ((b - r) / d + 2.0)
    else:
        h = 60.0 * ((r - g) / d + 4.0)
    return h, s, v


def _from_hsv(h, s, v):
    h = h % 360.0
    c = v * s
    x = c * (1.0 - abs((h / 60.0) % 2.0 - 1.0))
    m = v - c
    sector = [(c, x, 0), (x, c, 0), (0, c, x), (0, x, c), (x, 0, c), (c, 0, x)][int(h // 60.0) % 6]
    return [(p + m) * 255.0 for p in sector]


def blend_vivid(a, b, w):
    """Blend two colours along the colour wheel: the hue takes the shorter way round, saturation and brightness blend straight; a grey or black end has
    no hue of its own and takes the other end's. Mirrors vividBlend() in src/core.cpp."""
    ha, sa, va = _to_hsv(a)
    hb, sb, vb = _to_hsv(b)
    if va < 0.02 and vb >= 0.02:
        sa = sb
    elif vb < 0.02 and va >= 0.02:
        sb = sa
    da, db = sa >= 0.04 and va >= 0.02, sb >= 0.04 and vb >= 0.02
    if da and db:
        dh = hb - ha
        if dh > 180.0:
            dh -= 360.0
        elif dh <= -180.0:
            dh += 360.0
        h = ha + dh * w
    elif da:
        h = ha
    elif db:
        h = hb
    else:
        h = 0.0
    return _from_hsv(h, sa + (sb - sa) * w, va + (vb - va) * w)


def apply_table(table, zones, info, vivid=False):
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
        if vivid and a != b and f not in (0, 255):
            col = blend_vivid(zones[a], zones[b], w)
        else:
            col = [zones[a][c] + (zones[b][c] - zones[a][c]) * w for c in range(3)]
        length = len_start if i < first_zero else (len_end if i > last_zero else 0)
        if length > 0 and d > 0:
            g = smoothstep(d / float(length))
            avg = avg_start if i < first_zero else avg_end
            col = [col[c] + (avg[c] - col[c]) * g for c in range(3)]
        out.append(col)
    return out

def sharpen(table, percent):
    """How soft the change from one zone to the next is. f runs from 0 (at the first zone's centre) to 255 (at the next one's), the line between them at 127.5.
    percent 0 leaves it, 100 snaps every LED to the zone it sits in, in between the change is squeezed into a narrower band. Entries that use one zone are
    left alone. Mirrors sharpenTable() in src/core.cpp."""
    out = []
    for a, b, f, d in table:
        if percent <= 0 or a == b:
            out.append([a, b, f, d])
            continue
        if percent >= 100:
            nf = 0.0 if f < 127.5 else 255.0
        else:
            k = 1.0 / (1.0 - percent / 100.0)
            nf = min(255.0, max(0.0, (f - 127.5) * k + 127.5))
        out.append([a, b, int(nf + 0.5), d])
    return out
