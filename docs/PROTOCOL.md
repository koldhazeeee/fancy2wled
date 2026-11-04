# Sync Box broadcast protocol

How the FancyLEDs Sync Box (tested: model FSB3P2.1) tells its wireless satellite / strip what colours to show,
as worked out by capturing and decoding the traffic on the owner's own network. This is unofficial and may differ
between firmware versions; if your box behaves differently, please open an issue with a capture.

## Transport

* UDP **broadcast** from the box to `<subnet>.255`, destination port **19875** (source port 19876).
* ~13 frames/s while the box is processing video, ~17-18 frames/s for a static colour.
* No pairing, registration, authentication or encryption is needed to receive it: any host on the same layer-2 network
  can listen. (It therefore does not cross VLANs or routers, and some access points drop broadcast frames between
  clients - see *Troubleshooting* in the README.)
* The box also sends a Tuya discovery announcement to `255.255.255.255:6667`; that is unrelated.

## Frame

86-93 bytes. The length varies because of a variable-length key:

```
offset  size   meaning
0       3      55 AA 5A            magic
3       1      n = total length - 5  (0x51 .. 0x58)
4       1      00
5       1      varies (unknown)
6       1      derived from n (unknown purpose; n-0x1F for odd n, n-0x1D for even n)
7       1      varies (equals the last key byte in every capture)
8       q      KEY, q = total length - 83  (3..10 bytes), sent in the clear
8+q     75     DATA
```

`plain[i] = DATA[i] xor KEY[i mod q]` (the key repeats and restarts at the start of DATA), and the 75 plaintext
bytes are:

```
06 04 06               fixed marker (use it as the validity check)
24 x (R, G, B)         the 24 picture zones, 8 bits per channel
```

That is all there is: no checksum is needed. The "randomness" seen in raw captures is just the XOR key, which changes
every frame.

### Header fields and what the relay checks

| Byte | Meaning | Checked by the relay |
|---|---|---|
| 0-2 | magic `55 AA 5A` | yes |
| 3 | total length - 5 | yes (must match the datagram size, which must be 86-93) |
| 4 | always `00` | no |
| 5 | checksum: 167 + the sum of every byte except bytes 3 and 5, modulo 256 | no (holds on all 3,173 frames of a 4-minute capture) |
| 6 | a fixed function of byte 3 (81 -> 32, 82 -> 35, 83 -> 34, 84 -> 37, 85 -> 36, 86 -> 39, 87 -> 38, 88 -> 3B, hex) | no |
| 7 | the last byte of the key | no |

After the header: the key (size - 83 bytes) and 75 data bytes. The plaintext starts with `06 04 06`, which the relay requires (it doubles as the
integrity check). Those three bytes are probably the grid size (6 columns, 4 rows) plus one more value, which would fit the 24 zones, but that is a guess:
every box seen so far has the same grid.
## Zones

The 24 zones are numbered as a **6 x 4 grid**: `zone = row * 6 + col`, row 0 at the top, col 0 on the left. (The areas of the picture they look at are
not an even grid, see "Where each zone looks" below.)

```
 0  1  2  3  4  5
 6  7  8  9 10 11
12 13 14 15 16 17
18 19 20 21 22 23
```

Only the **20 border zones** drive the box's own LED strip; the 8 interior zones (7-10, 13-16) do not light it
(measured by lighting one zone at a time and filming the strip). Along each edge the strip follows the zones in screen
order with no mirroring: top left->right 0..5, right top->bottom 5,11,17,23, bottom left->right 18..23, left
top->bottom 0,6,12,18.

Each zone behaves like an *area average* of its region (a small bright square gives the same response anywhere inside
the zone), but the regions overlap their neighbours a little - a bright block exactly filling one tile also leaks into
the next zone.

## TV on / off (CEC)

The box sends **no separate packet** when the TV is switched on or off (tested on one box, FSB3P2.1, firmware Main 3.1.0, by switching the TV off and on
with its remote, which triggers CEC, while recording UDP ports 19874-19877 and 6666-6667 on a PC and the sync port on the relay). During the ~30 s the TV was off the
box kept broadcasting at its normal rate with unchanged header, key lengths, checksums and `06 04 06` marker, and every zone exactly `0,0,0`; the colours came back with
the first frame after the TV was on again. The Tuya announcements on 6667 did not change either. (Anything sent to a satellite privately, or inside the box's encrypted
local connection, would not have been heard by this test.)

So "TV off" can only be inferred: **all 24 zones are exactly zero for a while**. Normal viewing hardly ever does that (a 240 s capture of real content had only 0.1-0.2 s
stretches, a 57 s video clip none), but a long black screen does, so the relay waits for a configurable delay (`BlackTracker` / `tvIsOff` in `core.h`, default 15 s).

## If the protocol changes

The relay classifies every datagram on the port (understood / Sync Box framing with a payload it cannot read / unreadable) and reports it, so a firmware change
is noticed instead of looking like a switched-off box. See [RESILIENCE.md](RESILIENCE.md) for what it tolerates, what to capture before an update and how to compare.

