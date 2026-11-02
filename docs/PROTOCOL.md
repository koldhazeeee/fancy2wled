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

## Zones

The 24 zones are a **6 x 4 grid over the picture**: `zone = row * 6 + col`, row 0 at the top, col 0 on the left.

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

## Colour processing in the box

What comes out is not the raw picture:

* The curve is strongly non-linear: roughly `out ~ 201 * in^0.6`, with a hard black cut-off below about 10-25 % input.
* The maximum is about 200/255 for white and mixed colours (pure red/blue reach ~250).
* Green is yellowish: full-screen green arrived as about `(117, 195, 11)`.
* All 24 zones have identical gain and are neutral to within ~2 %.
* Temporal smoothing of roughly 300 ms (10-90 % rise/fall).

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