## Delivery

The box sends a frame about 14 times a second to `255.255.255.255` / the subnet broadcast address. Through an access point they do not
necessarily arrive evenly: on the measured network most arrived back-to-back or about 102 ms apart (the beacon interval), see
[LATENCY.md](LATENCY.md). A receiver should use the newest frame and not assume a steady cadence.

## Where each zone looks

Measured with `tools/sync_probe` (a laptop's HDMI into the box, a white rectangle growing across a known picture, recorded on 2026-11-04): every zone is
an exact **average over one window**, with no taper (the ramps are straight lines to within 0.2 %), and the windows do not tile the picture. On a
1920 x 1080 picture:

| Zones | Window (pixels) |
|---|---|
| 0-5 (top row) | y 0-60; x 0-300, 300-600, 600-900, 960-1260, 1260-1560, 1560-1860 |
| 18-23 (bottom row) | y 1020-1080; the same six x windows |
| 6-11 (second row) | y 240-480; zone 6 is a strip at x 0-60, zone 11 at x 1860-1920, zones 7-10 use the x windows of columns 1-4 |
| 12-17 (third row) | y 540-780; zone 12 is a strip at x 0-60, zone 17 at x 1860-1920, zones 13-16 use the x windows of columns 1-4 |

So there is a 60 px gap in the middle of the top and bottom edges, nothing covers the top right and bottom right 60 px corners, and the two middle rows sit a
little above the middle of the picture. The relay places each zone at the middle of its window (`kPosAcross`, `kPosDown` in `src/core.cpp`). The positions
are good to about 3-6 pixels; the picture was scaled up to 4K by the laptop's graphics driver on the way in, so they are positions on the picture, not
input pixels.

## Colour processing in the box

The numbers in this section come from real video (a streaming source). A controlled test with a PC as the source (`tools/sync_probe`, SDR, RGB 4:4:4)
found something simpler: **the box subtracts a constant from the code values it receives** and does not expand limited range (output = input - 28 at
the default speed of diffusion, - 26 at 100 %; everything below that reads 0; white 255 reads 227 / 229, and white of limited-range video, code 235, reads
209). The box's brightness slider and its diffusion mode change nothing in the broadcast. How the box treats other signal types (YCbCr, HDR) has not been
compared, which may explain the curve below.

What comes out is not the raw picture:

* The curve is strongly non-linear: roughly `out ~ 201 * in^0.6`, with a hard black cut-off below about 10-25 % input.
* The maximum is about 200/255 for white and mixed colours (pure red/blue reach ~250).
* Green is yellowish: full-screen green arrived as about `(117, 195, 11)`.
* All 24 zones have identical gain and are neutral to within ~2 %.
* Temporal smoothing: this is the box app's **Speed of Diffusion** (the diffusion mode, low / medium / high, changes nothing in the broadcast). At 40 %
  each packet moves 40 % of the way to the target (a change shows 40, 64, 78, 87, 92 % over successive packets); at **100 % there is no smoothing at all**
  (a change arrives in one packet, measured with `tools/sync_probe`). Wi-Fi drops some packets, and a gap of 200 ms between two received packets held three
  steps, so the number of steps between packets is about the gap divided by 73 ms. At 40 %: each frame the output moves 40 % of the way to the target, `y = 0.6 * previous y + 0.4 * target`, at about 14
  frames a second (about 140 ms to half way, 350 ms to 90 %; 10-90 % rise is roughly 300 ms). Measured on a Sync Box FSB3P2.1 from 8,400
  pairs of consecutive same-direction changes in a 60 s capture: the ratio `d[n+1] / d[n]` peaks sharply at 0.6 (median 0.59), which is
  what a first-order filter gives while its input is constant. Undoing it needs only the previous frame:
  `target = y + 1.5 * (y - previous y)`; see [LATENCY.md](LATENCY.md).

The relay copies these values as they are, so the WLED strip mimics what the box's own strip would show. The
brightness / gamma / saturation settings are there if you prefer something closer to the screen.

## How it was found (short)

1. `pktmon` capture of all traffic from the box with sync active: only the broadcast above (plus the Tuya announcement).
2. The payload looked random, but frames of a static colour were periodic with periods that were multiples of 3 and
   whose lengths depended on the frame length: the signature of a repeating XOR key whose length is `len - 83`.
3. Decoding with the first `q` bytes as the key gave `06 04 06` followed by flat `RGB RGB ...` for a static colour in
   every frame of every capture.
4. Zone layout: sweeping white bars across the screen and watching which zones respond.
5. Zone-to-strip mapping: lighting one zone at a time and filming the box's strip.

`tools/reference/protocol.py` and `layout.py` are small reference encoder/decoder; `tools/make_golden.py` produces test vectors that
the C++ firmware core and the JavaScript calibration code must both reproduce.
